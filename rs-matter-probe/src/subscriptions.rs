//! Bounded Interaction Model subscription establishment for operational endpoints.
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    error::Error,
    im::{
        client::{ImClient, SubscribeEstablished, SubscribeOutcome},
        AttrPath,
    },
    transport::exchange::Exchange,
    Matter,
};

pub const MIN_REPORT_INTERVAL_SECONDS: u16 = 1;
pub const MAX_REPORT_INTERVAL_SECONDS: u16 = 3600;

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
