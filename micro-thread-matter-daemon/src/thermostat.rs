//! Standard thermostat adapter. Field identifiers and scalar limits are shared with native code.
use crate::{
    control::Id,
    operations::{ResultValue, Value},
};
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    error::{Error, ErrorCode},
    im::{client::ImClient, AttrDataTag, AttrPath, CmdDataTag},
    tlv::{TLVArray, TLVElement, TLVTag, TLVWrite},
    transport::exchange::Exchange,
    Matter,
};
use serde::{Deserialize, Serialize};

pub struct Field {
    pub name: &'static str,
    pub cluster: u32,
    pub attribute: u32,
    pub kind: u8,
    pub scale: f64,
    pub minimum: f64,
    pub maximum: f64,
    pub step: f64,
    pub writable: bool,
}
include!(concat!(env!("OUT_DIR"), "/thermostat_fields.rs"));
pub fn field(name: &str) -> Option<&'static Field> {
    FIELDS.iter().find(|f| f.cluster == 513 && f.name == name)
}
fn attribute(id: u32) -> Option<&'static Field> {
    FIELDS
        .iter()
        .find(|f| f.cluster == 513 && f.attribute == id)
}
pub fn readable(id: u32) -> bool {
    attribute(id).is_some() || matches!(id, 65529 | 65531 | 65536..=65542)
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Transition {
    pub time: u16,
    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub heat: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none", default)]
    pub cool: Option<f64>,
}
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Schedule {
    pub mode: u8,
    pub transitions: Vec<Transition>,
}
impl Schedule {
    pub fn parse(text: &str) -> Result<Self, Error> {
        if text.len() > 1536 {
            return Err(ErrorCode::InvalidData.into());
        }
        let schedule = if text.starts_with('{') {
            serde_json::from_str(text).map_err(|_| ErrorCode::InvalidData)?
        } else {
            let mut transitions = Vec::new();
            for token in text.split_whitespace() {
                let (clock, temperature) = token.split_once('/').ok_or(ErrorCode::InvalidData)?;
                let (hour, minute) = clock.split_once(':').ok_or(ErrorCode::InvalidData)?;
                let hour: u16 = hour.parse().map_err(|_| ErrorCode::InvalidData)?;
                let minute: u16 = minute.parse().map_err(|_| ErrorCode::InvalidData)?;
                if hour > 23 || minute > 59 {
                    return Err(ErrorCode::InvalidData.into());
                }
                transitions.push(Transition {
                    time: hour * 60 + minute,
                    heat: Some(temperature.parse().map_err(|_| ErrorCode::InvalidData)?),
                    cool: None,
                });
            }
            Self {
                mode: 1,
                transitions,
            }
        };
        if !schedule.valid(false) {
            return Err(ErrorCode::ConstraintError.into());
        }
        Ok(schedule)
    }
    fn valid(&self, allow_empty: bool) -> bool {
        (1..=3).contains(&self.mode)
            && (allow_empty || !self.transitions.is_empty())
            && self.transitions.len() <= 32
            && self.transitions.windows(2).all(|w| w[0].time < w[1].time)
            && self.transitions.iter().all(|t| {
                t.time < 1440
                    && t.heat.is_some() == (self.mode & 1 != 0)
                    && t.cool.is_some() == (self.mode & 2 != 0)
                    && [t.heat, t.cool].into_iter().flatten().all(|v| {
                        v.is_finite()
                            && (-273.15..=327.67).contains(&v)
                            && (v * 100.0 - (v * 100.0).round()).abs() < 0.00001
                    })
            })
    }
}
pub fn request_valid(name: &str, text: &str) -> bool {
    if name.len() == 10
        && name.starts_with("schedule_")
        && matches!(name.as_bytes()[9], b'0'..=b'6')
    {
        return text == "__read__" || Schedule::parse(text).is_ok();
    }
    let Some(f) = field(name).filter(|f| f.writable) else {
        return false;
    };
    let Ok(value) = text.parse::<f64>() else {
        return false;
    };
    value.is_finite()
        && (f.minimum..=f.maximum).contains(&value)
        && (value * f.scale - (value * f.scale).round()).abs() < 0.00001
        && (name != "system_mode" || matches!(value as i64, 0 | 1 | 3 | 4))
}
pub fn value_valid(id: u32, value: &Value) -> bool {
    match value {
        Value::Thermostat(value) => attribute(id).is_some_and(|f| {
            value.is_none_or(|v| {
                let v = v as f64 / f.scale;
                (f.minimum..=f.maximum).contains(&v)
            })
        }),
        Value::ThermostatList(values) => {
            matches!(id, 65529 | 65531)
                && values.len() <= 128
                && values
                    .iter()
                    .enumerate()
                    .all(|(i, v)| !values[..i].contains(v))
        }
        Value::ThermostatSchedule(schedule) => {
            (65536..=65542).contains(&id) && schedule.valid(true)
        }
        _ => false,
    }
}
fn scalar(f: &Field, element: TLVElement<'_>) -> Result<Value, Error> {
    let value = if element.null().is_ok() {
        None
    } else if matches!(f.kind, 0x28 | 0x29) {
        Some(element.i64()?)
    } else {
        Some(i64::try_from(element.u64()?).map_err(|_| ErrorCode::InvalidData)?)
    };
    let value = Value::Thermostat(value);
    if !value_valid(f.attribute, &value) {
        return Err(ErrorCode::InvalidData.into());
    }
    Ok(value)
}
pub fn report_values(
    node: u64,
    report: &rs_matter::im::ReportDataResp<'_>,
) -> Result<Vec<ResultValue>, Error> {
    let mut result = Vec::new();
    for f in FIELDS.iter().filter(|f| f.cluster == 513) {
        for (endpoint, element) in report.attrs::<TLVElement>(513, f.attribute) {
            // Attribute status entries do not invalidate other supported values in a wildcard report.
            let Ok(element) = element else {
                continue;
            };
            result.push(ResultValue {
                node: Id(node),
                endpoint,
                cluster: 513,
                attribute: f.attribute,
                value: scalar(f, element)?,
            });
        }
    }
    for id in [65529, 65531] {
        for (endpoint, values) in report.attrs::<TLVArray<u32>>(513, id) {
            let Ok(values) = values else {
                continue;
            };
            let values = values.iter().take(129).collect::<Result<Vec<_>, _>>()?;
            let value = Value::ThermostatList(values);
            if !value_valid(id, &value) {
                return Err(ErrorCode::InvalidData.into());
            }
            result.push(ResultValue {
                node: Id(node),
                endpoint,
                cluster: 513,
                attribute: id,
                value,
            });
        }
    }
    Ok(result)
}
pub async fn read<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    id: u32,
) -> Result<ResultValue, Error> {
    if (65536..=65542).contains(&id) {
        return read_schedule(
            matter,
            crypto,
            fabric,
            node,
            endpoint,
            (id - 65536) as u8,
            3,
        )
        .await;
    }
    if !readable(id) {
        return Err(ErrorCode::InvalidData.into());
    }
    let path = AttrPath {
        endpoint: Some(endpoint),
        cluster: Some(513),
        attr: Some(id),
        ..AttrPath::default()
    };
    let mut chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .read_with(|b| {
            b.attr_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut result = None;
    loop {
        let report = chunk.response()?;
        // Propagate a status for the exact requested path rather than silently returning no value.
        for (returned, value) in report.attrs::<TLVElement>(513, id) {
            value?;
            if returned != endpoint {
                return Err(ErrorCode::InvalidData.into());
            }
        }
        for value in report_values(node, &report)? {
            if value.endpoint == endpoint && value.attribute == id {
                if result.is_some() {
                    return Err(ErrorCode::InvalidData.into());
                }
                result = Some(value);
            }
        }
        match chunk.complete().await? {
            Some(next) => chunk = next,
            None => break,
        }
    }
    result.ok_or_else(|| ErrorCode::NotFound.into())
}
async fn number<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    id: u32,
) -> Result<f64, Error> {
    match read(matter, crypto, fabric, node, endpoint, id)
        .await?
        .value
    {
        Value::Thermostat(Some(value)) => {
            Ok(value as f64 / attribute(id).ok_or(ErrorCode::InvalidData)?.scale)
        }
        _ => Err(ErrorCode::InvalidData.into()),
    }
}
async fn list<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    id: u32,
) -> Result<Vec<u32>, Error> {
    match read(matter, crypto, fabric, node, endpoint, id)
        .await?
        .value
    {
        Value::ThermostatList(value) => Ok(value),
        _ => Err(ErrorCode::InvalidData.into()),
    }
}
async fn limits<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    attributes: &[u32],
    heat: bool,
) -> Result<(f64, f64), Error> {
    let (lo, hi, abs_lo, abs_hi, default_lo, default_hi) = if heat {
        (0x15, 0x16, 3, 4, 7.0, 30.0)
    } else {
        (0x17, 0x18, 5, 6, 16.0, 32.0)
    };
    let lower = if attributes.contains(&lo) {
        number(matter, crypto, fabric, node, endpoint, lo).await?
    } else if attributes.contains(&abs_lo) {
        number(matter, crypto, fabric, node, endpoint, abs_lo).await?
    } else {
        default_lo
    };
    let upper = if attributes.contains(&hi) {
        number(matter, crypto, fabric, node, endpoint, hi).await?
    } else if attributes.contains(&abs_hi) {
        number(matter, crypto, fabric, node, endpoint, abs_hi).await?
    } else {
        default_hi
    };
    if lower > upper {
        return Err(ErrorCode::ConstraintError.into());
    }
    Ok((lower, upper))
}
pub async fn operate<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    name: &str,
    text: &str,
) -> Result<ResultValue, Error> {
    if !request_valid(name, text) {
        return Err(ErrorCode::InvalidData.into());
    }
    let attributes = list(matter, crypto, fabric, node, endpoint, 65531).await?;
    let features = number(matter, crypto, fabric, node, endpoint, 65532).await? as u32;
    if name.starts_with("schedule_") {
        let day = name.as_bytes()[9] - b'0';
        let commands = list(matter, crypto, fabric, node, endpoint, 65529).await?;
        if features & 8 == 0 || !commands.contains(&2) {
            return Err(ErrorCode::InvalidAction.into());
        }
        if text == "__read__" {
            return read_schedule(
                matter,
                crypto,
                fabric,
                node,
                endpoint,
                day,
                if features & 3 == 3 {
                    3
                } else if features & 1 != 0 {
                    1
                } else {
                    2
                },
            )
            .await;
        }
        if !commands.contains(&1) {
            return Err(ErrorCode::InvalidAction.into());
        }
        let schedule = Schedule::parse(text)?;
        if (schedule.mode & 1 != 0 && features & 1 == 0)
            || (schedule.mode & 2 != 0 && features & 2 == 0)
        {
            return Err(ErrorCode::ConstraintError.into());
        }
        let maximum = if attributes.contains(&0x22) {
            number(matter, crypto, fabric, node, endpoint, 0x22).await?
        } else if attributes.contains(&0x21) {
            number(matter, crypto, fabric, node, endpoint, 0x21).await?
        } else {
            10.0
        };
        if schedule.transitions.len() > maximum as usize {
            return Err(ErrorCode::ConstraintError.into());
        }
        let heat = limits(matter, crypto, fabric, node, endpoint, &attributes, true).await?;
        let cool = limits(matter, crypto, fabric, node, endpoint, &attributes, false).await?;
        let deadband = if attributes.contains(&0x19) {
            number(matter, crypto, fabric, node, endpoint, 0x19).await?
        } else {
            2.5
        };
        for t in &schedule.transitions {
            if t.heat.is_some_and(|v| v < heat.0 || v > heat.1)
                || t.cool.is_some_and(|v| v < cool.0 || v > cool.1)
                || t.heat.zip(t.cool).is_some_and(|(h, c)| h + deadband > c)
            {
                return Err(ErrorCode::ConstraintError.into());
            }
        }
        let mut response = Exchange::initiate(matter, crypto, fabric, node)
            .await?
            .invoke_with(None, |b| {
                b.invoke_requests()?
                    .push()?
                    .path(endpoint, 513, 1)?
                    .data(|w| {
                        w.start_struct(&TLVTag::Context(CmdDataTag::Data as u8))?;
                        w.u8(&TLVTag::Context(0), schedule.transitions.len() as u8)?;
                        w.u8(&TLVTag::Context(1), 1 << day)?;
                        w.u8(&TLVTag::Context(2), schedule.mode)?;
                        w.start_array(&TLVTag::Context(3))?;
                        for t in &schedule.transitions {
                            w.start_struct(&TLVTag::Anonymous)?;
                            w.u16(&TLVTag::Context(0), t.time)?;
                            if let Some(v) = t.heat {
                                w.i16(&TLVTag::Context(1), (v * 100.0).round() as i16)?;
                            } else {
                                w.null(&TLVTag::Context(1))?;
                            }
                            if let Some(v) = t.cool {
                                w.i16(&TLVTag::Context(2), (v * 100.0).round() as i16)?;
                            } else {
                                w.null(&TLVTag::Context(2))?;
                            }
                            w.end_container()?;
                        }
                        w.end_container()?;
                        w.end_container()
                    })?
                    .end()?
                    .end()?
                    .end()
            })
            .await?;
        let mut accepted = false;
        loop {
            for (e, status) in response
                .response()?
                .ok_or(ErrorCode::InvalidData)?
                .statuses(513, 1)
            {
                status?;
                if e != endpoint || accepted {
                    return Err(ErrorCode::InvalidData.into());
                }
                accepted = true;
            }
            match response.complete().await? {
                Some(next) => response = next,
                None => break,
            }
        }
        if !accepted {
            return Err(ErrorCode::InvalidData.into());
        }
        let result =
            read_schedule(matter, crypto, fabric, node, endpoint, day, schedule.mode).await?;
        if let Value::ThermostatSchedule(actual) = &result.value {
            if actual != &schedule {
                return Err(ErrorCode::Failure.into());
            }
        }
        return Ok(result);
    }
    let f = field(name)
        .filter(|f| f.writable && attributes.contains(&f.attribute))
        .ok_or(ErrorCode::InvalidAction)?;
    let value = text.parse::<f64>().map_err(|_| ErrorCode::InvalidData)?;
    if name == "system_mode"
        && !match value as u8 {
            0 => true,
            1 => features & 35 == 35,
            3 => features & 2 != 0,
            4 => features & 1 != 0,
            _ => false,
        }
    {
        return Err(ErrorCode::ConstraintError.into());
    }
    if matches!(f.attribute, 0x11..=0x14) {
        let heat = matches!(f.attribute, 0x12 | 0x14);
        let (lo, hi) = limits(matter, crypto, fabric, node, endpoint, &attributes, heat).await?;
        if value < lo || value > hi {
            return Err(ErrorCode::ConstraintError.into());
        }
        let other = if heat {
            f.attribute - 1
        } else {
            f.attribute + 1
        };
        if features & 3 == 3 && attributes.contains(&other) {
            let peer = number(matter, crypto, fabric, node, endpoint, other).await?;
            let deadband = if attributes.contains(&0x19) {
                number(matter, crypto, fabric, node, endpoint, 0x19).await?
            } else {
                2.5
            };
            if if heat {
                value + deadband > peer
            } else {
                value - deadband < peer
            } {
                return Err(ErrorCode::ConstraintError.into());
            }
        }
    }
    let raw = (value * f.scale).round() as i64;
    let response = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .write_with(None, |b| {
            b.write_requests()?
                .push()?
                .path(endpoint, 513, f.attribute)?
                .data(|w| {
                    let tag = TLVTag::Context(AttrDataTag::Data as u8);
                    match f.kind {
                        0x28 => w.i8(&tag, raw as i8),
                        0x29 => w.i16(&tag, raw as i16),
                        0x21 | 0x19 => w.u16(&tag, raw as u16),
                        0x23 => w.u32(&tag, raw as u32),
                        _ => w.u8(&tag, raw as u8),
                    }
                })?
                .end()?
                .end()?
                .end()
        })
        .await?;
    let mut accepted = false;
    for (e, status) in response.response()?.statuses(513, f.attribute) {
        status?;
        if e != endpoint || accepted {
            return Err(ErrorCode::InvalidData.into());
        }
        accepted = true;
    }
    if !accepted {
        return Err(ErrorCode::InvalidData.into());
    }
    drop(response); // Release the receive buffer before opening the readback exchange.
    let result = read(matter, crypto, fabric, node, endpoint, f.attribute).await?;
    if !matches!(result.value,Value::Thermostat(Some(actual)) if actual==raw) {
        return Err(ErrorCode::Failure.into());
    }
    Ok(result)
}
async fn read_schedule<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    day: u8,
    mode: u8,
) -> Result<ResultValue, Error> {
    use rs_matter::dm::clusters::decl::thermostat::{
        ScheduleDayOfWeekBitmap, ScheduleModeBitmap, ThermostatClient,
    };
    if day > 6 {
        return Err(ErrorCode::InvalidData.into());
    }
    let handle = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .thermostat()
        .get_weekly_schedule(endpoint, |b| {
            b.days_to_return(ScheduleDayOfWeekBitmap::from_bits_retain(1 << day))?
                .mode_to_return(ScheduleModeBitmap::from_bits_retain(mode))?
                .end()
        })
        .await?;
    let response = handle.response()?;
    if response.day_of_week_for_sequence()?.bits() != (1 << day) {
        return Err(ErrorCode::InvalidData.into());
    }
    let mode = response.mode_for_sequence()?.bits();
    let mut transitions = Vec::new();
    for transition in response.transitions()?.iter() {
        let t = transition?;
        transitions.push(Transition {
            time: t.transition_time()?,
            heat: if mode & 1 != 0 {
                t.heat_setpoint()?
                    .as_opt_ref()
                    .map(|v| f64::from(*v) / 100.0)
            } else {
                None
            },
            cool: if mode & 2 != 0 {
                t.cool_setpoint()?
                    .as_opt_ref()
                    .map(|v| f64::from(*v) / 100.0)
            } else {
                None
            },
        });
        if transitions.len() > 32 {
            return Err(ErrorCode::NoSpace.into());
        }
    }
    if transitions.len() != response.number_of_transitions_for_sequence()? as usize {
        return Err(ErrorCode::InvalidData.into());
    }
    let value = Value::ThermostatSchedule(Schedule { mode, transitions });
    if !value_valid(65536 + u32::from(day), &value) {
        return Err(ErrorCode::InvalidData.into());
    }
    handle.complete().await?;
    Ok(ResultValue {
        node: Id(node),
        endpoint,
        cluster: 513,
        attribute: 65536 + u32::from(day),
        value,
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn standard_fields_and_schedules() {
        assert!(request_valid("occupied_cooling_setpoint", "24.25"));
        assert!(!request_valid("external_temperature_input", "20"));
        assert!(!request_valid("system_mode", "2"));
        assert!(!request_valid("occupied_heating_setpoint", "NaN"));
        assert!(
            Schedule::parse(r#"{"mode":3,"transitions":[{"time":60,"heat":20,"cool":25}]}"#)
                .is_ok()
        );
        assert!(Schedule::parse("00:00/20 07:00/22").is_ok());
        assert!(Schedule::parse("00:00/20 00:00/21").is_err());
        assert!(Schedule::parse(r#"{"mode":2,"transitions":[{"time":60,"heat":20}]}"#).is_err());
        assert!(!value_valid(0, &Value::Thermostat(Some(-32768))));
        assert!(value_valid(0, &Value::Thermostat(None)));
    }
}
