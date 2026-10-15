//! Production commissioning owner. The phone only carries opaque GATT data.
use crate::{
    ble::BleRelay,
    catalog::Catalog,
    commissioning::{provision_thread, CommissioningPolicy, ThreadDataset},
    control::{Id, Request},
    entropy::OsEntropy,
    identity::Identity,
    responses::{self, Event},
    service::IpcServer,
    sessions::{Attempt, Phase, SessionError, Sessions, Ticket},
    store::PrivateStore,
    thread_lifecycle::{Lifecycle, State},
};
use rs_matter::{
    cert::{gen::VALID_FOREVER, MAX_CERT_TLV_AND_ASN1_LEN, MAX_CERT_TLV_LEN},
    crypto::Crypto,
    error::ErrorCode,
    onboard::{Commissioner, RegulatoryConfig},
    transport::network::{Address, BtAddr},
    Matter,
};
use serde::Serialize;
use std::{cell::RefCell, num::NonZeroU8, rc::Rc, task::Context, time::Instant};
use zeroize::Zeroizing;

#[derive(Serialize)]
struct Attached {
    generation: u32,
}
#[derive(Serialize)]
struct AcceptedIndication {
    sequence: u32,
}
#[derive(Serialize)]
struct CompletedWrite {
    write: u32,
}

/// A secure session belongs to one BLE connection, not to its reusable address.
/// Construct outside the async body so even an unpolled cancelled job cleans up.
struct BleConnectionSessions<'a> {
    matter: &'a Matter<'a>,
    peer: Address,
}
impl<'a> BleConnectionSessions<'a> {
    fn new(matter: &'a Matter<'a>, address: BtAddr) -> Self {
        let guard = Self {
            matter,
            peer: Address::Btp(address),
        };
        guard.clear();
        guard
    }
    fn clear(&self) {
        let removed = self.matter.with_state(|state| {
            state
                .sessions
                .remove_where(|session| session.get_peer_addr() == self.peer)
        });
        if removed != 0 {
            self.matter.transport().notify_session_removed();
            log::debug!("Commissioning BLE connection discarded {removed} transport session(s)");
        }
    }
}
impl Drop for BleConnectionSessions<'_> {
    fn drop(&mut self) {
        self.clear();
    }
}

