//! Strict, bounded request bodies for the private controller protocol.
//! Secret-bearing requests intentionally have no Debug/Display implementation.
#[cfg(any(test, feature = "controller-probe"))]
use crate::ipc::MAX_PAYLOAD;
use serde::{de::Error, Deserialize, Deserializer};
use std::num::NonZeroU32;

pub struct Schema;
impl<'de> Deserialize<'de> for Schema {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        if u8::deserialize(d)? != 1 {
            return Err(D::Error::custom("unsupported schema"));
        }
        Ok(Self)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Id(pub u64);
impl serde::Serialize for Id {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serializer.serialize_str(&format!("{:016x}", self.0))
    }
}
impl<'de> Deserialize<'de> for Id {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let value = String::deserialize(d)?;
        if value.len() != 16 || !value.bytes().all(lower_hex) {
            return Err(D::Error::custom("invalid identifier"));
        }
        let id =
            u64::from_str_radix(&value, 16).map_err(|_| D::Error::custom("invalid identifier"))?;
        if id == 0 {
            return Err(D::Error::custom("invalid identifier"));
        }
        Ok(Self(id))
    }
}
fn lower_hex(byte: u8) -> bool {
    byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)
}
pub struct HexBytes<const MAX: usize>(Vec<u8>);
impl<const MAX: usize> HexBytes<MAX> {
    pub fn bytes(&self) -> &[u8] {
        &self.0
    }
}
impl<'de, const MAX: usize> Deserialize<'de> for HexBytes<MAX> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        let value = String::deserialize(d)?;
        if value.is_empty()
            || value.len() % 2 != 0
            || value.len() > MAX * 2
            || !value.bytes().all(lower_hex)
        {
            return Err(D::Error::custom("invalid hex body"));
        }
        let bytes = (0..value.len())
            .step_by(2)
            .map(|offset| u8::from_str_radix(&value[offset..offset + 2], 16).unwrap())
            .collect();
        Ok(Self(bytes))
    }
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CommissionStart {
    pub schema: Schema,
    pub discriminator: u16,
    pub pin: u32,
    pub timeout_seconds: u16,
    pub country_code: String,
    pub regulatory_location: RegulatoryLocation,
}
#[derive(Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum RegulatoryLocation {
    Indoor,
    Outdoor,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SessionRequest {
    pub schema: Schema,
    pub session: Id,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CommissionStatusRequest {
    pub schema: Schema,
    pub session: Id,
    #[serde(default)]
    pub subscribe: bool,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BleAttach {
    pub schema: Schema,
    pub session: Id,
    pub address: HexBytes<6>,
    pub mtu: u16,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BleIndication {
    pub schema: Schema,
    pub session: Id,
    pub generation: NonZeroU32,
    pub sequence: NonZeroU32,
    pub data: HexBytes<244>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BleWriteComplete {
    pub schema: Schema,
    pub session: Id,
    pub generation: NonZeroU32,
    pub write: NonZeroU32,
    pub success: bool,
}
#[derive(Deserialize)]
#[serde(tag = "action", rename_all = "snake_case", deny_unknown_fields)]
pub enum ThreadRequest {
    Observe {
        schema: Schema,
        zigbee_channel: u8,
        zigbee_online: bool,
    },
    Status {
        schema: Schema,
        #[serde(default)]
        subscribe: bool,
    },
    Create {
        schema: Schema,
        zigbee_channel: u8,
    },
    Recreate {
        schema: Schema,
        zigbee_channel: u8,
    },
    Import {
        schema: Schema,
        zigbee_channel: u8,
        dataset: HexBytes<254>,
    },
    Retry {
        schema: Schema,
    },
}
#[derive(Deserialize)]
#[serde(tag = "action", rename_all = "snake_case", deny_unknown_fields)]
pub enum DeviceRequest {
    Subscriptions { schema: Schema, node: Id, restore: bool, cursor: u16,
    },
    Delete {
        schema: Schema,
        node: Id,
        local: bool,
    },
    // Synthetic commissioning probe compatibility; never exposed by the service.
    #[cfg(any(test, feature = "controller-probe"))]
    Snapshot {
        schema: Schema,
        #[serde(default)]
        cursor: u16,
        #[serde(default)]
        revision: Option<Id>,
    },
    Read {
        schema: Schema,
        node: Id,
        endpoint: u16,
        cluster: u32,
        attribute: u32,
    },
    Bind {
        schema: Schema,
        node: Id,
        endpoint: u16,
        targets: Vec<BindingTarget>,
    },
    Job {
        schema: Schema,
        job: Id,
    },
}
#[derive(Clone, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BindingTarget {
    pub node: Option<Id>,
    pub endpoint: Option<u16>,
    pub group: Option<u16>,
    #[serde(default)]
    pub members: Vec<BindingMember>,
    pub cluster: u32,
}
#[derive(Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub struct BindingMember {
    pub node: Id,
    pub endpoint: u16,
}
#[derive(Deserialize)]
#[serde(tag = "command", rename_all = "snake_case", deny_unknown_fields)]
pub enum CommandRequest {
    Thermostat { schema: Schema, node: Id, endpoint: u16, field: String, value: String,
    },
    On {
        schema: Schema,
        node: Id,
        endpoint: u16,
    },
    Off {
        schema: Schema,
        node: Id,
        endpoint: u16,
    },
    Identify { schema: Schema, node: Id, endpoint: u16 },
    Toggle {
        schema: Schema,
        node: Id,
        endpoint: u16,
    },
    MoveToLevel {
        schema: Schema,
        node: Id,
        endpoint: u16,
        level: u8,
        transition_deciseconds: u16,
    },
    SyncTime {
        schema: Schema,
        node: Id,
        endpoint: u16,
        time_zone: String,
        offset_seconds: i32,
    },
}
pub enum Request {
    MatterStatus,
    ThreadStatus,
    CommissionStart(CommissionStart),
    CommissionStatus(CommissionStatusRequest),
    CommissionCancel(SessionRequest),
    BleAttach(BleAttach),
    BleIndication(BleIndication),
    BleWriteComplete(BleWriteComplete),
    Thread(ThreadRequest),
    Device(DeviceRequest),
    Command(CommandRequest),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BodyError {
    Malformed,
    Unsupported,
}
#[cfg(any(test, feature = "controller-probe"))]
fn parse<'de, T: Deserialize<'de>>(bytes: &'de [u8]) -> Result<T, BodyError> {
    serde_json::from_slice(bytes).map_err(|_| BodyError::Malformed)
}
pub fn valid_pin(pin: u32) -> bool {
    (1..=99_999_998).contains(&pin)
        && !matches!(
            pin,
            11_111_111
                | 22_222_222
                | 33_333_333
                | 44_444_444
                | 55_555_555
                | 66_666_666
                | 77_777_777
                | 88_888_888
                | 12_345_678
                | 87_654_321
        )
}
impl Request {
    #[cfg(any(test, feature = "controller-probe"))]
    pub fn decode_json(opcode: u8, bytes: &[u8]) -> Result<Self, BodyError> {
        if bytes.len() > MAX_PAYLOAD {
            return Err(BodyError::Malformed);
        }
        let request = match opcode {
            1 if bytes.is_empty() => Self::MatterStatus,
            2 if bytes.is_empty() => Self::ThreadStatus,
            1 | 2 => return Err(BodyError::Malformed),
            3 => {
                let body: CommissionStart = parse(bytes)?;
                if body.discriminator > 4095
                    || !valid_pin(body.pin)
                    || !(30..=900).contains(&body.timeout_seconds)
                    || body.country_code.len() != 2
                    || !body.country_code.bytes().all(|byte| byte.is_ascii_uppercase())
                {
                    return Err(BodyError::Malformed);
                }
                Self::CommissionStart(body)
            }
            4 => Self::CommissionStatus(parse(bytes)?),
            5 => Self::CommissionCancel(parse(bytes)?),
            6 => {
                let body: BleAttach = parse(bytes)?;
                if body.address.bytes().len() != 6 || !(23..=517).contains(&body.mtu) {
                    return Err(BodyError::Malformed);
                }
                Self::BleAttach(body)
            }
            7 => Self::BleIndication(parse(bytes)?),
            8 => Self::BleWriteComplete(parse(bytes)?),
            9 => {
                let body: ThreadRequest = parse(bytes)?;
                let channel = match &body {
                    ThreadRequest::Status { .. } | ThreadRequest::Retry { .. } => None,
                    ThreadRequest::Observe {
                        zigbee_channel,
                        zigbee_online,
                        ..
                    } => {
                        if !zigbee_online && *zigbee_channel == 0 {
                            None
                        } else {
                            Some(zigbee_channel)
                        }
                    }
                    ThreadRequest::Create { zigbee_channel, .. }
                    | ThreadRequest::Recreate { zigbee_channel, .. }
                    | ThreadRequest::Import { zigbee_channel, .. } => Some(zigbee_channel),
                };
                if channel.is_some_and(|c| !(11..=26).contains(c)) {
                    return Err(BodyError::Malformed);
                }
                Self::Thread(body)
            }
            10 => Self::Device(parse(bytes)?),
            11 => {
                let body: CommandRequest = parse(bytes)?;
                if matches!(&body, CommandRequest::MoveToLevel { level: 255, .. }) {
                    return Err(BodyError::Malformed);
                }
                if let CommandRequest::SyncTime { time_zone, offset_seconds, .. } = &body {
                    if time_zone.is_empty() || time_zone.len() > 64 || !time_zone.bytes().all(|c| {
                            c.is_ascii_alphanumeric() || matches!(c, b'/' | b'_' | b'+' | b'-')
                        }) || !(-50_400..=50_400).contains(offset_seconds) {
                        return Err(BodyError::Malformed);
                    }
                }
                Self::Command(body)
            }
            _ => return Err(BodyError::Unsupported),
        };
        Ok(request)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn valid(op: u8, body: &str) {
        assert!(Request::decode_json(op, body.as_bytes()).is_ok());
    }
    fn invalid(op: u8, body: &str) {
        assert!(Request::decode_json(op, body.as_bytes()).is_err());
    }
    #[test]
    fn thread_subscription_is_optional_and_strictly_boolean() {
        valid(
            4,
            r#"{"schema":1,"session":"123456789abcdef0","subscribe":true}"#,
        );
        invalid(
            4,
            r#"{"schema":1,"session":"123456789abcdef0","subscribe":1}"#,
        );
        invalid(
            5,
            r#"{"schema":1,"session":"123456789abcdef0","subscribe":true}"#,
        );
        valid(9, r#"{"schema":1,"action":"status","subscribe":true}"#);
        valid(9, r#"{"schema":1,"action":"status","subscribe":false}"#);
        valid(9, r#"{"schema":1,"action":"retry"}"#);
        invalid(9, r#"{"schema":1,"action":"retry","zigbee_channel":15}"#);
        invalid(9, r#"{"schema":1,"action":"status","subscribe":1}"#);
        invalid(9, r#"{"schema":1,"action":"status","subscribe":"true"}"#);
        invalid(
            9,
            r#"{"schema":1,"action":"status","subscribe":true,"subscribe":false}"#,
        );
    }
    #[test]
    fn all_assigned_request_shapes_decode() {
        valid(1, "");
        valid(2, "");
        valid(
            3,
            r#"{"schema":1,"discriminator":3840,"pin":20202021,"timeout_seconds":120,"country_code":"DE","regulatory_location":"indoor"}"#,
        );
        for op in [4, 5] {
            valid(op, r#"{"schema":1,"session":"123456789abcdef0"}"#);
        }
        valid(
            6,
            r#"{"schema":1,"session":"123456789abcdef0","address":"aabbccddeeff","mtu":23}"#,
        );
        valid(
            7,
            r#"{"schema":1,"session":"123456789abcdef0","generation":1,"sequence":1,"data":"01"}"#,
        );
        valid(
            8,
            r#"{"schema":1,"session":"123456789abcdef0","generation":1,"write":1,"success":true}"#,
        );
        valid(9, r#"{"schema":1,"action":"status"}"#);
        valid(9, r#"{"schema":1,"action":"create","zigbee_channel":26}"#);
        valid(
            9,
            r#"{"schema":1,"action":"import","zigbee_channel":11,"dataset":"01"}"#,
        );
        valid(10, r#"{"schema":1,"action":"snapshot"}"#);
        valid(10, r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"node":"223456789abcdef0","endpoint":2,"cluster":6}] }"#,
        );
        valid(
            10,
            r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"group":7,"members":[{"node":"223456789abcdef0","endpoint":2}],"cluster":6}] }"#,
        );
        valid(
            10,
            r#"{"schema":1,"action":"read","node":"123456789abcdef0","endpoint":1,"cluster":6,"attribute":0}"#,
        );
        for command in ["on", "off", "toggle", "identify"] {
            valid(
                11,
                &format!(
                    r#"{{"schema":1,"command":"{command}","node":"123456789abcdef0","endpoint":1}}"#
                ),
            );
        }
        valid(
            11,
            r#"{"schema":1,"command":"move_to_level","node":"123456789abcdef0","endpoint":1,"level":254,"transition_deciseconds":65535}"#,
        );
        valid(11, r#"{"schema":1,"command":"sync_time","node":"123456789abcdef0","endpoint":0,"time_zone":"Europe/Berlin","offset_seconds":7200}"#,
        );
        invalid(12, "{}");
    }
    #[test]
    fn unknown_duplicate_version_and_bad_utf8_fail_closed() {
        for body in [
            r#"{"schema":1,"action":"status","extra":1}"#,
            r#"{"schema":1,"schema":1,"action":"status"}"#,
            r#"{"schema":2,"action":"status"}"#,
            r#"{"schema":1,"action":"status","zigbee_channel":11}"#,
            r#"{"schema":1,"action":"status"} {}"#,
            r#"{"schema":1,"action":"status","action":"status"}"#,
        ] {
            invalid(9, body);
        }
        assert!(Request::decode_json(9, &[0xff]).is_err());
        assert!(Request::decode_json(9, &vec![b' '; MAX_PAYLOAD + 1]).is_err());
        invalid(1, "{}");
    }
    #[test]
    fn bounds_and_canonical_ids_hex_are_enforced() {
        for id in [
            "0",
            "0000000000000000",
            "ABCDEF1234567890",
            "zzzzzzzzzzzzzzzz",
        ] {
            invalid(4, &format!(r#"{{"schema":1,"session":"{id}"}}"#));
        }
        for data in ["", "0", "FF", "zz", &"ab".repeat(245)] {
            invalid(
                7,
                &format!(
                    r#"{{"schema":1,"session":"123456789abcdef0","generation":1,"sequence":1,"data":"{data}"}}"#
                ),
            );
        }
        for pin in [0, 99_999_999, 11_111_111, 12_345_678, 87_654_321] {
            invalid(
                3,
                &format!(r#"{{"schema":1,"discriminator":1,"pin":{pin},"timeout_seconds":120,"country_code":"DE","regulatory_location":"indoor"}}"#),
            );
        }
        invalid(3, r#"{"schema":1,"discriminator":1,"pin":20202021,"timeout_seconds":120,"country_code":"de","regulatory_location":"indoor"}"#,
        );
        invalid(
            3,
            r#"{"schema":1,"discriminator":1,"pin":20202021,"timeout_seconds":120,"country_code":"DE","regulatory_location":"outside"}"#,
        );
        invalid(9, r#"{"schema":1,"action":"create","zigbee_channel":10}"#);
        invalid(
            7,
            r#"{"schema":1,"session":"123456789abcdef0","generation":0,"sequence":1,"data":"01"}"#,
        );
        invalid(
            11,
            r#"{"schema":1,"command":"on","node":"123456789abcdef0","endpoint":1,"level":1}"#,
        );
        invalid(
            11,
            r#"{"schema":1,"command":"move_to_level","node":"123456789abcdef0","endpoint":1,"level":255,"transition_deciseconds":0}"#,
        );
        invalid(
            6,
            r#"{"schema":1,"session":"123456789abcdef0","address":"01","mtu":22}"#,
        );
    }
}

// The service accepts generated protobuf messages directly. JSON deserialization
// above is retained only for explicit host/probe fixture inputs.
impl Request {
    pub fn decode(opcode: u8, body: &crate::protobuf::MessageBody) -> Result<Self, BodyError> {
        use crate::protobuf::MessageBodyChoice as B;
        fn req<T: Clone>(v: &Option<T>) -> Result<T, BodyError> {
            v.clone().ok_or(BodyError::Malformed)
        }
        fn id(v: u64) -> Result<Id, BodyError> {
            if v == 0 {
                Err(BodyError::Malformed)
            } else {
                Ok(Id(v))
            }
        }
        fn small<T: TryFrom<u32>>(v: u32) -> Result<T, BodyError> {
            T::try_from(v).map_err(|_| BodyError::Malformed)
        }
        fn schema(v: Option<u32>) -> Result<Schema, BodyError> {
            if v == Some(1) {
                Ok(Schema)
            } else {
                Err(BodyError::Malformed)
            }
        }
        fn bytes<const N: usize>(v: Vec<u8>) -> Result<HexBytes<N>, BodyError> {
            if v.is_empty() || v.len() > N {
                Err(BodyError::Malformed)
            } else {
                Ok(HexBytes(v))
            }
        }
        fn nullable(v: &Option<crate::protobuf::NullableUint>) -> Result<Option<u16>, BodyError> {
            use crate::protobuf::NullableUintChoice as N;
            match v.as_ref().and_then(|v| v.choice.as_ref()) {
                Some(N::Null(true)) => Ok(None),
                Some(N::Integer(n)) => {
                    Ok(Some(u16::try_from(*n).map_err(|_| BodyError::Malformed)?))
                }
                None if v.is_none() => Ok(None),
                _ => Err(BodyError::Malformed),
            }
        }
        macro_rules! count {($($v:expr),*)=>{[$($v.is_some()),*].iter().filter(|v|**v).count()}}
        let malformed = BodyError::Malformed;
        Ok(match (opcode, body.choice.as_ref()) {
            (1, Some(B::MatterStatusRequest(_))) => Self::MatterStatus,
            (2, Some(B::ThreadStatusRequest(_))) => Self::ThreadStatus,
            (3, Some(B::CommissioningStartRequest(f))) => {
                let body = CommissionStart {
                    schema: schema(f.schema)?,
                    discriminator: small(req(&f.discriminator)?)?,
                    pin: req(&f.pin)?,
                    timeout_seconds: small(req(&f.timeout_seconds)?)?,
                    country_code: req(&f.country_code)?,
                    regulatory_location: match f.regulatory_location.as_deref() {
                        Some("indoor") => RegulatoryLocation::Indoor,
                        Some("outdoor") => RegulatoryLocation::Outdoor,
                        _ => return Err(malformed),
                    },
                };
                if body.discriminator > 4095
                    || !valid_pin(body.pin)
                    || !(30..=900).contains(&body.timeout_seconds)
                    || body.country_code.len() != 2
                    || !body.country_code.bytes().all(|b| b.is_ascii_uppercase())
                {
                    return Err(malformed);
                }
                Self::CommissionStart(body)
            }
            (4, Some(B::CommissioningStatusRequest(f))) => {
                Self::CommissionStatus(CommissionStatusRequest {
                    schema: schema(f.schema)?,
                    session: id(req(&f.session)?)?,
                    subscribe: f.subscribe.unwrap_or(false),
                })
            }
            (5, Some(B::CommissioningCancelRequest(f))) => Self::CommissionCancel(SessionRequest {
                schema: schema(f.schema)?,
                session: id(req(&f.session)?)?,
            }),
            (6, Some(B::BleAttachRequest(f))) => {
                let body = BleAttach {
                    schema: schema(f.schema)?,
                    session: id(req(&f.session)?)?,
                    address: bytes(req(&f.address)?)?,
                    mtu: small(req(&f.mtu)?)?,
                };
                if body.address.bytes().len() != 6 || !(23..=517).contains(&body.mtu) {
                    return Err(malformed);
                }
                Self::BleAttach(body)
            }
            (7, Some(B::BleIndicationRequest(f))) => Self::BleIndication(BleIndication {
                schema: schema(f.schema)?,
                session: id(req(&f.session)?)?,
                generation: NonZeroU32::new(req(&f.generation)?).ok_or(malformed)?,
                sequence: NonZeroU32::new(req(&f.sequence)?).ok_or(malformed)?,
                data: bytes(req(&f.data)?)?,
            }),
            (8, Some(B::BleWriteCompleteRequest(f))) => Self::BleWriteComplete(BleWriteComplete {
                schema: schema(f.schema)?,
                session: id(req(&f.session)?)?,
                generation: NonZeroU32::new(req(&f.generation)?).ok_or(malformed)?,
                write: NonZeroU32::new(req(&f.write)?).ok_or(malformed)?,
                success: req(&f.success)?,
            }),
            (9, Some(B::ThreadControlRequest(f))) => {
                let fields = count!(
                    f.schema,
                    f.action,
                    f.subscribe,
                    f.zigbee_channel,
                    f.zigbee_online,
                    f.dataset
                );
                let s = schema(f.schema)?;
                let request = match f.action.as_deref() {
                    Some("status") if fields == 2 + usize::from(f.subscribe.is_some()) => {
                        ThreadRequest::Status {
                            schema: s,
                            subscribe: f.subscribe.unwrap_or(false),
                        }
                    }
                    Some("retry") if fields == 2 => ThreadRequest::Retry { schema: s },
                    Some("observe") if fields == 4 => ThreadRequest::Observe {
                        schema: s,
                        zigbee_channel: small(req(&f.zigbee_channel)?)?,
                        zigbee_online: req(&f.zigbee_online)?,
                    },
                    Some("create") if fields == 3 => ThreadRequest::Create {
                        schema: s,
                        zigbee_channel: small(req(&f.zigbee_channel)?)?,
                    },
                    Some("recreate") if fields == 3 => ThreadRequest::Recreate {
                        schema: s,
                        zigbee_channel: small(req(&f.zigbee_channel)?)?,
                    },
                    Some("import") if fields == 4 => ThreadRequest::Import {
                        schema: s,
                        zigbee_channel: small(req(&f.zigbee_channel)?)?,
                        dataset: bytes(req(&f.dataset)?)?,
                    },
                    _ => return Err(malformed),
                };
                let channel = match &request {
                    ThreadRequest::Status { .. } | ThreadRequest::Retry { .. } => None,
                    ThreadRequest::Observe {
                        zigbee_channel: 0,
                        zigbee_online: false,
                        ..
                    } => None,
                    ThreadRequest::Observe { zigbee_channel, .. }
                    | ThreadRequest::Create { zigbee_channel, .. }
                    | ThreadRequest::Recreate { zigbee_channel, .. }
                    | ThreadRequest::Import { zigbee_channel, .. } => Some(*zigbee_channel),
                };
                if channel.is_some_and(|c| !(11..=26).contains(&c)) {
                    return Err(malformed);
                }
                Self::Thread(request)
            }
            (10, Some(B::DeviceControlRequest(f))) => {
                let fields = count!(
                    f.schema,
                    f.action,
                    f.cursor,
                    f.revision,
                    f.node,
                    f.endpoint,
                    f.cluster,
                    f.attribute,
                    f.job,
                    f.targets,
                    f.local,
                    f.restore
                );
                let s = schema(f.schema)?;
                Self::Device(match f.action.as_deref() {
                    Some("subscriptions") if fields == 5 => DeviceRequest::Subscriptions {
                        schema: s,
                        node: id(req(&f.node)?)?,
                        restore: req(&f.restore)?,
                        cursor: small(req(&f.cursor)?)?,
                    },
                    Some("delete") if fields == 4 => DeviceRequest::Delete {
                        schema: s,
                        node: id(req(&f.node)?)?,
                        local: req(&f.local)?,
                    },
                    Some("read") if fields == 6 => DeviceRequest::Read {
                        schema: s,
                        node: id(req(&f.node)?)?,
                        endpoint: small(req(&f.endpoint)?)?,
                        cluster: req(&f.cluster)?,
                        attribute: req(&f.attribute)?,
                    },
                    Some("job") if fields == 3 => DeviceRequest::Job {
                        schema: s,
                        job: id(req(&f.job)?)?,
                    },
                    Some("bind") if fields == 5 => DeviceRequest::Bind {
                        schema: s,
                        node: id(req(&f.node)?)?,
                        endpoint: small(req(&f.endpoint)?)?,
                        targets: req(&f.targets)?
                            .items
                            .iter()
                            .map(|t| {
                                Ok(BindingTarget {
                                    node: t.node.map(id).transpose()?,
                                    endpoint: nullable(&t.endpoint)?,
                                    group: nullable(&t.group)?,
                                    members: t
                                        .members
                                        .as_ref()
                                        .map(|m| {
                                            m.items
                                                .iter()
                                                .map(|m| {
                                                    Ok(BindingMember {
                                                        node: id(req(&m.node)?)?,
                                                        endpoint: small(req(&m.endpoint)?)?,
                                                    })
                                                })
                                                .collect::<Result<_, BodyError>>()
                                        })
                                        .transpose()?
                                        .unwrap_or_default(),
                                    cluster: req(&t.cluster)?,
                                })
                            })
                            .collect::<Result<_, BodyError>>()?,
                    },
                    #[cfg(any(test, feature = "controller-probe"))]
                    Some("snapshot")
                        if fields
                            == 2 + usize::from(f.cursor.is_some())
                                + usize::from(f.revision.is_some()) =>
                    {
                        DeviceRequest::Snapshot {
                            schema: s,
                            cursor: small(f.cursor.unwrap_or(0))?,
                            revision: f.revision.map(id).transpose()?,
                        }
                    }
                    _ => return Err(malformed),
                })
            }
            (11, Some(B::DeviceCommandRequest(f))) => {
                let fields = count!(
                    f.schema,
                    f.node,
                    f.endpoint,
                    f.command,
                    f.level,
                    f.transition_deciseconds,
                    f.field,
                    f.value,
                    f.time_zone,
                    f.offset_seconds
                );
                let s = schema(f.schema)?;
                let node = id(req(&f.node)?)?;
                let endpoint = small(req(&f.endpoint)?)?;
                Self::Command(match f.command.as_deref() {
                    Some("on") if fields == 4 => CommandRequest::On {
                        schema: s,
                        node,
                        endpoint,
                    },
                    Some("off") if fields == 4 => CommandRequest::Off {
                        schema: s,
                        node,
                        endpoint,
                    },
                    Some("identify") if fields == 4 => CommandRequest::Identify { schema: s, node, endpoint },
                    Some("toggle") if fields == 4 => CommandRequest::Toggle {
                        schema: s,
                        node,
                        endpoint,
                    },
                    Some("move_to_level") if fields == 6 => {
                        let level = small(req(&f.level)?)?;
                        if level == 255 {
                            return Err(malformed);
                        }
                        CommandRequest::MoveToLevel {
                            schema: s,
                            node,
                            endpoint,
                            level,
                            transition_deciseconds: small(req(&f.transition_deciseconds)?)?,
                        }
                    }
                    Some("thermostat") if fields == 6 => CommandRequest::Thermostat {
                        schema: s,
                        node,
                        endpoint,
                        field: req(&f.field)?,
                        value: req(&f.value)?,
                    },
                    Some("sync_time") if fields == 6 => {
                        let time_zone = req(&f.time_zone)?;
                        let offset_seconds =
                            i32::try_from(req(&f.offset_seconds)?).map_err(|_| malformed)?;
                        if time_zone.is_empty()
                            || time_zone.len() > 64
                            || !time_zone.bytes().all(|c| {
                                c.is_ascii_alphanumeric() || matches!(c, b'/' | b'_' | b'+' | b'-')
                            })
                            || !(-50400..=50400).contains(&offset_seconds)
                        {
                            return Err(malformed);
                        }
                        CommandRequest::SyncTime {
                            schema: s,
                            node,
                            endpoint,
                            time_zone,
                            offset_seconds,
                        }
                    }
                    _ => return Err(malformed),
                })
            }
            _ => {
                return Err(if (1..=11).contains(&opcode) {
                    malformed
                } else {
                    BodyError::Unsupported
                })
            }
        })
    }
}

#[cfg(test)]
mod protobuf_request_tests {
    use super::*;
    use crate::{ipc::Decoder, protobuf as p};
    #[test]
    fn generated_requests_dispatch_from_all_shared_valid_request_frames() {
        let vectors: serde_json::Value =
            serde_json::from_str(include_str!("../../protocol/matter-wire-vectors.json")).unwrap();
        for v in vectors
            .as_array()
            .unwrap()
            .iter()
            .filter(|v| v["valid"] == true && v["kind"] == 0)
        {
            let hex = v["wire"].as_str().unwrap();
            let bytes: Vec<u8> = (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
                .collect();
            let mut decoder = Decoder::default();
            decoder.feed(&bytes).unwrap();
            let frame = decoder.pop().unwrap();
            assert!(
                Request::decode(frame.opcode, &frame.payload).is_ok(),
                "{}",
                v["name"]
            );
        }
    }
    #[test]
    fn nullable_group_binding_targets_stay_optional_generated_fields(){
        let body=crate::wire_json::from_json(0,10,br#"{"schema":1,"action":"bind","node":"ffffffffffffffff","endpoint":1,"targets":[{"node":null,"endpoint":null,"group":3,"cluster":6,"members":[{"node":"0000000000000002","endpoint":1}]}]}"#).unwrap();
        let Request::Device(DeviceRequest::Bind{node,targets,..})=Request::decode(10,&body).unwrap() else{panic!("wrong typed request")};
        assert_eq!(node.0,u64::MAX);assert!(targets[0].node.is_none());assert_eq!(targets[0].group,Some(3));assert_eq!(targets[0].members[0].node.0,2);
    }
    #[test]
    fn typed_requests_still_enforce_presence_actions_bounds_and_correlation() {
        let mut request = p::CommissioningStatusRequest {
            schema: Some(1),
            session: Some(u64::MAX),
            subscribe: Some(true),
        };
        let body = |v| p::MessageBody {
            choice: Some(p::MessageBodyChoice::CommissioningStatusRequest(v)),
        };
        assert!(
            matches!(Request::decode(4,&body(request.clone())),Ok(Request::CommissionStatus(r)) if r.session.0==u64::MAX&&r.subscribe)
        );
        assert!(Request::decode(5, &body(request.clone())).is_err());
        request.session = Some(0);
        assert!(Request::decode(4, &body(request.clone())).is_err());
        request.session = Some(1);
        request.schema = None;
        assert!(Request::decode(4, &body(request)).is_err());
        let extra = p::MessageBody {
            choice: Some(p::MessageBodyChoice::ThreadControlRequest(
                p::ThreadControlRequest {
                    schema: Some(1),
                    action: Some("status".into()),
                    dataset: Some(vec![1]),
                    ..Default::default()
                },
            )),
        };
        assert!(Request::decode(9, &extra).is_err());
    }
}
