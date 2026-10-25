//! Bounded protobuf version-3 framing for the private native-controller Unix stream.
//! Payloads can contain credentials, so frames and decoders have no Debug/Display.
use std::collections::VecDeque;

pub const MAX_PAYLOAD: usize = 2048;
pub const MAX_CHUNK: usize = 4096;
pub const MAX_QUEUED: usize = 32;
pub const OP_MATTER_STATUS: u8 = 1;
pub const OP_THREAD_STATUS: u8 = 2;
const HEADER: usize = 2;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProtocolError {
    Malformed,
    Oversized,
    Overloaded,
    Closed,
}
pub struct Frame {
    pub kind: u8,
    pub opcode: u8,
    pub id: u32,
    pub payload: crate::protobuf::MessageBody,
}
impl Frame {
    fn valid(&self) -> bool {
        self.kind <= 2 && (1..=12).contains(&self.opcode) && ((self.kind == 2) == (self.id == 0)) && ((self.kind == 2) == (self.opcode == 12))
    }
    pub fn encode(&self) -> Result<Vec<u8>, ProtocolError> {
        if !self.valid() {
            return Err(ProtocolError::Malformed);
        }
        crate::wire_protobuf::encode(self)
    }
}
#[derive(Default)]
pub struct Decoder {
    failed: bool,
    bytes: Vec<u8>,
    frames: VecDeque<Frame>,
}
impl Decoder {
    fn fail<T>(&mut self, error: ProtocolError) -> Result<T, ProtocolError> {
        self.failed = true;
        self.bytes.clear();
        self.frames.clear();
        Err(error)
    }
    pub fn feed(&mut self, bytes: &[u8]) -> Result<(), ProtocolError> {
        if self.failed {
            return Err(ProtocolError::Closed);
        }
        if bytes.len() > MAX_CHUNK
            || self.bytes.len() + bytes.len() > MAX_PAYLOAD + HEADER + MAX_CHUNK
        {
            return self.fail(ProtocolError::Oversized);
        }
        self.bytes.extend(bytes);
        while self.bytes.len() >= HEADER {
            let length = u16::from_le_bytes(self.bytes[..2].try_into().unwrap()) as usize;
            if length > MAX_PAYLOAD {
                return self.fail(ProtocolError::Oversized);
            }
            if self.bytes.len() < HEADER + length {
                return Ok(());
            }
            if self.frames.len() >= MAX_QUEUED {
                return self.fail(ProtocolError::Overloaded);
            }
            let frame = match crate::wire_protobuf::decode(&self.bytes[HEADER..HEADER + length]) {
                Ok(frame) => frame,
                Err(error) => return self.fail(error),
            };
            self.frames.push_back(frame);
            self.bytes.drain(..HEADER + length);
        }
        Ok(())
    }
    pub fn pop(&mut self) -> Option<Frame> {
        self.frames.pop_front()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shared_cpp_python_vectors_and_fragmentation() {
        let vectors: serde_json::Value =
            serde_json::from_str(include_str!("../../protocol/matter-wire-vectors.json")).unwrap();
        for v in vectors.as_array().unwrap() {
            let hex = v["wire"].as_str().unwrap();
            let wire: Vec<u8> = (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap())
                .collect();
            let mut decoder = Decoder::default();
            let accepted = wire.iter().try_for_each(|b| decoder.feed(&[*b])).is_ok();
            assert_eq!(accepted, v["valid"].as_bool().unwrap(), "{}", v["name"]);
            if accepted {
                let frame = decoder.pop().unwrap();
                assert_eq!(frame.id, u32::try_from(v["id"].as_u64().unwrap()).unwrap());
                let body = if frame.payload.is_empty() {
                    serde_json::json!({})
                } else {
                    serde_json::from_slice(&crate::wire_json::json_bytes(&frame.payload)).unwrap()
                };
                assert_eq!(body, v["body"], "{}", v["name"]);
                let encoded = frame.encode().unwrap_or_else(|e| panic!("{}: {e:?}", v["name"]));
                let mut roundtrip = Decoder::default();
                roundtrip.feed(&encoded).unwrap();
                assert_eq!(roundtrip.pop().unwrap().payload, frame.payload);
            } else {
                assert!(decoder.failed);
                assert!(decoder.pop().is_none());
                assert_eq!(decoder.feed(&wire), Err(ProtocolError::Closed));
            }
        }
    }
    #[test]
    fn queue_and_chunk_limits() {
        let frame = Frame {
            kind: 0,
            opcode: 1,
            id: 1,
            payload: crate::wire_json::from_json(0, 1, b"").unwrap(),
        }
        .encode()
        .unwrap();
        let mut decoder = Decoder::default();
        for _ in 0..MAX_QUEUED {
            decoder.feed(&frame).unwrap();
        }
        assert_eq!(decoder.feed(&frame), Err(ProtocolError::Overloaded));
        assert!(decoder.pop().is_none());
        let mut decoder = Decoder::default();
        assert_eq!(
            decoder.feed(&vec![0; MAX_CHUNK + 1]),
            Err(ProtocolError::Oversized)
        );
        let mut decoder = Decoder::default();
        assert_eq!(decoder.feed(&[1, 8]), Err(ProtocolError::Oversized));
    }
}

impl crate::protobuf::MessageBody {
    pub fn len(&self) -> usize {
        prost::Message::encoded_len(self)
    }
    pub fn is_empty(&self) -> bool {
        self.choice.is_none()
    }
    pub fn clear(&mut self) {
        self.choice = None;
    }
}

#[cfg(test)]
mod typed_encoder_tests {
    use super::*;
    use crate::protobuf::{EventPayloadChoice,MeasurementValueChoice,MessageBodyChoice,ReadingValueChoice};
    fn fixture(name:&str)->Frame {
        let vectors:serde_json::Value=serde_json::from_str(include_str!("../../protocol/matter-wire-vectors.json")).unwrap();
        let v=vectors.as_array().unwrap().iter().find(|v|v["name"]==name).unwrap();
        let hex=v["wire"].as_str().unwrap();let bytes:Vec<u8>=(0..hex.len()).step_by(2).map(|i|u8::from_str_radix(&hex[i..i+2],16).unwrap()).collect();
        let mut decoder=Decoder::default();decoder.feed(&bytes).unwrap();decoder.pop().unwrap()
    }
    fn reading(frame:&mut Frame)->&mut crate::protobuf::ReadingValue {
        let Some(MessageBodyChoice::Event(e))=&mut frame.payload.choice else{panic!("event")};
        let Some(EventPayloadChoice::Reading(r))=&mut e.data.as_mut().unwrap().choice else{panic!("reading")};
        r.value.as_mut().unwrap().value.as_mut().unwrap()
    }
    #[test]
    fn generated_outbound_checks_reject_nonfinite_and_oversized_lists(){
        let mut frame=fixture("measurement-event");
        let Some(ReadingValueChoice::Measurement(m))=&mut reading(&mut frame).choice else{panic!("measurement")};
        m.value.as_mut().unwrap().choice=Some(MeasurementValueChoice::Number(f64::NAN));
        assert_eq!(frame.encode().unwrap_err(),ProtocolError::Malformed);
        let mut frame=fixture("repeated-512");
        let Some(ReadingValueChoice::List(list))=&mut reading(&mut frame).choice else{panic!("list")};
        list.items.push(0);assert_eq!(frame.encode().unwrap_err(),ProtocolError::Malformed);
    }
    #[test]
    fn generated_body_must_match_the_header_before_encoding(){
        let mut frame=fixture("empty-status");frame.opcode=2;
        assert_eq!(frame.encode().unwrap_err(),ProtocolError::Malformed);
    }
}
