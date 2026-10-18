//! Capability-gated operational OnOff/Level/sensor transactions. No automatic mutation replay.
use crate::{
    control::{BindingMember, BindingTarget, CommandRequest, DeviceRequest, Id, Request},
    devices::Snapshot,
};
use core::num::NonZeroU8;
use rs_matter::tlv::Nullable;
use rs_matter::{
    crypto::Crypto,
    dm::clusters::app::{
        level_control::{LevelControlClient, OptionsBitmap},
        on_off::OnOffClient,
    },
    dm::clusters::decl::{
        air_quality::AirQualityClient,
        carbon_dioxide_concentration_measurement::CarbonDioxideConcentrationMeasurementClient,
        pm_25_concentration_measurement::Pm25ConcentrationMeasurementClient,
        relative_humidity_measurement::RelativeHumidityMeasurementClient,
        temperature_measurement::TemperatureMeasurementClient,
        time_synchronization::{GranularityEnum, TimeSourceEnum, TimeSynchronizationClient},
    },
    error::{Error, ErrorCode},
    im::{client::ImClient, AttrPath},
    transport::exchange::Exchange,
    Matter,
};
use serde::Serialize;

#[derive(Clone)]
pub enum Action {
    Delete { local: bool },
    Read,
    ReadLevel,
    ReadSensor {
        cluster: u32,
    },
    On,
    Off,
    Toggle,
    MoveToLevel {
        level: u8,
        transition_deciseconds: u16,
    },
    SyncTime { time_zone: String, offset_seconds: i32 },
    Bind { targets: Vec<BindingTarget> },
}
#[derive(Clone)]
pub struct Operation {
    pub node: u64,
    pub endpoint: u16,
    pub action: Action,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Failure {
    Malformed,
    NotReady,
    NotFound,
    Unsupported,
    Timeout,
    Accessory,
    Busy,
    Capacity,
    Storage,
}
#[derive(Clone, Serialize)]
#[serde(tag = "type", content = "value", rename_all = "snake_case")]
pub enum Value {
    Bool(bool),
    U8(Option<u8>),
    Measurement(MeasurementValue),
    AirQuality(AirQualityValue),
    RawMeasurement(Option<f32>),
}
#[derive(Clone, Serialize)]
pub struct MeasurementValue {
    pub value: Option<f32>,
    pub unit: &'static str,
    pub quality: &'static str,
}
#[derive(Clone, Serialize)]
pub struct AirQualityValue {
    pub value: &'static str,
    pub quality: &'static str,
}
#[derive(Clone, Serialize)]
pub struct ResultValue {
    pub node: Id,
    pub endpoint: u16,
    pub cluster: u32,
    pub attribute: u32,
    pub value: Value,
}

async fn provision_group_member<C: Crypto>(
    matter: &Matter<'_>, crypto: &C, fabric: NonZeroU8, member: BindingMember,
    group: u16, key: &[u8],
) -> Result<(), Error> {
    use rs_matter::dm::clusters::decl::group_key_management::{
        GroupKeyManagementClient, GroupKeyMapStructArrayBuilder,
        GroupKeyMulticastPolicyEnum, GroupKeySecurityPolicyEnum,
    };
    use rs_matter::dm::clusters::decl::groups::GroupsClient;
    use rs_matter::tlv::OctetStr;

    let exchange = Exchange::initiate(matter, crypto, fabric, member.node.0).await?;
    let mut key_map = exchange.group_key_management().group_key_map_read_with(0, |entries| {
        let entries = entries?;
        entries.iter().map(|entry| {
            let entry = entry?;
            Ok((entry.group_id()?, entry.group_key_set_id()?))
        }).collect::<Result<Vec<_>, Error>>()
    }).await??;
    if key_map.iter().any(|(mapped_group, key_id)|
        (*key_id == group && *mapped_group != group) || (*mapped_group == group && *key_id != group)) {
        return Err(ErrorCode::ConstraintError.into());
    }
    key_map.retain(|(mapped_group, _)| *mapped_group != group);
    key_map.push((group, group));

    Exchange::initiate(matter, crypto, fabric, member.node.0).await?
        .group_key_management().key_set_write(0, |request| {
            request.group_key_set()?.group_key_set_id(group)?
                .group_key_security_policy(GroupKeySecurityPolicyEnum::TrustFirst)?
                .epoch_key_0(Nullable::some(OctetStr::new(key)))?
                .epoch_start_time_0(Nullable::some(0))?
                .epoch_key_1(Nullable::none())?.epoch_start_time_1(Nullable::none())?
                .epoch_key_2(Nullable::none())?.epoch_start_time_2(Nullable::none())?
                .group_key_multicast_policy(GroupKeyMulticastPolicyEnum::PerGroupID)?.end()?.end()
        }).await?;

    let response = Exchange::initiate(matter, crypto, fabric, member.node.0).await?
        .write_with(None, |builder| {
            let mut data = builder.write_requests()?.push()?.path(0, 63, 0)?
                .data_builder::<GroupKeyMapStructArrayBuilder<_>>()?;
            for (mapped_group, key_id) in &key_map {
                data = data.push()?.group_id(*mapped_group)?.group_key_set_id(*key_id)?
                    .fabric_index(Some(fabric.get()))?.end()?;
            }
            data.end()?.end()?.end()?.end()
        }).await?;
    let mut map_written = false;
    for (endpoint, status) in response.response()?.statuses(63, 0) {
        status?;
        if endpoint != 0 || map_written { return Err(ErrorCode::InvalidData.into()); }
        map_written = true;
    }
    if !map_written { return Err(ErrorCode::InvalidData.into()); }

    let handle = Exchange::initiate(matter, crypto, fabric, member.node.0).await?
        .groups().add_group(member.endpoint, |request| {
            request.group_id(group)?.group_name("Gateway Matter group")?.end()
        }).await?;
    let response = handle.response()?;
    if response.group_id()? != group || response.status()? > 1 {
        return Err(ErrorCode::Failure.into());
    }
    handle.complete().await
}

/// Decode supported attribute paths from an unsolicited Matter ReportData
/// message into the same value shape used by confirmed reads. Unsupported
/// paths are ignored; malformed values for supported paths reject the report.
pub fn report_values(
    node: u64,
    report: &rs_matter::im::ReportDataResp<'_>,
) -> Result<Vec<ResultValue>, Error> {
    let mut values = Vec::new();
    for (endpoint, value) in report.attrs::<bool>(6, 0) {
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 6,
            attribute: 0,
            value: Value::Bool(value?),
        });
    }
    for (endpoint, value) in report.attrs::<bool>(69, 0) {
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 69,
            attribute: 0,
            value: Value::Bool(value?),
        });
    }
    for (endpoint, value) in report.attrs::<Nullable<u8>>(8, 0) {
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 8,
            attribute: 0,
            value: Value::U8(value?.as_opt_ref().copied()),
        });
    }
    for (endpoint, value) in
        report.attrs::<rs_matter::dm::clusters::decl::air_quality::AirQualityEnum>(91, 0)
    {
        use rs_matter::dm::clusters::decl::air_quality::AirQualityEnum;
        let (value, quality) = match value? {
            AirQualityEnum::Unknown => ("unknown", "unknown"),
            AirQualityEnum::Good => ("good", "measured"),
            AirQualityEnum::Fair => ("fair", "measured"),
            AirQualityEnum::Moderate => ("moderate", "measured"),
            AirQualityEnum::Poor => ("poor", "measured"),
            AirQualityEnum::VeryPoor => ("very_poor", "measured"),
            AirQualityEnum::ExtremelyPoor => ("extremely_poor", "measured"),
        };
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 91,
            attribute: 0,
            value: Value::AirQuality(AirQualityValue { value, quality }),
        });
    }
    for (endpoint, value) in report.attrs::<Nullable<i16>>(1026, 0) {
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 1026,
            attribute: 0,
            value: measurement(
                value?.as_opt_ref().map(|value| f32::from(*value) / 100.0),
                "celsius",
            ),
        });
    }
    for (endpoint, value) in report.attrs::<Nullable<u16>>(1029, 0) {
        let value = value?.as_opt_ref().copied();
        if value.is_some_and(|value| value > 10_000) {
            return Err(ErrorCode::InvalidData.into());
        }
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 1029,
            attribute: 0,
            value: measurement(value.map(|value| f32::from(value) / 100.0), "percent"),
        });
    }
    for (endpoint, value) in report.attrs::<Nullable<f32>>(1037, 0) {
        let value = value?.as_opt_ref().copied();
        if value.is_some_and(|value| !value.is_finite() || value < 0.0) {
            return Err(ErrorCode::InvalidData.into());
        }
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 1037,
            attribute: 0,
            value: Value::RawMeasurement(value),
        });
    }
    for (endpoint, value) in report.attrs::<Nullable<f32>>(1066, 0) {
        let value = value?.as_opt_ref().copied();
        if value.is_some_and(|value| !value.is_finite() || value < 0.0) {
            return Err(ErrorCode::InvalidData.into());
        }
        values.push(ResultValue {
            node: Id(node),
            endpoint,
            cluster: 1066,
            attribute: 0,
            value: Value::RawMeasurement(value),
        });
    }
    Ok(values)
}

