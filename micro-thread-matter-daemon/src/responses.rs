//! Typed, bounded outgoing bodies. No Debug or payload logging implementation.
use crate::ipc::{Frame, MAX_PAYLOAD};
use serde::{Serialize, Serializer};

pub fn serialize_hex<S: Serializer>(bytes: &[u8], serializer: S) -> Result<S::Ok, S::Error> {
    const DIGITS: &[u8] = b"0123456789abcdef";
    let mut value = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        value.push(DIGITS[usize::from(byte >> 4)] as char);
        value.push(DIGITS[usize::from(byte & 15)] as char);
    }
    serializer.serialize_str(&value)
}

pub trait WireResult {
    fn wire_result(&self) -> Option<crate::protobuf::OperationResult>;
}
pub trait WireError {
    fn wire_error(&self) -> &'static str;
}
impl WireResult for () {
    fn wire_result(&self) -> Option<crate::protobuf::OperationResult> {
        None
    }
}
impl<T: WireResult + ?Sized> WireResult for &T {
    fn wire_result(&self) -> Option<crate::protobuf::OperationResult> {
        (**self).wire_result()
    }
}
impl WireError for &'static str {
    fn wire_error(&self) -> &'static str {
        self
    }
}
pub fn reply<T: WireResult, E: WireError>(
    result: Result<T, E>,
) -> Option<crate::protobuf::MessageBody> {
    use crate::protobuf as p;
    let reply = match result {
        Ok(value) => p::OperationResponse {
            schema: Some(1),
            ok: Some(true),
            result: Some(value.wire_result()?),
            error: None,
        },
        Err(error) => p::OperationResponse {
            schema: Some(1),
            ok: Some(false),
            error: Some(error.wire_error().into()),
            result: None,
        },
    };
    let body = p::MessageBody {
        choice: Some(p::MessageBodyChoice::Reply(reply)),
    };
    (prost::Message::encoded_len(&body) <= MAX_PAYLOAD).then_some(body)
}

