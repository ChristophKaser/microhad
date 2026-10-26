//! Bounded Interaction Model subscription establishment for operational endpoints.
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    dm::clusters::icd_mgmt::IcdManagementClient,
    error::{Error, ErrorCode},
    im::{
        client::{ImClient, SubscribeEstablished, SubscribeOutcome},
        AttrPath, EventFilter, EventPath,
    },
    tlv::{FromTLV, TLVArray},
    transport::exchange::Exchange,
    Matter,
};
use std::{cell::RefCell, rc::Rc};

pub const MIN_REPORT_INTERVAL_SECONDS: u16 = 1;
pub const MAX_REPORT_INTERVAL_SECONDS: u16 = 3600;

/// ICD publishers must not be forced to wake to satisfy a nonzero floor.
pub fn minimum_report_interval(is_icd: bool) -> u16 {
    if is_icd {
        0
    } else {
        MIN_REPORT_INTERVAL_SECONDS
    }
}

/// matter.js PeerSubscription: negotiated maximum + twice the maximum
/// response time. For incoming UDP reports that response time includes the
/// local MRP retry ladder, 10s processing time and a 5s response buffer.
pub fn report_timeout(matter: &Matter<'_>, max_interval: u16) -> std::time::Duration {
    let config = matter.dev_det();
    report_timeout_with_mrp(
        max_interval,
        config.sai.unwrap_or(300),
        config.sii.unwrap_or(500),
    )
}

fn report_timeout_with_mrp(max_interval: u16, active_ms: u32, idle_ms: u32) -> std::time::Duration {
    let retry_ms = rs_matter::transport::mrp::RetransEntry::retransmission_timeout_ms(
        active_ms, idle_ms, 4000, false,
    );
    std::time::Duration::from_secs(u64::from(max_interval.max(1)))
        + std::time::Duration::from_millis(2 * (retry_ms + 10_000 + 5_000))
}
pub const MAX_REPORT_VALUES: usize = 128;

/// Match matter.js PhysicalDeviceProperties: ICDs use their idle duration,
/// capped only by the uint16 wire limit, with no ceiling jitter.
fn icd_maximum_interval(idle_seconds: u32) -> u16 {
    idle_seconds.min(u32::from(u16::MAX)) as u16
}

async fn interval_bounds<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    icd_endpoint: Option<u16>,
    default_maximum: u16,
) -> Result<(u16, u16), Error> {
    if let Some(endpoint) = icd_endpoint {
        let idle = Exchange::initiate(matter, crypto, fabric, node)
            .await?
            .icd_management()
            .idle_mode_duration_read(endpoint)
            .await?;
        Ok((minimum_report_interval(true), icd_maximum_interval(idle)))
    } else {
        Ok((minimum_report_interval(false), default_maximum))
    }
}

/// Shared with the report receiver so retired IDs are rejected before ACKing
/// any chunk. Keys include the authenticated fabric and peer, not just the ID.
#[derive(Clone, Default)]
pub struct ReportSubscriptions(Rc<RefCell<Vec<(u8, u64, u32)>>>);

impl ReportSubscriptions {
    pub fn replace(&self, fabric: u8, identities: impl Iterator<Item = (u64, u32)>) {
        let mut active = self.0.borrow_mut();
        active.clear();
        active.extend(identities.map(|(node, id)| (fabric, node, id)));
    }

