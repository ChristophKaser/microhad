//! CBOR boundary adapter. Internal JSON models retain their public/persistence encodings.
//! No payload, field name, or value may enter diagnostics.
use crate::ipc::{Frame, ProtocolError, MAX_PAYLOAD};
use ciborium::value::Value as Cbor;
use serde_json::Value as Json;
use std::collections::HashSet;

fn child(shape: &str, key: &str) -> Option<&'static str> {
    crate::wire_shapes::FIELDS
        .iter()
        .find(|(s, k, _)| *s == shape && *k == key)
        .map(|(_, _, c)| *c)
}
fn text(value: &Cbor) -> &str {
    if let Cbor::Text(s) = value {
        s
    } else {
        ""
    }
}
fn has(value: &Cbor, name: &str) -> bool {
    matches!(value, Cbor::Map(m) if m.iter().any(|(k,_)| text(k)==name))
}
fn field<'a>(value: &'a Cbor, name: &str) -> Option<&'a Cbor> {
    if let Cbor::Map(m) = value {
        m.iter().find(|(k, _)| text(k) == name).map(|(_, v)| v)
    } else {
        None
    }
}
fn shape(context: &str, value: &Cbor, kind: u8, opcode: u8, event: &str) -> String {
    match context {
        "body" if kind == 0 => {
            let name = if opcode == 11 { "command" } else { "action" };
            let action = field(value, name).map(text).unwrap_or("");
            if (9..=11).contains(&opcode) && !action.is_empty() {
                format!("request{opcode}:{action}")
            } else {
                format!("request{opcode}")
            }
        }
        "body" if kind == 2 => "event".into(),
        "body" if opcode == 1 => "matter".into(),
        "body" if opcode == 2 => "thread".into(),
        "body" => "reply".into(),
        "result" => match opcode {
            3..=5 => "session".into(),
            6..=8 => format!("ack{opcode}"),
            9 => "thread".into(),
            10 if has(value, "devices") => "page".into(),
            10 | 11 => "job".into(),
            _ => "".into(),
        },
        "event-data" => match event {
            "thread" => "thread",
            "commissioning" => "session",
            "ble_write" => "ble-write",
            "matter_report" => "reading",
            "matter_button" => "button",
            _ => "",
        }
        .into(),
        _ => context.into(),
    }
}
fn to_cbor(
    value: Json,
    context: &str,
    kind: u8,
    opcode: u8,
    event: &str,
) -> Result<Cbor, ProtocolError> {
    // Resolve dynamic shapes using field names without losing exact integer values.
    let probe = match &value {
        Json::Object(m) => Cbor::Map(
            m.keys()
                .map(|k| {
                    (
                        Cbor::Text(k.clone()),
                        m[k].as_str()
                            .map(|s| Cbor::Text(s.to_owned()))
                            .unwrap_or(Cbor::Null),
                    )
                })
                .collect(),
        ),
        _ => Cbor::Null,
    };
    let ctx = shape(context, &probe, kind, opcode, event);
    if context == "@id" && !value.is_null() {
        let s = value.as_str().ok_or(ProtocolError::Malformed)?;
        if s.len() != 16
            || !s
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        {
            return Err(ProtocolError::Malformed);
        }
        return Ok(Cbor::Integer(
            u64::from_str_radix(s, 16)
                .map_err(|_| ProtocolError::Malformed)?
                .into(),
        ));
    }
    if context == "@bytes" {
        let s = value.as_str().ok_or(ProtocolError::Malformed)?;
        if s.len() % 2 != 0
            || !s
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        {
            return Err(ProtocolError::Malformed);
        }
        let bytes = (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect();
        return Ok(Cbor::Bytes(bytes));
    }
    Ok(match value {
        Json::Null => Cbor::Null,
        Json::Bool(b) => Cbor::Bool(b),
        Json::String(s) => Cbor::Text(s),
        Json::Number(n) => {
            if let Some(u) = n.as_u64() {
                Cbor::Integer(u.into())
            } else if let Some(i) = n.as_i64() {
                Cbor::Integer(i.into())
            } else {
                Cbor::Float(n.as_f64().ok_or(ProtocolError::Malformed)?)
            }
        }
        Json::Array(a) => Cbor::Array(
            a.into_iter()
                .map(|v| to_cbor(v, &ctx, kind, opcode, event))
                .collect::<Result<_, _>>()?,
        ),
        Json::Object(m) => {
            let ev = m
                .get("event")
                .and_then(Json::as_str)
                .unwrap_or(event)
                .to_owned();
            Cbor::Map(
                m.into_iter()
                    .map(|(k, v)| {
                        let child = child(&ctx, &k).unwrap_or("");
                        Ok((Cbor::Text(k), to_cbor(v, child, kind, opcode, &ev)?))
                    })
                    .collect::<Result<_, _>>()?,
            )
        }
    })
}
fn from_cbor(
    value: Cbor,
    context: &str,
    kind: u8,
    opcode: u8,
    event: &str,
    unknown: &mut usize,
) -> Result<Json, ProtocolError> {
    let ctx = shape(context, &value, kind, opcode, event);
    if context == "@id" && !matches!(value, Cbor::Null) {
        let Cbor::Integer(i) = value else {
            return Err(ProtocolError::Malformed);
        };
        let id = u64::try_from(i).map_err(|_| ProtocolError::Malformed)?;
        return Ok(Json::String(format!("{id:016x}")));
    }
    if context == "@bytes" {
        let Cbor::Bytes(b) = value else {
            return Err(ProtocolError::Malformed);
        };
        return Ok(Json::String(b.iter().map(|b| format!("{b:02x}")).collect()));
    }
    Ok(match value {
        Cbor::Null => Json::Null,
        Cbor::Bool(b) => Json::Bool(b),
        Cbor::Text(s) => Json::String(s),
        Cbor::Integer(i) => {
            if let Ok(u) = u64::try_from(i) {
                Json::from(u)
            } else {
                Json::from(i64::try_from(i).map_err(|_| ProtocolError::Malformed)?)
            }
        }
        Cbor::Float(f) => {
            Json::from(serde_json::Number::from_f64(f).ok_or(ProtocolError::Malformed)?)
        }
        Cbor::Array(a) => Json::Array(
            a.into_iter()
                .map(|v| from_cbor(v, &ctx, kind, opcode, event, unknown))
                .collect::<Result<_, _>>()?,
        ),
        Cbor::Map(m) => {
            let ev = m
                .iter()
                .find(|(k, _)| text(k) == "event")
                .map(|(_, v)| text(v))
                .unwrap_or(event)
                .to_owned();
            let mut out = serde_json::Map::new();
            for (k, v) in m {
                let Cbor::Text(k) = k else {
                    return Err(ProtocolError::Malformed);
                };
                let Some(child) = child(&ctx, &k) else {
                    *unknown += 1;
                    continue;
                };
                out.insert(k, from_cbor(v, child, kind, opcode, &ev, unknown)?);
            }
            Json::Object(out)
        }
        _ => return Err(ProtocolError::Malformed),
    })
}
// Validate advertised counts BEFORE the general-purpose deserializer allocates.
fn scan(bytes: &[u8], at: &mut usize, depth: usize) -> Result<(), ProtocolError> {
    let start = *at;
    let initial = *bytes.get(*at).ok_or(ProtocolError::Malformed)?;
    *at += 1;
    let major = initial >> 5;
    let ai = initial & 31;
    if ai >= 28 {
        return Err(ProtocolError::Malformed);
    }
    let n = if ai < 24 {
        u64::from(ai)
    } else {
        let width = 1usize << (ai - 24);
        let end = at.checked_add(width).ok_or(ProtocolError::Malformed)?;
        let part = bytes.get(*at..end).ok_or(ProtocolError::Malformed)?;
        *at = end;
        part.iter().fold(0u64, |n, b| (n << 8) | u64::from(*b))
    };
    match major {
        0 => (),
        1 if n <= i64::MAX as u64 => (),
        2 | 3 => {
            let size = usize::try_from(n).map_err(|_| ProtocolError::Malformed)?;
            let end = at.checked_add(size).ok_or(ProtocolError::Malformed)?;
            let s = bytes.get(*at..end).ok_or(ProtocolError::Malformed)?;
            if major == 3 {
                std::str::from_utf8(s).map_err(|_| ProtocolError::Malformed)?;
            }
            *at = end;
        }
        4 | 5 => {
            if depth >= 16 || n > if major == 4 { 512 } else { 256 } {
                return Err(ProtocolError::Malformed);
            }
            let mut keys = HashSet::new();
            for _ in 0..n {
                if major == 5 {
                    let start = *at;
                    if bytes.get(start).is_none_or(|b| b >> 5 != 3) {
                        return Err(ProtocolError::Malformed);
                    }
                    scan(bytes, at, depth + 1)?;
                    let mut reader = &bytes[start..*at];
                    let key: Cbor =
                        ciborium::from_reader(&mut reader).map_err(|_| ProtocolError::Malformed)?;
                    if !keys.insert(text(&key).to_owned()) {
                        return Err(ProtocolError::Malformed);
                    }
                }
                scan(bytes, at, depth + 1)?;
            }
        }
        7 if matches!(ai,20..=22|25..=27) => {
            if ai >= 25 {
                let value: Cbor = ciborium::from_reader(&bytes[start..*at])
                    .map_err(|_| ProtocolError::Malformed)?;
                if !matches!(value,Cbor::Float(f) if f.is_finite()) {
                    return Err(ProtocolError::Malformed);
                }
            }
        }
        _ => return Err(ProtocolError::Malformed),
    }
    Ok(())
}
pub fn encode(frame: &Frame) -> Result<Vec<u8>, ProtocolError> {
    let body = if frame.payload.is_empty() {
        Json::Object(Default::default())
    } else {
        serde_json::from_slice(&frame.payload).map_err(|_| ProtocolError::Malformed)?
    };
    let body = to_cbor(body, "body", frame.kind, frame.opcode, "")?;
    let message = Cbor::Map(vec![
        (Cbor::Text("version".into()), Cbor::Integer(2.into())),
        (Cbor::Text("kind".into()), Cbor::Integer(frame.kind.into())),
        (
            Cbor::Text("opcode".into()),
            Cbor::Integer(frame.opcode.into()),
        ),
        (Cbor::Text("id".into()), Cbor::Integer(frame.id.into())),
        (Cbor::Text("body".into()), body),
    ]);
    let mut out = Vec::new();
    ciborium::into_writer(&message, &mut out).map_err(|_| ProtocolError::Malformed)?;
    if out.len() > MAX_PAYLOAD {
        return Err(ProtocolError::Oversized);
    }
    let mut at = 0;
    scan(&out, &mut at, 0)?;
    let mut result = (out.len() as u16).to_le_bytes().to_vec();
    result.extend(out);
    Ok(result)
}
pub fn decode(bytes: &[u8]) -> Result<Frame, ProtocolError> {
    let mut at = 0;
    scan(bytes, &mut at, 0)?;
    if at != bytes.len() {
        return Err(ProtocolError::Malformed);
    }
    let message: Cbor = ciborium::from_reader(bytes).map_err(|_| ProtocolError::Malformed)?;
    let integer = |name| {
        field(&message, name)
            .and_then(|v| {
                if let Cbor::Integer(i) = v {
                    u64::try_from(*i).ok()
                } else {
                    None
                }
            })
            .ok_or(ProtocolError::Malformed)
    };
    if integer("version")? != 2 {
        return Err(ProtocolError::Malformed);
    }
    let kind = u8::try_from(integer("kind")?).map_err(|_| ProtocolError::Malformed)?;
    let opcode = u8::try_from(integer("opcode")?).map_err(|_| ProtocolError::Malformed)?;
    let id = u32::try_from(integer("id")?).map_err(|_| ProtocolError::Malformed)?;
    if kind > 2 || !(1..=12).contains(&opcode) || (kind == 2) != (id == 0) {
        return Err(ProtocolError::Malformed);
    }
    let body = field(&message, "body").ok_or(ProtocolError::Malformed)?;
    if !matches!(body, Cbor::Map(_)) {
        return Err(ProtocolError::Malformed);
    }
    let mut unknown = if let Cbor::Map(m) = &message {
        m.iter()
            .filter(|(k, _)| child("envelope", text(k)).is_none())
            .count()
    } else {
        0
    };
    let json = from_cbor(body.clone(), "body", kind, opcode, "", &mut unknown);
    if unknown != 0 {
        warning(opcode, unknown);
    }
    let payload = match json {
        Ok(Json::Object(m)) if kind == 0 && opcode <= 2 && m.is_empty() => vec![],
        Ok(value) => serde_json::to_vec(&value).map_err(|_| ProtocolError::Malformed)?,
        Err(_) => b"!invalid CBOR body".to_vec(),
    };
    Ok(Frame {
        kind,
        opcode,
        id,
        payload,
    })
}
fn warning(opcode: u8, count: usize) {
    #[cfg(feature = "controller-service")]
    {
        let message = std::ffi::CString::new(format!(
            "Matter IPC opcode {opcode}: ignored {count} unknown fields"
        ))
        .unwrap();
        unsafe {
            libc::syslog(libc::LOG_WARNING, b"%s\0".as_ptr().cast(), message.as_ptr());
        }
    }
    #[cfg(not(feature = "controller-service"))]
    log::debug!("Matter IPC opcode {opcode}: ignored {count} unknown fields");
}