impl Operation {
    pub fn decode(request: Request) -> Result<Self, Failure> {
        let (node, endpoint, action) = match request {
            Request::Device(DeviceRequest::Delete { node, local, .. }) => (node, 0, Action::Delete { local }),
            Request::Device(DeviceRequest::Bind { node, endpoint, targets, .. }) => {
                if targets.is_empty() || targets.len() > 32 || targets.iter().any(|target| {
                    !matches!(target.cluster, 6 | 8) ||
                    (target.group.is_some() == (target.node.is_some() || target.endpoint.is_some())) ||
                    target.endpoint.is_some() != target.node.is_some() ||
                    target.node.is_some_and(|node| node.0 < 2) || target.endpoint == Some(u16::MAX) ||
                    target.group == Some(0) || target.members.len() > 32 ||
                    (target.group.is_some() && target.members.is_empty()) ||
                    (target.group.is_none() && !target.members.is_empty()) ||
                    target.members.iter().any(|member| member.node.0 < 2 || member.endpoint == u16::MAX ||
                        target.members.iter().filter(|candidate| candidate.node == member.node &&
                            candidate.endpoint == member.endpoint).count() != 1)
                }) { return Err(Failure::Malformed); }
                for (index, target) in targets.iter().enumerate() {
                    if target.group.is_some() && targets[..index].iter().any(|previous|
                        previous.group == target.group && previous.members != target.members) {
                        return Err(Failure::Malformed);
                    }
                }
                (node, endpoint, Action::Bind { targets })
            }
            Request::Device(DeviceRequest::Read {
                node,
                endpoint,
                cluster: 6,
                attribute: 0,
                ..
            }) => (node, endpoint, Action::Read),
            Request::Device(DeviceRequest::Read {
                node,
                endpoint,
                cluster: 8,
                attribute: 0,
                ..
            }) => (node, endpoint, Action::ReadLevel),
            Request::Device(DeviceRequest::Read {
                node,
                endpoint,
                cluster,
                attribute: 0,
                ..
            }) if matches!(cluster, 69 | 91 | 1026 | 1029 | 1037 | 1066) => {
                (node, endpoint, Action::ReadSensor { cluster })
            }
            Request::Command(CommandRequest::On { node, endpoint, .. }) => {
                (node, endpoint, Action::On)
            }
            Request::Command(CommandRequest::Off { node, endpoint, .. }) => {
                (node, endpoint, Action::Off)
            }
            Request::Command(CommandRequest::Toggle { node, endpoint, .. }) => {
                (node, endpoint, Action::Toggle)
            }
            Request::Command(CommandRequest::MoveToLevel {
                node,
                endpoint,
                level,
                transition_deciseconds,
                ..
            }) => (
                node,
                endpoint,
                Action::MoveToLevel {
                    level,
                    transition_deciseconds,
                },
            ),
            Request::Command(CommandRequest::SyncTime { node, endpoint, time_zone, offset_seconds, .. }) => {
                (node, endpoint, Action::SyncTime { time_zone, offset_seconds })
            }
            _ => return Err(Failure::Unsupported),
        };
        if node.0 < 2 || endpoint == u16::MAX {
            return Err(Failure::Malformed);
        }
        Ok(Self {
            node: node.0,
            endpoint,
            action,
        })
    }
    pub fn authorize(&self, snapshots: &[Snapshot]) -> Result<(), Failure> {
        // Decommissioning also works for interrupted nodes without endpoint metadata.
        if matches!(self.action, Action::Delete { .. }) { return Ok(()); }
        let snapshot = snapshots
            .iter()
            .find(|s| s.node == self.node)
            .ok_or(Failure::NotFound)?;
        if !snapshot.online {
            return Err(Failure::NotReady);
        }
        let endpoint = snapshot
            .endpoints
            .iter()
            .find(|e| e.id == self.endpoint)
            .ok_or(Failure::NotFound)?;
        let required_cluster = match &self.action {
            Action::Delete { .. } => unreachable!(),
            Action::Read | Action::On | Action::Off | Action::Toggle => 6,
            Action::ReadLevel | Action::MoveToLevel { .. } => 8,
            Action::ReadSensor { cluster } => *cluster,
            Action::Bind { .. } => 59,
            Action::SyncTime { .. } => 56,
        };
        if !endpoint.servers.contains(&required_cluster) ||
            (matches!(self.action, Action::Bind { .. }) && !endpoint.servers.contains(&30)) {
            return Err(Failure::Unsupported);
        }
        if let Action::Bind { targets } = &self.action {
            for target in targets {
                if let (Some(node), Some(endpoint)) = (target.node, target.endpoint) {
                    let target_endpoint = snapshots.iter().find(|candidate| candidate.node == node.0 && candidate.online)
                        .and_then(|candidate| candidate.endpoints.iter().find(|item| item.id == endpoint))
                        .ok_or(Failure::NotFound)?;
                    if !target_endpoint.servers.contains(&target.cluster) { return Err(Failure::Unsupported); }
                }
                if target.group.is_some() {
                    for member in &target.members {
                        let snapshot = snapshots.iter().find(|candidate| candidate.node == member.node.0 && candidate.online)
                            .ok_or(Failure::NotFound)?;
                        let group_endpoint = snapshot.endpoints.iter().find(|item| item.id == member.endpoint)
                            .ok_or(Failure::NotFound)?;
                        if !group_endpoint.servers.contains(&4) { return Err(Failure::Unsupported); }
                        let key_management = snapshot.endpoints.iter().find(|item| item.id == 0)
                            .ok_or(Failure::Unsupported)?;
                        if !key_management.servers.contains(&63) { return Err(Failure::Unsupported); }
                    }
                }
            }
        }
        Ok(())
    }
    /// Callers authorize against the current catalog/snapshot before starting.
    /// Cancellation after invocation may leave a changed accessory; do not replay.
    pub async fn run<C: Crypto>(
        self,
        matter: &Matter<'_>,
        crypto: C,
        fabric: NonZeroU8,
    ) -> Result<ResultValue, Failure> {
        self.run_for(
            matter,
            crypto,
            fabric,
            embassy_time::Duration::from_secs(30),
        )
        .await
    }
    pub async fn run_for<C: Crypto>(
        self,
        matter: &Matter<'_>,
        crypto: C,
        fabric: NonZeroU8,
        timeout: embassy_time::Duration,
    ) -> Result<ResultValue, Failure> {
        let transaction = async {
            if let Action::Delete { local } = self.action {
                if !local {
                    use rs_matter::dm::clusters::noc::{OperationalCredentialsClient, NodeOperationalCertStatusEnum};
                    // Fabric indexes are local to the accessory, not the controller.
                    let index = Exchange::initiate(matter, &crypto, fabric, self.node).await?
                        .operational_credentials().current_fabric_index_read(0).await?;
                    if index == 0 { return Err(ErrorCode::InvalidData.into()); }
                    let handle = Exchange::initiate(matter, &crypto, fabric, self.node).await?
                        .operational_credentials().remove_fabric(0, |request| request.fabric_index(index)?.end()).await?;
                    if handle.response()?.status_code()? != NodeOperationalCertStatusEnum::OK {
                        return Err(ErrorCode::Failure.into());
                    }
                    handle.complete().await?;
                }
                return Ok(ResultValue { node: Id(self.node), endpoint: 0, cluster: 62, attribute: 0, value: Value::Bool(true) });
            }
            if let Action::Bind { targets } = &self.action {
                use rs_matter::dm::clusters::decl::binding::{BindingClient, TargetStructArrayBuilder};
                let mut provisioned_groups = Vec::new();
                for target in targets.iter().filter(|target| target.group.is_some()) {
                    let group = target.group.ok_or(ErrorCode::InvalidData)?;
                    if provisioned_groups.contains(&group) { continue; }
                    let mut key = zeroize::Zeroizing::new([0u8; 16]);
                    use rs_matter::crypto::Rng;
                    crypto.rand()?.fill_bytes(&mut *key);
                    for member in &target.members {
                        provision_group_member(matter, &crypto, fabric, *member, group, &key[..]).await?;
                    }
                    provisioned_groups.push(group);
                }
                let read_exchange = Exchange::initiate(matter, &crypto, fabric, self.node).await?;
                let mut merged = read_exchange.binding().binding_read_with(self.endpoint, |read| {
                    let entries = read?;
                    entries.iter().map(|entry| {
                        let entry = entry?;
                        Ok(BindingTarget {
                            node: entry.node()?.map(Id), endpoint: entry.endpoint()?,
                            group: entry.group()?, members: vec![], cluster: entry.cluster()?.ok_or(ErrorCode::InvalidData)?,
                        })
                    }).collect::<Result<Vec<_>, Error>>()
                }).await??;
                for target in targets.iter() {
                    let duplicate = merged.iter().any(|entry| entry.node.map(|id| id.0) == target.node.map(|id| id.0) &&
                        entry.endpoint == target.endpoint && entry.group == target.group && entry.cluster == target.cluster);
                    if !duplicate { merged.push(target.clone()); }
                }
                if merged.len() > 128 { return Err(ErrorCode::ResourceExhausted.into()); }
                let exchange = Exchange::initiate(matter, &crypto, fabric, self.node).await?;
                let response = exchange.write_with(None, |builder| {
                    let mut data = builder.write_requests()?.push()?.path(self.endpoint, 30, 0)?
                        .data_builder::<TargetStructArrayBuilder<_>>()?;
                    for target in &merged {
                        data = data.push()?.node(target.node.map(|node| node.0))?
                            .group(target.group)?.endpoint(target.endpoint)?
                            .cluster(Some(target.cluster))?.fabric_index(Some(fabric.get()))?.end()?;
                    }
                    data.end()?.end()?.end()?.end()
                }).await?;
                let mut found = false;
                for (endpoint, result) in response.response()?.statuses(30, 0) {
                    result?;
                    if endpoint != self.endpoint || found { return Err(ErrorCode::InvalidData.into()); }
                    found = true;
                }
                if !found { return Err(ErrorCode::InvalidData.into()); }
                return Ok(ResultValue {
                    node: Id(self.node), endpoint: self.endpoint, cluster: 30, attribute: 0,
                    value: Value::Bool(true),
                });
            }
            if !matches!(
                self.action,
                Action::Read | Action::ReadLevel | Action::ReadSensor { .. }
            ) {
                let exchange = Exchange::initiate(matter, &crypto, fabric, self.node).await?;
                match self.action {
                    Action::On => exchange.on_off().on(self.endpoint).await?,
                    Action::Off => exchange.on_off().off(self.endpoint).await?,
                    Action::Toggle => exchange.on_off().toggle(self.endpoint).await?,
                    Action::MoveToLevel {
                        level,
                        transition_deciseconds,
                    } => {
                        exchange
                            .level_control()
                            .move_to_level(self.endpoint, |request| {
                                request
                                    .level(level)?
                                    .transition_time(Nullable::some(transition_deciseconds))?
                                    .options_mask(OptionsBitmap::empty())?
                                    .options_override(OptionsBitmap::empty())?
                                    .end()
                            })
                            .await?
                    }
                    Action::SyncTime { ref time_zone, offset_seconds } => {
                        const MATTER_EPOCH_UNIX_SECONDS: u64 = 946_684_800;
                        let unix = std::time::SystemTime::now()
                            .duration_since(std::time::UNIX_EPOCH)
                            .map_err(|_| ErrorCode::InvalidData)?;
                        let seconds = unix.as_secs().checked_sub(MATTER_EPOCH_UNIX_SECONDS)
                            .ok_or(ErrorCode::InvalidData)?;
                        let utc_us = seconds.checked_mul(1_000_000)
                            .and_then(|value| value.checked_add(u64::from(unix.subsec_micros())))
                            .ok_or(ErrorCode::InvalidData)?;
                        let name = time_zone.as_str();
                        let mut zone = exchange.time_synchronization().set_time_zone(self.endpoint, |request| {
                            request.time_zone()?.push()?.offset(offset_seconds)?.valid_at(utc_us)?
                                .name(Some(name))?.end()?.end()?.end()
                        }).await?;
                        let requires_dst = zone.response()?.dst_offset_required()?;
                        zone.complete().await?;
                        if requires_dst { return Err(ErrorCode::InvalidInState.into()); }
                        Exchange::initiate(matter, &crypto, fabric, self.node).await?.time_synchronization().set_utc_time(self.endpoint, |request| {
                            request.utc_time(utc_us)?
                                .granularity(GranularityEnum::MicrosecondsGranularity)?
                                .time_source(Some(TimeSourceEnum::Admin))?.end()
                        }).await?
                    }
                    Action::Bind { .. } | Action::Delete { .. } => unreachable!(),
                    Action::Read | Action::ReadLevel | Action::ReadSensor { .. } => unreachable!(),
                }
            }
            if matches!(self.action, Action::SyncTime { .. }) {
                return Ok(ResultValue { node: Id(self.node), endpoint: self.endpoint, cluster: 56,
                    attribute: 0, value: Value::Bool(true) });
            }
            // An invocation acknowledgement is insufficient: return the actual value.
            let exchange = Exchange::initiate(matter, &crypto, fabric, self.node).await?;
            if let Action::ReadSensor { cluster } = self.action {
                let (value, attribute) = match cluster {
                    69 => {
                        let path = AttrPath {
                            endpoint: Some(self.endpoint),
                            cluster: Some(69),
                            attr: Some(0),
                            ..AttrPath::default()
                        };
                        let mut chunk = exchange
                            .read_with(|builder| {
                                builder
                                    .attr_requests_from(core::slice::from_ref(&path))?
                                    .fabric_filtered(true)?
                                    .end()
                            })
                            .await?;
                        let mut state = None;
                        loop {
                            {
                                let response = chunk.response()?;
                                for (endpoint, result) in response.attrs::<bool>(69, 0) {
                                    if endpoint != self.endpoint || state.is_some() {
                                        return Err(ErrorCode::InvalidData.into());
                                    }
                                    state = Some(result?);
                                }
                            }
                            match chunk.complete().await? {
                                Some(next) => chunk = next,
                                None => break,
                            }
                        }
                        (Value::Bool(state.ok_or(ErrorCode::NotFound)?), 0)
                    }
                    91 => {
                        use rs_matter::dm::clusters::decl::air_quality::AirQualityEnum;
                        let value = exchange
                            .air_quality()
                            .air_quality_read(self.endpoint)
                            .await?;
                        let (value, quality) = match value {
                            AirQualityEnum::Unknown => ("unknown", "unknown"),
                            AirQualityEnum::Good => ("good", "measured"),
                            AirQualityEnum::Fair => ("fair", "measured"),
                            AirQualityEnum::Moderate => ("moderate", "measured"),
                            AirQualityEnum::Poor => ("poor", "measured"),
                            AirQualityEnum::VeryPoor => ("very_poor", "measured"),
                            AirQualityEnum::ExtremelyPoor => ("extremely_poor", "measured"),
                        };
                        (Value::AirQuality(AirQualityValue { value, quality }), 0)
                    }
                    1026 => {
                        let raw = exchange
                            .temperature_measurement()
                            .measured_value_read(self.endpoint)
                            .await?;
                        let value = raw.as_opt_ref().map(|value| f32::from(*value) / 100.0);
                        (measurement(value, "celsius"), 0)
                    }
                    1029 => {
                        let raw = exchange
                            .relative_humidity_measurement()
                            .measured_value_read(self.endpoint)
                            .await?;
                        if raw.as_opt_ref().is_some_and(|value| *value > 10_000) {
                            return Err(ErrorCode::InvalidData.into());
                        }
                        let value = raw.as_opt_ref().map(|value| f32::from(*value) / 100.0);
                        (measurement(value, "percent"), 0)
                    }
                    1037 => {
                        let raw = exchange
                            .carbon_dioxide_concentration_measurement()
                            .measured_value_read(self.endpoint)
                            .await?;
                        let unit = Exchange::initiate(matter, &crypto, fabric, self.node)
                            .await?
                            .carbon_dioxide_concentration_measurement()
                            .measurement_unit_read(self.endpoint)
                            .await?;
                        let value = raw.as_opt_ref().copied();
                        if value.is_some_and(|value| !value.is_finite() || value < 0.0) {
                            return Err(ErrorCode::InvalidData.into());
                        }
                        (measurement(value, measurement_unit(unit)), 0)
                    }
                    1066 => {
                        let raw = exchange
                            .pm_25_concentration_measurement()
                            .measured_value_read(self.endpoint)
                            .await?;
                        let unit = Exchange::initiate(matter, &crypto, fabric, self.node)
                            .await?
                            .pm_25_concentration_measurement()
                            .measurement_unit_read(self.endpoint)
                            .await?;
                        let value = raw.as_opt_ref().copied();
                        if value.is_some_and(|value| !value.is_finite() || value < 0.0) {
                            return Err(ErrorCode::InvalidData.into());
                        }
                        (measurement(value, measurement_unit(unit)), 0)
                    }
                    _ => return Err(ErrorCode::InvalidData.into()),
                };
                return Ok(ResultValue {
                    node: Id(self.node),
                    endpoint: self.endpoint,
                    cluster,
                    attribute,
                    value,
                });
            }
            if matches!(self.action, Action::ReadLevel | Action::MoveToLevel { .. }) {
                let value = exchange
                    .level_control()
                    .current_level_read(self.endpoint)
                    .await?;
                let value = value.as_opt_ref().copied();
                if matches!(self.action, Action::MoveToLevel { .. }) && value.is_none() {
                    return Err(ErrorCode::Failure.into());
                }
                return Ok(ResultValue {
                    node: Id(self.node),
                    endpoint: self.endpoint,
                    cluster: 8,
                    attribute: 0,
                    value: Value::U8(value),
                });
            }
            let value = exchange.on_off().on_off_read(self.endpoint).await?;
            Ok::<_, Error>(ResultValue {
                node: Id(self.node),
                endpoint: self.endpoint,
                cluster: 6,
                attribute: 0,
                value: Value::Bool(value),
            })
        };
        embassy_time::with_timeout(timeout, transaction)
            .await
            .map_err(|_| Failure::Timeout)?
            .map_err(|error| match error.code() {
                ErrorCode::RxTimeout => Failure::Timeout,
                _ => Failure::Accessory,
            })
    }
}