    fn accepts(&self, fabric: u8, node: u64, id: u32) -> bool {
        self.0.borrow().contains(&(fabric, node, id))
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClockEvent {
    pub endpoint: u16,
    pub cluster: u32,
    pub number: u64,
}

pub struct ReceivedReport {
    pub clock_events: Vec<ClockEvent>,
    pub fabric: u8,
    pub node: u64,
    pub subscription_id: u32,
    pub values: Vec<crate::operations::ResultValue>,
    pub buttons: Vec<MatterButtonEvent>,
}
#[derive(Clone, Debug, serde::Serialize)]
pub struct MatterButtonEvent {
    pub node: crate::control::Id,
    pub endpoint: u16,
    pub event: u8,
    pub position: Option<u8>,
    pub press_count: Option<u8>,
}
pub enum IncomingMessage {
    Report(ReceivedReport),
}
pub struct DeviceSubscription {
    pub clock_events: Vec<ClockEvent>,
    pub established: SubscribeEstablished,
    pub unit: Option<&'static str>,
    pub priming_values: Vec<crate::operations::ResultValue>,
}

/// Establish one bounded attribute subscription and drain every priming chunk.
/// Active reports are received separately on server-initiated exchanges.
pub async fn establish_attribute<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
    attribute: u32,
) -> Result<SubscribeEstablished, Error> {
    let path = AttrPath {
        endpoint: Some(endpoint),
        cluster: Some(cluster),
        attr: if cluster == 513 && attribute == u32::MAX {
            None
        } else {
            Some(attribute)
        },
        ..AttrPath::default()
    };
    let chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .subscribe_with(|builder| {
            builder
                .keep_subs(true)?
                .min_int_floor(MIN_REPORT_INTERVAL_SECONDS)?
                .max_int_ceil(MAX_REPORT_INTERVAL_SECONDS)?
                .attr_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut chunk = chunk;
    loop {
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => return Ok(established),
        }
    }
}

/// Establish one endpoint subscription and capture metadata needed to normalize
/// its reports. Matter concentration-unit attributes are static for the
/// lifetime of the endpoint subscription and are read before it is accepted.
pub async fn establish_device_attribute<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
    attribute: u32,
) -> Result<DeviceSubscription, Error> {
    establish_device_attribute_with_interval(
        matter,
        crypto,
        fabric,
        node,
        endpoint,
        cluster,
        attribute,
        MAX_REPORT_INTERVAL_SECONDS,
        None,
    )
    .await
}

pub async fn establish_device_attribute_with_interval<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
    attribute: u32,
    max_interval: u16,
    icd_endpoint: Option<u16>,
) -> Result<DeviceSubscription, Error> {
    let (min_interval, max_interval) =
        interval_bounds(matter, crypto, fabric, node, icd_endpoint, max_interval).await?;
    let unit = match cluster {
        1037 | 1066 => Some(
            crate::operations::read_measurement_unit(
                matter, crypto, fabric, node, endpoint, cluster,
            )
            .await?,
        ),
        _ => None,
    };
    let path = AttrPath {
        endpoint: Some(endpoint),
        cluster: Some(cluster),
        attr: if cluster == 513 && attribute == u32::MAX {
            None
        } else {
            Some(attribute)
        },
        ..AttrPath::default()
    };
    let mut chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .subscribe_with(|builder| {
            builder
                .keep_subs(true)?
                .min_int_floor(min_interval)?
                .max_int_ceil(max_interval)?
                .attr_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut priming_values = Vec::new();
    let established = loop {
        let report = chunk.response()?;
        let mut values = crate::operations::report_values(node, &report)?;
        if priming_values.len().saturating_add(values.len()) > MAX_REPORT_VALUES {
            return Err(ErrorCode::NoSpace.into());
        }
        priming_values.append(&mut values);
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => break established,
        }
    };
    Ok(DeviceSubscription {
        clock_events: Vec::new(),
        established,
        unit,
        priming_values,
    })
}

pub async fn establish_switch_events<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
) -> Result<DeviceSubscription, Error> {
    establish_switch_events_for_device(matter, crypto, fabric, node, endpoint, None).await
}

