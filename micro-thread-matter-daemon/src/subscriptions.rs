//! Bounded Interaction Model subscription establishment for operational endpoints.
use core::num::NonZeroU8;
use std::{cell::RefCell, rc::Rc};
use rs_matter::{
    crypto::Crypto,
    error::{Error, ErrorCode},
    im::{
        client::{ImClient, SubscribeEstablished, SubscribeOutcome},
        AttrPath, EventPath,
    },
    tlv::FromTLV,
    transport::exchange::Exchange,
    Matter,
};

pub const MIN_REPORT_INTERVAL_SECONDS: u16 = 1;
pub const MAX_REPORT_INTERVAL_SECONDS: u16 = 3600;
pub const MAX_REPORT_VALUES: usize = 128;

/// Shared with the report receiver so retired IDs are rejected before ACKing
/// any chunk. Keys include the authenticated fabric and peer, not just the ID.
#[derive(Clone, Default)]
pub struct ReportSubscriptions(Rc<RefCell<Vec<(u8, u64, u32)>>>);

impl ReportSubscriptions {
    pub fn replace(&self, fabric: u8, identities: impl Iterator<Item = (u64, u32)>) {
        let mut active = self.0.borrow_mut();
        active.clear();
        active.extend(identities.map(|(node, id)| (fabric, node, id)));
    }

    fn accepts(&self, fabric: u8, node: u64, id: u32) -> bool {
        self.0.borrow().contains(&(fabric, node, id))
    }
}
pub struct ReceivedReport {
    pub fabric: u8,
    pub node: u64,
    pub subscription_id: u32,
    pub values: Vec<crate::operations::ResultValue>,
    pub buttons: Vec<MatterButtonEvent>,
}
#[derive(Clone, Debug, serde::Serialize)]
pub struct MatterButtonEvent {
    pub node: crate::control::Id,
    pub endpoint: u16,
    pub event: u8,
    pub position: Option<u8>,
    pub press_count: Option<u8>,
}
pub enum IncomingMessage {
    Report(ReceivedReport),
}
pub struct DeviceSubscription {
    pub established: SubscribeEstablished,
    pub unit: Option<&'static str>,
    pub priming_values: Vec<crate::operations::ResultValue>,
}

/// Establish one bounded attribute subscription and drain every priming chunk.
/// Active reports are received separately on server-initiated exchanges.
pub async fn establish_attribute<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
    attribute: u32,
) -> Result<SubscribeEstablished, Error> {
    let path = AttrPath {
        endpoint: Some(endpoint),
        cluster: Some(cluster),
        attr: if cluster == 513 && attribute == u32::MAX { None } else { Some(attribute) },
        ..AttrPath::default()
    };
    let chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .subscribe_with(|builder| {
            builder
                .keep_subs(true)?
                .min_int_floor(MIN_REPORT_INTERVAL_SECONDS)?
                .max_int_ceil(MAX_REPORT_INTERVAL_SECONDS)?
                .attr_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut chunk = chunk;
    loop {
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => return Ok(established),
        }
    }
}

/// Establish one endpoint subscription and capture metadata needed to normalize
/// its reports. Matter concentration-unit attributes are static for the
/// lifetime of the endpoint subscription and are read before it is accepted.
pub async fn establish_device_attribute<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    fabric: NonZeroU8,
    node: u64,
    endpoint: u16,
    cluster: u32,
    attribute: u32,
) -> Result<DeviceSubscription, Error> {
    establish_device_attribute_with_interval(matter, crypto, fabric, node, endpoint, cluster, attribute,
        MAX_REPORT_INTERVAL_SECONDS).await
}

pub async fn establish_device_attribute_with_interval<C: Crypto>(
    matter: &Matter<'_>, crypto: &C, fabric: NonZeroU8, node: u64, endpoint: u16,
    cluster: u32, attribute: u32, max_interval: u16,
) -> Result<DeviceSubscription, Error> {
    let unit = match cluster {
        1037 | 1066 => Some(
            crate::operations::read_measurement_unit(
                matter, crypto, fabric, node, endpoint, cluster,
            )
            .await?,
        ),
        _ => None,
    };
    let path = AttrPath {
        endpoint: Some(endpoint),
        cluster: Some(cluster),
        attr: if cluster == 513 && attribute == u32::MAX { None } else { Some(attribute) },
        ..AttrPath::default()
    };
    let mut chunk = Exchange::initiate(matter, crypto, fabric, node)
        .await?
        .subscribe_with(|builder| {
            builder
                .keep_subs(true)?
                .min_int_floor(MIN_REPORT_INTERVAL_SECONDS)?
                .max_int_ceil(max_interval)?
                .attr_requests_from(core::slice::from_ref(&path))?
                .fabric_filtered(true)?
                .end()
        })
        .await?;
    let mut priming_values = Vec::new();
    let established = loop {
        let report = chunk.response()?;
        let mut values = crate::operations::report_values(node, &report)?;
        if priming_values.len().saturating_add(values.len()) > MAX_REPORT_VALUES {
            return Err(ErrorCode::NoSpace.into());
        }
        priming_values.append(&mut values);
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => break established,
        }
    };
    Ok(DeviceSubscription {
        established,
        unit,
        priming_values,
    })
}

