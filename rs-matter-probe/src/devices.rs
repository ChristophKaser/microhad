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
// Seven standard reportable values across at most 32 discovered endpoints.
pub const MAX_DEVICE_SUBSCRIPTIONS: usize = MAX_ENDPOINTS * 7;
const MAX_RETAINED_JOBS: usize = 16;
const MAX_QUEUED_REPORT_VALUES: usize = 512;

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
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct SubscriptionKey {
    node: u64,
    endpoint: u16,
    cluster: u32,
    attribute: u32,
}
struct SubscriptionState {
    key: SubscriptionKey,
    id: Option<u32>,
    next_retry: Instant,
    failures: u8,
    unit: Option<&'static str>,
    last_report: Option<Vec<u8>>,
}
type SubscriptionWork<'a> = Pin<
    Box<
        dyn Future<Output = Result<crate::subscriptions::DeviceSubscription, Error>> + 'a,
    >,
>;
type ReportWork<'a> = Pin<
    Box<dyn Future<Output = Result<crate::subscriptions::ReceivedReport, Error>> + 'a>,
>;
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
    subscription_limit: usize,
    subscriptions: Vec<SubscriptionState>,
    subscription_work: Option<(SubscriptionKey, Instant, SubscriptionWork<'a>)>,
    report_work: Option<ReportWork<'a>>,
    reports: VecDeque<crate::operations::ResultValue>,
}
impl<'a, C: Crypto + 'a> Manager<'a, C> {
    pub fn new(
        matter: &'a Matter<'a>,
        crypto: &'a C,
        fabric: NonZeroU8,
        catalog: Catalog,
    ) -> Result<Self, std::io::Error> {
        Self::new_with_subscription_limit(
            matter,
            crypto,
            fabric,
            catalog,
            MAX_DEVICE_SUBSCRIPTIONS,
        )
    }
    pub fn new_with_subscription_limit(
        matter: &'a Matter<'a>,
        crypto: &'a C,
        fabric: NonZeroU8,
        catalog: Catalog,
        subscription_limit: usize,
    ) -> Result<Self, std::io::Error> {
        if subscription_limit > MAX_DEVICE_SUBSCRIPTIONS {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "Matter subscription limit exceeds manager bound",
            ));
        }
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
            subscription_limit,
            subscriptions: Vec::new(),
            subscription_work: None,
            report_work: None,
            reports: VecDeque::new(),
        })
    }
    pub fn snapshots(&self) -> &[Snapshot] {
        &self.snapshots
    }
    pub fn subscription_id(
        &self,
        node: u64,
        endpoint: u16,
        cluster: u32,
        attribute: u32,
    ) -> Option<u32> {
        self.subscriptions
            .iter()
            .find(|subscription| {
                subscription.key
                    == SubscriptionKey {
                        node,
                        endpoint,
                        cluster,
                        attribute,
                    }
            })
            .and_then(|subscription| subscription.id)
    }
    pub fn pop_report(&mut self) -> Option<crate::operations::ResultValue> {
        self.reports.pop_front()
    }

    /// Deliver queued production Manager reports to connected IPC clients.
    /// Keep the front report queued until at least one client accepts it.
    pub fn publish_reports(
        &mut self,
        ipc: &mut crate::service::IpcServer,
    ) -> std::io::Result<usize> {
        let mut published = 0;
        while let Some(reading) = self.front_report() {
            let event = crate::responses::event(crate::responses::Event::MatterReport(reading))
                .ok_or_else(|| std::io::Error::new(
                    std::io::ErrorKind::InvalidData,
                    "invalid Matter report event",
                ))?;
            if ipc.publish_all(&event)? == 0 {
                break;
            }
            self.pop_report();
            published += 1;
        }
        Ok(published)
    }

    pub fn front_report(&self) -> Option<&crate::operations::ResultValue> {
        self.reports.front()
    }
    #[cfg(feature = "controller-service-test-hooks")]
    /// Exercise snapshot and report correlation in the daemon host fixture.
    pub fn seed_host_test_sensor_reports(&mut self, node: u64) -> Result<(), &'static str> {
        if !self
            .catalog
            .nodes()
            .iter()
            .any(|entry| entry.id == node && entry.phase == Phase::Complete)
        {
            return Err("test report node is not complete in catalog");
        }
        let now = Instant::now();
        let endpoints = vec![Endpoint {
            id: 1,
            servers: vec![6, 8, 91, 1026, 1029, 1037, 1066],
        }];
        if let Some(snapshot) = self.snapshots.iter_mut().find(|entry| entry.node == node) {
            snapshot.endpoints = endpoints;
            snapshot.online = true;
            snapshot.next = now + Duration::from_secs(3600);
        } else {
            self.snapshots.push(Snapshot {
                node,
                endpoints,
                online: true,
                next: now + Duration::from_secs(3600),
                failures: 0,
            });
        }
        self.changed();
        let reports = [
            crate::operations::ResultValue {
                node: crate::control::Id(node),
                endpoint: 1,
                cluster: 1026,
                attribute: 0,
                value: crate::operations::Value::Measurement(crate::operations::MeasurementValue {
                    value: Some(-4.25),
                    unit: "celsius",
                    quality: "measured",
                }),
            },
            crate::operations::ResultValue {
                node: crate::control::Id(node),
                endpoint: 1,
                cluster: 1029,
                attribute: 0,
                value: crate::operations::Value::Measurement(crate::operations::MeasurementValue {
                    value: Some(63.0),
                    unit: "percent",
                    quality: "measured",
                }),
            },
            crate::operations::ResultValue {
                node: crate::control::Id(node),
                endpoint: 1,
                cluster: 91,
                attribute: 0,
                value: crate::operations::Value::AirQuality(crate::operations::AirQualityValue {
                    value: "poor",
                    quality: "measured",
                }),
            },
            crate::operations::ResultValue {
                node: crate::control::Id(node),
                endpoint: 1,
                cluster: 1037,
                attribute: 0,
                value: crate::operations::Value::Measurement(crate::operations::MeasurementValue {
                    value: Some(850.0),
                    unit: "ppm",
                    quality: "measured",
                }),
            },
            crate::operations::ResultValue {
                node: crate::control::Id(node),
                endpoint: 1,
                cluster: 1066,
                attribute: 0,
                value: crate::operations::Value::Measurement(crate::operations::MeasurementValue {
                    value: Some(30.5),
                    unit: "ug/m3",
                    quality: "measured",
                }),
            },
        ];
        for value in reports {
            let subscription_id = value.cluster + 1000;
            self.subscriptions.push(SubscriptionState {
                key: SubscriptionKey {
                    node,
                    endpoint: 1,
                    cluster: value.cluster,
                    attribute: 0,
                },
                id: Some(subscription_id),
                next_retry: now + Duration::from_secs(3600),
                failures: 0,
                unit: None,
                last_report: None,
            });
            self.record_report(crate::subscriptions::ReceivedReport {
                fabric: self.fabric.get(),
                node,
                subscription_id,
                values: vec![value],
            });
        }
        Ok(())
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
        if !ready {
            self.work = None;
            self.subscription_work = None;
            self.report_work = None;
            let changed = self.snapshots.iter().any(|s| s.online);
            for snapshot in &mut self.snapshots {
                snapshot.online = false;
                snapshot.next = now;
            }
            for subscription in &mut self.subscriptions {
                subscription.id = None;
                subscription.next_retry = now;
            }
            if changed {
                self.changed();
            }
            return;
        }
        if self.report_work.is_none() {
            self.report_work = Some(Box::pin(crate::subscriptions::receive_report(self.matter)));
        }
        if let Some(work) = &mut self.report_work {
            if let Poll::Ready(result) = work.as_mut().poll(context) {
                self.report_work = None;
                if let Ok(report) = result {
                    self.record_report(report);
                }
            }
        }
        if commissioning {
            self.work = None;
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
        if let Some((key, deadline, work)) = &mut self.subscription_work {
            let result = if now >= *deadline {
                Some(Err(ErrorCode::RxTimeout.into()))
            } else {
                match work.as_mut().poll(context) {
                    Poll::Ready(result) => Some(result),
                    Poll::Pending => None,
                }
            };
            if let Some(result) = result {
                let key = *key;
                self.subscription_work = None;
                if let Some(subscription) = self
                    .subscriptions
                    .iter_mut()
                    .find(|subscription| subscription.key == key)
                {
                    match result {
                        Ok(established) => {
                            subscription.id = Some(established.established.subscription_id);
                            subscription.failures = 0;
                            subscription.next_retry = now
                                + Duration::from_secs(
                                    u64::from(established.established.max_int).max(1),
                                );
                            subscription.unit = established.unit;
                        }
                        Err(_) => {
                            subscription.id = None;
                            subscription.failures = subscription.failures.saturating_add(1);
                            subscription.next_retry = now + retry_delay(subscription.failures);
                        }
                    }
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
                self.sync_subscriptions(now);
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
            return;
        }
        self.sync_subscriptions(now);
        if let Some(subscription) = self
            .subscriptions
            .iter()
            .filter(|subscription| subscription.next_retry <= now)
            .min_by_key(|subscription| subscription.next_retry)
        {
            let key = subscription.key;
            self.subscription_work = Some((
                key,
                now + Duration::from_secs(30),
                Box::pin(crate::subscriptions::establish_device_attribute(
                    self.matter,
                    self.crypto,
                    self.fabric,
                    key.node,
                    key.endpoint,
                    key.cluster,
                    key.attribute,
                )),
            ));
        }
    }
    fn sync_subscriptions(&mut self, now: Instant) {
        let desired = subscription_keys(&self.snapshots, self.subscription_limit);
        self.subscriptions
            .retain(|subscription| desired.contains(&subscription.key));
        for key in desired {
            if !self
                .subscriptions
                .iter()
                .any(|subscription| subscription.key == key)
            {
                self.subscriptions.push(SubscriptionState {
                    key,
                    id: None,
                    next_retry: now,
                    failures: 0,
                    unit: None,
                    last_report: None,
                });
            }
        }
    }
    fn record_report(&mut self, report: crate::subscriptions::ReceivedReport) {
        if report.fabric != self.fabric.get() {
            return;
        }
        let Some(subscription) = self.subscriptions.iter_mut().find(|subscription| {
            subscription.key.node == report.node
                && subscription.id == Some(report.subscription_id)
        }) else {
            return;
        };
        for mut value in report.values {
            if value.node.0 != subscription.key.node
                || value.endpoint != subscription.key.endpoint
                || value.cluster != subscription.key.cluster
                || value.attribute != subscription.key.attribute
            {
                continue;
            }
            if let crate::operations::Value::RawMeasurement(measured) = &value.value {
                let Some(unit) = subscription.unit else {
                    continue;
                };
                let quality = if measured.is_some() {
                    "measured"
                } else {
                    "unknown"
                };
                value.value = crate::operations::Value::Measurement(
                    crate::operations::MeasurementValue {
                        value: *measured,
                        unit,
                        quality,
                    },
                );
            }
            let Ok(encoded) = serde_json::to_vec(&value) else {
                continue;
            };
            if subscription.last_report.as_ref() == Some(&encoded) {
                continue;
            }
            subscription.last_report = Some(encoded);
            if self.reports.len() == MAX_QUEUED_REPORT_VALUES {
                self.reports.pop_front();
            }
            self.reports.push_back(value);
        }
    }
}
fn subscription_keys(snapshots: &[Snapshot], maximum: usize) -> Vec<SubscriptionKey> {
    snapshots
        .iter()
        .filter(|snapshot| snapshot.online)
        .flat_map(|snapshot| {
            snapshot.endpoints.iter().flat_map(move |endpoint| {
                [6, 8, 91, 1026, 1029, 1037, 1066]
                    .into_iter()
                    .filter(move |cluster| endpoint.servers.contains(cluster))
                    .map(move |cluster| SubscriptionKey {
                        node: snapshot.node,
                        endpoint: endpoint.id,
                        cluster,
                        attribute: 0,
                    })
            })
        })
        .take(maximum)
        .collect()
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
    fn manager_report_queue_drains_only_after_ipc_delivery() {
        use rs_matter::{
            crypto::{default_crypto, CryptoSensitiveRef},
            dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET},
            utils::init::InitMaybeUninit,
        };
        use std::{
            io::Read,
            os::unix::{fs::PermissionsExt, net::UnixStream},
            time::Duration,
        };

        let directory = std::env::temp_dir().join(format!(
            "matter-report-ipc-{}",
            std::process::id()
        ));
        std::fs::create_dir(&directory).unwrap();
        std::fs::set_permissions(&directory, std::fs::Permissions::from_mode(0o700)).unwrap();
        let socket = directory.join("service.sock");
        let mut ipc = crate::service::IpcServer::bind(&socket).unwrap();
        let state_directory = directory.join("state");
        let store = crate::store::PrivateStore::open(&state_directory).unwrap();
        let catalog = Catalog::open(store, 1, 2).unwrap();
        let crypto = crate::entropy::OsEntropy::open().unwrap();
        let crypto = default_crypto(crypto, CryptoSensitiveRef::new(&[9; 32]));
        let mut memory = Box::new(std::mem::MaybeUninit::uninit());
        let matter = memory.init_with(Matter::init(
            &TEST_DEV_DET,
            TEST_DEV_COMM,
            &TEST_DEV_ATT,
            5540,
        ));
        let mut manager =
            Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog).unwrap();
        manager.subscriptions.push(SubscriptionState {
            key: SubscriptionKey {
                node: 2,
                endpoint: 1,
                cluster: 1026,
                attribute: 0,
            },
            id: Some(77),
            next_retry: Instant::now(),
            failures: 0,
            unit: None,
            last_report: None,
        });
        manager.record_report(crate::subscriptions::ReceivedReport {
            fabric: 1,
            node: 2,
            subscription_id: 77,
            values: vec![crate::operations::ResultValue {
                node: crate::control::Id(2),
                endpoint: 1,
                cluster: 1026,
                attribute: 0,
                value: crate::operations::Value::Measurement(
                    crate::operations::MeasurementValue {
                        value: Some(-5.25),
                        unit: "celsius",
                        quality: "measured",
                    },
                ),
            }],
        });
        assert_eq!(manager.publish_reports(&mut ipc).unwrap(), 0);
        assert!(manager.front_report().is_some());

        let mut peer = UnixStream::connect(&socket).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        ipc.poll(|_| None).unwrap();
        assert_eq!(manager.publish_reports(&mut ipc).unwrap(), 1);
        ipc.poll(|_| None).unwrap();
        let mut header = [0; 10];
        peer.read_exact(&mut header).unwrap();
        let length = usize::from(u16::from_le_bytes([header[0], header[1]]));
        let mut bytes = header.to_vec();
        bytes.resize(10 + length, 0);
        peer.read_exact(&mut bytes[10..]).unwrap();
        let mut decoder = crate::ipc::Decoder::default();
        decoder.feed(&bytes).unwrap();
        let event = decoder.pop().unwrap();
        assert_eq!((event.kind, event.opcode, event.id), (2, 12, 0));
        assert_eq!(
            event.payload,
            br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1026,"attribute":0,"value":{"type":"measurement","value":{"value":-5.25,"unit":"celsius","quality":"measured"}}}}"#
        );
        assert!(manager.front_report().is_none());
        drop(manager);
        drop(ipc);
        drop(peer);
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn subscription_candidates_follow_advertised_clusters_and_stay_bounded() {
        let now = Instant::now();
        let snapshot = Snapshot {
            node: 2,
            endpoints: vec![
                Endpoint {
                    id: 1,
                    servers: vec![6, 1026, 0x1d],
                },
                Endpoint {
                    id: 2,
                    servers: vec![8, 91, 1029, 1037, 1066, 0x1d],
                },
            ],
            online: true,
            next: now,
            failures: 0,
        };
        let offline = Snapshot {
            node: 3,
            endpoints: snapshot.endpoints.iter().map(|endpoint| Endpoint {
                id: endpoint.id,
                servers: endpoint.servers.clone(),
            }).collect(),
            online: false,
            next: now,
            failures: 0,
        };
        let keys = subscription_keys(&[snapshot, offline], MAX_DEVICE_SUBSCRIPTIONS);
        assert_eq!(keys.len(), 7);
        assert!(keys.contains(&SubscriptionKey {
            node: 2,
            endpoint: 1,
            cluster: 6,
            attribute: 0,
        }));
        assert!(keys.contains(&SubscriptionKey {
            node: 2,
            endpoint: 2,
            cluster: 1066,
            attribute: 0,
        }));
        assert!(keys.iter().all(|key| key.node == 2));
        let oversized = Snapshot {
            node: 4,
            endpoints: (1..=40)
                .map(|id| Endpoint {
                    id,
                    servers: vec![6, 8, 91, 1026, 1029, 1037, 1066],
                })
                .collect(),
            online: true,
            next: now,
            failures: 0,
        };
        assert_eq!(
            subscription_keys(&[oversized], MAX_DEVICE_SUBSCRIPTIONS).len(),
            MAX_DEVICE_SUBSCRIPTIONS
        );
    }

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
