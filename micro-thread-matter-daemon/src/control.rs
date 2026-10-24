//! Strict, bounded request bodies for the private controller protocol.
//! Secret-bearing requests intentionally have no Debug/Display implementation.
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
    Subscriptions { schema: Schema, node: Id, restore: bool, cursor: u16 },
    Delete { schema: Schema, node: Id, local: bool },
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
    Thermostat { schema: Schema, node: Id, endpoint: u16, field: String, value: String },
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
    pub fn decode(opcode: u8, bytes: &[u8]) -> Result<Self, BodyError> {
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
                    if time_zone.is_empty() || time_zone.len() > 64 || !time_zone.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'/' | b'_' | b'+' | b'-')) || !(-50_400..=50_400).contains(offset_seconds) {
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
        assert!(Request::decode(op, body.as_bytes()).is_ok());
    }
    fn invalid(op: u8, body: &str) {
        assert!(Request::decode(op, body.as_bytes()).is_err());
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
        valid(10, r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"node":"223456789abcdef0","endpoint":2,"cluster":6}] }"#);
        valid(10, r#"{"schema":1,"action":"bind","node":"123456789abcdef0","endpoint":1,"targets":[{"group":7,"members":[{"node":"223456789abcdef0","endpoint":2}],"cluster":6}] }"#);
        valid(
            10,
            r#"{"schema":1,"action":"read","node":"123456789abcdef0","endpoint":1,"cluster":6,"attribute":0}"#,
        );
        for command in ["on", "off", "toggle"] {
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
        valid(11, r#"{"schema":1,"command":"sync_time","node":"123456789abcdef0","endpoint":0,"time_zone":"Europe/Berlin","offset_seconds":7200}"#);
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
        assert!(Request::decode(9, &[0xff]).is_err());
        assert!(Request::decode(9, &vec![b' '; MAX_PAYLOAD + 1]).is_err());
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
        invalid(3, r#"{"schema":1,"discriminator":1,"pin":20202021,"timeout_seconds":120,"country_code":"de","regulatory_location":"indoor"}"#);
        invalid(3, r#"{"schema":1,"discriminator":1,"pin":20202021,"timeout_seconds":120,"country_code":"DE","regulatory_location":"outside"}"#);
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
