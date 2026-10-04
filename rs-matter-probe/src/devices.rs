//! Bounded operational endpoint discovery. No optimistic device capabilities.
use crate::catalog::{Catalog, Phase};
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    dm::clusters::desc::DescriptorClient,
    error::{Error, ErrorCode},
    transport::exchange::Exchange,
    Matter,
};
use std::{
    future::Future,
    pin::Pin,
    task::{Context, Poll},
    time::{Duration, Instant},
};

pub const MAX_ENDPOINTS: usize = 32;
pub const MAX_CLUSTERS: usize = 64;

#[derive(Debug, PartialEq, Eq)]
pub struct Endpoint {
    pub id: u16,
    pub servers: Vec<u32>,
}

pub struct Snapshot {
    pub node: u64,
    pub endpoints: Vec<Endpoint>,
    pub online: bool,
    next: Instant,
    failures: u8,
}
type Work<'a> = Pin<Box<dyn Future<Output = Result<Vec<Endpoint>, Error>> + 'a>>;

/// One operational job at a time, yielding to commissioning and radio recovery.
pub struct Manager<'a, C: Crypto> {
    matter: &'a Matter<'a>,
    crypto: &'a C,
    fabric: NonZeroU8,
    catalog: Catalog,
    snapshots: Vec<Snapshot>,
    work: Option<(u64, Instant, Work<'a>)>,
}
impl<'a, C: Crypto + 'a> Manager<'a, C> {
    pub fn new(matter: &'a Matter<'a>, crypto: &'a C, fabric: NonZeroU8, catalog: Catalog) -> Self {
        Self {
            matter,
            crypto,
            fabric,
            catalog,
            snapshots: Vec::new(),
            work: None,
        }
    }
    pub fn snapshots(&self) -> &[Snapshot] {
        &self.snapshots
    }
    pub fn poll(
        &mut self,
        ready: bool,
        commissioning: bool,
        now: Instant,
        context: &mut Context<'_>,
    ) {
        if !ready || commissioning {
            self.work = None;
            if !ready {
                for snapshot in &mut self.snapshots {
                    snapshot.online = false;
                    snapshot.next = now;
                }
            }
            return;
        }
        if let Some((node, deadline, work)) = &mut self.work {
            let result = if now >= *deadline {
                Some(Err(ErrorCode::RxTimeout.into()))
            } else {
                match work.as_mut().poll(context) {
                    Poll::Ready(result) => Some(result),
                    Poll::Pending => None,
                }
            };
            if let Some(result) = result {
                let node = *node;
                self.work = None;
                let snapshot = self.snapshots.iter_mut().find(|s| s.node == node).unwrap();
                match result {
                    Ok(endpoints) => {
                        snapshot.endpoints = endpoints;
                        snapshot.online = true;
                        snapshot.failures = 0;
                        snapshot.next = now + Duration::from_secs(300);
                    }
                    Err(_) => {
                        snapshot.online = false;
                        snapshot.failures = snapshot.failures.saturating_add(1);
                        snapshot.next = now + retry_delay(snapshot.failures);
                    }
                }
            }
            return;
        }
        for node in self
            .catalog
            .nodes()
            .into_iter()
            .filter(|n| n.phase == Phase::Complete)
        {
            if !self.snapshots.iter().any(|s| s.node == node.id) {
                self.snapshots.push(Snapshot {
                    node: node.id,
                    endpoints: Vec::new(),
                    online: false,
                    next: now,
                    failures: 0,
                });
            }
        }
        if let Some(snapshot) = self
            .snapshots
            .iter()
            .filter(|s| s.next <= now)
            .min_by_key(|s| s.next)
        {
            self.work = Some((
                snapshot.node,
                now + Duration::from_secs(30),
                Box::pin(discover(
                    self.matter,
                    self.crypto,
                    self.fabric,
                    snapshot.node,
                )),
            ));
        }
    }
}
fn retry_delay(failures: u8) -> Duration {
    Duration::from_secs(5u64 << failures.saturating_sub(1).min(6))
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
    fn scheduler_deadlines_preemption_fairness_and_no_flash_writes() {
        use crate::{entropy::OsEntropy, store::PrivateStore};
        use rs_matter::{
            crypto::{default_crypto, CryptoSensitiveRef},
            dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET},
            utils::init::InitMaybeUninit,
        };
        let directory = std::env::temp_dir().join(format!("matter-devices-{}", std::process::id()));
        let store = PrivateStore::open(&directory).unwrap();
        let catalog = Catalog::open(store, 1, 5).unwrap();
        for node in [2, 3] {
            let reservation = catalog.reserve(node).unwrap();
            reservation.fabric_installed().unwrap();
            reservation.thread_provisioned().unwrap();
            reservation.complete().unwrap();
        }
        drop(catalog.reserve(4).unwrap());
        let before = std::fs::read(directory.join("credentials.bin")).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[9; 32]),
        );
        let mut memory = Box::new(std::mem::MaybeUninit::uninit());
        let matter = memory.init_with(Matter::init(
            &TEST_DEV_DET,
            TEST_DEV_COMM,
            &TEST_DEV_ATT,
            5540,
        ));
        let mut manager = Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog);
        let now = Instant::now();
        let mut context = Context::from_waker(std::task::Waker::noop());
        manager.poll(true, true, now, &mut context);
        assert!(manager.work.is_none());
        manager.poll(true, false, now, &mut context);
        assert_eq!(manager.snapshots.len(), 2); // incomplete node is not silently trusted
        assert_eq!(manager.work.as_ref().unwrap().0, 2);
        manager.work = Some((
            2,
            now + Duration::from_secs(30),
            Box::pin(std::future::pending()),
        ));
        manager.poll(true, false, now + Duration::from_secs(30), &mut context);
        assert!(manager.work.is_none());
        assert!(!manager.snapshots[0].online);
        assert_eq!(manager.snapshots[0].next, now + Duration::from_secs(35));
        manager.poll(true, false, now + Duration::from_secs(30), &mut context);
        assert_eq!(manager.work.as_ref().unwrap().0, 3);
        manager.work = Some((
            3,
            now + Duration::from_secs(60),
            Box::pin(async {
                Ok(vec![Endpoint {
                    id: 0,
                    servers: vec![0x1d],
                }])
            }),
        ));
        manager.poll(true, false, now + Duration::from_secs(30), &mut context);
        assert!(manager.snapshots[1].online);
        manager.poll(false, false, now + Duration::from_secs(31), &mut context);
        assert!(manager.snapshots.iter().all(|s| !s.online));
        manager.poll(true, false, now + Duration::from_secs(31), &mut context);
        assert!(manager.work.is_some());
        manager.poll(true, true, now + Duration::from_secs(31), &mut context);
        assert!(manager.work.is_none());
        assert_eq!(
            std::fs::read(directory.join("credentials.bin")).unwrap(),
            before
        );
        drop(manager);
        std::fs::remove_dir_all(directory).unwrap();
    }
    #[test]
    fn retries_are_exponential_and_capped() {
        assert_eq!(retry_delay(1), Duration::from_secs(5));
        assert_eq!(retry_delay(2), Duration::from_secs(10));
        assert_eq!(retry_delay(7), Duration::from_secs(320));
        assert_eq!(retry_delay(u8::MAX), Duration::from_secs(320));
    }
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
