//! Bounded operational endpoint discovery. No optimistic device capabilities.
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    dm::clusters::desc::DescriptorClient,
    error::{Error, ErrorCode},
    transport::exchange::Exchange,
    Matter,
};

pub const MAX_ENDPOINTS: usize = 32;
pub const MAX_CLUSTERS: usize = 64;

#[derive(Debug, PartialEq, Eq)]
pub struct Endpoint {
    pub id: u16,
    pub servers: Vec<u32>,
}

fn bounded<T: Copy + Ord>(
    values: impl Iterator<Item = Result<T, Error>>,
    maximum: usize,
) -> Result<Vec<T>, Error> {
    let mut result = Vec::new();
    for value in values {
        if result.len() == maximum {
            return Err(ErrorCode::NoSpace.into());
        }
        let value = value?;
        if result.contains(&value) {
            return Err(ErrorCode::InvalidData.into());
        }
        result.push(value);
    }
    result.sort_unstable();
    Ok(result)
}

/// Root PartsList defines the node's endpoints, including bridged endpoints.
/// Each read gets its own exchange; return a snapshot only after every read succeeds.
pub async fn discover<C: Crypto>(
    matter: &Matter<'_>,
    crypto: C,
    fabric: NonZeroU8,
    node: u64,
) -> Result<Vec<Endpoint>, Error> {
    let exchange = Exchange::initiate(matter, &crypto, fabric, node).await?;
    let mut ids = exchange
        .descriptor()
        .parts_list_read_with(0, |value| bounded(value?.iter(), MAX_ENDPOINTS - 1))
        .await??;
    if ids.contains(&0) || ids.contains(&u16::MAX) {
        return Err(ErrorCode::InvalidData.into());
    }
    ids.insert(0, 0);
    let mut endpoints = Vec::with_capacity(ids.len());
    for id in ids {
        let exchange = Exchange::initiate(matter, &crypto, fabric, node).await?;
        let servers = exchange
            .descriptor()
            .server_list_read_with(id, |value| bounded(value?.iter(), MAX_CLUSTERS))
            .await??;
        if !servers.contains(&0x1d) {
            return Err(ErrorCode::InvalidData.into());
        }
        endpoints.push(Endpoint { id, servers });
    }
    Ok(endpoints)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn peer_lists_are_bounded_unique_and_canonical() {
        assert_eq!(
            bounded([Ok(9u16), Ok(1)].into_iter(), 2).unwrap(),
            vec![1, 9]
        );
        assert!(bounded([Ok(1u16), Ok(1)].into_iter(), 2).is_err());
        assert!(bounded([Ok(1u16), Ok(2)].into_iter(), 1).is_err());
        assert!(bounded(
            [Err::<u16, _>(ErrorCode::InvalidData.into())].into_iter(),
            2
        )
        .is_err());
        assert!(bounded(std::iter::empty::<Result<u16, Error>>(), 0)
            .unwrap()
            .is_empty());
    }
}
