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
use serde::Serialize;
use std::{
    collections::VecDeque,
    future::Future,
    pin::Pin,
    task::{Context, Poll},
    time::{Duration, Instant},
};

pub const MAX_ENDPOINTS: usize = 32;
pub const MAX_CLUSTERS: usize = 64;
pub const MAX_OPERATION_JOBS: usize = 8;
const MAX_RETAINED_JOBS: usize = 16;

#[derive(Serialize)]
pub struct Device {
    pub node: crate::control::Id,
    pub state: &'static str,
    pub online: bool,
    pub id: Option<String>,
    pub endpoint: Option<u16>,
    pub servers: Vec<u32>,
}
#[derive(Serialize)]
pub struct Page {
    pub revision: crate::control::Id,
    pub devices: Vec<Device>,
    pub next_cursor: Option<u16>,
}
#[derive(Debug, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PageError {
    Malformed,
    StaleRevision,
    Capacity,
}

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
type OperationWork<'a> = Pin<
    Box<
        dyn Future<Output = Result<crate::operations::ResultValue, crate::operations::Failure>>
            + 'a,
    >,
>;
struct QueuedOperation {
    id: u64,
    operation: crate::operations::Operation,
    deadline: Instant,
}
struct ActiveOperation<'a> {
    id: u64,
    deadline: Instant,
    work: OperationWork<'a>,
}
#[derive(Debug, Serialize)]
pub struct JobAccepted {
    pub job: crate::control::Id,
    pub state: &'static str,
}
#[derive(Serialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum JobState {
    Queued {
        job: crate::control::Id,
    },
    Running {
        job: crate::control::Id,
    },
    Complete {
        job: crate::control::Id,
        result: crate::operations::ResultValue,
    },
    Failed {
        job: crate::control::Id,
        error: crate::operations::Failure,
    },
}
struct JobRecord {
    id: u64,
    state: JobRecordState,
}
enum JobRecordState {
    Queued,
    Running,
    Complete(crate::operations::ResultValue),
    Failed(crate::operations::Failure),
}

