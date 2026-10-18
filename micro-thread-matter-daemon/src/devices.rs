//! Bounded operational endpoint discovery. No optimistic device capabilities.
use crate::catalog::{Catalog, Phase};
use core::num::NonZeroU8;
use rs_matter::{
    crypto::Crypto,
    dm::clusters::{basic_info::BasicInformationClient, desc::DescriptorClient, icd_mgmt::IcdManagementClient},
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
// Reserve up to 12 reportable values per endpoint; the current candidate list
// uses eight and leaves four slots for additional supported report clusters.
pub const MAX_DEVICE_SUBSCRIPTIONS: usize = MAX_ENDPOINTS * 12;
const MAX_RETAINED_JOBS: usize = 16;
const MAX_QUEUED_REPORT_VALUES: usize = 512;
const SWITCH_EVENT_ATTRIBUTE: u32 = u32::MAX;
const ICD_MANAGEMENT_CLUSTER: u32 = 0x0046;
const MAX_ICD_CANDIDATES: usize = crate::icd::MAX_ICD_CLIENTS * MAX_ENDPOINTS;
// Allow a short transport/scheduler delay after the publisher's negotiated
// maximum interval before treating a healthy subscription as lost.
const SUBSCRIPTION_REPORT_GRACE: Duration = Duration::from_secs(10);

#[derive(Serialize)]
pub struct Device {
    pub node: crate::control::Id,
    pub state: &'static str,
    pub online: bool,
    pub id: Option<String>,
    pub endpoint: Option<u16>,
    pub servers: Vec<u32>,
    pub clients: Vec<u32>,
    pub device_types: Vec<u32>,
    pub manufacturer: String,
    pub model: String,
    pub rssi_dbm: Option<i8>,
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
    pub clients: Vec<u32>,
    pub device_types: Vec<u32>,
}

/// Bounded inventory records. Identity records precede endpoint records; a
/// node commit makes its verified endpoint set atomic at the receiver.
#[derive(Serialize)]
pub struct InventoryRecord {
    pub kind: &'static str,
    pub device: Device,
    pub endpoint_count: usize,
}
#[derive(Serialize)]
pub struct InventoryNodes { pub nodes: Vec<InventoryNode> }
#[derive(Serialize)]
pub struct InventoryNode { pub node: crate::control::Id }
struct InventoryPeer {
    peer: u64,
    revision: u64,
    index: usize,
    next: Instant,
}

pub struct Snapshot {
    pub node: u64,
    pub endpoints: Vec<Endpoint>,
    pub manufacturer: String,
    pub model: String,
    pub rssi_dbm: Option<i8>,
    pub online: bool,
    next: Instant,
    failures: u8,
}
struct Discovery {
    endpoints: Vec<Endpoint>,
    manufacturer: String,
    model: String,
}
type Work<'a> = Pin<Box<dyn Future<Output = Result<Discovery, Error>> + 'a>>;
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
    node: u64,
    deleting: bool,
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
    max_interval: u16,
    failures: u8,
    unit: Option<&'static str>,
    last_report: Option<Vec<u8>>,
}
type SubscriptionWork<'a> =
    Pin<Box<dyn Future<Output = Result<crate::subscriptions::DeviceSubscription, Error>> + 'a>>;
type ReportWork<'a> =
    Pin<Box<dyn Future<Output = Result<crate::subscriptions::IncomingMessage, Error>> + 'a>>;