fn measurement(value: Option<f32>, unit: &'static str) -> Value {
    let quality = if value.is_some() {
        "measured"
    } else {
        "unknown"
    };
    Value::Measurement(MeasurementValue {
        value,
        unit,
        quality,
    })
}

pub fn measurement_unit(
    unit: rs_matter::dm::clusters::decl::globals::MeasurementUnitEnum,
) -> &'static str {
    use rs_matter::dm::clusters::decl::globals::MeasurementUnitEnum;
    match unit {
        MeasurementUnitEnum::PPM => "ppm",
        MeasurementUnitEnum::PPB => "ppb",
        MeasurementUnitEnum::PPT => "ppt",
        MeasurementUnitEnum::MGM3 => "mg/m3",
        MeasurementUnitEnum::UGM3 => "ug/m3",
        MeasurementUnitEnum::NGM3 => "ng/m3",
        MeasurementUnitEnum::PM3 => "particles/m3",
        MeasurementUnitEnum::BQM3 => "Bq/m3",
        MeasurementUnitEnum::DBPM => "dB/m",
        MeasurementUnitEnum::PCFT => "particles/ft3",
    }
}

pub async fn read_measurement_unit<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
) -> Result<&'static str, Error> {
    let exchange = Exchange::initiate(matter, crypto, fabric, node).await?;
    let unit = match cluster {
        1037 => {
            exchange
                .carbon_dioxide_concentration_measurement()
                .measurement_unit_read(endpoint)
                .await?
        }
        1066 => {
            exchange
                .pm_25_concentration_measurement()
                .measurement_unit_read(endpoint)
                .await?
        }
        _ => return Err(ErrorCode::InvalidData.into()),
    };
    Ok(measurement_unit(unit))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn binding_requests_require_supported_targets_and_complete_group_membership() {
        let decode = |body: &str| {
            let request = Request::decode(10, body.as_bytes()).unwrap();
            Operation::decode(request)
        };
        assert!(decode(r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"node":"223456789abcdef0","endpoint":2,"cluster":6}]}"#).is_ok());
        assert!(decode(r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"group":7,"members":[{"node":"223456789abcdef0","endpoint":2}],"cluster":6}]}"#).is_ok());
        assert!(decode(r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"group":7,"cluster":6}]}"#).is_err());
        assert!(decode(r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"group":7,"members":[{"node":"223456789abcdef0","endpoint":2}],"cluster":42}]}"#).is_err());
    }
    use rs_matter::{
        im::{AttrData, AttrPath, AttrResp, ReportDataResp},
        tlv::{FromTLV, TLVElement, TLVTag, TLVWrite, ToTLV},
        utils::storage::WriteBuf,
    };

    fn append_attribute_report(writer: &mut WriteBuf<'_>, cluster: u32, data: &[u8]) {
        AttrResp::from(AttrData::new(
            None,
            AttrPath {
                endpoint: Some(1),
                cluster: Some(cluster),
                attr: Some(0),
                ..Default::default()
            },
            TLVElement::new(data),
        ))
        .to_tlv(&TLVTag::Anonymous, writer)
        .unwrap_or_else(|error| panic!("attribute report cluster {cluster}: {error:?}"));
    }

    fn encode_attribute_value<T: ToTLV>(value: &T) -> Vec<u8> {
        let mut buffer = [0u8; 32];
        let mut writer = WriteBuf::new(&mut buffer);
        value.to_tlv(&TLVTag::Context(2), &mut writer).unwrap();
        writer.as_slice().to_vec()
    }

    #[test]
    fn report_decoder_normalizes_supported_sensor_attributes() {
        let mut buffer = [0u8; 512];
        let mut writer = WriteBuf::new(&mut buffer);
        writer.start_struct(&TLVTag::Anonymous).unwrap();
        writer.u32(&TLVTag::Context(0), 7).unwrap();
        writer.start_array(&TLVTag::Context(1)).unwrap();
        append_attribute_report(&mut writer, 69, &encode_attribute_value(&true));
        append_attribute_report(
            &mut writer,
            1026,
            &encode_attribute_value(&Nullable::some(-525i16)),
        );
        append_attribute_report(
            &mut writer,
            1026,
            &encode_attribute_value(&Nullable::<i16>::none()),
        );
        append_attribute_report(
            &mut writer,
            1029,
            &encode_attribute_value(&Nullable::some(6400u16)),
        );
        append_attribute_report(
            &mut writer,
            91,
            &encode_attribute_value(
                &rs_matter::dm::clusters::decl::air_quality::AirQualityEnum::Poor,
            ),
        );
        append_attribute_report(
            &mut writer,
            1037,
            &encode_attribute_value(&Nullable::some(800.0f32)),
        );
        append_attribute_report(
            &mut writer,
            1066,
            &encode_attribute_value(&Nullable::some(25.5f32)),
        );
        writer.end_container().unwrap();
        writer.u8(&TLVTag::Context(0xff), 12).unwrap();
        writer.end_container().unwrap();
        let report = ReportDataResp::from_tlv(&TLVElement::new(writer.as_slice())).unwrap();

        let values = report_values(2, &report).unwrap();
        assert_eq!(values.len(), 7);
        assert!(matches!(values[0].value, Value::Bool(true)));
        assert!(matches!(values[1].value, Value::AirQuality(ref value)
            if value.value == "poor" && value.quality == "measured"));
        assert!(matches!(values[2].value, Value::Measurement(ref value)
            if value.value == Some(-5.25) && value.unit == "celsius" && value.quality == "measured"));
        assert!(matches!(values[3].value, Value::Measurement(ref value)
            if value.value.is_none() && value.unit == "celsius" && value.quality == "unknown"));
        assert!(matches!(values[4].value, Value::Measurement(ref value)
            if value.value == Some(64.0) && value.unit == "percent" && value.quality == "measured"));
        assert!(matches!(values[5].value, Value::RawMeasurement(Some(value)) if value == 800.0));
        assert!(matches!(values[6].value, Value::RawMeasurement(Some(value)) if value == 25.5));
    }
    #[test]
    fn unsupported_paths_and_invalid_identity_are_rejected() {
        let decode = |opcode, body: &str| {
            Operation::decode(Request::decode(opcode, body.as_bytes()).unwrap())
        };
        let level_read = decode(10, r#"{"schema":1,"action":"read","node":"0000000000000002","endpoint":1,"cluster":8,"attribute":0}"#).unwrap();
        assert!(matches!(level_read.action, Action::ReadLevel));
        assert_eq!(level_read.authorize(&[]), Err(Failure::NotFound));
        let move_to_level = decode(11, r#"{"schema":1,"command":"move_to_level","node":"0000000000000002","endpoint":1,"level":124,"transition_deciseconds":20}"#).unwrap();
        assert_eq!(move_to_level.authorize(&[]), Err(Failure::NotFound));
        for cluster in [69, 91, 1026, 1029, 1037, 1066] {
            let body = format!(
                r#"{{"schema":1,"action":"read","node":"0000000000000002","endpoint":1,"cluster":{cluster},"attribute":0}}"#
            );
            let sensor = decode(10, &body).unwrap();
            assert!(
                matches!(sensor.action, Action::ReadSensor { cluster: actual } if actual == cluster)
            );
        }
        assert!(matches!(
            decode(
                10,
                r#"{"schema":1,"action":"read","node":"0000000000000002","endpoint":1,"cluster":1066,"attribute":1}"#
            ),
            Err(Failure::Unsupported)
        ));
        assert!(matches!(
            decode(
                11,
                r#"{"schema":1,"command":"on","node":"0000000000000001","endpoint":1}"#
            ),
            Err(Failure::Malformed)
        ));
        assert!(matches!(
            decode(
                11,
                r#"{"schema":1,"command":"off","node":"0000000000000002","endpoint":65535}"#
            ),
            Err(Failure::Malformed)
        ));
        let operation = decode(
            11,
            r#"{"schema":1,"command":"toggle","node":"0000000000000002","endpoint":1}"#,
        )
        .unwrap();
        assert_eq!(operation.authorize(&[]), Err(Failure::NotFound));
        let encoded = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 6,
            attribute: 0,
            value: Value::Bool(false),
        }))
        .unwrap();
        assert_eq!(encoded, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":6,"attribute":0,"value":{"type":"bool","value":false}}}"#);
        let encoded = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 8,
            attribute: 0,
            value: Value::U8(Some(124)),
        }))
        .unwrap();
        assert_eq!(encoded, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":8,"attribute":0,"value":{"type":"u8","value":124}}}"#);
    }

    #[test]
    fn typed_sensor_values_preserve_unit_and_null_quality() {
        let measured = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 1026,
            attribute: 0,
            value: measurement(Some(-5.25), "celsius"),
        }))
        .unwrap();
        assert_eq!(measured, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":1026,"attribute":0,"value":{"type":"measurement","value":{"value":-5.25,"unit":"celsius","quality":"measured"}}}}"#);
        let unknown = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 1066,
            attribute: 0,
            value: measurement(None, "ug/m3"),
        }))
        .unwrap();
        assert_eq!(unknown, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":1066,"attribute":0,"value":{"type":"measurement","value":{"value":null,"unit":"ug/m3","quality":"unknown"}}}}"#);
        let air = crate::responses::reply::<_, Failure>(Ok(ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 91,
            attribute: 0,
            value: Value::AirQuality(AirQualityValue {
                value: "moderate",
                quality: "measured",
            }),
        }))
        .unwrap();
        assert_eq!(air, br#"{"schema":1,"ok":true,"result":{"node":"0000000000000002","endpoint":1,"cluster":91,"attribute":0,"value":{"type":"air_quality","value":{"value":"moderate","quality":"measured"}}}}"#);
    }
}