pub struct Controller<'a, C: Crypto> {
    sessions: Sessions<'a>,
    matter: &'a Matter<'a>,
    crypto: &'a C,
    identity: Rc<RefCell<Identity>>,
    store: PrivateStore,
    fabric: NonZeroU8,
    compressed_fabric_id: u64,
    next_node: u64,
    catalog: Catalog,
    peer: Option<u64>,
    last_snapshot: Vec<u8>,
}
impl<'a, C: Crypto + 'a> Controller<'a, C> {
    pub fn new(
        relay: &'a BleRelay,
        matter: &'a Matter<'a>,
        crypto: &'a C,
        identity: Identity,
        store: PrivateStore,
        fabric: NonZeroU8,
    ) -> Result<Self, rs_matter::error::Error> {
        let compressed_fabric_id = matter.with_state(|state| {
            Ok::<_, rs_matter::error::Error>(state.fabrics.fabric(fabric)?.compressed_fabric_id())
        })?;
        let next_node = identity.next_node();
        let catalog = Catalog::open(store.clone(), identity.fabric_id(), identity.next_node())?;
        Ok(Self {
            sessions: Sessions::new(relay),
            matter,
            crypto,
            identity: Rc::new(RefCell::new(identity)),
            store,
            fabric,
            compressed_fabric_id,
            next_node,
            catalog,
            peer: None,
            last_snapshot: Vec::new(),
        })
    }
    fn ready(lifecycle: &Lifecycle, now: Instant) -> bool {
        lifecycle.snapshot().configured
            && lifecycle.boot_channel(now).is_some()
            && matches!(
                lifecycle.snapshot().state,
                State::Attaching | State::Attached
            )
    }
    pub fn handle(
        &mut self,
        request: Request,
        peer: u64,
        lifecycle: &Lifecycle,
        entropy: &mut OsEntropy,
        now: Instant,
    ) -> Option<Vec<u8>> {
        match request {
            Request::CommissionStart(request) => {
                let result = if Self::ready(lifecycle, now) && !self.catalog.legacy_pending() {
                    self.sessions.start(request, entropy, now)
                } else {
                    Err(SessionError::NotReady)
                };
                if result.is_ok() {
                    self.peer = Some(peer);
                    self.last_snapshot.clear();
                }
                responses::reply(result)
            }
            Request::CommissionStatus(request) => {
                let result = self.sessions.snapshot(request.session, now);
                if result.is_ok() && request.subscribe {
                    self.peer = Some(peer);
                    self.last_snapshot.clear();
                }
                responses::reply(result)
            }
            Request::CommissionCancel(request) => {
                responses::reply(self.sessions.cancel(request.session, now))
            }
            Request::BleAttach(request) => {
                let result = (|| {
                    if !Self::ready(lifecycle, now) || self.catalog.legacy_pending() {
                        return Err(SessionError::NotReady);
                    }
                    let channel = lifecycle.boot_channel(now).ok_or(SessionError::NotReady)?;
                    let dataset = lifecycle.dataset_copy().ok_or(SessionError::NotReady)?;
                    let address = BtAddr(
                        request
                            .address
                            .bytes()
                            .try_into()
                            .map_err(|_| SessionError::Malformed)?,
                    );
                    let ticket =
                        self.sessions
                            .attach(request.session, address, request.mtu, now)?;
                    let attempt = self.sessions.attempt(ticket, now)?;
                    let connection_sessions = BleConnectionSessions::new(self.matter, address);
                    let work = commission(
                        self.matter,
                        self.crypto,
                        self.identity.clone(),
                        self.store.clone(),
                        self.fabric,
                        self.catalog.clone(),
                        address,
                        attempt,
                        dataset,
                        channel,
                        connection_sessions,
                    );
                    self.sessions.install(ticket, work, now)?;
                    self.peer = Some(peer);
                    self.last_snapshot.clear();
                    Ok(Attached {
                        generation: ticket.generation,
                    })
                })();
                responses::reply(result)
            }
            Request::BleIndication(request) => {
                let ticket = Ticket {
                    session: request.session,
                    generation: request.generation.get(),
                };
                responses::reply(
                    self.sessions
                        .indication(ticket, request.sequence.get(), request.data.bytes(), now)
                        .map(|_| AcceptedIndication {
                            sequence: request.sequence.get(),
                        }),
                )
            }
            Request::BleWriteComplete(request) => {
                let ticket = Ticket {
                    session: request.session,
                    generation: request.generation.get(),
                };
                responses::reply(
                    self.sessions
                        .write_complete(ticket, request.write.get(), request.success, now)
                        .map(|_| CompletedWrite {
                            write: request.write.get(),
                        }),
                )
            }
            _ => None,
        }
    }
    pub fn fatal(&self) -> bool {
        self.catalog.fatal()
    }
    pub fn catalog(&self) -> Catalog {
        self.catalog.clone()
    }
    pub fn discovery_identity(&self) -> (u64, u64) {
        (self.compressed_fabric_id, self.next_node)
    }
    pub fn commissioning(&mut self, now: Instant) -> bool {
        self.sessions
            .current_snapshot(now)
            .map(|s| s.state < Phase::Complete)
            .unwrap_or(false)
    }
    pub fn poll(&mut self, lifecycle: &Lifecycle, now: Instant, context: &mut Context<'_>) {
        self.sessions.tick(now);
        if !Self::ready(lifecycle, now) {
            self.sessions.fail(SessionError::NotReady);
        }
        self.sessions.poll(now, context);
    }
    pub fn publish(&mut self, ipc: &mut IpcServer, now: Instant) -> std::io::Result<()> {
        let Some(peer) = self.peer else {
            return Ok(());
        };
        if !ipc.has_peer(peer) {
            if let Some(snapshot) = self.sessions.current_snapshot(now) {
                if snapshot.state < Phase::Complete {
                    let _ = self.sessions.cancel(snapshot.session, now);
                }
            }
            self.peer = None;
            self.last_snapshot.clear();
            return Ok(());
        }
        if let Some(snapshot) = self.sessions.current_snapshot(now) {
            let event =
                responses::event(Event::Commissioning(&snapshot)).ok_or_else(event_error)?;
            if event.payload != self.last_snapshot && ipc.publish(peer, &event)? {
                self.last_snapshot = event.payload;
            }
        }
        // Consume a C1 write only when its selected connection still exists.
        // Losing the peer after enqueue keeps the relay's ten-second deadline.
        if ipc.has_peer(peer) {
            if let Ok(Some(write)) = self.sessions.next_write(now) {
                let event = responses::event(Event::BleWrite(&write)).ok_or_else(event_error)?;
                ipc.publish(peer, &event)?;
            }
        }
        Ok(())
    }
}
fn event_error() -> std::io::Error {
    std::io::Error::new(
        std::io::ErrorKind::InvalidData,
        "Commissioning event unavailable",
    )
}