type IcdWork<'a> = Pin<
    Box<
        dyn Future<
                Output = (
                    crate::icd::ClientRegistry,
                    crate::entropy::OsEntropy,
                    Result<(), Error>,
                ),
            > + 'a,
    >,
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
    compressed_fabric: u64,
    catalog: Catalog,
    address_refresh: crate::discovery::RefreshRequests,
    signals: crate::discovery::SignalBook,
    icd_registry: Option<crate::icd::ClientRegistry>,
    local_node: u64,
    icd_entropy: Option<crate::entropy::OsEntropy>,
    icd_work: Option<(u64, u16, Instant, IcdWork<'a>)>,
    registered_nodes: Vec<u64>,
    icd_checked: Vec<(u64, u16)>,
    icd_retry_after: Vec<((u64, u16), Instant)>,
    awake_until: Vec<(u64, Instant)>,
    snapshots: Vec<Snapshot>,
    work: Option<(u64, Instant, Work<'a>)>,
    revision: u64,
    inventory_peers: Vec<InventoryPeer>,
    inventory_bootstrap_until: Instant,
    seen: Vec<crate::catalog::Node>,
    next_job: u64,
    queued: VecDeque<QueuedOperation>,
    active: Option<ActiveOperation<'a>>,
    jobs: VecDeque<JobRecord>,
    subscription_limit: usize,
    subscriptions: Vec<SubscriptionState>,
    report_subscriptions: crate::subscriptions::ReportSubscriptions,
    subscription_work: Option<(SubscriptionKey, Instant, SubscriptionWork<'a>)>,
    report_work: Option<ReportWork<'a>>,
    #[cfg(feature = "controller-service-test-hooks")]
    host_test_reports: VecDeque<crate::subscriptions::ReceivedReport>,
    reports: VecDeque<crate::operations::ResultValue>,
    button_events: VecDeque<crate::subscriptions::MatterButtonEvent>,
}
impl<'a, C: Crypto + 'a> Manager<'a, C> {
    pub fn new(
        matter: &'a Matter<'a>,
        crypto: &'a C,
        fabric: NonZeroU8,
        catalog: Catalog,
    ) -> Result<Self, std::io::Error> {
        Self::new_with_subscription_limit(matter, crypto, fabric, catalog, MAX_DEVICE_SUBSCRIPTIONS)
    }
    pub fn new_with_subscription_limit(
        matter: &'a Matter<'a>,
        crypto: &'a C,
        fabric: NonZeroU8,
        catalog: Catalog,
        subscription_limit: usize,
    ) -> Result<Self, std::io::Error> {
        Self::new_with_address_refresh(
            matter,
            crypto,
            fabric,
            catalog,
            subscription_limit,
            0,
            crate::discovery::RefreshRequests::default(),
        )
    }
    pub fn new_with_address_refresh(
        matter: &'a Matter<'a>,
        crypto: &'a C,
        fabric: NonZeroU8,
        catalog: Catalog,
        subscription_limit: usize,
        compressed_fabric: u64,
        address_refresh: crate::discovery::RefreshRequests,
    ) -> Result<Self, std::io::Error> {
        Self::new_with_address_refresh_and_signals(matter, crypto, fabric, catalog, subscription_limit,
            compressed_fabric, address_refresh, crate::discovery::SignalBook::default())
    }
    pub fn new_with_address_refresh_and_signals(
        matter: &'a Matter<'a>, crypto: &'a C, fabric: NonZeroU8, catalog: Catalog,
        subscription_limit: usize, compressed_fabric: u64, address_refresh: crate::discovery::RefreshRequests,
        signals: crate::discovery::SignalBook,
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
        let local_node = matter
            .with_state(|state| Ok::<_, Error>(state.fabrics.fabric(fabric)?.node_id()))
            .unwrap_or(0);
        let icd_registry = crate::icd::ClientRegistry::open(catalog.store())
            .map_err(|_| std::io::Error::other("ICD registry unavailable"))?;
        let registered_nodes = icd_registry.registered_nodes(fabric);
        Ok(Self {
            matter,
            crypto,
            fabric,
            compressed_fabric,
            catalog,
            address_refresh,
            signals,
            icd_registry: Some(icd_registry),
            local_node,
            icd_entropy: Some(crate::entropy::OsEntropy::open()?),
            icd_work: None,
            registered_nodes,
            icd_checked: Vec::new(),
            icd_retry_after: Vec::new(),
            awake_until: Vec::new(),
            snapshots: Vec::new(),
            work: None,
            revision,
            inventory_peers: Vec::new(),
            inventory_bootstrap_until: Instant::now(),
            seen: Vec::new(),
            next_job,
            queued: VecDeque::new(),
            active: None,
            jobs: VecDeque::new(),
            subscription_limit,
            subscriptions: Vec::new(),
            report_subscriptions: crate::subscriptions::ReportSubscriptions::default(),
            subscription_work: None,
            report_work: None,
            #[cfg(feature = "controller-service-test-hooks")]
            host_test_reports: VecDeque::new(),
            reports: VecDeque::new(),
            button_events: VecDeque::new(),
        })
    }
    pub fn defer_interviews(&mut self, duration: Duration) {
        self.inventory_bootstrap_until = Instant::now() + duration;
    }
    fn inventory_pending(&self) -> bool {
        let records = self.catalog.nodes().len() * 2
            + self.snapshots.iter().map(|snapshot| snapshot.endpoints.len()).sum::<usize>();
        self.inventory_peers.iter().any(|peer| peer.revision != self.revision || peer.index < records)
    }
    pub fn subscribe_inventory(&mut self, peer: u64) {
        if !self.inventory_peers.iter().any(|entry| entry.peer == peer) {
            self.inventory_peers.push(InventoryPeer {
                peer, revision: 0, index: 0, next: Instant::now(),
            });
        }
    }
    pub fn publish_inventory(&mut self, ipc: &mut crate::service::IpcServer) -> std::io::Result<()> {
        if self.catalog.legacy_pending() { return Ok(()); }
        let nodes = self.catalog.nodes();
        if nodes != self.seen {
            self.seen = nodes.clone();
            self.changed();
        }
        self.inventory_peers.retain(|entry| ipc.has_peer(entry.peer));
        let now = Instant::now();
        // No retained full-catalog frames: generate at most one bounded frame
        // per peer per tick, and respect output backpressure without dropping it.
        for index in 0..self.inventory_peers.len() {
            if now < self.inventory_peers[index].next { continue; }
            if self.inventory_peers[index].revision != self.revision {
                let manifest = InventoryNodes { nodes: nodes.iter().map(|node| InventoryNode { node: crate::control::Id(node.id) }).collect() };
                let event = crate::responses::event(crate::responses::Event::MatterInventoryNodes(&manifest))
                    .ok_or_else(|| std::io::Error::other("inventory manifest exceeds IPC bound"))?;
                if !ipc.publish_inventory(self.inventory_peers[index].peer, &event)? { continue; }
                self.inventory_peers[index].revision = self.revision;
                self.inventory_peers[index].index = 0;
            }
            let offset = self.inventory_peers[index].index;
            let mut record = None;
            let mut cursor = 0;
            for identity_only in [true, false] {
                for node in &nodes {
                    let snapshot = self.snapshots.iter().find(|entry| entry.node == node.id);
                    let endpoints = snapshot.map(|entry| entry.endpoints.as_slice()).unwrap_or(&[]);
                    let count = if identity_only { 1 } else { endpoints.len() + 1 };
                    if offset >= cursor && offset < cursor + count {
                        let endpoint = if identity_only { None } else { endpoints.get(offset - cursor) };
                        record = Some(InventoryRecord {
                            kind: if identity_only { "identity" } else if endpoint.is_some() { "endpoint" } else { "commit" },
                            endpoint_count: endpoints.len(),
                            device: Device {
                                node: crate::control::Id(node.id),
                                state: match node.phase { Phase::Complete => "complete", Phase::RecoveryRequired => "recovery_required", _ => "commissioning" },
                                online: node.phase == Phase::Complete && snapshot.is_some_and(|entry| entry.online),
                                id: Some(endpoint.map(|entry| self.catalog.endpoint_id(node.id, entry.id).unwrap())
                                    .unwrap_or_else(|| self.catalog.endpoint_id(node.id, 0).unwrap()[..40].to_owned())),
                                endpoint: endpoint.map(|entry| entry.id),
                                servers: endpoint.map(|entry| entry.servers.clone()).unwrap_or_default(),
                                clients: endpoint.map(|entry| entry.clients.clone()).unwrap_or_default(),
                                device_types: endpoint.map(|entry| entry.device_types.clone()).unwrap_or_default(),
                                manufacturer: snapshot.map(|entry| entry.manufacturer.clone()).unwrap_or_default(),
                                model: snapshot.map(|entry| entry.model.clone()).unwrap_or_default(),
                                rssi_dbm: snapshot.and_then(|entry| entry.rssi_dbm),
                            },
                        });
                        break;
                    }
                    cursor += count;
                }
                if record.is_some() { break; }
            }
            if let Some(record) = record {
                let event = crate::responses::event(crate::responses::Event::MatterInventory(&record))
                    .ok_or_else(|| std::io::Error::other("inventory exceeds IPC bound"))?;
                if ipc.publish_inventory(self.inventory_peers[index].peer, &event)? {
                    self.inventory_peers[index].index += 1;
                    self.inventory_peers[index].next = now + Duration::from_millis(20);
                }
            }
        }
        Ok(())
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
    /// Take the next bounded, still-encrypted ICD Check-In captured by the
    /// Secure Channel responder.
    pub fn pop_icd_check_in(&self) -> Option<rs_matter::IcdCheckIn> {
        self.matter.pop_icd_check_in()
    }

    fn node_awake(&self, node: u64, now: Instant) -> bool {
        !self.registered_nodes.contains(&node)
            || self
                .awake_until
                .iter()
                .any(|(awake_node, until)| *awake_node == node && *until > now)
    }

    /// Deliver queued production Manager reports to connected IPC clients.
    /// Keep the front report queued until at least one client accepts it.
    pub fn publish_reports(
        &mut self,
        ipc: &mut crate::service::IpcServer,
    ) -> std::io::Result<usize> {
        let mut published = 0;
        while let Some(button) = self.button_events.front() {
            let event = crate::responses::event(crate::responses::Event::MatterButton(button))
                .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::InvalidData, "invalid Matter button event"))?;
            if ipc.publish_all(&event)? == 0 { break; }
            self.button_events.pop_front();
            published += 1;
        }
        while let Some(reading) = self.front_report() {
            let event = crate::responses::event(crate::responses::Event::MatterReport(reading))
                .ok_or_else(|| {
                    std::io::Error::new(
                        std::io::ErrorKind::InvalidData,
                        "invalid Matter report event",
                    )
                })?;
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
    /// Exercise daemon polling and report correlation in the host fixture.
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
            servers: vec![6, 8, 29, 69, 91, 1026, 1029, 1037, 1066],
            clients: vec![],
            device_types: vec![],
        }];
        if let Some(snapshot) = self.snapshots.iter_mut().find(|entry| entry.node == node) {
            snapshot.endpoints = endpoints;
            snapshot.online = true;
            snapshot.next = now + Duration::from_secs(3600);
        } else {
            self.snapshots.push(Snapshot {
                node,
                endpoints,
                manufacturer: String::new(), model: String::new(), rssi_dbm: None,
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
                cluster: 69,
                attribute: 0,
                value: crate::operations::Value::Bool(true),
            },
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
                max_interval: 3600,
                failures: 0,
                unit: None,
                last_report: None,
            });
            self.host_test_reports
                .push_back(crate::subscriptions::ReceivedReport {
                    fabric: self.fabric.get(),
                    node,
                    subscription_id,
                    values: vec![value],
                    buttons: vec![],
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
        let local_delete = matches!(&request, crate::control::Request::Device(crate::control::DeviceRequest::Delete { local: true, .. }));
        if !ready && !local_delete { return Err(Failure::NotReady); }
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
            .any(|node| node.id == operation.node &&
                (node.phase == Phase::Complete || matches!(operation.action, crate::operations::Action::Delete { .. })))
        {
            return Err(Failure::NotFound);
        }
        operation.authorize(&self.snapshots)?;
        if self.next_job == 0 {
            return Err(Failure::Capacity);
        }
        if self.queued.iter().any(|job| job.operation.node == operation.node &&
            (matches!(job.operation.action, crate::operations::Action::Delete { .. }) || matches!(operation.action, crate::operations::Action::Delete { .. }))) ||
            self.active.as_ref().is_some_and(|job| job.node == operation.node && (job.deleting || matches!(operation.action, crate::operations::Action::Delete { .. }))) {
            return Err(Failure::Busy);
        }
        let timeout = if matches!(operation.action, crate::operations::Action::Delete { .. }) { 15 } else { 30 };
        let id = self.next_job;
        self.next_job = self.next_job.checked_add(1).unwrap_or(0);
        let state = if local_delete {
            self.remove_node(operation.node)?;
            JobRecordState::Complete(crate::operations::ResultValue {
                node: crate::control::Id(operation.node), endpoint: 0, cluster: 62, attribute: 0,
                value: crate::operations::Value::Bool(true),
            })
        } else {
            self.queued.push_back(QueuedOperation { id, operation, deadline: now + Duration::from_secs(timeout) });
            JobRecordState::Queued
        };
        self.jobs.push_back(JobRecord { id, state });
        self.trim_jobs();
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
        self.trim_jobs();
    }
    fn trim_jobs(&mut self) {
        while self.jobs.len() > MAX_RETAINED_JOBS {
            let Some(index) = self.jobs.iter().position(|record|
                matches!(record.state, JobRecordState::Complete(_) | JobRecordState::Failed(_))) else { break; };
            self.jobs.remove(index);
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
    fn remove_node(&mut self, node: u64) -> Result<(), crate::operations::Failure> {
        use crate::operations::Failure;
        // Remove wake credentials first. A failed catalog write retains a visible retryable record.
        if let Some(registry) = &mut self.icd_registry { registry.remove(self.fabric, node).map_err(|_| Failure::Storage)?; }
        self.catalog.remove(node).map_err(|_| Failure::Storage)?;
        self.snapshots.retain(|entry| entry.node != node);
        if self.work.as_ref().is_some_and(|(id, _, _)| *id == node) { self.work = None; }
        if self.icd_work.as_ref().is_some_and(|(id, _, _, _)| *id == node) { self.icd_work = None; }
        self.registered_nodes.retain(|id| *id != node);
        self.icd_checked.retain(|(id, _)| *id != node);
        self.icd_retry_after.retain(|((id, _), _)| *id != node);
        self.awake_until.retain(|(id, _)| *id != node);
        self.subscriptions.retain(|entry| entry.key.node != node);
        if self.subscription_work.as_ref().is_some_and(|(key, _, _)| key.node == node) { self.subscription_work = None; }
        self.report_subscriptions.replace(self.fabric.get(), self.subscriptions.iter().filter_map(|entry| entry.id.map(|id| (entry.key.node, id))));
        self.reports.retain(|entry| entry.node.0 != node);
        self.button_events.retain(|entry| entry.node.0 != node);
        let removed = self.matter.with_state(|state| state.sessions.remove_where(|session|
            session.get_local_fabric_idx() == self.fabric.get() && session.get_peer_node_id() == Some(node)));
        if removed != 0 { self.matter.transport().notify_session_removed(); }
        self.changed();
        Ok(())
    }
    fn changed(&mut self) {
        if self.revision != 0 {
            self.revision = self.revision.checked_add(1).unwrap_or(0);
        }
    }
    fn mark_node_unreachable(&mut self, node: u64, now: Instant) {
        self.address_refresh.request(self.compressed_fabric, node);
        let was_online = if let Some(snapshot) = self
            .snapshots
            .iter_mut()
            .find(|snapshot| snapshot.node == node)
        {
            let was_online = snapshot.online;
            snapshot.online = false;
            snapshot.next = now;
            was_online
        } else {
            false
        };
        if was_online {
            self.changed();
        }
    }
    #[cfg(any(test, feature = "controller-probe"))]
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
                        clients: endpoint.map(|e| e.clients.clone()).unwrap_or_default(),
                        device_types: endpoint.map(|e| e.device_types.clone()).unwrap_or_default(),
                        manufacturer: snapshot.map(|entry| entry.manufacturer.clone()).unwrap_or_default(),
                        model: snapshot.map(|entry| entry.model.clone()).unwrap_or_default(),
                        rssi_dbm: snapshot.and_then(|entry| entry.rssi_dbm),
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
            self.sync_report_subscriptions();
            if changed {
                self.changed();
            }
            return;
        }
        #[cfg(feature = "controller-service-test-hooks")]
        {
            let now = Instant::now();
            let host_subscriptions: Vec<_> = self
                .host_test_reports
                .iter()
                .flat_map(|report| {
                    report.values.iter().map(move |value| {
                        (
                            report.node,
                            value.endpoint,
                            value.cluster,
                            value.attribute,
                            report.subscription_id,
                        )
                    })
                })
                .collect();
            for (node, endpoint, cluster, attribute, id) in host_subscriptions {
                if let Some(subscription) = self.subscriptions.iter_mut().find(|subscription| {
                    subscription.key
                        == SubscriptionKey {
                            node,
                            endpoint,
                            cluster,
                            attribute,
                        }
                }) {
                    subscription.id = Some(id);
                } else {
                    self.subscriptions.push(SubscriptionState {
                        key: SubscriptionKey {
                            node,
                            endpoint,
                            cluster,
                            attribute,
                        },
                        id: Some(id),
                        next_retry: now + Duration::from_secs(3600),
                        max_interval: 3600,
                        failures: 0,
                        unit: None,
                        last_report: None,
                    });
                }
            }
        }
        if self.report_work.is_none() {
            #[cfg(feature = "controller-service-test-hooks")]
            if let Some(report) = self.host_test_reports.pop_front() {
                self.report_work = Some(Box::pin(async move {
                    Ok(crate::subscriptions::IncomingMessage::Report(report))
                }));
            }
            if self.report_work.is_none() {
                self.report_work = Some(Box::pin(crate::subscriptions::receive_incoming(
                    self.matter,
                    self.report_subscriptions.clone(),
                )));
            }
        }
        if let Some(work) = &mut self.report_work {
            if let Poll::Ready(result) = work.as_mut().poll(context) {
                self.report_work = None;
                match result {
                    Ok(crate::subscriptions::IncomingMessage::Report(report)) => {
                        self.record_report(report);
                    }
                    Err(_) => {}
                }
            }
        }
        // Finish the cached inventory bootstrap before expensive CASE work.
        // On MIPS a crypto poll can monopolize the service for seconds. HTTP
        // listing must not wait behind an interview of an unreachable node.
        if now < self.inventory_bootstrap_until || self.inventory_pending() {
            return;
        }
        if self.poll_icd(now, context) {
            return;
        }
        if commissioning {
            self.work = None;
            return;
        }
        if self.icd_work.is_some() {
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
            if let Some(mut result) = completed {
                let id = active.id;
                let node = active.node;
                self.active = None;
                if result.as_ref().is_ok_and(|value| value.cluster == 62) {
                    result = self.remove_node(node).map(|()| result.unwrap());
                }
                if let Err(error) = result {
                    if matches!(
                        error,
                        crate::operations::Failure::Accessory | crate::operations::Failure::Timeout
                    ) {
                        self.mark_node_unreachable(node, now);
                    }
                }
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
        while let Some(index) = self.queued.iter().position(|queued| queued.deadline <= now) {
            let queued = self.queued.remove(index).unwrap();
            self.record_state(
                queued.id,
                JobRecordState::Failed(crate::operations::Failure::Timeout),
            );
        }
        let ready_job = self
            .queued
            .iter()
            .position(|queued| matches!(queued.operation.action, crate::operations::Action::Delete { .. }) || self.node_awake(queued.operation.node, now));
        if let Some(index) = ready_job {
            let queued = self.queued.remove(index).unwrap();
            let id = queued.id;
            let operation_node = queued.operation.node;
            let authorization = self
                .catalog
                .nodes()
                .iter()
                .any(|node| node.id == queued.operation.node &&
                    (node.phase == Phase::Complete || matches!(queued.operation.action, crate::operations::Action::Delete { .. })))
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
            let deleting = matches!(queued.operation.action, crate::operations::Action::Delete { .. });
            let work = queued
                .operation
                .run_for(self.matter, self.crypto, self.fabric, timeout);
            self.active = Some(ActiveOperation {
                id,
                node: operation_node,
                deleting,
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
                let failed = result
                    .as_ref()
                    .err()
                    .is_some_and(|error| error.code() == ErrorCode::RxTimeout);
                let mut priming_report = None;
                if let Some(subscription) = self
                    .subscriptions
                    .iter_mut()
                    .find(|subscription| subscription.key == key)
                {
                    match result {
                        Ok(established) => {
                            let subscription_id = established.established.subscription_id;
                            subscription.id = Some(subscription_id);
                            subscription.failures = 0;
                            subscription.max_interval = established.established.max_int.max(1);
                            subscription.next_retry = now
                                + Duration::from_secs(u64::from(subscription.max_interval))
                                + SUBSCRIPTION_REPORT_GRACE;
                            subscription.unit = established.unit;
                            priming_report = Some(crate::subscriptions::ReceivedReport {
                                fabric: self.fabric.get(),
                                node: key.node,
                                subscription_id,
                                values: established.priming_values,
                                buttons: Vec::new(),
                            });
                        }
                        Err(_) => {
                            subscription.id = None;
                            subscription.failures = subscription.failures.saturating_add(1);
                            subscription.next_retry = now + retry_delay(subscription.failures);
                        }
                    }
                }
                if let Some(report) = priming_report {
                    self.sync_report_subscriptions();
                    self.record_report(report);
                }
                if failed {
                    self.mark_node_unreachable(key.node, now);
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
                let recovery_required = self
                    .catalog
                    .nodes()
                    .iter()
                    .any(|entry| entry.id == node && entry.phase == Phase::RecoveryRequired);
                let result = match result {
                    Ok(endpoints) if recovery_required => {
                        self.catalog.recover_complete(node).map(|()| endpoints)
                    }
                    result => result,
                };
                if result.is_ok() {
                    self.icd_checked
                        .retain(|(checked_node, _)| *checked_node != node);
                }
                let rssi_dbm = self.signals.rssi(self.compressed_fabric, node);
                let snapshot = self.snapshots.iter_mut().find(|s| s.node == node).unwrap();
                let visible_changed = match &result {
                    Ok(discovery) => !snapshot.online || snapshot.endpoints != discovery.endpoints ||
                        snapshot.manufacturer != discovery.manufacturer || snapshot.model != discovery.model || snapshot.rssi_dbm != rssi_dbm,
                    Err(_) => snapshot.online,
                };
                if result.is_err() {
                    self.address_refresh.request(self.compressed_fabric, node);
                }
                match result {
                    Ok(discovery) => {
                        snapshot.endpoints = discovery.endpoints;
                        snapshot.manufacturer = discovery.manufacturer;
                        snapshot.model = discovery.model;
                        snapshot.rssi_dbm = rssi_dbm;
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
            .filter(|n| matches!(n.phase, Phase::Complete | Phase::RecoveryRequired))
        {
            if !self.snapshots.iter().any(|s| s.node == node.id) {
                self.snapshots.push(Snapshot {
                    node: node.id,
                    endpoints: Vec::new(),
                    manufacturer: String::new(),
                    model: String::new(),
                    rssi_dbm: None,
                    online: false,
                    next: now,
                    failures: 0,
                });
            }
        }
        if let Some(snapshot) = self
            .snapshots
            .iter()
            .filter(|s| s.next <= now && self.node_awake(s.node, now))
            .min_by_key(|s| (self.catalog.nodes().iter().find(|node| node.id == s.node).is_none_or(|node| node.phase != Phase::Complete), s.next))
        {
            self.work = Some((
                snapshot.node,
                now + Duration::from_secs(30),
                Box::pin(discover_with_metadata(
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
            .filter(|subscription| {
                subscription.next_retry <= now && self.node_awake(subscription.key.node, now)
            })
            .min_by_key(|subscription| subscription.next_retry)
        {
            let key = subscription.key;
            // Stop acknowledging this ID before asking for its replacement.
            // KeepSubscriptions must stay true to preserve other attributes.
            if let Some(subscription) = self.subscriptions.iter_mut().find(|entry| entry.key == key) {
                subscription.id = None;
            }
            self.sync_report_subscriptions();
            let work: SubscriptionWork<'a> = if key.cluster == 59 && key.attribute == SWITCH_EVENT_ATTRIBUTE {
                Box::pin(crate::subscriptions::establish_switch_events(
                    self.matter, self.crypto, self.fabric, key.node, key.endpoint))
            } else {
                Box::pin(crate::subscriptions::establish_device_attribute(
                    self.matter, self.crypto, self.fabric, key.node, key.endpoint, key.cluster, key.attribute))
            };
            self.subscription_work = Some((
                key,
                now + Duration::from_secs(30),
                work,
            ));
        }
    }
    fn poll_icd(&mut self, now: Instant, context: &mut Context<'_>) -> bool {
        let mut progressed = false;
        // Check-In capture is bounded inside Matter. Drain at most one message
        // per service turn so authentication and private-store sync cannot
        // starve Thread, IPC, or the Matter transport pump.
        if self.icd_work.is_none() {
            if let Some(message) = self.matter.pop_icd_check_in() {
                let accepted = self
                    .icd_registry
                    .as_mut()
                    .map(|registry| registry.accept(self.crypto, &message));
                if let Some(Err(error)) = accepted.as_ref() {
                    log::warn!(
                        "Matter ICD Check-In rejected for {:016x}/fabric {}: {:?}",
                        message.node, message.fabric, error
                    );
                }
                if let Some(Ok(wake)) = accepted {
                    progressed = true;
                    let until = now
                        + Duration::from_millis(u64::from(wake.active_mode_threshold_ms).max(1));
                    if let Some((_, awake_until)) = self
                        .awake_until
                        .iter_mut()
                        .find(|(node, _)| *node == wake.node)
                    {
                        *awake_until = until;
                    } else if self.awake_until.len() < crate::icd::MAX_ICD_CLIENTS {
                        self.awake_until.push((wake.node, until));
                    }
                    if wake.refresh_required {
                        if let Some(endpoint) = self
                            .icd_registry
                            .as_ref()
                            .and_then(|registry| registry.registration(wake.fabric, wake.node))
                            .map(|registration| registration.endpoint)
                        {
                            let matter = self.matter;
                            let crypto = self.crypto;
                            let Some(mut registry) = self.icd_registry.take() else {
                                return progressed;
                            };
                            let Some(mut entropy) = self.icd_entropy.take() else {
                                self.icd_registry = Some(registry);
                                return progressed;
                            };
                            let fabric = wake.fabric;
                            let node = wake.node;
                            let local_node = self.local_node;
                            self.icd_work = Some((
                                node,
                                endpoint,
                                now + Duration::from_secs(10),
                                Box::pin(async move {
                                    let result = registry
                                        .register(
                                            matter,
                                            crypto,
                                            fabric,
                                            node,
                                            endpoint,
                                            local_node,
                                            &mut entropy,
                                        )
                                        .await;
                                    (registry, entropy, result)
                                }),
                            ));
                        }
                    }
                } else if self.icd_registry.is_none() {
                    log::warn!(
                        "Matter ICD Check-In received without a client registry for {:016x}/fabric {}",
                        message.node, message.fabric
                    );
                }
            }
        }
        if self.icd_work.is_none() {
            let candidate = (self.local_node != 0)
                .then(|| {
                    self.snapshots.iter().find_map(|snapshot| {
                        snapshot.endpoints.iter().find_map(|endpoint| {
                            (endpoint.servers.contains(&ICD_MANAGEMENT_CLUSTER)
                                && !self.icd_checked.contains(&(snapshot.node, endpoint.id))
                                && !self.registered_nodes.contains(&snapshot.node)
                                && !self.icd_retry_after.iter().any(|(key, until)| {
                                    *key == (snapshot.node, endpoint.id) && *until > now
                                }))
                            .then_some((snapshot.node, endpoint.id))
                        })
                    })
                })
                .flatten();
            if let Some((node, endpoint)) = candidate {
                log::debug!(
                    "Matter ICD Management found at {:016x}/{}; probing features",
                    node, endpoint
                );
                let matter = self.matter;
                let crypto = self.crypto;
                let Some(mut registry) = self.icd_registry.take() else {
                    return progressed;
                };
                let Some(mut entropy) = self.icd_entropy.take() else {
                    self.icd_registry = Some(registry);
                    return progressed;
                };
                let fabric = self.fabric;
                let local_node = self.local_node;
                self.icd_work = Some((
                    node,
                    endpoint,
                    now + Duration::from_secs(10),
                    Box::pin(async move {
                        let result = async {
                            let features = Exchange::initiate(matter, crypto, fabric, node)
                                .await?
                                .icd_management()
                                .feature_map_read(endpoint)
                                .await?;
                            let lit_features = rs_matter::dm::clusters::icd_mgmt::Feature::CHECK_IN_PROTOCOL_SUPPORT
                                | rs_matter::dm::clusters::icd_mgmt::Feature::LONG_IDLE_TIME_SUPPORT;
                            if rs_matter::dm::clusters::icd_mgmt::Feature::from_bits_truncate(features)
                                .contains(lit_features)
                            {
                                registry
                                    .register(
                                        matter,
                                        crypto,
                                        fabric,
                                        node,
                                        endpoint,
                                        local_node,
                                        &mut entropy,
                                    )
                                    .await?;
                            }
                            Ok(())
                        }
                        .await;
                        (registry, entropy, result)
                    }),
                ));
            }
        }
        let timed_out = self
            .icd_work
            .as_ref()
            .filter(|(_, _, deadline, _)| now >= *deadline)
            .map(|(node, endpoint, _, _)| (*node, *endpoint));
        if let Some(key) = timed_out {
            self.icd_work = None;
            self.icd_registry = crate::icd::ClientRegistry::open(self.catalog.store()).ok();
            self.icd_entropy = crate::entropy::OsEntropy::open().ok();
            log::warn!(
                "Matter ICD Management probe timed out for {:016x}/{}",
                key.0, key.1
            );
            let retry_at = now + Duration::from_secs(5);
            if let Some((_, until)) = self
                .icd_retry_after
                .iter_mut()
                .find(|(retry_key, _)| *retry_key == key)
            {
                *until = retry_at;
            } else if self.icd_retry_after.len() < MAX_ICD_CANDIDATES {
                self.icd_retry_after.push((key, retry_at));
            }
            return true;
        }
        if let Some((node, endpoint, _, work)) = &mut self.icd_work {
            if let Poll::Ready((registry, entropy, result)) = work.as_mut().poll(context) {
                progressed = true;
                let key = (*node, *endpoint);
                let node_registered = registry.is_registered(self.fabric, key.0);
                self.icd_work = None;
                self.icd_registry = Some(registry);
                self.icd_entropy = Some(entropy);
                if node_registered && !self.registered_nodes.contains(&key.0) {
                    self.registered_nodes.push(key.0);
                }
                if result.is_ok() {
                    self.icd_retry_after
                        .retain(|(retry_key, _)| *retry_key != key);
                    if !self.icd_checked.contains(&key)
                        && self.icd_checked.len() < MAX_ICD_CANDIDATES
                    {
                        self.icd_checked.push(key);
                    }
                } else {
                    log::warn!(
                        "Matter ICD management probe failed for {:016x}/{}: {:?}",
                        key.0, key.1, result
                    );
                    let retry_at = now + Duration::from_secs(5);
                    if let Some((_, until)) = self
                        .icd_retry_after
                        .iter_mut()
                        .find(|(retry_key, _)| *retry_key == key)
                    {
                        *until = retry_at;
                    } else if self.icd_retry_after.len() < MAX_ICD_CANDIDATES {
                        self.icd_retry_after.push((key, retry_at));
                    }
                }
            }
        }
        progressed
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
                    max_interval: crate::subscriptions::MAX_REPORT_INTERVAL_SECONDS,
                    failures: 0,
                    unit: None,
                    last_report: None,
                });
            }
        }
        self.sync_report_subscriptions();
    }
    fn sync_report_subscriptions(&self) {
        self.report_subscriptions.replace(
            self.fabric.get(),
            self.subscriptions.iter().filter_map(|subscription| {
                subscription.id.map(|id| (subscription.key.node, id))
            }),
        );
    }
    fn record_report(&mut self, report: crate::subscriptions::ReceivedReport) {
        if report.fabric != self.fabric.get() {
            return;
        }
        let Some(index) = self.subscriptions.iter().position(|subscription| {
            subscription.key.node == report.node && subscription.id == Some(report.subscription_id)
        }) else { return; };
        let became_online = self.snapshots.iter_mut().find(|snapshot| snapshot.node == report.node)
            .is_some_and(|snapshot| { let changed = !snapshot.online; snapshot.online = true; changed });
        if became_online { self.changed(); }
        let subscription = &mut self.subscriptions[index];
        // max_int is the publisher's report/keepalive cadence, not a lease
        // that requires periodic re-subscription. Only re-establish after
        // reports actually stop arriving.
        subscription.next_retry = Instant::now()
            + Duration::from_secs(u64::from(subscription.max_interval.max(1)))
            + SUBSCRIPTION_REPORT_GRACE;
        if subscription.key.cluster == 59 && subscription.key.attribute == SWITCH_EVENT_ATTRIBUTE {
            for button in report.buttons {
                if button.node.0 != subscription.key.node || button.endpoint != subscription.key.endpoint { continue; }
                if self.button_events.len() == MAX_QUEUED_REPORT_VALUES { self.button_events.pop_front(); }
                self.button_events.push_back(button);
            }
            return;
        }
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
                value.value =
                    crate::operations::Value::Measurement(crate::operations::MeasurementValue {
                        value: *measured,
                        unit,
                        quality,
                    });
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
    let mut keys = Vec::new();
    for snapshot in snapshots.iter().filter(|snapshot| snapshot.online) {
        for endpoint in &snapshot.endpoints {
            for cluster in [6, 8, 69, 91, 1026, 1029, 1037, 1066] {
                if endpoint.servers.contains(&cluster) {
                    keys.push(SubscriptionKey { node: snapshot.node, endpoint: endpoint.id, cluster, attribute: 0 });
                }
            }
            if endpoint.servers.contains(&47) {
                keys.push(SubscriptionKey { node: snapshot.node, endpoint: endpoint.id, cluster: 47, attribute: 12 });
            }
            if endpoint.servers.contains(&59) {
                keys.push(SubscriptionKey { node: snapshot.node, endpoint: endpoint.id, cluster: 59, attribute: SWITCH_EVENT_ATTRIBUTE });
            }
        }
    }
    keys.truncate(maximum);
    keys
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
async fn discover_with_metadata<C: Crypto>(
    matter: &Matter<'_>,
    crypto: C,
    fabric: NonZeroU8,
    node: u64,
) -> Result<Discovery, Error> {
    let exchange = Exchange::initiate(matter, &crypto, fabric, node).await?;
    let manufacturer = exchange
        .basic_information()
        .vendor_name_read_with(0, |value| value.map(String::from))
        .await??;
    let exchange = Exchange::initiate(matter, &crypto, fabric, node).await?;
    let model = exchange
        .basic_information()
        .product_name_read_with(0, |value| value.map(String::from))
        .await??;
    if manufacturer.len() > 64 || model.len() > 64 || manufacturer.bytes().any(|byte| byte < 0x20) ||
        model.bytes().any(|byte| byte < 0x20) {
        return Err(ErrorCode::InvalidData.into());
    }
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
        let exchange = Exchange::initiate(matter, &crypto, fabric, node).await?;
        let clients = exchange
            .descriptor()
            .client_list_read_with(id, |value| bounded(value?.iter(), MAX_CLUSTERS))
            .await??;
        let exchange = Exchange::initiate(matter, &crypto, fabric, node).await?;
        let device_types = exchange
            .descriptor()
            .device_type_list_read_with(id, |value| {
                bounded(value?.iter().map(|entry| entry.and_then(|entry| entry.device_type())), MAX_CLUSTERS)
            })
            .await??;
        if !servers.contains(&0x1d) {
            return Err(ErrorCode::InvalidData.into());
        }
        endpoints.push(Endpoint { id, servers, clients, device_types });
    }
    Ok(Discovery { endpoints, manufacturer, model })
}

pub async fn discover<C: Crypto>(
    matter: &Matter<'_>, crypto: C, fabric: NonZeroU8, node: u64,
) -> Result<Vec<Endpoint>, Error> {
    Ok(discover_with_metadata(matter, crypto, fabric, node).await?.endpoints)
}

#[cfg(test)]
mod tests {
    #[test]
    fn inventory_maximum_endpoint_fits_wire_and_round_trips() {
        let record = super::InventoryRecord {
            kind: "endpoint", endpoint_count: super::MAX_ENDPOINTS,
            device: super::Device {
                node: crate::control::Id(2), state: "complete", online: true,
                id: Some("matter:0000000000000001:0000000000000002:fffe".into()),
                endpoint: Some(65534),
                servers: (u32::MAX-63..=u32::MAX).collect(),
                clients: (u32::MAX-63..=u32::MAX).collect(),
                device_types: vec![67],
                manufacturer: "Acme".into(), model: "Model X".into(), rssi_dbm: Some(-42),
            },
        };
        let event=crate::responses::event(crate::responses::Event::MatterInventory(&record)).unwrap();
        let encoded=event.encode().unwrap();
        assert!(encoded.len()<=crate::ipc::MAX_PAYLOAD+2);
        let mut decoder=crate::ipc::Decoder::default();
        decoder.feed(&encoded).unwrap();
        let decoded=decoder.pop().unwrap();
        let json: serde_json::Value=serde_json::from_slice(&decoded.payload).unwrap();
        assert_eq!(json["data"]["device"]["servers"][63],u32::MAX);
        assert_eq!(json["data"]["device"]["endpoint"],65534);
    }

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

        let directory =
            std::env::temp_dir().join(format!("matter-report-ipc-{}", std::process::id()));
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
            Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog.clone()).unwrap();
        manager.subscriptions.push(SubscriptionState {
            key: SubscriptionKey {
                node: 2,
                endpoint: 1,
                cluster: 1026,
                attribute: 0,
            },
            id: Some(77),
            next_retry: Instant::now(),
            max_interval: 60,
            failures: 0,
            unit: None,
            last_report: None,
        });
        manager.record_report(crate::subscriptions::ReceivedReport {
            fabric: 1,
            node: 2,
            subscription_id: 77,
            buttons: vec![],
            values: vec![crate::operations::ResultValue {
                node: crate::control::Id(2),
                endpoint: 1,
                cluster: 1026,
                attribute: 0,
                value: crate::operations::Value::Measurement(crate::operations::MeasurementValue {
                    value: Some(-5.25),
                    unit: "celsius",
                    quality: "measured",
                }),
            }],
        });
        manager.button_events.push_back(crate::subscriptions::MatterButtonEvent {
            node: crate::control::Id(2), endpoint: 2, event: 0,
            position: Some(1), press_count: Some(1),
        });
        assert_eq!(manager.publish_reports(&mut ipc).unwrap(), 0);
        assert!(manager.front_report().is_some());

        let mut peer = UnixStream::connect(&socket).unwrap();
        peer.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        ipc.poll(|_| None).unwrap();
        assert_eq!(manager.publish_reports(&mut ipc).unwrap(), 2);
        ipc.poll(|_| None).unwrap();
        let mut events = Vec::new();
        for _ in 0..2 {
            let mut header = [0; 2];
            peer.read_exact(&mut header).unwrap();
            let length = usize::from(u16::from_le_bytes([header[0], header[1]]));
            let mut bytes = header.to_vec();
            bytes.resize(2 + length, 0);
            peer.read_exact(&mut bytes[2..]).unwrap();
            let mut decoder = crate::ipc::Decoder::default();
            decoder.feed(&bytes).unwrap();
            let event = decoder.pop().unwrap();
            assert_eq!((event.kind, event.opcode, event.id), (2, 12, 0));
            events.push(event.payload);
        }
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&events[0]).unwrap(), serde_json::from_slice::<serde_json::Value>(br#"{"schema":1,"event":"matter_button","data":{"node":"0000000000000002","endpoint":2,"event":0,"position":1,"press_count":1}}"#).unwrap());
        assert_eq!(serde_json::from_slice::<serde_json::Value>(&events[1]).unwrap(), serde_json::from_slice::<serde_json::Value>(br#"{"schema":1,"event":"matter_report","data":{"node":"0000000000000002","endpoint":1,"cluster":1026,"attribute":0,"value":{"type":"measurement","value":{"value":-5.25,"unit":"celsius","quality":"measured"}}}}"#).unwrap());
        assert!(manager.front_report().is_none());
        drop(manager);
        drop(ipc);
        drop(peer);
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn manager_report_queue_filters_duplicates_and_evicts_oldest_at_capacity() {
        use rs_matter::{
            crypto::{default_crypto, CryptoSensitiveRef},
            dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET},
            utils::init::InitMaybeUninit,
        };
        use std::os::unix::fs::PermissionsExt;

        fn report(value: f32) -> crate::subscriptions::ReceivedReport {
            crate::subscriptions::ReceivedReport {
                fabric: 1,
                node: 2,
                subscription_id: 77,
                buttons: vec![],
                values: vec![crate::operations::ResultValue {
                    node: crate::control::Id(2),
                    endpoint: 1,
                    cluster: 1026,
                    attribute: 0,
                    value: crate::operations::Value::Measurement(
                        crate::operations::MeasurementValue {
                            value: Some(value),
                            unit: "celsius",
                            quality: "measured",
                        },
                    ),
                }],
            }
        }

        let directory =
            std::env::temp_dir().join(format!("matter-report-bound-{}", std::process::id()));
        std::fs::create_dir(&directory).unwrap();
        std::fs::set_permissions(&directory, std::fs::Permissions::from_mode(0o700)).unwrap();
        let store = crate::store::PrivateStore::open(&directory).unwrap();
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
            max_interval: 60,
            failures: 0,
            unit: None,
            last_report: None,
        });

        manager.record_report(report(-5.25));
        manager.record_report(report(-5.25));
        assert_eq!(manager.reports.len(), 1);

        // An empty Matter ReportData is a liveness keepalive even when it
        // contains no changed attribute values.
        manager.subscriptions[0].next_retry = Instant::now() - Duration::from_secs(1);
        manager.record_report(crate::subscriptions::ReceivedReport {
            fabric: 1,
            node: 2,
            subscription_id: 77,
            values: Vec::new(),
            buttons: Vec::new(),
        });
        assert!(manager.subscriptions[0].next_retry > Instant::now() + Duration::from_secs(60));

        for value in 0..MAX_QUEUED_REPORT_VALUES {
            manager.record_report(report(value as f32));
        }
        assert_eq!(manager.reports.len(), MAX_QUEUED_REPORT_VALUES);
        assert!(matches!(
            manager.reports.front().map(|report| &report.value),
            Some(crate::operations::Value::Measurement(measurement))
                if measurement.value == Some(0.0)
        ));
        assert!(matches!(
            manager.reports.back().map(|report| &report.value),
            Some(crate::operations::Value::Measurement(measurement))
                if measurement.value == Some((MAX_QUEUED_REPORT_VALUES - 1) as f32)
        ));
        manager.record_report(report((MAX_QUEUED_REPORT_VALUES - 1) as f32));
        assert_eq!(manager.reports.len(), MAX_QUEUED_REPORT_VALUES);

        drop(manager);
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
                    servers: vec![6, 69, 1026, 0x1d],
                    clients: vec![59],
                    device_types: vec![],
                },
                Endpoint {
                    id: 2,
                    servers: vec![8, 91, 1029, 1037, 1066, 0x1d],
                    clients: vec![],
                    device_types: vec![],
                },
            ],
            manufacturer: String::new(), model: String::new(), rssi_dbm: None,
            online: true,
            next: now,
            failures: 0,
        };
        let offline = Snapshot {
            node: 3,
            endpoints: snapshot
                .endpoints
                .iter()
                .map(|endpoint| Endpoint {
                    id: endpoint.id,
                    servers: endpoint.servers.clone(),
                    clients: endpoint.clients.clone(),
                    device_types: endpoint.device_types.clone(),
                })
                .collect(),
            manufacturer: String::new(), model: String::new(), rssi_dbm: None,
            online: false,
            next: now,
            failures: 0,
        };
        let keys = subscription_keys(&[snapshot, offline], MAX_DEVICE_SUBSCRIPTIONS);
        assert_eq!(keys.len(), 8);
        assert!(keys.contains(&SubscriptionKey {
            node: 2,
            endpoint: 1,
            cluster: 69,
            attribute: 0,
        }));
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
            endpoints: (1..=64)
                .map(|id| Endpoint {
                    id,
                    servers: vec![6, 8, 69, 91, 1026, 1029, 1037, 1066],
                    clients: vec![],
                    device_types: vec![],
                })
                .collect(),
            manufacturer: String::new(), model: String::new(), rssi_dbm: None,
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
    fn local_deletion_retires_incomplete_nodes_and_icd_keys_without_radio() {
        use crate::{control::Request, entropy::OsEntropy, store::PrivateStore};
        use rs_matter::{crypto::{default_crypto, CryptoSensitiveRef},
            dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET}, utils::init::InitMaybeUninit};
        let directory = std::env::temp_dir().join(format!("matter-delete-{}", std::process::id()));
        let store = PrivateStore::open(&directory).unwrap();
        let catalog = Catalog::open(store.clone(), 1, 2).unwrap();
        drop(catalog.reserve(2).unwrap()); // Interrupted commissioning: recovery_required.
        let reservation = catalog.reserve(3).unwrap();
        reservation.fabric_installed().unwrap(); reservation.thread_provisioned().unwrap(); reservation.complete().unwrap();
        let crypto = default_crypto(OsEntropy::open().unwrap(), CryptoSensitiveRef::new(&[9; 32]));
        let mut memory = Box::new(std::mem::MaybeUninit::uninit());
        let matter = memory.init_with(Matter::init(&TEST_DEV_DET, TEST_DEV_COMM, &TEST_DEV_ATT, 5540));
        let fabric = NonZeroU8::new(1).unwrap();
        let mut manager = Manager::new(matter, &crypto, fabric, catalog.clone()).unwrap();
        manager.icd_registry.as_mut().unwrap().install(fabric, 2, 1, [0x42; 16], 7).unwrap();
        manager.registered_nodes.push(2);
        let request = || Request::decode(10, br#"{"schema":1,"action":"delete","node":"0000000000000002","local":true}"#).unwrap();
        let now = Instant::now();
        assert_eq!(manager.submit(request(), false, true, now).unwrap_err(), crate::operations::Failure::Busy);
        assert_eq!(catalog.nodes().len(), 2);
        let accepted = manager.submit(request(), false, false, now).unwrap();
        assert!(matches!(manager.job(accepted.job).unwrap(), JobState::Complete { .. }));
        assert!(!manager.registered_nodes.contains(&2));
        assert!(!manager.icd_registry.as_ref().unwrap().is_registered(fabric, 2));
        assert_eq!(catalog.nodes().iter().map(|node| node.id).collect::<Vec<_>>(), vec![3]);
        assert_eq!(manager.submit(request(), false, false, now).unwrap_err(), crate::operations::Failure::NotFound);
        // Immediate local deletions must obey the same retained-job bound as queued operations.
        for node in 4..25 {
            drop(catalog.reserve(node).unwrap());
            let body = format!("{{\"schema\":1,\"action\":\"delete\",\"node\":\"{node:016x}\",\"local\":true}}");
            let accepted = manager.submit(Request::decode(10, body.as_bytes()).unwrap(), false, false, now).unwrap();
            assert!(matches!(manager.job(accepted.job).unwrap(), JobState::Complete { .. }));
            assert!(manager.jobs.len() <= MAX_RETAINED_JOBS);
        }
        // A durable reload keeps the other node and does not resurrect the removed identity.
        drop(manager); drop(catalog); drop(store);
        let store = PrivateStore::open(&directory).unwrap();
        let catalog = Catalog::open(store.clone(), 1, 4).unwrap();
        assert_eq!(catalog.nodes().iter().map(|node| node.id).collect::<Vec<_>>(), vec![3]);
        assert!(!crate::icd::ClientRegistry::open(store.clone()).unwrap().is_registered(fabric, 2));
        drop(catalog); drop(store); std::fs::remove_dir_all(directory).unwrap();
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
        let catalog = Catalog::open(store, 1, 2).unwrap();
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
            Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog.clone()).unwrap();
        manager
            .icd_registry
            .as_mut()
            .unwrap()
            .install(NonZeroU8::new(1).unwrap(), 2, 1, [0x42; 16], 7)
            .unwrap();
        manager.registered_nodes.push(2);
        manager.snapshots.push(Snapshot {
            node: 2,
            endpoints: vec![Endpoint {
                id: 1,
                servers: vec![6],
                clients: vec![],
                device_types: vec![],
            }],
            manufacturer: String::new(), model: String::new(), rssi_dbm: None,
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
        manager.poll(true, false, now, &mut context);
        assert!(matches!(
            manager.job(first.job).unwrap(),
            JobState::Queued { .. }
        ));
        manager.awake_until.push((2, now + Duration::from_secs(1)));
        manager.poll(true, false, now + Duration::from_millis(1), &mut context);
        assert!(matches!(
            manager.job(first.job).unwrap(),
            JobState::Running { .. }
        ));
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
        let _ = std::fs::remove_dir_all(&directory);
        let store = PrivateStore::open(&directory).unwrap();
        let catalog = Catalog::open(store, 1, 2).unwrap();
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
            Manager::new(matter, &crypto, NonZeroU8::new(1).unwrap(), catalog.clone()).unwrap();
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
        manager.inventory_peers.push(super::InventoryPeer {
            peer: 1, revision: 0, index: 0, next: now,
        });
        manager.poll(true, false, now, &mut context);
        assert!(manager.work.is_none(), "inventory bootstrap must precede CASE discovery");
        manager.inventory_peers.clear();
        manager.poll(true, false, now, &mut context);
        assert_eq!(manager.snapshots.len(), 3); // recovery nodes are retried, not discarded
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
                    clients: vec![],
                    device_types: vec![],
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
                clients: vec![],
                device_types: vec![],
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
        let recovery_now = now + Duration::from_secs(1_000);
        manager.snapshots[0].next = recovery_now + Duration::from_secs(60);
        manager.snapshots[1].next = recovery_now + Duration::from_secs(60);
        manager.snapshots[2].next = recovery_now;
        manager.poll(true, false, recovery_now, &mut context);
        assert_eq!(manager.work.as_ref().unwrap().0, 4);
        manager.work = Some((
            4,
            recovery_now + Duration::from_secs(30),
            Box::pin(std::future::pending()),
        ));
        manager.poll(
            true,
            false,
            recovery_now + Duration::from_secs(30),
            &mut context,
        );
        assert!(!manager.snapshots[2].online);
        assert!(catalog
            .nodes()
            .iter()
            .any(|entry| entry.id == 4 && entry.phase == Phase::RecoveryRequired));
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
    fn unreachable_case_node_is_hidden_and_queued_for_address_refresh() {
        use rs_matter::{
            crypto::{default_crypto, CryptoSensitiveRef},
            dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET},
            utils::init::InitMaybeUninit,
        };
        use std::os::unix::fs::PermissionsExt;

        let directory =
            std::env::temp_dir().join(format!("matter-address-recovery-{}", std::process::id()));
        std::fs::create_dir(&directory).unwrap();
        std::fs::set_permissions(&directory, std::fs::Permissions::from_mode(0o700)).unwrap();
        let store = crate::store::PrivateStore::open(&directory).unwrap();
        let catalog = Catalog::open(store, 1, 2).unwrap();
        let entropy = crate::entropy::OsEntropy::open().unwrap();
        let crypto = default_crypto(entropy, CryptoSensitiveRef::new(&[9; 32]));
        let mut memory = Box::new(std::mem::MaybeUninit::uninit());
        let matter = memory.init_with(Matter::init(
            &TEST_DEV_DET,
            TEST_DEV_COMM,
            &TEST_DEV_ATT,
            5540,
        ));
        let refresh = crate::discovery::RefreshRequests::default();
        let mut manager = Manager::new_with_address_refresh(
            matter,
            &crypto,
            NonZeroU8::new(1).unwrap(),
            catalog,
            MAX_DEVICE_SUBSCRIPTIONS,
            17,
            refresh.clone(),
        )
        .unwrap();
        let next = Instant::now() + Duration::from_secs(60);
        manager.snapshots.push(Snapshot {
            node: 2,
            endpoints: vec![Endpoint {
                id: 1,
                servers: vec![6],
                clients: vec![],
                device_types: vec![],
            }],
            manufacturer: String::new(), model: String::new(), rssi_dbm: None,
            online: true,
            next,
            failures: 0,
        });
        let now = Instant::now();

        manager.mark_node_unreachable(2, now);

        assert!(!manager.snapshots[0].online);
        assert_eq!(manager.snapshots[0].next, now);
        assert_eq!(manager.snapshots[0].endpoints[0].servers, vec![6]);
        assert!(refresh.take_for(17, 2));
        assert!(!refresh.take_for(17, 2));

        drop(manager);
        std::fs::remove_dir_all(directory).unwrap();
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
