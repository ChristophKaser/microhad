//! Protobuf boundary adapter. Internal JSON models retain their public/persistence encodings.
//! No payload, field name, or value may enter diagnostics.
use crate::ipc::{Frame, ProtocolError, MAX_PAYLOAD};
#[derive(Clone)]
enum Value { Null, Bool(bool), Integer(i128), Float(f64), Text(String), Bytes(Vec<u8>), Array(Vec<Value>), Map(Vec<(Value,Value)>) }
#[allow(non_snake_case, dead_code, unused_variables, unused_mut)]
mod generated { include!("../../protocol/generated/protobuf.rs"); }

use serde_json::Value as Json;
use std::collections::HashSet;

fn child(shape: &str, key: &str) -> Option<&'static str> {
    let fields=crate::wire_shapes::FIELDS;
    fields.binary_search_by(|(s,k,_)| (*s,*k).cmp(&(shape,key))).ok().map(|i|fields[i].2)
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
                format!("{}:{action}",generated::request_name(opcode))
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
        "body" if kind==0 => {
            let action=value.get(if opcode==11 {"command"}else{"action"}).and_then(Json::as_str).unwrap_or("");
            if (9..=11).contains(&opcode)&&!action.is_empty(){format!("{}:{action}",generated::request_name(opcode))}else{generated::request_name(opcode).to_owned()}
        }
        "result" if opcode==10 => if value.get("subscriptions").is_some(){"subscription-health"}else if value.get("devices").is_some(){"page"}else{"job"}.into(),
        _ => shape(context,&Value::Null,kind,opcode,event),
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
        let digits=b"0123456789abcdef";
        let mut out=String::with_capacity(b.len()*2);
        for byte in b {out.push(digits[usize::from(byte>>4)] as char);out.push(digits[usize::from(byte&15)] as char);}
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
fn warning(opcode: u8, count: usize) {
    #[cfg(feature = "controller-service")]
    {
        log::warn!("Matter IPC opcode {opcode}: ignored {count} unknown fields");
    }
    #[cfg(not(feature = "controller-service"))]
    log::debug!("Matter IPC opcode {opcode}: ignored {count} unknown fields");
}
fn schema(name: &str) -> &'static [generated::Field] { generated::schema(name) }
fn nfield<'a>(v: &'a Value, key: &str) -> Result<&'a Value, ProtocolError> { field(v,key).ok_or(ProtocolError::Malformed) }
fn normalize(v: Value, msg: &str, kind:u8, op:u8, ev:&str, incoming:bool) -> Result<Value, ProtocolError> {
    let fields=schema(msg);
    if fields.len()==1 && fields[0].repeated && fields[0].name=="items" {
        let values=if incoming { match v {Value::Map(mut m)=>match m.iter().position(|(k,_)|text(k)=="items").map(|i|m.remove(i).1){Some(Value::Array(a))=>a,None=>vec![],_=>return Err(ProtocolError::Malformed)},_=>return Err(ProtocolError::Malformed)} } else { match v { Value::Array(a)=>a,_=>return Err(ProtocolError::Malformed) } };
        let f=&fields[0];let values=values.into_iter().map(|x|if f.kind==11 {normalize(x,f.message,kind,op,ev,incoming)}else{Ok(x)}).collect::<Result<Vec<_>,_>>()?;
        return Ok(if incoming { Value::Array(values) } else { Value::Map(vec![(Value::Text("items".into()),Value::Array(values))]) });
    }
    if fields.first().is_some_and(|f|f.oneof) {
        if incoming {
            let Value::Map(mut m)=v else {return Err(ProtocolError::Malformed)};
            if m.len()!=1{return Err(ProtocolError::Malformed)}
            let (k,x)=m.remove(0);let key=text(&k);let f=fields.iter().find(|f|f.name==key).ok_or(ProtocolError::Malformed)?;
            return if key=="null" {if matches!(x,Value::Bool(true)){Ok(Value::Null)}else{Err(ProtocolError::Malformed)}}else if f.kind==11 {normalize(x,f.message,kind,op,ev,true)}else{Ok(x)};
        }
        let key=match msg {
            "MessageBody"=>if kind==0 {generated::request_name(op).to_owned()}else if kind==2{"event".into()}else if op==1{"matter".into()}else if op==2{"thread".into()}else{"reply".into()},
            "OperationResult"=>if op<=5{"session".into()}else if op<=8{generated::ack_name(op).to_owned()}else if op==9{"thread".into()}else if has(&v,"subscriptions"){"subscription_health".into()}else if has(&v,"devices"){"page".into()}else{"job".into()},
            "EventPayload"=>match ev {"thread"=>"thread","commissioning"=>"session","ble_write"=>"ble_write","matter_report"=>"reading","matter_button"=>"button","matter_inventory"=>"inventory","matter_inventory_nodes"=>"inventory_nodes",_=>return Err(ProtocolError::Malformed)}.into(),
            _=>match &v {Value::Null=>"null",_ if msg=="NullableText"=>"text",_ if msg=="NullableUint"||msg=="NullableInt"=>"integer",Value::Bool(_)=>"boolean",Value::Float(_)=>"number",Value::Integer(i)=>if *i>=0&&msg=="ReadingValue"{"unsigned"}else{"integer"},Value::Text(_)=>"text",Value::Array(_)=>"list",_=>"measurement"}.into(),
        };
        let f=fields.iter().find(|f|f.name==key).ok_or(ProtocolError::Malformed)?;
        let x=if key=="null"{Value::Bool(true)}else if f.kind==11{normalize(v,f.message,kind,op,ev,false)?}else{v};
        return Ok(Value::Map(vec![(Value::Text(key),x)]));
    }
    let event=field(&v,"event").map(text).filter(|s|!s.is_empty()).unwrap_or(ev).to_owned();
    let Value::Map(m)=v else {return Err(ProtocolError::Malformed)};
    let mut out=Vec::new();
    for (k,x) in m {
        let Some(f)=fields.iter().find(|f|f.name==text(&k)) else {continue};
        let x=if f.kind==11 {normalize(x,f.message,kind,op,&event,incoming)?}else{x};
        out.push((k,x));
    }
    Ok(Value::Map(out))
}
fn varint(b:&[u8],at:&mut usize)->Result<u64,ProtocolError>{
    let mut n=0;
    for i in 0..10 {let x=*b.get(*at).ok_or(ProtocolError::Malformed)?;*at+=1;if i==9&&x>1{return Err(ProtocolError::Malformed)}n|=u64::from(x&127)<<(i*7);if x<128{return Ok(n)}}
    Err(ProtocolError::Malformed)
}
fn scan(b:&[u8],msg:&str,depth:usize,unknown:&mut usize)->Result<(),ProtocolError>{
    if depth>16{return Err(ProtocolError::Malformed)}
    let fields=schema(msg);let mut seen=HashSet::new();let mut counts=std::collections::HashMap::new();let mut choice=false;let mut at=0;
    while at<b.len(){
        let tag=varint(b,&mut at)?;let num=tag>>3;let wt=tag&7;
        if num==0||num>=1<<29{return Err(ProtocolError::Malformed)}
        let mut number=None;
        let data=match wt {0=>{number=Some(varint(b,&mut at)?);&b[0..0]},1|2|5=>{let n=if wt==2{usize::try_from(varint(b,&mut at)?).map_err(|_|ProtocolError::Malformed)?}else if wt==1{8}else{4};let end=at.checked_add(n).ok_or(ProtocolError::Malformed)?;let data=b.get(at..end).ok_or(ProtocolError::Malformed)?;at=end;data},_=>return Err(ProtocolError::Malformed)};
        let Some(f)=fields.iter().find(|f|u64::from(f.tag)==num)else{*unknown+=1;continue};
        let typ=f.kind;let expected=if typ==1{1}else if matches!(typ,9|11|12){2}else{0};
        if f.repeated && wt==2 && matches!(typ,4|13|18|8) {
            let mut p=0;
            while p<data.len(){let n=varint(data,&mut p)?;if (typ==13&&n>u32::MAX.into())||(typ==8&&n>1){return Err(ProtocolError::Malformed)}let count=counts.entry(num).or_insert(0);*count+=1;if *count>512{return Err(ProtocolError::Malformed)}}
            continue;
        }
        if wt!=expected || (!f.repeated&&!seen.insert(num)) || (f.oneof&&choice){return Err(ProtocolError::Malformed)}
        choice|=f.oneof;
        if f.repeated{let n=counts.entry(num).or_insert(0);*n+=1;if *n>512{return Err(ProtocolError::Malformed)}}
        match typ {1=>{if !f64::from_le_bytes(data.try_into().unwrap()).is_finite(){return Err(ProtocolError::Malformed)}},8 if number.unwrap()>1=>return Err(ProtocolError::Malformed),13 if number.unwrap()>u32::MAX.into()=>return Err(ProtocolError::Malformed),9=>{std::str::from_utf8(data).map_err(|_|ProtocolError::Malformed)?;},11=>scan(data,f.message,depth+1,unknown)?,_=>()}
    }
    Ok(())
}
pub fn encode(frame:&Frame)->Result<Vec<u8>,ProtocolError>{
    let json=if frame.payload.is_empty(){serde_json::json!({})}else{serde_json::from_slice(&frame.payload).map_err(|_|ProtocolError::Malformed)?};
    let body=to_wire(json,"body",frame.kind,frame.opcode,"")?;
    let body=normalize(body,"MessageBody",frame.kind,frame.opcode,"",false)?;
    let env=Value::Map(vec![(Value::Text("version".into()),Value::Integer(3)),(Value::Text("kind".into()),Value::Integer(frame.kind.into())),(Value::Text("opcode".into()),Value::Integer(frame.opcode.into())),(Value::Text("id".into()),Value::Integer(frame.id.into())),(Value::Text("body".into()),body)]);
    let out=generated::encode(&env)?;
    if out.len()>MAX_PAYLOAD{return Err(ProtocolError::Oversized)}
    scan(&out,"IpcEnvelope",0,&mut 0)?;
    let mut bytes=(out.len() as u16).to_le_bytes().to_vec();bytes.extend(out);Ok(bytes)
}
pub fn decode(bytes:&[u8])->Result<Frame,ProtocolError>{
    if bytes.is_empty()||bytes.len()>MAX_PAYLOAD{return Err(ProtocolError::Malformed)}
    let mut unknown=0;scan(bytes,"IpcEnvelope",0,&mut unknown)?;
    let mut env=generated::decode(bytes)?;
    let integer=|key|match nfield(&env,key)?{Value::Integer(i)=>u64::try_from(*i).map_err(|_|ProtocolError::Malformed),_=>Err(ProtocolError::Malformed)};
    if integer("version")?!=3{return Err(ProtocolError::Malformed)}
    let kind=u8::try_from(integer("kind")?).map_err(|_|ProtocolError::Malformed)?;let opcode=u8::try_from(integer("opcode")?).map_err(|_|ProtocolError::Malformed)?;let id=u32::try_from(integer("id")?).map_err(|_|ProtocolError::Malformed)?;
    if kind>2||!(1..=12).contains(&opcode)||(kind==2)!=(id==0)||(kind==2)!=(opcode==12){return Err(ProtocolError::Malformed)}
    let wrapped=nfield(&env,"body")?;let key=if kind==0{generated::request_name(opcode).to_owned()}else if kind==2{"event".into()}else if opcode==1{"matter".into()}else if opcode==2{"thread".into()}else{"reply".into()};
    if !has(wrapped,&key){return Err(ProtocolError::Malformed)}
    let Value::Map(ref mut fields)=env else {return Err(ProtocolError::Malformed)};
    let body_index=fields.iter().position(|(key,_)|text(key)=="body").ok_or(ProtocolError::Malformed)?;
    let body_value=fields.remove(body_index).1;
    let body=normalize(body_value,"MessageBody",kind,opcode,"",true)
        .and_then(|body| from_wire(body,"body",kind,opcode,"",&mut unknown));
    if unknown>0{warning(opcode,unknown)}
    let payload=match body{Ok(Json::Object(m))if kind==0&&opcode<=2&&m.is_empty()=>vec![],Ok(v)=>serde_json::to_vec(&v).map_err(|_|ProtocolError::Malformed)?,Err(_)=>b"!invalid protobuf body".to_vec()};
    Ok(Frame{kind,opcode,id,payload})
}
