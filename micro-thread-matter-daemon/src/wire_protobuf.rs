//! Direct typed protobuf framing. No JSON/value-tree adapter is compiled here.
use crate::{
    ipc::{Frame, ProtocolError, MAX_PAYLOAD},
    protobuf as generated,
};
use generated::Bounded;
use prost::Message;
fn varint(b: &[u8], at: &mut usize) -> Result<u64, ProtocolError> {
    let mut n = 0;
    for i in 0..10 {
        let x = *b.get(*at).ok_or(ProtocolError::Malformed)?;
        *at += 1;
        if i == 9 && x > 1 {
            return Err(ProtocolError::Malformed);
        }
        n |= u64::from(x & 127) << (i * 7);
        if x < 128 {
            return Ok(n);
        }
    }
    Err(ProtocolError::Malformed)
}
fn scan(b: &[u8], msg: usize, depth: usize, unknown: &mut usize) -> Result<(), ProtocolError> {
    if depth > 16 {
        return Err(ProtocolError::Malformed);
    }
    let fields = generated::schema(msg);
    let mut seen = 0u64;
    let mut counts = [0u16; generated::MAX_FIELDS];
    let mut choice = false;
    let mut at = 0;
    while at < b.len() {
        let tag = varint(b, &mut at)?;
        let num = tag >> 3;
        let wt = tag & 7;
        if num == 0 || num >= 1 << 29 {
            return Err(ProtocolError::Malformed);
        }
        let mut number = None;
        let data = match wt {
            0 => {
                number = Some(varint(b, &mut at)?);
                &b[0..0]
            }
            1 | 2 | 5 => {
                let n = if wt == 2 {
                    usize::try_from(varint(b, &mut at)?).map_err(|_| ProtocolError::Malformed)?
                } else if wt == 1 {
                    8
                } else {
                    4
                };
                let end = at.checked_add(n).ok_or(ProtocolError::Malformed)?;
                let data = b.get(at..end).ok_or(ProtocolError::Malformed)?;
                at = end;
                data
            }
            _ => return Err(ProtocolError::Malformed),
        };
        let Some((index, f)) = fields
            .iter()
            .enumerate()
            .find(|(_, f)| u64::from(f.tag) == num)
        else {
            *unknown += 1;
            continue;
        };
        let typ = f.kind;
        let expected = if typ == 1 {
            1
        } else if matches!(typ, 9 | 11 | 12) {
            2
        } else {
            0
        };
        if f.repeated && wt == 2 && matches!(typ, 4 | 13 | 18 | 8) {
            let mut p = 0;
            while p < data.len() {
                let n = varint(data, &mut p)?;
                if (typ == 13 && n > u32::MAX.into()) || (typ == 8 && n > 1) {
                    return Err(ProtocolError::Malformed);
                }
                counts[index] += 1;
                if counts[index] > 512 {
                    return Err(ProtocolError::Malformed);
                }
            }
            continue;
        }
        if wt != expected || (!f.repeated && seen & (1u64 << index) != 0) || (f.oneof && choice) {
            return Err(ProtocolError::Malformed);
        }
        seen |= 1u64 << index;
        choice |= f.oneof;
        if f.repeated {
            counts[index] += 1;
            if counts[index] > 512 {
                return Err(ProtocolError::Malformed);
            }
        }
        match typ {
            1 => {
                if !f64::from_le_bytes(data.try_into().unwrap()).is_finite() {
                    return Err(ProtocolError::Malformed);
                }
            }
            8 if number.unwrap() > 1 => return Err(ProtocolError::Malformed),
            13 if number.unwrap() > u32::MAX.into() => return Err(ProtocolError::Malformed),
            9 => {
                std::str::from_utf8(data).map_err(|_| ProtocolError::Malformed)?;
            }
            11 => scan(data, f.message, depth + 1, unknown)?,
            _ => (),
        }
    }
    Ok(())
}

fn body_matches(kind: u8, opcode: u8, payload: &generated::MessageBody) -> bool {
    use generated::MessageBodyChoice as B;
    match (kind, opcode, payload.choice.as_ref()) {
        (0, 1, Some(B::MatterStatusRequest(_)))
        | (0, 2, Some(B::ThreadStatusRequest(_)))
        | (0, 3, Some(B::CommissioningStartRequest(_)))
        | (0, 4, Some(B::CommissioningStatusRequest(_)))
        | (0, 5, Some(B::CommissioningCancelRequest(_)))
        | (0, 6, Some(B::BleAttachRequest(_)))
        | (0, 7, Some(B::BleIndicationRequest(_)))
        | (0, 8, Some(B::BleWriteCompleteRequest(_)))
        | (0, 9, Some(B::ThreadControlRequest(_)))
        | (0, 10, Some(B::DeviceControlRequest(_)))
        | (0, 11, Some(B::DeviceCommandRequest(_)))
        | (1, 1, Some(B::Matter(_)))
        | (1, 2, Some(B::Thread(_)))
        | (1, 3..=11, Some(B::Reply(_)))
        | (2, 12, Some(B::Event(_))) => true,
        _ => false,
    }
}
pub fn encode(frame: &Frame) -> Result<Vec<u8>, ProtocolError> {
    if !body_matches(frame.kind, frame.opcode, &frame.payload) {
        return Err(ProtocolError::Malformed);
    }
    let env = generated::IpcEnvelope {
        version: Some(3),
        kind: Some(frame.kind.into()),
        opcode: Some(frame.opcode.into()),
        id: Some(frame.id),
        body: Some(frame.payload.clone()),
    };
    if !env.bounded(0) {
        return Err(ProtocolError::Malformed);
    }
    let size = env.encoded_len();
    if size == 0 || size > MAX_PAYLOAD {
        return Err(ProtocolError::Oversized);
    }
    let mut bytes = Vec::with_capacity(size + 2);
    bytes.extend_from_slice(&(size as u16).to_le_bytes());
    env.encode(&mut bytes)
        .map_err(|_| ProtocolError::Malformed)?;
    Ok(bytes)
}
pub fn decode(bytes: &[u8]) -> Result<Frame, ProtocolError> {
    if bytes.is_empty() || bytes.len() > MAX_PAYLOAD {
        return Err(ProtocolError::Malformed);
    }
    let mut unknown = 0;
    scan(bytes, generated::IPC_ENVELOPE_SCHEMA, 0, &mut unknown)?;
    let env = generated::IpcEnvelope::decode(bytes).map_err(|_| ProtocolError::Malformed)?;
    if env.version != Some(3) {
        return Err(ProtocolError::Malformed);
    }
    let kind = u8::try_from(env.kind.ok_or(ProtocolError::Malformed)?)
        .map_err(|_| ProtocolError::Malformed)?;
    let opcode = u8::try_from(env.opcode.ok_or(ProtocolError::Malformed)?)
        .map_err(|_| ProtocolError::Malformed)?;
    let id = env.id.ok_or(ProtocolError::Malformed)?;
    if kind > 2
        || !(1..=12).contains(&opcode)
        || (kind == 2) != (id == 0)
        || (kind == 2) != (opcode == 12)
    {
        return Err(ProtocolError::Malformed);
    }
    let payload = env.body.ok_or(ProtocolError::Malformed)?;
    if !body_matches(kind, opcode, &payload) {
        return Err(ProtocolError::Malformed);
    }
    if unknown > 0 {
        log::warn!("Matter IPC opcode {opcode}: ignored {unknown} unknown fields");
    }
    Ok(Frame {
        kind,
        opcode,
        id,
        payload,
    })
}
