//! Bounded version-1 framing for the private native-controller Unix stream.
//! Payloads can contain credentials, so frames and decoders have no Debug/Display.
use std::collections::VecDeque;

pub const MAX_PAYLOAD: usize = 2048;
pub const MAX_CHUNK: usize = 4096;
pub const MAX_QUEUED: usize = 32;
pub const OP_MATTER_STATUS: u8 = 1;
pub const OP_THREAD_STATUS: u8 = 2;
const HEADER: usize = 10;
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
    pub payload: Vec<u8>,
}
impl Frame {
    fn valid(&self) -> bool {
        self.kind <= 2
            && (1..=12).contains(&self.opcode)
            && ((self.kind == 2) == (self.id == 0))
            && self.payload.len() <= MAX_PAYLOAD
    }
    pub fn encode(&self) -> Result<Vec<u8>, ProtocolError> {
        if !self.valid() {
            return Err(ProtocolError::Malformed);
        }
        let mut bytes = Vec::with_capacity(HEADER + self.payload.len());
        bytes.extend((self.payload.len() as u16).to_le_bytes());
        bytes.extend([1, self.kind, self.opcode, 0]);
        bytes.extend(self.id.to_le_bytes());
        bytes.extend(&self.payload);
        Ok(bytes)
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
            let mut frame = Frame {
                kind: self.bytes[3],
                opcode: self.bytes[4],
                id: u32::from_le_bytes(self.bytes[6..10].try_into().unwrap()),
                payload: Vec::new(),
            };
            if self.bytes[2] != 1 || self.bytes[5] != 0 || !frame.valid() {
                return self.fail(ProtocolError::Malformed);
            }
            if length > MAX_PAYLOAD {
                return self.fail(ProtocolError::Oversized);
            }
            if self.bytes.len() < HEADER + length {
                return Ok(());
            }
            if self.frames.len() >= MAX_QUEUED {
                return self.fail(ProtocolError::Overloaded);
            }
            frame.payload.extend(&self.bytes[HEADER..HEADER + length]);
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
    const GOLDEN: &[u8] = &[3, 0, 1, 0, 7, 0, 0x78, 0x56, 0x34, 0x12, 0, 0x80, 0xff];
    #[test]
    fn matches_native_cpp_wire_vector_and_preserves_fragment_boundaries() {
        let frame = Frame {
            kind: 0,
            opcode: 7,
            id: 0x12345678,
            payload: vec![0, 0x80, 0xff],
        };
        assert_eq!(frame.encode().unwrap(), GOLDEN);
        let mut decoder = Decoder::default();
        for byte in GOLDEN {
            decoder.feed(&[*byte]).unwrap();
        }
        let got = decoder.pop().unwrap();
        assert_eq!(got.id, frame.id);
        assert_eq!(got.payload, frame.payload);
        assert!(decoder.pop().is_none());
        let mut joined = GOLDEN.to_vec();
        joined.extend(GOLDEN);
        decoder.feed(&joined).unwrap();
        assert!(decoder.pop().is_some() && decoder.pop().is_some() && decoder.pop().is_none());
    }
    #[test]
    fn malformed_or_oversized_frame_closes_decoder_and_discards_queued_data() {
        for index in [2, 3, 4, 5] {
            let mut bytes = GOLDEN.to_vec();
            bytes[index] = 0xff;
            let mut decoder = Decoder::default();
            decoder.feed(GOLDEN).unwrap();
            assert!(decoder.feed(&bytes).is_err());
            assert!(decoder.pop().is_none());
            assert_eq!(decoder.feed(GOLDEN), Err(ProtocolError::Closed));
        }
        let mut decoder = Decoder::default();
        assert_eq!(
            decoder.feed(&[0; MAX_CHUNK + 1]),
            Err(ProtocolError::Oversized)
        );
    }
    #[test]
    fn maximum_payload_and_queue_overflow_are_bounded() {
        let frame = Frame {
            kind: 1,
            opcode: 1,
            id: 1,
            payload: vec![0xff; MAX_PAYLOAD],
        };
        let mut decoder = Decoder::default();
        decoder.feed(&frame.encode().unwrap()).unwrap();
        assert_eq!(decoder.pop().unwrap().payload.len(), MAX_PAYLOAD);
        let event = Frame {
            kind: 2,
            opcode: 12,
            id: 0,
            payload: vec![],
        }
        .encode()
        .unwrap();
        for _ in 0..MAX_QUEUED {
            decoder.feed(&event).unwrap();
        }
        assert_eq!(decoder.feed(&event), Err(ProtocolError::Overloaded));
        assert!(decoder.pop().is_none());
    }
}
