//! Production commissioning owner. The phone only carries opaque GATT data.
use crate::{
    ble::BleRelay,
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
    onboard::Commissioner,
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

pub struct Controller<'a, C: Crypto> {
    sessions: Sessions<'a>,
    matter: &'a Matter<'a>,
    crypto: &'a C,
    identity: Rc<RefCell<Identity>>,
    store: PrivateStore,
    fabric: NonZeroU8,
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
    ) -> Self {
        Self {
            sessions: Sessions::new(relay),
            matter,
            crypto,
            identity: Rc::new(RefCell::new(identity)),
            store,
            fabric,
            peer: None,
            last_snapshot: Vec::new(),
        }
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
                let result = if Self::ready(lifecycle, now) {
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
                    if !Self::ready(lifecycle, now) {
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
                    let work = commission(
                        self.matter,
                        self.crypto,
                        self.identity.clone(),
                        self.store.clone(),
                        self.fabric,
                        address,
                        attempt,
                        dataset,
                        channel,
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
    pub fn poll(&mut self, lifecycle: &Lifecycle, now: Instant, context: &mut Context<'_>) {
        self.sessions.tick(now);
        if !Self::ready(lifecycle, now) {
            self.sessions.fail(SessionError::NotReady);
        }
        self.sessions.poll(now, context);
    }
    pub fn publish(&mut self, ipc: &mut IpcServer, now: Instant) -> std::io::Result<()> {
        let Some(peer) = self.peer.filter(|peer| ipc.has_peer(*peer)) else {
            return Ok(());
        };
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
    address: BtAddr,
    attempt: Attempt,
    dataset: Zeroizing<Vec<u8>>,
    channel: u8,
) -> Result<Id, SessionError> {
    // The old pinned task (and its authority borrow) is dropped before attach
    // installs another one. Allocator gaps on cancellation are intentional.
    let mut authority = identity.borrow_mut();
    let node = authority.allocate_node(&store).map_err(|error| {
        if error.code() == ErrorCode::NoSpace {
            SessionError::Capacity
        } else {
            SessionError::Storage
        }
    })?;
    let mut noc_buffer = [0; MAX_CERT_TLV_AND_ASN1_LEN];
    let mut signer = authority
        .signer(&mut noc_buffer)
        .map_err(|_| SessionError::Storage)?;
    let mut cert_buffer = [0; MAX_CERT_TLV_LEN];
    let mut commissioner = Commissioner::new(matter, crypto, fabric, &mut signer, &mut cert_buffer);
    let peer = Address::Btp(address);
    let result = commissioner
        .commission(
            peer,
            *attempt.pin,
            &CommissioningPolicy::default().options(),
            node,
            VALID_FOREVER,
        )
        .await
        .map_err(accessory_error)?;
    if result.device_node_id != node {
        return Err(SessionError::Accessory);
    }
    attempt.progress.set(Phase::Provisioning);
    let parsed = ThreadDataset::parse(&dataset, channel).map_err(|_| SessionError::Storage)?;
    provision_thread(matter, crypto, peer, *attempt.pin, &parsed)
        .await
        .map_err(accessory_error)?;
    attempt.progress.set(Phase::WaitingThread);
    // Completion requires operational DNS-SD/CASE; no fixture address injection
    // or fake completion is permitted. The owned session deadline bounds it.
    attempt.progress.set(Phase::Case);
    commissioner
        .complete_via_case(&result)
        .await
        .map_err(accessory_error)?;
    attempt.progress.set(Phase::Completing);
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
        let mut owner = Controller::new(&relay, matter, &crypto, identity, store.clone(), fabric);
        let mut lifecycle = Lifecycle::load(store.clone()).unwrap();
        let mut entropy = OsEntropy::open().unwrap();
        let now = Instant::now();
        let start = br#"{"schema":1,"discriminator":3840,"pin":20202021,"timeout_seconds":30}"#;
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