pub async fn establish_switch_events_for_device<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    icd_endpoint: Option<u16>,
) -> Result<DeviceSubscription, Error> {
    let (min_interval, max_interval) = interval_bounds(
        matter,
        crypto,
        fabric,
        node,
        icd_endpoint,
        MAX_REPORT_INTERVAL_SECONDS,
    )
    .await?;
    let path = EventPath {
        endpoint: Some(endpoint),
        cluster: Some(59),
        event: None,
        ..EventPath::default()
    };
    let chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .subscribe_with(|builder| {
            builder
                .keep_subs(true)?
                .min_int_floor(min_interval)?
                .max_int_ceil(max_interval)?
                .event_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut chunk = chunk;
    loop {
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => {
                return Ok(DeviceSubscription {
                    clock_events: Vec::new(),
                    established,
                    unit: None,
                    priming_values: Vec::new(),
                })
            }
        }
    }
}

/// Parse only the two recovery triggers, preserving their node-wide event
/// numbers for replay suppression. Other events remain unrelated to clocks.
fn clock_event(event: &rs_matter::im::EventData<'_>) -> Result<Option<ClockEvent>, Error> {
    let Some(endpoint) = event.path.endpoint else {
        return Ok(None);
    };
    let Some(cluster) = event.path.cluster else {
        return Ok(None);
    };
    if event.path.event != Some(0) || !matches!(cluster, 40 | 56) {
        return Ok(None);
    }
    let data = event.data.structure()?;
    if cluster == 40 {
        data.ctx(0)?.u32()?;
    } // StartUp.softwareVersion
    Ok(Some(ClockEvent {
        endpoint,
        cluster,
        number: event.event_number,
    }))
}

fn merge_clock_events(
    target: &mut Vec<ClockEvent>,
    events: impl IntoIterator<Item = ClockEvent>,
) -> Result<(), Error> {
    for event in events {
        if let Some(old) = target
            .iter_mut()
            .find(|old| old.endpoint == event.endpoint && old.cluster == event.cluster)
        {
            if event.number > old.number {
                *old = event;
            }
        } else {
            if target.len() >= 64 {
                return Err(ErrorCode::NoSpace.into());
            }
            target.push(event);
        }
    }
    Ok(())
}

fn clock_events(report: &rs_matter::im::ReportDataResp<'_>) -> Result<Vec<ClockEvent>, Error> {
    let mut result = Vec::new();
    if let Some(events) = report.event_reports.as_ref() {
        for event in events.iter() {
            if let rs_matter::im::EventResp::Data(data) = event? {
                if let Some(event) = clock_event(&data)? {
                    merge_clock_events(&mut result, [event])?;
                }
            }
        }
    }
    Ok(result)
}

/// Read EventList once during establishment. Explicitly unsupported metadata
/// disables that path; transport, permission, and malformed-data errors retry.
async fn event_list<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
) -> Result<Vec<u32>, Error> {
    let path = AttrPath {
        endpoint: Some(endpoint),
        cluster: Some(cluster),
        attr: Some(65530),
        ..AttrPath::default()
    };
    let mut chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .read_with(|builder| {
            builder
                .attr_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut result = Vec::new();
    let mut found = false;
    loop {
        for (ep, value) in chunk.response()?.attrs::<TLVArray<'_, u32>>(cluster, 65530) {
            if ep != endpoint || found {
                return Err(ErrorCode::InvalidData.into());
            }
            found = true;
            match value {
                Ok(events) => {
                    for event in events.iter() {
                        if result.len() >= 64 {
                            return Err(ErrorCode::NoSpace.into());
                        }
                        result.push(event?);
                    }
                }
                Err(error)
                    if matches!(
                        error.code(),
                        ErrorCode::AttributeNotFound | ErrorCode::ClusterNotFound
                    ) => {}
                Err(error) => return Err(error),
            }
        }
        match chunk.complete().await? {
            Some(next) => chunk = next,
            None => break,
        }
    }
    if !found {
        return Err(ErrorCode::InvalidData.into());
    }
    log::info!(
        "Matter clock EventList: node={:016x} endpoint={} cluster={} events={:?}",
        node,
        endpoint,
        cluster,
        result
    );
    Ok(result)
}

/// Combined urgent StartUp/TimeFailure subscription. Priming chunks retain
/// missed reboot events. A ten-minute keepalive bounds recovery if an accessory
/// loses the subscription itself on reboot; no clock-read timer is installed.
pub async fn establish_clock_events<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    last: Option<u64>,
    icd_endpoint: Option<u16>,
) -> Result<Option<DeviceSubscription>, Error> {
    let boot = event_list(matter, crypto, fabric, node, 0, 40).await?;
    let time = event_list(matter, crypto, fabric, node, endpoint, 56).await?;
    let mut paths = Vec::new();
    if boot.contains(&0) {
        paths.push(EventPath {
            endpoint: Some(0),
            cluster: Some(40),
            event: Some(0),
            is_urgent: Some(true),
            ..EventPath::default()
        });
    }
    if time.contains(&0) {
        paths.push(EventPath {
            endpoint: Some(endpoint),
            cluster: Some(56),
            event: Some(0),
            is_urgent: Some(true),
            ..EventPath::default()
        });
    }
    if paths.is_empty() {
        return Ok(None);
    }
    let filters: Vec<_> = last
        .and_then(|n| n.checked_add(1))
        .map(|n| EventFilter {
            node: None,
            event_min: Some(n),
        })
        .into_iter()
        .collect();
    let (min, max) = interval_bounds(matter, crypto, fabric, node, icd_endpoint, 600).await?;
    let mut chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .subscribe_with(|builder| {
            builder
                .keep_subs(true)?
                .min_int_floor(min)?
                .max_int_ceil(max)?
                .event_requests_from(&paths)?
                .event_filters_from(&filters)?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut events = Vec::new();
    let established = loop {
        let received = clock_events(&chunk.response()?)?;
        merge_clock_events(&mut events, received)?;
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => break established,
        }
    };
    Ok(Some(DeviceSubscription {
        established,
        unit: None,
        priming_values: Vec::new(),
        clock_events: events,
    }))
}

/// Accept and acknowledge one active server-initiated ReportData exchange.
/// Chunked reports are combined under a strict value bound, and the authenticated
/// fabric/node and subscription ID are retained for manager-side correlation.
pub async fn receive_report(
    matter: &Matter<'_>,
    active: ReportSubscriptions,
) -> Result<ReceivedReport, Error> {
    receive_report_exchange(
        Exchange::accept_matching(
            matter,
            rs_matter::im::PROTO_ID_INTERACTION_MODEL,
            rs_matter::im::OpCode::ReportData as u8,
        )
        .await?,
        active,
    )
    .await
}

pub async fn receive_incoming(
    matter: &Matter<'_>,
    active: ReportSubscriptions,
) -> Result<IncomingMessage, Error> {
    let exchange = Exchange::accept_matching(
        matter,
        rs_matter::im::PROTO_ID_INTERACTION_MODEL,
        rs_matter::im::OpCode::ReportData as u8,
    )
    .await?;
    receive_report_exchange(exchange, active)
        .await
        .map(IncomingMessage::Report)
}

async fn receive_report_exchange(
    mut exchange: Exchange<'_>,
    active: ReportSubscriptions,
) -> Result<ReceivedReport, Error> {
    let (fabric, node) = exchange.peer_identity()?.ok_or(ErrorCode::InvalidData)?;
    let mut subscription_id = None;
    let mut values = Vec::new();
    let mut buttons = Vec::new();
    let mut received_clock_events = Vec::new();
    loop {
        let (
            received_id,
            more_chunks,
            suppress_response,
            mut chunk_values,
            mut chunk_buttons,
            chunk_clock_events,
        ) = {
            let rx = exchange.recv_fetch().await?;
            if rx.meta().proto_opcode != rs_matter::im::OpCode::ReportData as u8 {
                return Err(ErrorCode::InvalidOpcode.into());
            }
            let report = rs_matter::im::ReportDataResp::from_tlv(
                &rs_matter::tlv::TLVElement::new(rx.payload()),
            )?;
            let values = crate::operations::report_values(node, &report)?;
            let mut chunk_buttons = Vec::new();
            if let Some(events) = report.event_reports.as_ref() {
                for event in events.iter() {
                    let rs_matter::im::EventResp::Data(event) = event? else {
                        continue;
                    };
                    if event.path.cluster != Some(59)
                        || event.path.event.is_none()
                        || event.path.endpoint.is_none()
                    {
                        continue;
                    }
                    let event_id = event.path.event.unwrap();
                    if event_id > 5 {
                        continue;
                    }
                    let data = event.data.structure()?;
                    let position = data.ctx(0).ok().and_then(|value| value.u8().ok());
                    let press_count = data.ctx(1).ok().and_then(|value| value.u8().ok());
                    chunk_buttons.push(MatterButtonEvent {
                        node: crate::control::Id(node),
                        endpoint: event.path.endpoint.unwrap(),
                        event: event_id as u8,
                        position,
                        press_count,
                    });
                }
            }
            (
                report.subscription_id.ok_or(ErrorCode::InvalidData)?,
                report.more_chunks.unwrap_or(false),
                report.suppress_response.unwrap_or(false),
                values,
                chunk_buttons,
                clock_events(&report)?,
            )
        };
        if subscription_id.is_some_and(|id| id != received_id)
            || values.len().saturating_add(chunk_values.len()) > MAX_REPORT_VALUES
            || buttons.len().saturating_add(chunk_buttons.len()) > MAX_REPORT_VALUES
            || (more_chunks && suppress_response)
        {
            return Err(ErrorCode::InvalidData.into());
        }
        if !active.accepts(fabric, node, received_id) {
            exchange
                .send_with(|_, writer| {
                    rs_matter::im::StatusResp::write(
                        writer,
                        rs_matter::im::IMStatusCode::InvalidSubscription,
                    )?;
                    Ok(Some(rs_matter::im::OpCode::StatusResponse.into()))
                })
                .await?;
            return Err(ErrorCode::InvalidData.into());
        }
        subscription_id = Some(received_id);
        values.append(&mut chunk_values);
        buttons.append(&mut chunk_buttons);
        merge_clock_events(&mut received_clock_events, chunk_clock_events)?;
        if !suppress_response {
            exchange
                .send_with(|_, writer| {
                    rs_matter::im::StatusResp::write(writer, rs_matter::im::IMStatusCode::Success)?;
                    Ok(Some(rs_matter::im::OpCode::StatusResponse.into()))
                })
                .await?;
        }
        if !more_chunks {
            break;
        }
    }
    Ok(ReceivedReport {
        clock_events: received_clock_events,
        fabric,
        node,
        subscription_id: subscription_id.ok_or(ErrorCode::InvalidData)?,
        values,
        buttons,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clock_event_paths_payloads_and_history_are_bounded() {
        use rs_matter::{
            im::{EventData, EventDataTimestamp, EventPriority},
            tlv::TLVElement,
        };
        let startup = [0x15, 0x24, 0, 1, 0x18]; // structure { softwareVersion: 1 }
        let empty = [0x15, 0x18];
        let mut event = EventData::new(
            EventPath {
                endpoint: Some(0),
                cluster: Some(40),
                event: Some(0),
                ..Default::default()
            },
            17,
            EventPriority::Critical,
            EventDataTimestamp::SystemTimestamp(0),
            TLVElement::new(&startup),
        );
        assert_eq!(
            clock_event(&event).unwrap(),
            Some(ClockEvent {
                endpoint: 0,
                cluster: 40,
                number: 17
            })
        );
        event.data = TLVElement::new(&empty);
        assert!(
            clock_event(&event).is_err(),
            "StartUp requires its software version"
        );
        event.path.cluster = Some(56);
        assert!(clock_event(&event).unwrap().is_some());
        event.path.event = Some(1);
        assert!(
            clock_event(&event).unwrap().is_none(),
            "unrelated time events must not reset clocks"
        );
        event.path.cluster = Some(59);
        event.path.event = Some(0);
        assert!(clock_event(&event).unwrap().is_none());
        let mut history = Vec::new();
        for number in 0..1000 {
            merge_clock_events(
                &mut history,
                [ClockEvent {
                    endpoint: 0,
                    cluster: 40,
                    number,
                }],
            )
            .unwrap();
        }
        assert_eq!(
            history,
            vec![ClockEvent {
                endpoint: 0,
                cluster: 40,
                number: 999
            }]
        );
        merge_clock_events(
            &mut history,
            [ClockEvent {
                endpoint: 0,
                cluster: 40,
                number: 2,
            }],
        )
        .unwrap();
        assert_eq!(history[0].number, 999);
    }

    #[test]
    fn retired_ids_and_wrong_peers_are_rejected_by_the_running_receiver() {
        let active = ReportSubscriptions::default();
        let receiver = active.clone();
        active.replace(1, [(2, 77), (3, 77), (2, 78)].into_iter());
        assert!(receiver.accepts(1, 2, 77));
        assert!(receiver.accepts(1, 3, 77));
        assert!(!receiver.accepts(2, 2, 77));
        assert!(!receiver.accepts(1, 4, 77));
        active.replace(1, [(2, 79), (3, 77), (2, 78)].into_iter());
        assert!(!receiver.accepts(1, 2, 77));
        assert!(receiver.accepts(1, 2, 79));
        assert!(receiver.accepts(1, 2, 78));
        active.replace(1, core::iter::empty());
        assert!(!receiver.accepts(1, 2, 79));
    }
}

#[cfg(test)]
mod subscription_policy_tests {
    use super::*;

    #[test]
    fn icd_floor_and_incoming_report_liveness() {
        assert_eq!(minimum_report_interval(true), 0);
        assert_eq!(minimum_report_interval(false), 1);
        assert_eq!(icd_maximum_interval(120), 120);
        assert_eq!(icd_maximum_interval(7200), 7200);
        assert_eq!(icd_maximum_interval(u32::MAX), u16::MAX);
        let timeout = report_timeout_with_mrp(120, 300, 500);
        // Allow both response windows, not just a fixed ten-second grace.
        assert!(timeout > std::time::Duration::from_secs(150));
        assert!(timeout < std::time::Duration::from_secs(170));
        assert!(report_timeout_with_mrp(120, 1000, 5000) > timeout);
        assert_eq!(
            report_timeout_with_mrp(3600, 300, 500) - timeout,
            std::time::Duration::from_secs(3480)
        );
    }
}