async fn commission<C: Crypto>(
    matter: &Matter<'_>,
    crypto: &C,
    identity: Rc<RefCell<Identity>>,
    store: PrivateStore,
    fabric: NonZeroU8,
    catalog: Catalog,
    address: BtAddr,
    attempt: Attempt,
    dataset: Zeroizing<Vec<u8>>,
    channel: u8,
    _connection_sessions: BleConnectionSessions<'_>,
) -> Result<Id, SessionError> {
    // The old pinned task (and its authority borrow) is dropped before attach
    // installs another one. Allocator gaps on cancellation are intentional.
    if catalog.fatal() {
        return Err(SessionError::Storage);
    }
    if !catalog.has_capacity() {
        return Err(SessionError::Capacity);
    }
    let mut authority = identity.borrow_mut();
    let node = authority.allocate_node(&store).map_err(|error| {
        if error.code() == ErrorCode::NoSpace {
            SessionError::Capacity
        } else {
            SessionError::Storage
        }
    })?;
    let reservation = catalog.reserve(node).map_err(|_| SessionError::Storage)?;
    let mut noc_buffer = [0; MAX_CERT_TLV_AND_ASN1_LEN];
    let mut signer = authority
        .signer(&mut noc_buffer)
        .map_err(|_| SessionError::Storage)?;
    let mut cert_buffer = [0; MAX_CERT_TLV_LEN];
    let mut commissioner = Commissioner::new(matter, crypto, fabric, &mut signer, &mut cert_buffer);
    let peer = Address::Btp(address);
    let mut commissioning_options = CommissioningPolicy::default().options();
    commissioning_options.fail_safe_secs = attempt.timeout_seconds;
    commissioning_options.regulatory_config = Some(RegulatoryConfig {
        country_code: attempt
            .country_code
            .as_bytes()
            .try_into()
            .map_err(|_| SessionError::Malformed)?,
        location: match attempt.regulatory_location {
            crate::control::RegulatoryLocation::Indoor => {
                rs_matter::dm::clusters::gen_comm::RegulatoryLocationTypeEnum::Indoor
            }
            crate::control::RegulatoryLocation::Outdoor => {
                rs_matter::dm::clusters::gen_comm::RegulatoryLocationTypeEnum::Outdoor
            }
        },
    });
    // Authenticate this BLE connection before reporting the first encrypted command.
    attempt.progress.step("pase");
    let exchange =
        rs_matter::transport::exchange::Exchange::initiate_pase(matter, crypto, peer, *attempt.pin)
            .await
            .map_err(accessory_error)?;
    drop(exchange);
    let result = commissioner
        .commission_with_progress(
            peer,
            *attempt.pin,
            &commissioning_options,
            node,
            VALID_FOREVER,
            |step| attempt.progress.step(step),
        )
        .await
        .map_err(accessory_error)?;
    if result.device_node_id != node {
        return Err(SessionError::Accessory);
    }
    reservation
        .fabric_installed()
        .map_err(|_| SessionError::Storage)?;
    attempt.progress.set(Phase::Provisioning);
    attempt.progress.step("thread_provisioning");
    let parsed = ThreadDataset::parse(&dataset, channel).map_err(|_| SessionError::Storage)?;
    provision_thread(matter, crypto, peer, *attempt.pin, &parsed)
        .await
        .map_err(accessory_error)?;
    reservation
        .thread_provisioned()
        .map_err(|_| SessionError::Storage)?;
    attempt.progress.set(Phase::WaitingThread);
    // Completion requires operational DNS-SD/CASE; no fixture address injection
    // or fake completion is permitted. The owned session deadline bounds it.
    attempt.progress.set(Phase::Case);
    attempt.progress.step("case_and_commissioning_complete");
    commissioner
        .complete_via_case(&result)
        .await
        .map_err(accessory_error)?;
    attempt.progress.set(Phase::Completing);
    reservation.complete().map_err(|_| SessionError::Storage)?;
    Ok(Id(node))
}
fn accessory_error(error: rs_matter::error::Error) -> SessionError {
    match error.code() {
        ErrorCode::RxTimeout => SessionError::Timeout,
        ErrorCode::NoSpace => SessionError::Capacity,
        _ => SessionError::Accessory,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::thread_lifecycle::{Failure, Radio};
    use rs_matter::crypto::{default_crypto, CryptoSensitiveRef};
    use rs_matter::dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET};
    use rs_matter::utils::init::InitMaybeUninit;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::time::Duration;
    static NEXT: AtomicU32 = AtomicU32::new(0);
    struct Directory(std::path::PathBuf);
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    struct FakeRadio;
    impl Radio for FakeRadio {
        fn apply(&self, _: &[u8], _: u8) -> Result<(), Failure> {
            Ok(())
        }
        fn disable(&self) -> Result<(), Failure> {
            Ok(())
        }
        fn attached(&self) -> bool {
            true
        }
    }
    fn seed_pase<C: Crypto>(matter: &Matter<'_>, crypto: &C, address: BtAddr) -> u32 {
        use rs_matter::transport::session::{ReservedSession, SessionMode};
        let mut reserved = ReservedSession::reserve_now(matter, crypto).unwrap();
        reserved
            .update(
                0,
                0,
                123,
                456,
                Address::Btp(address),
                SessionMode::Pase { fab_idx: 0 },
                None,
                None,
                None,
                None,
            )
            .unwrap();
        reserved.complete();
        matter.with_state(|state| {
            state
                .sessions
                .iter()
                .find(|s| s.get_peer_addr() == Address::Btp(address))
                .unwrap()
                .id()
        })
    }

    #[test]
    fn ble_retry_and_all_terminal_paths_remove_only_connection_sessions() {
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
        let address = BtAddr([1; 6]);
        let other = seed_pase(matter, &crypto, BtAddr([2; 6]));
        let operational = matter.with_state(|state| {
            state
                .sessions
                .add(0, false, Address::default(), Some(42), &TEST_DEV_DET)
                .unwrap()
                .id()
        });
        let stale = seed_pase(matter, &crypto, address);
        let present = |id| matter.with_state(|state| state.sessions.get(id).is_some());
        let relay = BleRelay::new();
        let mut sessions = Sessions::new(&relay);
        let mut entropy = OsEntropy::open().unwrap();
        let request = || {
            match Request::decode(3, br#"{"schema":1,"discriminator":3840,"pin":20202021,"timeout_seconds":30,"country_code":"DE","regulatory_location":"indoor"}"#).unwrap() {
            Request::CommissionStart(request) => request,
            _ => unreachable!(),
        }
        };
        struct Wake;
        impl std::task::Wake for Wake {
            fn wake(self: std::sync::Arc<Self>) {}
        }
        let waker = std::task::Waker::from(std::sync::Arc::new(Wake));
        let mut context = Context::from_waker(&waker);
        // Failure, cancellation before first poll, cancellation after polling,
        // deadline expiry, and reconnect all drop the same owned guard.
        for ending in 0..5 {
            let now = Instant::now();
            let id = sessions
                .start(request(), &mut entropy, now)
                .unwrap()
                .session;
            let ticket = sessions.attach(id, address, 247, now).unwrap();
            let guard = BleConnectionSessions::new(matter, address);
            assert!(!present(stale));
            let current = seed_pase(matter, &crypto, address);
            sessions
                .install(
                    ticket,
                    async move {
                        let _guard = guard;
                        if ending == 0 {
                            return Err(SessionError::Timeout);
                        }
                        std::future::pending::<Result<Id, SessionError>>().await
                    },
                    now,
                )
                .unwrap();
            if ending != 1 {
                sessions.poll(now, &mut context);
            }
            match ending {
                0 => assert_eq!(
                    sessions.snapshot(id, now).unwrap().error,
                    Some(SessionError::Timeout)
                ),
                1 | 2 => {
                    sessions.cancel(id, now).unwrap();
                }
                3 => sessions.tick(now + Duration::from_secs(31)),
                _ => {
                    sessions.attach(id, address, 247, now).unwrap();
                    sessions.cancel(id, now).unwrap();
                }
            }
            assert!(
                !present(current),
                "connection session survived ending {ending}"
            );
            assert!(present(other));
            assert!(present(operational));
        }
    }

    #[test]
    fn actual_owner_readiness_cancel_deadline_and_no_status_writes() {
        let directory = Directory(std::env::temp_dir().join(format!(
            "matter-owner-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        )));
        let store = PrivateStore::open(&directory.0).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[9; 32]),
        );
        let identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        let mut memory = Box::new(std::mem::MaybeUninit::uninit());
        let matter = memory.init_with(Matter::init(
            &TEST_DEV_DET,
            TEST_DEV_COMM,
            &TEST_DEV_ATT,
            5540,
        ));
        matter.startup(&matter.kv(store.clone())).unwrap();
        let fabric = identity.install(matter, &crypto, &store).unwrap();
        let relay = BleRelay::new();
        let mut owner =
            Controller::new(&relay, matter, &crypto, identity, store.clone(), fabric).unwrap();
        let mut lifecycle = Lifecycle::load(store.clone()).unwrap();
        let mut entropy = OsEntropy::open().unwrap();
        let now = Instant::now();
        let start = br#"{"schema":1,"discriminator":3840,"pin":20202021,"timeout_seconds":30,"country_code":"DE","regulatory_location":"indoor"}"#;
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        let body = owner
            .handle(
                Request::decode(3, start).unwrap(),
                1,
                &lifecycle,
                &mut entropy,
                now,
            )
            .unwrap();
        assert_eq!(body, br#"{"schema":1,"ok":false,"error":"not_ready"}"#);
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
        lifecycle.observe(15, true, Some(&FakeRadio), now).unwrap();
        lifecycle.create(15, &mut entropy, &FakeRadio, now).unwrap();
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        let body = owner
            .handle(
                Request::decode(3, start).unwrap(),
                1,
                &lifecycle,
                &mut entropy,
                now,
            )
            .unwrap();
        let snapshot: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_eq!(snapshot["result"]["state"], "waiting_ble");
        assert!(!String::from_utf8(body).unwrap().contains("20202021"));
        let id = snapshot["result"]["session"].as_str().unwrap();
        let status = format!(r#"{{"schema":1,"session":"{id}"}}"#);
        assert_eq!(
            owner
                .handle(
                    Request::decode(3, start).unwrap(),
                    1,
                    &lifecycle,
                    &mut entropy,
                    now
                )
                .unwrap(),
            br#"{"schema":1,"ok":false,"error":"busy"}"#
        );
        for _ in 0..10 {
            assert!(owner
                .handle(
                    Request::decode(4, status.as_bytes()).unwrap(),
                    1,
                    &lifecycle,
                    &mut entropy,
                    now
                )
                .is_some());
        }
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
        let timeout = owner
            .handle(
                Request::decode(4, status.as_bytes()).unwrap(),
                1,
                &lifecycle,
                &mut entropy,
                now + Duration::from_secs(31),
            )
            .unwrap();
        let timeout: serde_json::Value = serde_json::from_slice(&timeout).unwrap();
        assert_eq!(timeout["result"]["state"], "timed_out");
        assert_eq!(timeout["result"]["error"], "timeout");
        let again = now + Duration::from_secs(32);
        lifecycle
            .observe(15, true, Some(&FakeRadio), again)
            .unwrap();
        let body = owner
            .handle(
                Request::decode(3, start).unwrap(),
                1,
                &lifecycle,
                &mut entropy,
                again,
            )
            .unwrap();
        let snapshot: serde_json::Value = serde_json::from_slice(&body).unwrap();
        assert_ne!(snapshot["result"]["session"], id);
        let cancel = format!(
            r#"{{"schema":1,"session":{}}}"#,
            snapshot["result"]["session"]
        );
        let first = owner
            .handle(
                Request::decode(5, cancel.as_bytes()).unwrap(),
                1,
                &lifecycle,
                &mut entropy,
                again,
            )
            .unwrap();
        assert_eq!(
            owner
                .handle(
                    Request::decode(5, cancel.as_bytes()).unwrap(),
                    1,
                    &lifecycle,
                    &mut entropy,
                    again
                )
                .unwrap(),
            first
        );
        let cancelled: serde_json::Value = serde_json::from_slice(&first).unwrap();
        assert_eq!(cancelled["result"]["state"], "cancelled");
        assert_eq!(cancelled["result"]["deadline_remaining_seconds"], 0);
    }
}
