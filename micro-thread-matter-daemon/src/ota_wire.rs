//! Bounded binary messages for the private OTA control socket.
//! All integers are little-endian. Frames start with a u16 payload length.

pub const MAX_FRAME: usize = 1024;

#[derive(Debug, PartialEq, Eq)]
pub struct Request {
    pub action: u8,
    pub endpoint: u16,
    pub node: u64,
    pub path: String,
}

pub fn decode_request(frame: &[u8]) -> Option<Request> {
    if frame.len() < 16 || frame.len() > MAX_FRAME + 2 { return None; }
    let size = u16::from_le_bytes([frame[0], frame[1]]) as usize;
    if size != frame.len() - 2 || frame[2] != 1 || !(1..=4).contains(&frame[3]) { return None; }
    let endpoint = u16::from_le_bytes([frame[4], frame[5]]);
    let node = u64::from_le_bytes(frame[6..14].try_into().ok()?);
    let path_len = u16::from_le_bytes([frame[14], frame[15]]) as usize;
    if path_len != frame.len() - 16 { return None; }
    let path = std::str::from_utf8(&frame[16..]).ok()?.to_owned();
    if path.as_bytes().contains(&0) { return None; }
    Some(Request { action: frame[3], endpoint, node, path })
}

pub fn encode_response(state: &str, error: &str, progress: u64, version: u32, manufacturer: u16, image_type: u16) -> Option<Vec<u8>> {
    if state.len() > u8::MAX as usize || error.len() > u16::MAX as usize || state.contains('\0') || error.contains('\0') { return None; }
    let mut out = vec![0, 0, 1, state.len() as u8];
    out.extend_from_slice(&(error.len() as u16).to_le_bytes());
    out.extend_from_slice(&progress.to_le_bytes());
    out.extend_from_slice(&version.to_le_bytes());
    out.extend_from_slice(&manufacturer.to_le_bytes());
    out.extend_from_slice(&image_type.to_le_bytes());
    out.extend_from_slice(state.as_bytes());
    out.extend_from_slice(error.as_bytes());
    if out.len() - 2 > MAX_FRAME { return None; }
    let size = ((out.len() - 2) as u16).to_le_bytes();
    out[0] = size[0]; out[1] = size[1];
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn request_is_bounded_and_exact() {
        let mut frame = vec![14, 0, 1, 4, 2, 0];
        frame.extend_from_slice(&42u64.to_le_bytes());
        frame.extend_from_slice(&0u16.to_le_bytes());
        assert_eq!(decode_request(&frame).unwrap().node, 42);
        assert!(decode_request(&frame[..15]).is_none());
        frame[3] = 0; assert!(decode_request(&frame).is_none());
        frame[3] = 4; frame[0] = 15; assert!(decode_request(&frame).is_none());
    }
    #[test]
    fn response_has_consistent_length_and_limits() {
        let frame = encode_response("waiting", "", 25, 7, 2, 3).unwrap();
        assert_eq!(u16::from_le_bytes([frame[0], frame[1]]) as usize, frame.len()-2);
        assert!(encode_response("waiting", &"x".repeat(MAX_FRAME), 0, 0, 0, 0).is_none());
    }
}