#[derive(Serialize)]
#[serde(tag = "event", content = "data", rename_all = "snake_case")]
pub enum Event<'a> {
    Commissioning(&'a crate::sessions::Snapshot),
    BleWrite(&'a crate::sessions::Write),
    MatterReport(&'a crate::operations::ResultValue),
    #[cfg(feature = "controller-service")]
    MatterButton(&'a crate::subscriptions::MatterButtonEvent),
    #[cfg(feature = "controller-service")]
    MatterInventory(&'a crate::devices::InventoryRecord),
    #[cfg(feature = "controller-service")]
    MatterInventoryNodes(&'a crate::devices::InventoryNodes),
    #[cfg(feature = "controller-service")]
    Thread(&'a crate::thread_lifecycle::Snapshot),
}
pub fn event(event: Event<'_>) -> Option<Frame> {
    // ATT relay bytes remain opaque and are permitted only in this typed event.
    if let Event::BleWrite(write) = &event {
        if write.data.is_empty()
            || write.data.len() > 244
            || write.session.0 == 0
            || write.generation == 0
            || write.write == 0
        {
            return None;
        }
    }
    if let Event::MatterReport(reading) = &event {
        if !matter_report_valid(reading) {
            return None;
        }
    }
    #[cfg(feature = "controller-service")]
    if let Event::MatterButton(button) = &event {
        if button.node.0 < 2
            || button.endpoint == u16::MAX
            || button.event > 5
            || button.position.is_some_and(|value| value > 254)
            || button.press_count.is_some_and(|value| value > 32)
        {
            return None;
        }
    }
    use crate::protobuf as p;
    let (name, data) = match event {
        Event::Commissioning(v) => ("commissioning", p::EventPayloadChoice::Session(session(v))),
        Event::BleWrite(v) => (
            "ble_write",
            p::EventPayloadChoice::BleWrite(p::BleWriteEvent {
                session: Some(v.session.0),
                generation: Some(v.generation),
                write: Some(v.write),
                data: Some(v.data.clone()),
            }),
        ),
        Event::MatterReport(v) => ("matter_report", p::EventPayloadChoice::Reading(reading(v))),
        #[cfg(feature = "controller-service")]
        Event::MatterButton(v) => (
            "matter_button",
            p::EventPayloadChoice::Button(p::ButtonEvent {
                node: Some(v.node.0),
                endpoint: Some(v.endpoint.into()),
                event: Some(v.event.into()),
                position: v.position.map(u32::from),
                press_count: v.press_count.map(u32::from),
            }),
        ),
        #[cfg(feature = "controller-service")]
        Event::MatterInventory(v) => (
            "matter_inventory",
            p::EventPayloadChoice::Inventory(p::InventoryRecord {
                kind: Some(v.kind.into()),
                device: Some(device(&v.device)),
                endpoint_count: Some(u32::try_from(v.endpoint_count).ok()?),
            }),
        ),
        #[cfg(feature = "controller-service")]
        Event::MatterInventoryNodes(v) => (
            "matter_inventory_nodes",
            p::EventPayloadChoice::InventoryNodes(p::InventoryNodeManifest {
                nodes: Some(p::InventoryNodeList {
                    items: v
                        .nodes
                        .iter()
                        .map(|n| p::InventoryNode {
                            node: Some(n.node.0),
                        })
                        .collect(),
                }),
            }),
        ),
        #[cfg(feature = "controller-service")]
        Event::Thread(v) => ("thread", p::EventPayloadChoice::Thread(thread(v))),
    };
    let payload = p::MessageBody {
        choice: Some(p::MessageBodyChoice::Event(p::EventNotification {
            schema: Some(1),
            event: Some(name.into()),
            data: Some(p::EventPayload { choice: Some(data) }),
        })),
    };
    (prost::Message::encoded_len(&payload) <= MAX_PAYLOAD).then_some(Frame {
        kind: 2,
        opcode: 12,
        id: 0,
        payload,
    })
}

fn matter_report_valid(reading: &crate::operations::ResultValue) -> bool {
    use crate::operations::Value;
    if reading.node.0 < 2
        || reading.endpoint == u16::MAX
        || (reading.cluster != 513
            && reading.attribute != 0
            && !(reading.cluster == 47 && reading.attribute == 12))
    {
        return false;
    }
    match (reading.cluster, &reading.value) {
        (513, _) => crate::thermostat::value_valid(reading.attribute, &reading.value),
        (6, Value::Bool(_)) => true,
        (69, Value::Bool(_)) => true,
        (8, Value::U8(value)) => value.is_none_or(|value| value <= 254),
        (47, Value::U8(value)) => value.is_none_or(|value| value <= 200),
        (91, Value::AirQuality(value)) => {
            let known = matches!(
                value.value,
                "good" | "fair" | "moderate" | "poor" | "very_poor" | "extremely_poor"
            );
            (known && value.quality == "measured")
                || (value.value == "unknown" && value.quality == "unknown")
        }
        (1026 | 1029 | 1037 | 1066, Value::Measurement(value)) => {
            let unit_valid = match reading.cluster {
                1026 => value.unit == "celsius",
                1029 => value.unit == "percent",
                1037 | 1066 => matches!(
                    value.unit,
                    "ppm"
                        | "ppb"
                        | "ppt"
                        | "mg/m3"
                        | "ug/m3"
                        | "ng/m3"
                        | "particles/m3"
                        | "Bq/m3"
                        | "dB/m"
                        | "particles/ft3"
                ),
                _ => false,
            };
            let quality_valid = if value.value.is_some() {
                value.quality == "measured"
            } else {
                value.quality == "unknown"
            };
            let range_valid = value.value.is_none_or(|number| {
                number.is_finite()
                    && match reading.cluster {
                        1026 => (-327.68..=327.67).contains(&number),
                        1029 => (0.0..=100.0).contains(&number),
                        1037 | 1066 => number >= 0.0,
                        _ => false,
                    }
            });
            unit_valid && quality_valid && range_valid
        }
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        control::Id,
        sessions::{Phase, SessionError, Snapshot, Write},
    };

    #[test]
    fn fixed_reply_and_session_event_vectors() {
        let failure = reply::<(), _>(Err(SessionError::StaleGeneration)).unwrap();
        assert_eq!(
            failure,
            br#"{"schema":1,"ok":false,"error":"stale_generation"}"#
        );
        let snapshot = Snapshot {
            session: Id(0xabcdef),
            state: Phase::Cancelled,
            deadline_remaining_seconds: 0,
            error: Some(SessionError::Cancelled),
            node: None,
        };
        assert_eq!(reply::<_, SessionError>(Ok(&snapshot)).unwrap(),
            br#"{"schema":1,"ok":true,"result":{"session":"0000000000abcdef","state":"cancelled","deadline_remaining_seconds":0,"error":"cancelled"}}"#);
        let frame = event(Event::Commissioning(&snapshot)).unwrap();
        assert_eq!((frame.kind, frame.opcode, frame.id), (2, 12, 0));
        assert_eq!(frame.payload,
            br#"{"schema":1,"event":"commissioning","data":{"session":"0000000000abcdef","state":"cancelled","deadline_remaining_seconds":0,"error":"cancelled"}}"#);
    }
    #[test]
    fn opaque_write_hex_limits_and_bounds() {
        let mut write = Write {
            session: Id(1),
            generation: 2,
            write: 3,
            data: vec![0, 15, 255],
        };
        assert_eq!(event(Event::BleWrite(&write)).unwrap().payload,
            br#"{"schema":1,"event":"ble_write","data":{"session":"0000000000000001","generation":2,"write":3,"data":"000fff"}}"#);
        write.data = vec![255; 244];
        assert!(event(Event::BleWrite(&write)).is_some());
        write.data.push(0);
        assert!(event(Event::BleWrite(&write)).is_none());
        write.data.clear();
        assert!(event(Event::BleWrite(&write)).is_none());
        let oversized=crate::protobuf::ThreadStatus{state:Some("x".repeat(MAX_PAYLOAD)),..Default::default()};
        assert!(reply::<_,SessionError>(Ok(oversized)).is_none());
    }

    #[test]
    fn matter_report_events_are_typed_and_strictly_validated() {
        let reading = crate::operations::ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 6,
            attribute: 0,
            value: crate::operations::Value::Bool(true),
        };
        let frame = event(Event::MatterReport(&reading)).unwrap();
        assert_eq!((frame.kind, frame.opcode, frame.id), (2, 12, 0));
        assert_eq!(
            frame.payload,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":6,"attribute":0,"value":{"type":"bool","value":true}}}"#
        );
        let contact = crate::operations::ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 69,
            attribute: 0,
            value: crate::operations::Value::Bool(true),
        };
        let frame = event(Event::MatterReport(&contact)).unwrap();
        assert_eq!(
            frame.payload,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":69,"attribute":0,"value":{"type":"bool","value":true}}}"#
        );
        let invalid = crate::operations::ResultValue {
            node: Id(2),
            endpoint: 1,
            cluster: 8,
            attribute: 0,
            value: crate::operations::Value::U8(Some(255)),
        };
        assert!(event(Event::MatterReport(&invalid)).is_none());
    }

    #[test]
    fn normalized_sensor_reports_match_shared_ipc_golden_vectors() {
        use crate::operations::{AirQualityValue, MeasurementValue, ResultValue, Value};

        let readings = [
            ResultValue {
                node: Id(2),
                endpoint: 1,
                cluster: 1026,
                attribute: 0,
                value: Value::Measurement(MeasurementValue {
                    value: Some(-5.25),
                    unit: "celsius",
                    quality: "measured",
                }),
            },
            ResultValue {
                node: Id(2),
                endpoint: 1,
                cluster: 1029,
                attribute: 0,
                value: Value::Measurement(MeasurementValue {
                    value: None,
                    unit: "percent",
                    quality: "unknown",
                }),
            },
            ResultValue {
                node: Id(2),
                endpoint: 1,
                cluster: 91,
                attribute: 0,
                value: Value::AirQuality(AirQualityValue {
                    value: "moderate",
                    quality: "measured",
                }),
            },
            ResultValue {
                node: Id(2),
                endpoint: 1,
                cluster: 1037,
                attribute: 0,
                value: Value::Measurement(MeasurementValue {
                    value: Some(758.5),
                    unit: "ppm",
                    quality: "measured",
                }),
            },
            ResultValue {
                node: Id(2),
                endpoint: 1,
                cluster: 1066,
                attribute: 0,
                value: Value::Measurement(MeasurementValue {
                    value: Some(12.75),
                    unit: "ug/m3",
                    quality: "measured",
                }),
            },
        ];
        let expected: [&[u8]; 5] = [
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1026,"attribute":0,"value":{"type":"measurement","value":{"value":-5.25,"unit":"celsius","quality":"measured"}}}}"#,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1029,"attribute":0,"value":{"type":"measurement","value":{"value":null,"unit":"percent","quality":"unknown"}}}}"#,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":91,"attribute":0,"value":{"type":"air_quality","value":{"value":"moderate","quality":"measured"}}}}"#,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1037,"attribute":0,"value":{"type":"measurement","value":{"value":758.5,"unit":"ppm","quality":"measured"}}}}"#,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1066,"attribute":0,"value":{"type":"measurement","value":{"value":12.75,"unit":"ug/m3","quality":"measured"}}}}"#,
        ];
        for (reading, expected) in readings.iter().zip(expected) {
            let frame = event(Event::MatterReport(reading)).unwrap();
            assert_eq!((frame.kind, frame.opcode, frame.id), (2, 12, 0));
            assert_eq!(frame.payload, expected);
        }
    }
}

use crate::protobuf as p;
impl WireError for crate::sessions::SessionError {
    fn wire_error(&self) -> &'static str {
        use crate::sessions::SessionError::*;
        match self {
            Malformed => "malformed",
            NotReady => "not_ready",
            Busy => "busy",
            StaleSession => "stale_session",
            StaleGeneration => "stale_generation",
            Timeout => "timeout",
            Cancelled => "cancelled",
            Transport => "transport",
            Storage => "storage",
            Capacity => "capacity",
            Accessory => "accessory",
        }
    }
}
impl WireError for crate::operations::Failure {
    fn wire_error(&self) -> &'static str {
        use crate::operations::Failure::*;
        match self {
            Malformed => "malformed",
            NotReady => "not_ready",
            NotFound => "not_found",
            Unsupported => "unsupported",
            Timeout => "timeout",
            Accessory => "accessory",
            Busy => "busy",
            Capacity => "capacity",
            Storage => "storage",
        }
    }
}
fn result(choice: p::OperationResultChoice) -> Option<p::OperationResult> {
    Some(p::OperationResult {
        choice: Some(choice),
    })
}
fn session(v: &crate::sessions::Snapshot) -> p::CommissioningSession {
    use crate::sessions::Phase::*;
    let state = match v.state {
        WaitingBle => "waiting_ble",
        Pase => "pase",
        Provisioning => "provisioning",
        WaitingThread => "waiting_thread",
        Case => "case",
        Completing => "completing",
        Complete => "complete",
        Cancelled => "cancelled",
        Failed => "failed",
        TimedOut => "timed_out",
    };
    p::CommissioningSession {
        session: Some(v.session.0),
        state: Some(state.into()),
        deadline_remaining_seconds: Some(v.deadline_remaining_seconds.into()),
        error: v.error.map(|e| e.wire_error().into()),
        node: v.node.map(|n| n.0),
    }
}
impl WireResult for crate::sessions::Snapshot {
    fn wire_result(&self) -> Option<p::OperationResult> {
        result(p::OperationResultChoice::Session(session(self)))
    }
}
fn null_uint(n: Option<u64>) -> p::NullableUint {
    p::NullableUint {
        choice: Some(
            n.map(p::NullableUintChoice::Integer)
                .unwrap_or(p::NullableUintChoice::Null(true)),
        ),
    }
}
fn number(n: Option<f64>) -> p::MeasurementValue {
    p::MeasurementValue {
        choice: Some(
            n.map(p::MeasurementValueChoice::Number)
                .unwrap_or(p::MeasurementValueChoice::Null(true)),
        ),
    }
}
pub fn reading(v: &crate::operations::ResultValue) -> p::AttributeReading {
    use crate::operations::Value;
    use p::ReadingValueChoice as R;
    let (kind, value) = match &v.value {
        Value::Bool(v) => ("bool", R::Boolean(*v)),
        Value::U8(v) => (
            "u8",
            v.map(|v| R::Unsigned(v.into())).unwrap_or(R::Null(true)),
        ),
        Value::Thermostat(v) => ("thermostat", v.map(R::Integer).unwrap_or(R::Null(true))),
        Value::ThermostatList(v) => (
            "thermostat_list",
            R::List(p::ThermostatCapabilityList { items: v.clone() }),
        ),
        Value::ThermostatSchedule(v) => (
            "thermostat_schedule",
            R::Measurement(p::Measurement {
                mode: Some(v.mode.into()),
                transitions: Some(p::ThermostatTransitionList {
                    items: v
                        .transitions
                        .iter()
                        .map(|t| p::ThermostatTransition {
                            time: Some(t.time.into()),
                            heat: t.heat,
                            cool: t.cool,
                        })
                        .collect(),
                }),
                ..Default::default()
            }),
        ),
        Value::Measurement(v) => (
            "measurement",
            R::Measurement(p::Measurement {
                value: Some(number(v.value.map(f64::from))),
                unit: Some(v.unit.into()),
                quality: Some(v.quality.into()),
                ..Default::default()
            }),
        ),
        Value::AirQuality(v) => (
            "air_quality",
            R::Measurement(p::Measurement {
                value: Some(p::MeasurementValue {
                    choice: Some(p::MeasurementValueChoice::Text(v.value.into())),
                }),
                quality: Some(v.quality.into()),
                ..Default::default()
            }),
        ),
        Value::RawMeasurement(v) => (
            "raw_measurement",
            v.map(|v| R::Number(v.into())).unwrap_or(R::Null(true)),
        ),
    };
    p::AttributeReading {
        node: Some(v.node.0),
        endpoint: Some(v.endpoint.into()),
        cluster: Some(v.cluster),
        attribute: Some(v.attribute),
        value: Some(p::AttributeValue {
            r#type: Some(kind.into()),
            value: Some(p::ReadingValue {
                choice: Some(value),
            }),
        }),
    }
}
#[cfg(feature = "controller-service")]
pub fn thread(v: &crate::thread_lifecycle::Snapshot) -> p::ThreadStatus {
    use crate::thread_lifecycle::State::*;
    let state = match v.state {
        AwaitingCoordinator => "awaiting_coordinator",
        Detached => "detached",
        Attaching => "attaching",
        Attached => "attached",
        ChannelMismatch => "channel_mismatch",
        Error => "error",
    };
    p::ThreadStatus {
        enabled: Some(v.enabled),
        available: Some(v.available),
        configured: Some(v.configured),
        attached: Some(v.attached),
        state: Some(state.into()),
        linux_ipv6_or_tun: Some(v.linux_ipv6_or_tun),
        zigbee_channel: v.zigbee_channel.map(u32::from),
        dataset_channel: v.dataset_channel.map(u32::from),
    }
}
#[cfg(feature = "controller-service")]
impl WireResult for crate::thread_lifecycle::Snapshot {
    fn wire_result(&self) -> Option<p::OperationResult> {
        result(p::OperationResultChoice::Thread(thread(self)))
    }
}
#[cfg(feature = "controller-service")]
impl WireError for crate::thread_lifecycle::Failure {
    fn wire_error(&self) -> &'static str {
        use crate::thread_lifecycle::Failure::*;
        match self {
            Malformed => "malformed",
            NotReady => "not_ready",
            Busy => "busy",
            Transport => "transport",
            Storage => "storage",
        }
    }
}
#[cfg(feature = "controller-service")]
fn device(v: &crate::devices::Device) -> p::MatterDevice {
    p::MatterDevice {
        node: Some(v.node.0),
        state: Some(v.state.into()),
        online: Some(v.online),
        id: Some(p::NullableText {
            choice: Some(
                v.id.clone()
                    .map(p::NullableTextChoice::Text)
                    .unwrap_or(p::NullableTextChoice::Null(true)),
            ),
        }),
        endpoint: Some(null_uint(v.endpoint.map(u64::from))),
        servers: Some(p::UInt32List {
            items: v.servers.clone(),
        }),
        clients: Some(p::UInt32List {
            items: v.clients.clone(),
        }),
        device_types: Some(p::UInt32List {
            items: v.device_types.clone(),
        }),
        manufacturer: Some(v.manufacturer.clone()),
        model: Some(v.model.clone()),
        rssi_dbm: Some(p::NullableInt {
            choice: Some(
                v.rssi_dbm
                    .map(|n| p::NullableIntChoice::Integer(n.into()))
                    .unwrap_or(p::NullableIntChoice::Null(true)),
            ),
        }),
    }
}
#[cfg(feature = "controller-service")]
impl WireResult for crate::devices::Page {
    fn wire_result(&self) -> Option<p::OperationResult> {
        result(p::OperationResultChoice::Page(p::DeviceSnapshotPage {
            revision: Some(self.revision.0),
            devices: Some(p::MatterDeviceList {
                items: self.devices.iter().map(device).collect(),
            }),
            next_cursor: Some(null_uint(self.next_cursor.map(u64::from))),
        }))
    }
}
#[cfg(feature = "controller-service")]
impl WireError for crate::devices::PageError {
    fn wire_error(&self) -> &'static str {
        use crate::devices::PageError::*;
        match self {
            Malformed => "malformed",
            StaleRevision => "stale_revision",
            Capacity => "capacity",
        }
    }
}
#[cfg(feature = "controller-service")]
impl WireResult for crate::devices::JobAccepted {
    fn wire_result(&self) -> Option<p::OperationResult> {
        result(p::OperationResultChoice::Job(p::DeviceOperationJob {
            job: Some(self.job.0),
            state: Some(self.state.into()),
            ..Default::default()
        }))
    }
}
#[cfg(feature = "controller-service")]
impl WireResult for crate::devices::JobState {
    fn wire_result(&self) -> Option<p::OperationResult> {
        use crate::devices::JobState::*;
        let (job, state, error, value) = match self {
            Queued { job } => (*job, "queued", None, None),
            Running { job } => (*job, "running", None, None),
            Failed { job, error } => (*job, "failed", Some(error.wire_error().into()), None),
            Complete { job, result } => (*job, "complete", None, Some(reading(result))),
        };
        result(p::OperationResultChoice::Job(p::DeviceOperationJob {
            job: Some(job.0),
            state: Some(state.into()),
            error,
            result: value,
        }))
    }
}
#[cfg(feature = "controller-service")]
impl WireResult for p::SubscriptionHealth {
    fn wire_result(&self) -> Option<p::OperationResult> {
        result(p::OperationResultChoice::SubscriptionHealth(self.clone()))
    }
}

impl WireResult for p::ThreadStatus {
    fn wire_result(&self) -> Option<p::OperationResult> {
        result(p::OperationResultChoice::Thread(self.clone()))
    }
}
