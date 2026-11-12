//! Matter electrical measurements. Wire addresses stay intact, including energy subfields.
use rs_matter::sc::ExchangeInitiator as _;
use crate::{control::Id, operations::{ResultValue, Value}};
use rs_matter::{error::{Error,ErrorCode}, im::{ReportDataResp,AttrPath,ImClient}, tlv::{TLVElement,TLVArray}, crypto::Crypto, transport::exchange::Exchange, Matter};
use core::num::NonZeroU8;
use serde::Serialize;
#[derive(Clone,Debug,PartialEq,Serialize)]
pub struct Quantity {
    pub key: String,
    #[serde(skip_serializing_if="Option::is_none")]
    pub value: Option<f64>,
    pub unit: String,
    pub known: bool,
}
#[derive(Clone,Debug,Default,PartialEq,Serialize)]
pub struct Reading {
    pub quantities: Vec<Quantity>,
    #[serde(skip_serializing_if="Option::is_none")] pub start_timestamp: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub end_timestamp: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub start_systime: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub end_systime: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub imported_reset_timestamp: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub exported_reset_timestamp: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub imported_reset_systime: Option<u64>,
    #[serde(skip_serializing_if="Option::is_none")] pub exported_reset_systime: Option<u64>,
}
#[derive(Debug)]
pub struct Definition { pub cluster:u32,pub attribute:u32,pub sub:u8,pub key:&'static str,pub unit:&'static str,pub divisor:f64 }
include!("electrical_definitions.rs");
pub fn definitions() -> &'static [Definition] { DEFINITIONS }
pub fn supported(cluster:u32,attribute:u32)->bool {
    (cluster==145 && attribute==5) || definitions().iter().any(|d|d.cluster==cluster && d.attribute==attribute)
}
fn optional_time(element:&TLVElement<'_>,tag:u8)->Result<Option<u64>,Error>{
    if element.null().is_ok(){return Ok(None)}
    let field=element.structure()?.find_ctx(tag)?;
    if field.is_empty() || field.null().is_ok(){Ok(None)}else{Ok(Some(field.u64()?))}
}
pub fn decode(cluster:u32,attribute:u32,element:TLVElement<'_>)->Result<Reading,Error>{
    let mut result=Reading::default();
    if cluster==145 {
        if attribute==5 {
            result.imported_reset_timestamp=optional_time(&element,0)?;
            result.exported_reset_timestamp=optional_time(&element,1)?;
            result.imported_reset_systime=optional_time(&element,2)?;
            result.exported_reset_systime=optional_time(&element,3)?;
            return Ok(result)
        }
        result.start_timestamp=optional_time(&element,1)?;
        result.end_timestamp=optional_time(&element,2)?;
        result.start_systime=optional_time(&element,3)?;
        result.end_systime=optional_time(&element,4)?;
    }
    for d in definitions().iter().filter(|d|d.cluster==cluster && d.attribute==attribute) {
        let value=if element.null().is_ok(){None}else{
            let e=if cluster==145{element.structure()?.find_ctx(d.sub)?}else{element.clone()};
            if e.is_empty() {
                if d.sub==0{return Err(ErrorCode::InvalidData.into())} else {continue}
            }
            if e.null().is_ok(){None}else{
                let raw=e.i64()?;
                if raw==i64::MIN || (cluster==144 && attribute==17 && !(-10000..=10000).contains(&raw)) {return Err(ErrorCode::InvalidData.into())}
                Some(raw as f64/d.divisor)
            }
        };
        result.quantities.push(Quantity{key:d.key.into(),value,unit:d.unit.into(),known:value.is_some()});
    }
    Ok(result)
}
pub fn report_values(node:u64,report:&ReportDataResp<'_>)->Result<Vec<ResultValue>,Error>{
    let mut values=Vec::new();
    for cluster in [144,145] {
        for attribute in 0..=18 {
            if !supported(cluster,attribute){continue}
            for (endpoint,value) in report.attrs::<TLVElement>(cluster,attribute){
                values.push(ResultValue{node:Id(node),endpoint,cluster,attribute,value:Value::Electrical(decode(cluster,attribute,match value {Ok(v)=>v,Err(_)=>continue})?)});
            }
        }
    }
    Ok(values)
}
pub async fn paths<C:Crypto>(matter:&Matter<'_>,crypto:&C,fabric:NonZeroU8,node:u64,endpoint:u16,cluster:u32)->Result<Vec<AttrPath>,Error>{
    let requested=[AttrPath{endpoint:Some(endpoint),cluster:Some(cluster),attr:Some(65531),..Default::default()},AttrPath{endpoint:Some(endpoint),cluster:Some(cluster),attr:Some(65532),..Default::default()}];
    let mut chunk=Exchange::initiate(matter,crypto,fabric,node).await?.read_with(|b|b.attr_requests_from(&requested)?.fabric_filtered(true)?.end()).await?;
    let mut paths=Vec::new();
    loop {
        let report=chunk.response()?;
        for (ep,value) in report.attrs::<TLVArray<'_,u32>>(cluster,65531){
            if ep!=endpoint{return Err(ErrorCode::InvalidData.into())}
            for attr in value?.iter(){let attr=attr?;if supported(cluster,attr){paths.push(AttrPath{endpoint:Some(ep),cluster:Some(cluster),attr:Some(attr),..Default::default()});}}
        }
        for (_,value) in report.attrs::<u32>(cluster,65532){value?;}
        match chunk.complete().await?{Some(next)=>chunk=next,None=>break}
    }
    if paths.is_empty() || paths.len()>20 {return Err(ErrorCode::InvalidData.into())}
    Ok(paths)
}
pub async fn read<C:Crypto>(matter:&Matter<'_>,crypto:&C,fabric:NonZeroU8,node:u64,endpoint:u16,cluster:u32,attribute:u32)->Result<ResultValue,Error>{
    let path=AttrPath{endpoint:Some(endpoint),cluster:Some(cluster),attr:Some(attribute),..Default::default()};
    let mut chunk=Exchange::initiate(matter,crypto,fabric,node).await?.read_with(|b|b.attr_requests_from(core::slice::from_ref(&path))?.fabric_filtered(true)?.end()).await?;
    let mut result=None;
    loop{
        for value in report_values(node,&chunk.response()?)?{if value.endpoint==endpoint && value.cluster==cluster && value.attribute==attribute{result=Some(value)}}
        match chunk.complete().await?{Some(next)=>chunk=next,None=>break}
    }
    result.ok_or_else(||ErrorCode::NotFound.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use rs_matter::{tlv::{TLVTag,TLVWrite,ToTLV},utils::storage::WriteBuf};
    fn scalar(cluster:u32,attribute:u32,n:i64)->Reading {
        let mut bytes=[0;32];let mut w=WriteBuf::new(&mut bytes);
        n.to_tlv(&TLVTag::Anonymous,&mut w).unwrap();
        decode(cluster,attribute,TLVElement::new(w.as_slice())).unwrap()
    }
    #[test]fn signed_zero_and_units(){
        assert_eq!(scalar(144,8,-12345).quantities[0].value,Some(-12.345));
        assert_eq!(scalar(144,8,0).quantities[0].value,Some(0.0));
        assert_eq!(scalar(144,4,230123).quantities[0].value,Some(230.123));
        assert_eq!(scalar(144,5,1234).quantities[0].value,Some(1.234));
        assert_eq!(scalar(144,14,50000).quantities[0].value,Some(50.0));
        assert_eq!(scalar(144,17,-9500).quantities[0].value,Some(-0.95));
        let unknown=decode(144,8,TLVElement::new(&[0x14])).unwrap();
        assert!(!unknown.quantities[0].known);assert_eq!(unknown.quantities[0].value,None);
        assert!(decode(144,8,TLVElement::new(&[0x08])).is_err());
    }
    #[test]fn energy_subfields_and_period_metadata(){
        let mut bytes=[0;128];let mut w=WriteBuf::new(&mut bytes);
        w.start_struct(&TLVTag::Anonymous).unwrap();
        w.i64(&TLVTag::Context(0),123456789000).unwrap();
        w.u64(&TLVTag::Context(1),1000).unwrap();w.u64(&TLVTag::Context(2),1100).unwrap();
        w.i64(&TLVTag::Context(5),2000000).unwrap();w.i64(&TLVTag::Context(6),-3000000).unwrap();w.end_container().unwrap();
        let value=decode(145,3,TLVElement::new(w.as_slice())).unwrap();
        assert_eq!(value.quantities.len(),3);assert_eq!(value.quantities[0].value,Some(123456.789));
        assert_eq!(value.quantities[1].unit,"kVAh");assert_eq!(value.quantities[2].value,Some(-3.0));
        assert_eq!(value.start_timestamp,Some(1000));assert_eq!(value.end_timestamp,Some(1100));
    }
    #[test]fn electrical_ipc_roundtrip_preserves_double_precision(){
        let reading=ResultValue{node:Id(40),endpoint:2,cluster:145,attribute:1,value:Value::Electrical(scalar_energy())};
        let message=crate::responses::event(crate::responses::Event::MatterReport(&reading));
        // Production protobuf plus the fixture JSON adapter must recognize the new choice.
        assert!(message.is_some());
        let json=serde_json::json!({"schema":1,"event":"matter_report","data":reading});
        let body=crate::wire_json::from_json(2,12,&serde_json::to_vec(&json).unwrap()).unwrap();
        let encoded=crate::wire_json::json_bytes(&body);
        let decoded:serde_json::Value=serde_json::from_slice(&encoded).unwrap();
        assert_eq!(decoded["data"]["value"]["value"]["quantities"][0]["value"],123456.789012_f64);
    }
    fn scalar_energy()->Reading{Reading{quantities:vec![Quantity{key:"cumulative_imported_energy".into(),value:Some(123456.789012),unit:"kWh".into(),known:true}],..Default::default()}}
}
