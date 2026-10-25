//! JSON fixture adapter; excluded from the production controller service.
//! No payload, field name, or value may enter diagnostics.
use crate::ipc::ProtocolError;
#[derive(Clone)]
enum Value {
    Null,
    Bool(bool),
    Integer(i128),
    Float(f64),
    Text(String),
    Bytes(Vec<u8>),
    Array(Vec<Value>),
    Map(Vec<(Value, Value)>),
}
#[allow(non_snake_case, dead_code, unused_variables, unused_mut)]
mod generated {
    include!("../../protocol/generated/protobuf.rs");
}

use serde_json::Value as Json;

fn child(shape: &str, key: &str) -> Option<&'static str> {
    let fields = crate::wire_shapes::FIELDS;
    fields
        .binary_search_by(|(s, k, _)| (*s, *k).cmp(&(shape, key)))
        .ok()
        .map(|i| fields[i].2)
}
fn text(value: &Value) -> &str {
    if let Value::Text(s) = value {
        s
    } else {
        ""
    }
}
fn has(value: &Value, name: &str) -> bool {
    matches!(value, Value::Map(m) if m.iter().any(|(k,_)| text(k)==name))
}
fn field<'a>(value: &'a Value, name: &str) -> Option<&'a Value> {
    if let Value::Map(m) = value {
        m.iter().find(|(k, _)| text(k) == name).map(|(_, v)| v)
    } else {
        None
    }
}
fn shape(context: &str, value: &Value, kind: u8, opcode: u8, event: &str) -> String {
    match context {
        "body" if kind == 0 => {
            let name = if opcode == 11 { "command" } else { "action" };
            let action = field(value, name).map(text).unwrap_or("");
            if (9..=11).contains(&opcode) && !action.is_empty() {
                format!("{}:{action}", generated::request_name(opcode))
            } else {
                generated::request_name(opcode).to_owned()
            }
        }
        "body" if kind == 2 => "event".into(),
        "body" if opcode == 1 => "matter".into(),
        "body" if opcode == 2 => "thread".into(),
        "body" => "reply".into(),
        "result" => match opcode {
            3..=5 => "session".into(),
            6..=8 => generated::ack_name(opcode).to_owned(),
            9 => "thread".into(),
            10 if has(value, "subscriptions") => "subscription-health".into(),
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
            "matter_inventory" => "inventory",
            "matter_inventory_nodes" => "inventory-nodes",
            _ => "",
        }
        .into(),
        _ => context.into(),
    }
}
fn to_wire(
    value: Json,
    context: &str,
    kind: u8,
    opcode: u8,
    event: &str,
) -> Result<Value, ProtocolError> {
    // Read only the discriminator fields; do not build a second tree to find the shape.
    let ctx = match context {
        "body" if kind == 0 => {
            let action = value
                .get(if opcode == 11 { "command" } else { "action" })
                .and_then(Json::as_str)
                .unwrap_or("");
            if (9..=11).contains(&opcode) && !action.is_empty() {
                format!("{}:{action}", generated::request_name(opcode))
            } else {
                generated::request_name(opcode).to_owned()
            }
        }
        "result" if opcode == 10 => if value.get("subscriptions").is_some() {
            "subscription-health"
        } else if value.get("devices").is_some() {
            "page"
        } else {
            "job"
        }
        .into(),
        _ => shape(context, &Value::Null, kind, opcode, event),
    };
    if context == "@id" && !value.is_null() {
        let s = value.as_str().ok_or(ProtocolError::Malformed)?;
        if s.len() != 16
            || !s
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        {
            return Err(ProtocolError::Malformed);
        }
        return Ok(Value::Integer(
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
        return Ok(Value::Bytes(bytes));
    }
    Ok(match value {
        Json::Null => Value::Null,
        Json::Bool(b) => Value::Bool(b),
        Json::String(s) => Value::Text(s),
        Json::Number(n) => {
            if let Some(u) = n.as_u64() {
                Value::Integer(u.into())
            } else if let Some(i) = n.as_i64() {
                Value::Integer(i.into())
            } else {
                Value::Float(n.as_f64().ok_or(ProtocolError::Malformed)?)
            }
        }
        Json::Array(a) => Value::Array(
            a.into_iter()
                .map(|v| to_wire(v, &ctx, kind, opcode, event))
                .collect::<Result<_, _>>()?,
        ),
        Json::Object(m) => {
            let ev = m
                .get("event")
                .and_then(Json::as_str)
                .unwrap_or(event)
                .to_owned();
            Value::Map(
                m.into_iter()
                    .map(|(k, v)| {
                        let child = child(&ctx, &k).unwrap_or("");
                        Ok((Value::Text(k), to_wire(v, child, kind, opcode, &ev)?))
                    })
                    .collect::<Result<_, _>>()?,
            )
        }
    })
}
fn from_wire(
    value: Value,
    context: &str,
    kind: u8,
    opcode: u8,
    event: &str,
    unknown: &mut usize,
) -> Result<Json, ProtocolError> {
    let ctx = shape(context, &value, kind, opcode, event);
    if context == "@id" && !matches!(value, Value::Null) {
        let Value::Integer(i) = value else {
            return Err(ProtocolError::Malformed);
        };
        let id = u64::try_from(i).map_err(|_| ProtocolError::Malformed)?;
        return Ok(Json::String(format!("{id:016x}")));
    }
    if context == "@bytes" {
        let Value::Bytes(b) = value else {
            return Err(ProtocolError::Malformed);
        };
        let digits = b"0123456789abcdef";
        let mut out = String::with_capacity(b.len() * 2);
        for byte in b {
            out.push(digits[usize::from(byte >> 4)] as char);
            out.push(digits[usize::from(byte & 15)] as char);
        }
        return Ok(Json::String(out));
    }
    Ok(match value {
        Value::Null => Json::Null,
        Value::Bool(b) => Json::Bool(b),
        Value::Text(s) => Json::String(s),
        Value::Bytes(_) => return Err(ProtocolError::Malformed),
        Value::Integer(i) => {
            if let Ok(u) = u64::try_from(i) {
                Json::from(u)
            } else {
                Json::from(i64::try_from(i).map_err(|_| ProtocolError::Malformed)?)
            }
        }
        Value::Float(f) => {
            Json::from(serde_json::Number::from_f64(f).ok_or(ProtocolError::Malformed)?)
        }
        Value::Array(a) => Json::Array(
            a.into_iter()
                .map(|v| from_wire(v, &ctx, kind, opcode, event, unknown))
                .collect::<Result<_, _>>()?,
        ),
        Value::Map(m) => {
            let ev = m
                .iter()
                .find(|(k, _)| text(k) == "event")
                .map(|(_, v)| text(v))
                .unwrap_or(event)
                .to_owned();
            let mut out = serde_json::Map::new();
            for (k, v) in m {
                let Value::Text(k) = k else {
                    return Err(ProtocolError::Malformed);
                };
                let Some(child) = child(&ctx, &k) else {
                    *unknown += 1;
                    continue;
                };
                out.insert(k, from_wire(v, child, kind, opcode, &ev, unknown)?);
            }
            Json::Object(out)
        }
    })
}
fn schema(name: &str) -> &'static [generated::Field] {
    generated::schema(name)
}
fn normalize(
    v: Value,
    msg: &str,
    kind: u8,
    op: u8,
    ev: &str,
    incoming: bool,
) -> Result<Value, ProtocolError> {
    let fields = schema(msg);
    if fields.len() == 1 && fields[0].repeated && fields[0].name == "items" {
        let values = if incoming {
            match v {
                Value::Map(mut m) => match m
                    .iter()
                    .position(|(k, _)| text(k) == "items")
                    .map(|i| m.remove(i).1)
                {
                    Some(Value::Array(a)) => a,
                    None => vec![],
                    _ => return Err(ProtocolError::Malformed),
                },
                _ => return Err(ProtocolError::Malformed),
            }
        } else {
            match v {
                Value::Array(a) => a,
                _ => return Err(ProtocolError::Malformed),
            }
        };
        let f = &fields[0];
        let values = values
            .into_iter()
            .map(|x| {
                if f.kind == 11 {
                    normalize(x, f.message, kind, op, ev, incoming)
                } else {
                    Ok(x)
                }
            })
            .collect::<Result<Vec<_>, _>>()?;
        return Ok(if incoming {
            Value::Array(values)
        } else {
            Value::Map(vec![(Value::Text("items".into()), Value::Array(values))])
        });
    }
    if fields.first().is_some_and(|f| f.oneof) {
        if incoming {
            let Value::Map(mut m) = v else {
                return Err(ProtocolError::Malformed);
            };
            if m.len() != 1 {
                return Err(ProtocolError::Malformed);
            }
            let (k, x) = m.remove(0);
            let key = text(&k);
            let f = fields
                .iter()
                .find(|f| f.name == key)
                .ok_or(ProtocolError::Malformed)?;
            return if key == "null" {
                if matches!(x, Value::Bool(true)) {
                    Ok(Value::Null)
                } else {
                    Err(ProtocolError::Malformed)
                }
            } else if f.kind == 11 {
                normalize(x, f.message, kind, op, ev, true)
            } else {
                Ok(x)
            };
        }
        let key = match msg {
            "MessageBody" => {
                if kind == 0 {
                    generated::request_name(op).to_owned()
                } else if kind == 2 {
                    "event".into()
                } else if op == 1 {
                    "matter".into()
                } else if op == 2 {
                    "thread".into()
                } else {
                    "reply".into()
                }
            }
            "OperationResult" => {
                if op <= 5 {
                    "session".into()
                } else if op <= 8 {
                    generated::ack_name(op).to_owned()
                } else if op == 9 {
                    "thread".into()
                } else if has(&v, "subscriptions") {
                    "subscription_health".into()
                } else if has(&v, "devices") {
                    "page".into()
                } else {
                    "job".into()
                }
            }
            "EventPayload" => match ev {
                "thread" => "thread",
                "commissioning" => "session",
                "ble_write" => "ble_write",
                "matter_report" => "reading",
                "matter_button" => "button",
                "matter_inventory" => "inventory",
                "matter_inventory_nodes" => "inventory_nodes",
                _ => return Err(ProtocolError::Malformed),
            }
            .into(),
            _ => match &v {
                Value::Null => "null",
                _ if msg == "NullableText" => "text",
                _ if msg == "NullableUint" || msg == "NullableInt" => "integer",
                Value::Bool(_) => "boolean",
                Value::Float(_) => "number",
                Value::Integer(i) => {
                    if *i >= 0 && msg == "ReadingValue" {
                        "unsigned"
                    } else {
                        "integer"
                    }
                }
                Value::Text(_) => "text",
                Value::Array(_) => "list",
                _ => "measurement",
            }
            .into(),
        };
        let f = fields
            .iter()
            .find(|f| f.name == key)
            .ok_or(ProtocolError::Malformed)?;
        let x = if key == "null" {
            Value::Bool(true)
        } else if f.kind == 11 {
            normalize(v, f.message, kind, op, ev, false)?
        } else {
            v
        };
        return Ok(Value::Map(vec![(Value::Text(key), x)]));
    }
    let event = field(&v, "event")
        .map(text)
        .filter(|s| !s.is_empty())
        .unwrap_or(ev)
        .to_owned();
    let Value::Map(m) = v else {
        return Err(ProtocolError::Malformed);
    };
    let mut out = Vec::new();
    for (k, x) in m {
        if msg=="BindingTarget"&&text(&k)=="node"&&matches!(x,Value::Null){continue;}
        let Some(f) = fields.iter().find(|f| f.name == text(&k)) else {
            continue;
        };
        let x = if f.kind == 11 {
            normalize(x, f.message, kind, op, &event, incoming)?
        } else {
            x
        };
        out.push((k, x));
    }
    Ok(Value::Map(out))
}

use prost::Message;
pub fn from_json(
    kind: u8,
    opcode: u8,
    bytes: &[u8],
) -> Result<crate::protobuf::MessageBody, ProtocolError> {
    let json = if bytes.is_empty() {
        serde_json::json!({})
    } else {
        serde_json::from_slice(bytes).map_err(|_| ProtocolError::Malformed)?
    };
    let body = normalize(
        to_wire(json, "body", kind, opcode, "")?,
        "MessageBody",
        kind,
        opcode,
        "",
        false,
    )?;
    let env = Value::Map(vec![
        (Value::Text("version".into()), Value::Integer(3)),
        (Value::Text("kind".into()), Value::Integer(kind.into())),
        (Value::Text("opcode".into()), Value::Integer(opcode.into())),
        (
            Value::Text("id".into()),
            Value::Integer(if kind == 2 { 0 } else { 1 }),
        ),
        (Value::Text("body".into()), body),
    ]);
    let bytes = generated::encode(&env)?;
    Ok(crate::protobuf::IpcEnvelope::decode(bytes.as_slice())
        .map_err(|_| ProtocolError::Malformed)?
        .body
        .unwrap())
}
pub fn json_bytes(body: &crate::protobuf::MessageBody) -> Vec<u8> {
    use crate::protobuf::{MessageBodyChoice as B, OperationResultChoice as R};
    let (kind, op) = match body.choice.as_ref() {
        Some(B::MatterStatusRequest(_)) => (0, 1),
        Some(B::ThreadStatusRequest(_)) => (0, 2),
        Some(B::CommissioningStartRequest(_)) => (0, 3),
        Some(B::CommissioningStatusRequest(_)) => (0, 4),
        Some(B::CommissioningCancelRequest(_)) => (0, 5),
        Some(B::BleAttachRequest(_)) => (0, 6),
        Some(B::BleIndicationRequest(_)) => (0, 7),
        Some(B::BleWriteCompleteRequest(_)) => (0, 8),
        Some(B::ThreadControlRequest(_)) => (0, 9),
        Some(B::DeviceControlRequest(_)) => (0, 10),
        Some(B::DeviceCommandRequest(_)) => (0, 11),
        Some(B::Matter(_)) => (1, 1),
        Some(B::Thread(_)) => (1, 2),
        Some(B::Event(_)) => (2, 12),
        Some(B::Reply(reply)) => (
            1,
            match reply.result.as_ref().and_then(|r| r.choice.as_ref()) {
                Some(R::BleAttachResult(_)) => 6,
                Some(R::BleIndicationResult(_)) => 7,
                Some(R::BleWriteCompleteResult(_)) => 8,
                Some(R::Thread(_)) => 9,
                Some(R::Session(_)) | None => 3,
                _ => 10,
            },
        ),
        None => return vec![],
    };
    let env = crate::protobuf::IpcEnvelope {
        version: Some(3),
        kind: Some(kind),
        opcode: Some(op),
        id: Some(if kind == 2 { 0 } else { 1 }),
        body: Some(body.clone()),
    };
    let env = generated::decode(&env.encode_to_vec()).unwrap();
    let body = normalize(
        field(&env, "body").unwrap().clone(),
        "MessageBody",
        kind as u8,
        op as u8,
        "",
        true,
    )
    .unwrap();
    serde_json::to_vec(&from_wire(body, "body", kind as u8, op as u8, "", &mut 0).unwrap()).unwrap()
}
#[cfg(test)]
impl<const N: usize> PartialEq<&[u8; N]> for crate::protobuf::MessageBody {
    fn eq(&self, other: &&[u8; N]) -> bool {
        serde_json::from_slice::<Json>(&json_bytes(self)).ok()
            == serde_json::from_slice::<Json>(*other).ok()
    }
}

#[cfg(test)]
impl PartialEq<&[u8]> for crate::protobuf::MessageBody {
    fn eq(&self, other: &&[u8]) -> bool {
        serde_json::from_slice::<Json>(&json_bytes(self)).ok()
            == serde_json::from_slice::<Json>(other).ok()
    }
}