pub async fn establish_switch_events<C: Crypto>(
    matter: &Matter<'_>, crypto: &C, fabric: NonZeroU8, node: u64, endpoint: u16,
) -> Result<DeviceSubscription, Error> {
    let path = EventPath { endpoint: Some(endpoint), cluster: Some(59), event: None, ..EventPath::default() };
    let chunk = Exchange::initiate(matter, crypto, fabric, node).await?.subscribe_with(|builder| {
        builder.keep_subs(true)?
            .min_int_floor(MIN_REPORT_INTERVAL_SECONDS)?
            .max_int_ceil(MAX_REPORT_INTERVAL_SECONDS)?
            .event_requests_from(core::slice::from_ref(&path))?
            .fabric_filtered(true)?
            .end()
    }).await?;
    let mut chunk = chunk;
    loop {
        match chunk.complete().await? {
            SubscribeOutcome::NextChunk(next) => chunk = next,
            SubscribeOutcome::Established(established) => {
                return Ok(DeviceSubscription {
                    established,
                    unit: None,
                    priming_values: Vec::new(),
                })
            }
        }
    }
}

/// Accept and acknowledge one active server-initiated ReportData exchange.
/// Chunked reports are combined under a strict value bound, and the authenticated
/// fabric/node and subscription ID are retained for manager-side correlation.
pub async fn receive_report(
    matter: &Matter<'_>, active: ReportSubscriptions,
) -> Result<ReceivedReport, Error> {
    receive_report_exchange(
        Exchange::accept_matching(
            matter,
            rs_matter::im::PROTO_ID_INTERACTION_MODEL,
            rs_matter::im::OpCode::ReportData as u8,
        )
        .await?, active,
    )
    .await
}

pub async fn receive_incoming(
    matter: &Matter<'_>, active: ReportSubscriptions,
) -> Result<IncomingMessage, Error> {
    let exchange = Exchange::accept_matching(
        matter,
        rs_matter::im::PROTO_ID_INTERACTION_MODEL,
        rs_matter::im::OpCode::ReportData as u8,
    )
    .await?;
    receive_report_exchange(exchange, active)
        .await
        .map(IncomingMessage::Report)
}

async fn receive_report_exchange(
    mut exchange: Exchange<'_>, active: ReportSubscriptions,
) -> Result<ReceivedReport, Error> {
    let (fabric, node) = exchange.peer_identity()?.ok_or(ErrorCode::InvalidData)?;
    let mut subscription_id = None;
    let mut values = Vec::new();
    let mut buttons = Vec::new();
    loop {
        let (received_id, more_chunks, suppress_response, mut chunk_values, mut chunk_buttons) = {
            let rx = exchange.recv_fetch().await?;
            if rx.meta().proto_opcode != rs_matter::im::OpCode::ReportData as u8 {
                return Err(ErrorCode::InvalidOpcode.into());
            }
            let report = rs_matter::im::ReportDataResp::from_tlv(
                &rs_matter::tlv::TLVElement::new(rx.payload()),
            )?;
            let values = crate::operations::report_values(node, &report)?;
            let mut chunk_buttons = Vec::new();
            if let Some(events) = report.event_reports.as_ref() {
                for event in events.iter() {
                    let rs_matter::im::EventResp::Data(event) = event? else { continue };
                    if event.path.cluster != Some(59) || event.path.event.is_none() || event.path.endpoint.is_none() { continue; }
                    let event_id = event.path.event.unwrap();
                    if event_id > 5 { continue; }
                    let data = event.data.structure()?;
                    let position = data.ctx(0).ok().and_then(|value| value.u8().ok());
                    let press_count = data.ctx(1).ok().and_then(|value| value.u8().ok());
                    chunk_buttons.push(MatterButtonEvent {
                        node: crate::control::Id(node), endpoint: event.path.endpoint.unwrap(),
                        event: event_id as u8, position, press_count,
                    });
                }
            }
            (
                report.subscription_id.ok_or(ErrorCode::InvalidData)?,
                report.more_chunks.unwrap_or(false),
                report.suppress_response.unwrap_or(false),
                values,
                chunk_buttons,
            )
        };
        if subscription_id.is_some_and(|id| id != received_id)
            || values.len().saturating_add(chunk_values.len()) > MAX_REPORT_VALUES
            || buttons.len().saturating_add(chunk_buttons.len()) > MAX_REPORT_VALUES
            || (more_chunks && suppress_response)
        {
            return Err(ErrorCode::InvalidData.into());
        }
        if !active.accepts(fabric, node, received_id) {
            exchange.send_with(|_, writer| {
                rs_matter::im::StatusResp::write(writer, rs_matter::im::IMStatusCode::InvalidSubscription)?;
                Ok(Some(rs_matter::im::OpCode::StatusResponse.into()))
            }).await?;
            return Err(ErrorCode::InvalidData.into());
        }
        subscription_id = Some(received_id);
        values.append(&mut chunk_values);
        buttons.append(&mut chunk_buttons);
        if !suppress_response {
            exchange
                .send_with(|_, writer| {
                    rs_matter::im::StatusResp::write(writer, rs_matter::im::IMStatusCode::Success)?;
                    Ok(Some(rs_matter::im::OpCode::StatusResponse.into()))
                })
                .await?;
        }
        if !more_chunks {
            break;
        }
    }
    Ok(ReceivedReport {
        fabric,
        node,
        subscription_id: subscription_id.ok_or(ErrorCode::InvalidData)?,
        values,
        buttons,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retired_ids_and_wrong_peers_are_rejected_by_the_running_receiver() {
        let active = ReportSubscriptions::default();
        let receiver = active.clone();
        active.replace(1, [(2, 77), (3, 77), (2, 78)].into_iter());
        assert!(receiver.accepts(1, 2, 77));
        assert!(receiver.accepts(1, 3, 77));
        assert!(!receiver.accepts(2, 2, 77));
        assert!(!receiver.accepts(1, 4, 77));
        active.replace(1, [(2, 79), (3, 77), (2, 78)].into_iter());
        assert!(!receiver.accepts(1, 2, 77));
        assert!(receiver.accepts(1, 2, 79));
        assert!(receiver.accepts(1, 2, 78));
        active.replace(1, core::iter::empty());
        assert!(!receiver.accepts(1, 2, 79));
    }
}