/// One operational job at a time, yielding to commissioning and radio recovery.
pub struct Manager<'a, C: Crypto> {
    matter: &'a Matter<'a>,
    crypto: &'a C,
    fabric: NonZeroU8,
    catalog: Catalog,
    snapshots: Vec<Snapshot>,
    work: Option<(u64, Instant, Work<'a>)>,
    revision: u64,
    seen: Vec<crate::catalog::Node>,
    next_job: u64,
    queued: VecDeque<QueuedOperation>,
    active: Option<ActiveOperation<'a>>,
    jobs: VecDeque<JobRecord>,
}
impl<'a, C: Crypto + 'a> Manager<'a, C> {
    pub fn new(
        matter: &'a Matter<'a>,
        crypto: &'a C,
        fabric: NonZeroU8,
        catalog: Catalog,
    ) -> Result<Self, std::io::Error> {
        use rs_matter::crypto::TryRng;
        let mut entropy = crate::entropy::OsEntropy::open()?;
        let revision = entropy.try_next_u64().unwrap().max(1);
        let next_job = entropy.try_next_u64().unwrap().max(1);
        Ok(Self {
            matter,
            crypto,
            fabric,
            catalog,
            snapshots: Vec::new(),
            work: None,
            revision,
            seen: Vec::new(),
            next_job,
            queued: VecDeque::new(),
            active: None,
            jobs: VecDeque::new(),
        })
    }
    pub fn snapshots(&self) -> &[Snapshot] {
        &self.snapshots
    }
    pub fn operations_busy(&self) -> bool {
        self.active.is_some() || !self.queued.is_empty()
    }
    pub fn submit(
        &mut self,
        request: crate::control::Request,
        ready: bool,
        commissioning: bool,
        now: Instant,
    ) -> Result<JobAccepted, crate::operations::Failure> {
        use crate::operations::Failure;
        if !ready {
            return Err(Failure::NotReady);
        }
        if commissioning {
            return Err(Failure::Busy);
        }
        if self.queued.len() + usize::from(self.active.is_some()) >= MAX_OPERATION_JOBS {
            return Err(Failure::Capacity);
        }
        let operation = crate::operations::Operation::decode(request)?;
        if !self
            .catalog
            .nodes()
            .iter()
            .any(|node| node.id == operation.node && node.phase == Phase::Complete)
        {
            return Err(Failure::NotFound);
        }
        operation.authorize(&self.snapshots)?;
        if self.next_job == 0 {
            return Err(Failure::Capacity);
        }
        let id = self.next_job;
        self.next_job = self.next_job.checked_add(1).unwrap_or(0);
        self.queued.push_back(QueuedOperation {
            id,
            operation,
            deadline: now + Duration::from_secs(30),
        });
        self.jobs.push_back(JobRecord {
            id,
            state: JobRecordState::Queued,
        });
        Ok(JobAccepted {
            job: crate::control::Id(id),
            state: "queued",
        })
    }
    pub fn job(&self, id: crate::control::Id) -> Result<JobState, crate::operations::Failure> {
        let record = self
            .jobs
            .iter()
            .find(|record| record.id == id.0)
            .ok_or(crate::operations::Failure::NotFound)?;
        Ok(match &record.state {
            JobRecordState::Queued => JobState::Queued { job: id },
            JobRecordState::Running => JobState::Running { job: id },
            JobRecordState::Complete(result) => JobState::Complete {
                job: id,
                result: result.clone(),
            },
            JobRecordState::Failed(error) => JobState::Failed {
                job: id,
                error: *error,
            },
        })
    }
    fn record_state(&mut self, id: u64, state: JobRecordState) {
        if let Some(record) = self.jobs.iter_mut().find(|record| record.id == id) {
            record.state = state;
        }
        while self.jobs.len() > MAX_RETAINED_JOBS {
            if self.jobs.front().is_some_and(|record| {
                matches!(
                    record.state,
                    JobRecordState::Queued | JobRecordState::Running
                )
            }) {
                break;
            }
            self.jobs.pop_front();
        }
    }
    pub fn prepare(
        &self,
        request: crate::control::Request,
    ) -> Result<
        impl Future<Output = Result<crate::operations::ResultValue, crate::operations::Failure>> + 'a,
        crate::operations::Failure,
    > {
        let operation = crate::operations::Operation::decode(request)?;
        if !self
            .catalog
            .nodes()
            .iter()
            .any(|node| node.id == operation.node && node.phase == Phase::Complete)
        {
            return Err(crate::operations::Failure::NotFound);
        }
        operation.authorize(&self.snapshots)?;
        Ok(operation.run(self.matter, self.crypto, self.fabric))
    }
    fn changed(&mut self) {
        if self.revision != 0 {
            self.revision = self.revision.checked_add(1).unwrap_or(0);
        }
    }
    pub fn page(
        &mut self,
        cursor: u16,
        revision: Option<crate::control::Id>,
    ) -> Result<Page, PageError> {
        let nodes = self.catalog.nodes();
        if nodes != self.seen {
            self.seen = nodes.clone();
            self.changed();
        }
        if self.revision == 0 {
            return Err(PageError::Capacity);
        }
        if (cursor != 0 && revision.is_none()) || revision.is_some_and(|r| r.0 != self.revision) {
            return Err(PageError::StaleRevision);
        }
        let mut offset = 0usize;
        let mut device = None;
        for node in nodes {
            let snapshot = self.snapshots.iter().find(|s| s.node == node.id);
            let endpoints = snapshot.map(|s| s.endpoints.as_slice()).unwrap_or(&[]);
            let state = match node.phase {
                Phase::Complete => "complete",
                Phase::RecoveryRequired => "recovery_required",
                _ => "commissioning",
            };
            for index in 0..endpoints.len().max(1) {
                if offset == usize::from(cursor) {
                    let endpoint = endpoints.get(index);
                    device = Some(Device {
                        node: crate::control::Id(node.id),
                        state,
                        online: node.phase == Phase::Complete && snapshot.is_some_and(|s| s.online),
                        id: endpoint.map(|e| self.catalog.endpoint_id(node.id, e.id).unwrap()),
                        endpoint: endpoint.map(|e| e.id),
                        servers: endpoint.map(|e| e.servers.clone()).unwrap_or_default(),
                    });
                }
                offset += 1;
            }
        }
        if usize::from(cursor) > offset || (usize::from(cursor) == offset && cursor != 0) {
            return Err(PageError::Malformed);
        }
        Ok(Page {
            revision: crate::control::Id(self.revision),
            devices: device.into_iter().collect(),
            next_cursor: (usize::from(cursor) + 1 < offset).then_some(cursor + 1),
        })
    }
    pub fn poll(
        &mut self,
        ready: bool,
        commissioning: bool,
        now: Instant,
        context: &mut Context<'_>,
    ) {
        if !ready {
            if let Some(active) = self.active.take() {
                self.record_state(
                    active.id,
                    JobRecordState::Failed(crate::operations::Failure::NotReady),
                );
            }
            while let Some(queued) = self.queued.pop_front() {
                self.record_state(
                    queued.id,
                    JobRecordState::Failed(crate::operations::Failure::NotReady),
                );
            }
        }
        if !ready || commissioning {
            self.work = None;
            if !ready {
                let changed = self.snapshots.iter().any(|s| s.online);
                for snapshot in &mut self.snapshots {
                    snapshot.online = false;
                    snapshot.next = now;
                }
                if changed {
                    self.changed();
                }
            }
            return;
        }
        if let Some(active) = &mut self.active {
            let completed = if now >= active.deadline {
                Some(Err(crate::operations::Failure::Timeout))
            } else {
                match active.work.as_mut().poll(context) {
                    Poll::Ready(result) => Some(result),
                    Poll::Pending => None,
                }
            };
            if let Some(result) = completed {
                let id = active.id;
                self.active = None;
                self.record_state(
                    id,
                    match result {
                        Ok(value) => JobRecordState::Complete(value),
                        Err(error) => JobRecordState::Failed(error),
                    },
                );
            }
            return;
        }
        if let Some(queued) = self.queued.pop_front() {
            let id = queued.id;
            if now >= queued.deadline {
                self.record_state(
                    id,
                    JobRecordState::Failed(crate::operations::Failure::Timeout),
                );
                return;
            }
            let authorization = self
                .catalog
                .nodes()
                .iter()
                .any(|node| node.id == queued.operation.node && node.phase == Phase::Complete)
                .then(|| queued.operation.authorize(&self.snapshots))
                .unwrap_or(Err(crate::operations::Failure::NotFound));
            if let Err(error) = authorization {
                self.record_state(id, JobRecordState::Failed(error));
                return;
            }
            let remaining = queued.deadline.saturating_duration_since(now);
            let timeout = embassy_time::Duration::from_millis(
                remaining.as_millis().min(u64::MAX as u128) as u64,
            );
            let work = queued
                .operation
                .run_for(self.matter, self.crypto, self.fabric, timeout);
            self.active = Some(ActiveOperation {
                id,
                deadline: queued.deadline,
                work: Box::pin(work),
            });
            self.record_state(id, JobRecordState::Running);
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
                let visible_changed = match &result {
                    Ok(endpoints) => !snapshot.online || snapshot.endpoints != *endpoints,
                    Err(_) => snapshot.online,
                };
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
                if visible_changed {
                    self.changed();
                }
            }
            return;
        }
        if self.operations_busy() {
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
    fn operational_jobs_are_bounded_status_addressable_and_fail_on_radio_loss() {
        use crate::{control::Request, entropy::OsEntropy, store::PrivateStore};
        use rs_matter::{
            crypto::{default_crypto, CryptoSensitiveRef},
            dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET},
            utils::init::InitMaybeUninit,
        };
        let directory = std::env::temp_dir().join(format!("matter-jobs-{}", std::process::id()));
        let store = PrivateStore::open(&directory).unwrap();
        let catalog = Catalog::open(store, 1, 5).unwrap();
        let reservation = catalog.reserve(2).unwrap();
        reservation.fabric_installed().unwrap();
        reservation.thread_provisioned().unwrap();
        reservation.complete().unwrap();
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
        let mut manager =
            Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog).unwrap();
        manager.snapshots.push(Snapshot {
            node: 2,
            endpoints: vec![Endpoint {
                id: 1,
                servers: vec![6],
            }],
            online: true,
            next: Instant::now() + Duration::from_secs(300),
            failures: 0,
        });
        let request = || {
            Request::decode(
                11,
                br#"{"schema":1,"command":"toggle","node":"0000000000000002","endpoint":1}"#,
            )
            .unwrap()
        };
        let now = Instant::now();
        let first = manager.submit(request(), true, false, now).unwrap();
        assert_eq!(first.state, "queued");
        assert!(matches!(
            manager.job(first.job).unwrap(),
            JobState::Queued { .. }
        ));
        assert!(manager.operations_busy());
        assert_eq!(
            manager.submit(request(), false, false, now).unwrap_err(),
            crate::operations::Failure::NotReady
        );
        for _ in 1..MAX_OPERATION_JOBS {
            manager.submit(request(), true, false, now).unwrap();
        }
        assert_eq!(
            manager.submit(request(), true, false, now).unwrap_err(),
            crate::operations::Failure::Capacity
        );
        let mut context = Context::from_waker(std::task::Waker::noop());
        manager.poll(false, false, now, &mut context);
        assert!(!manager.operations_busy());
        assert!(matches!(
            manager.job(first.job).unwrap(),
            JobState::Failed {
                error: crate::operations::Failure::NotReady,
                ..
            }
        ));
        assert_eq!(
            manager.submit(request(), true, false, now).unwrap_err(),
            crate::operations::Failure::NotReady
        );
        drop(manager);
        std::fs::remove_dir_all(directory).unwrap();
    }
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
        let mut manager =
            Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog).unwrap();
        let now = Instant::now();
        let mut context = Context::from_waker(std::task::Waker::noop());
        let first = manager.page(0, None).unwrap();
        assert_eq!(first.devices[0].node.0, 2);
        assert_eq!(first.next_cursor, Some(1));
        assert_eq!(first.devices[0].endpoint, None);
        assert!(manager.page(1, None).is_err());
        assert_eq!(
            manager.page(2, Some(first.revision)).unwrap().devices[0].state,
            "recovery_required"
        );
        assert!(manager.page(3, Some(first.revision)).is_err());
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
        assert!(manager.page(1, Some(first.revision)).is_err());
        let refreshed = manager.page(0, None).unwrap();
        let second = manager.page(1, Some(refreshed.revision)).unwrap();
        assert_eq!(second.devices[0].endpoint, Some(0));
        assert!(second.devices[0].online);
        assert_eq!(
            second.devices[0].id.as_deref(),
            Some("matter:0000000000000001:0000000000000003:0000")
        );
        assert!(
            crate::responses::reply::<_, PageError>(Ok(second))
                .unwrap()
                .len()
                < crate::ipc::MAX_PAYLOAD
        );
        manager.poll(false, false, now + Duration::from_secs(31), &mut context);
        assert!(manager.snapshots.iter().all(|s| !s.online));
        manager.poll(true, false, now + Duration::from_secs(31), &mut context);
        assert!(manager.work.is_some());
        manager.poll(true, true, now + Duration::from_secs(31), &mut context);
        assert!(manager.work.is_none());
        manager.snapshots[1].endpoints = (0..MAX_ENDPOINTS)
            .map(|id| Endpoint {
                id: id as u16,
                servers: (u32::MAX - MAX_CLUSTERS as u32 + 1..=u32::MAX).collect(),
            })
            .collect();
        manager.changed();
        let revision = manager.page(0, None).unwrap().revision;
        for cursor in 1..=MAX_ENDPOINTS as u16 {
            let page = manager.page(cursor, Some(revision)).unwrap();
            assert_eq!(page.devices[0].servers.len(), MAX_CLUSTERS);
            assert!(
                crate::responses::reply::<_, PageError>(Ok(page))
                    .unwrap()
                    .len()
                    <= crate::ipc::MAX_PAYLOAD
            );
        }
        assert!(manager.page(u16::MAX, Some(revision)).is_err());
        manager.revision = u64::MAX;
        manager.changed();
        manager.changed();
        assert!(matches!(manager.page(0, None), Err(PageError::Capacity)));
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
