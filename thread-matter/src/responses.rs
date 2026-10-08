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

#[derive(Serialize)]
#[serde(untagged)]
enum Reply<T, E> {
    Success { schema: u8, ok: bool, result: T },
    Failure { schema: u8, ok: bool, error: E },
}
pub fn reply<T: Serialize, E: Serialize>(result: Result<T, E>) -> Option<Vec<u8>> {
    let body = match result {
        Ok(result) => Reply::Success {
            schema: 1,
            ok: true,
            result,
        },
        Err(error) => Reply::Failure {
            schema: 1,
            ok: false,
            error,
        },
    };
    let bytes = serde_json::to_vec(&body).ok()?;
    (bytes.len() <= MAX_PAYLOAD).then_some(bytes)
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
    Thread(&'a crate::thread_lifecycle::Snapshot),
}
#[derive(Serialize)]
struct Envelope<'a> {
    schema: u8,
    #[serde(flatten)]
    event: Event<'a>,
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
        if button.node.0 < 2 || button.endpoint == u16::MAX || button.event > 5 ||
            button.position.is_some_and(|value| value > 254) ||
            button.press_count.is_some_and(|value| value > 32) { return None; }
    }
    let payload = serde_json::to_vec(&Envelope { schema: 1, event }).ok()?;
    (payload.len() <= MAX_PAYLOAD).then_some(Frame {
        kind: 2,
        opcode: 12,
        id: 0,
        payload,
    })
}

fn matter_report_valid(reading: &crate::operations::ResultValue) -> bool {
    use crate::operations::Value;
    if reading.node.0 < 2 || reading.endpoint == u16::MAX || reading.attribute != 0 {
        return false;
    }
    match (reading.cluster, &reading.value) {
        (6, Value::Bool(_)) => true,
        (69, Value::Bool(_)) => true,
        (8, Value::U8(value)) => value.is_none_or(|value| value <= 254),
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
        assert!(reply::<_, SessionError>(Ok("x".repeat(MAX_PAYLOAD))).is_none());
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
