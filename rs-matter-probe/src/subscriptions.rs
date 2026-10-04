//! Bounded Interaction Model subscription establishment for operational endpoints.
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    error::{Error, ErrorCode},
    im::{
        client::{ImClient, SubscribeEstablished, SubscribeOutcome},
        AttrPath,
    },
    transport::exchange::Exchange,
    tlv::FromTLV,
    Matter,
};

pub const MIN_REPORT_INTERVAL_SECONDS: u16 = 1;
pub const MAX_REPORT_INTERVAL_SECONDS: u16 = 3600;
pub const MAX_REPORT_VALUES: usize = 128;

pub struct ReceivedReport {
    pub fabric: u8,
    pub node: u64,
    pub subscription_id: u32,
    pub values: Vec<crate::operations::ResultValue>,
}
pub struct DeviceSubscription {
    pub established: SubscribeEstablished,
    pub unit: Option<&'static str>,
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
        attr: Some(attribute),
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
    let unit = match cluster {
        1037 | 1066 => Some(crate::operations::read_measurement_unit(
            matter, crypto, fabric, node, endpoint, cluster,
        ).await?),
        _ => None,
    };
    let established = establish_attribute(
        matter, crypto, fabric, node, endpoint, cluster, attribute,
    )
    .await?;
    Ok(DeviceSubscription { established, unit })
}

/// Accept and acknowledge one active server-initiated ReportData exchange.
/// Chunked reports are combined under a strict value bound, and the authenticated
/// fabric/node and subscription ID are retained for manager-side correlation.
pub async fn receive_report(matter: &Matter<'_>) -> Result<ReceivedReport, Error> {
    let mut exchange = Exchange::accept(matter).await?;
    let (fabric, node) = exchange
        .peer_identity()?
        .ok_or(ErrorCode::InvalidData)?;
    let mut subscription_id = None;
    let mut values = Vec::new();
    loop {
        let (received_id, more_chunks, suppress_response, mut chunk_values) = {
            let rx = exchange.recv_fetch().await?;
            if rx.meta().proto_opcode != rs_matter::im::OpCode::ReportData as u8 {
                return Err(ErrorCode::InvalidOpcode.into());
            }
            let report = rs_matter::im::ReportDataResp::from_tlv(
                &rs_matter::tlv::TLVElement::new(rx.payload()),
            )?;
            (
                report.subscription_id.ok_or(ErrorCode::InvalidData)?,
                report.more_chunks.unwrap_or(false),
                report.suppress_response.unwrap_or(false),
                crate::operations::report_values(node, &report)?,
            )
        };
        if subscription_id.is_some_and(|id| id != received_id)
            || values.len().saturating_add(chunk_values.len()) > MAX_REPORT_VALUES
            || (more_chunks && suppress_response)
        {
            return Err(ErrorCode::InvalidData.into());
        }
        subscription_id = Some(received_id);
        values.append(&mut chunk_values);
        if !suppress_response {
            exchange
                .send_with(|_, writer| {
                    rs_matter::im::StatusResp::write(
                        writer,
                        rs_matter::im::IMStatusCode::Success,
                    )?;
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
    })
}
