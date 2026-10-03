//! Thread credentials and shared-channel gating. No secret has Debug/Display.
use crate::commissioning::ThreadDataset;
use crate::openthread::OpenThread;
use crate::store::PrivateStore;
use rs_matter::crypto::{TryCryptoRng, TryRng};
use serde::Serialize;
use std::time::{Duration, Instant};
use zeroize::Zeroizing;

pub const DATASET_KEY: u16 = 0x1001;
const MAGIC: &[u8; 8] = b"TMDT\x01\0\0\0";
const MAX_RECORD: usize = 265;
const OBSERVATION_TTL: Duration = Duration::from_secs(5);
#[derive(Clone, Copy, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum State {
    AwaitingCoordinator,
    Detached,
    Attaching,
    Attached,
    ChannelMismatch,
    Error,
}
#[derive(Clone, Copy, Serialize, Debug, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Failure {
    Malformed,
    NotReady,
    Busy,
    Transport,
    Storage,
}
#[derive(Serialize)]
pub struct Snapshot {
    pub enabled: bool,
    pub available: bool,
    pub configured: bool,
    pub attached: bool,
    pub state: State,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub zigbee_channel: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub dataset_channel: Option<u8>,
    pub linux_ipv6_or_tun: bool,
}
pub trait Radio {
    fn apply(&self, bytes: &[u8], channel: u8) -> Result<(), Failure>;
    fn disable(&self) -> Result<(), Failure>;
    fn attached(&self) -> bool;
}
impl Radio for OpenThread {
    fn apply(&self, bytes: &[u8], channel: u8) -> Result<(), Failure> {
        self.set_dataset(bytes, channel)
            .map_err(|_| Failure::Transport)
    }
    fn disable(&self) -> Result<(), Failure> {
        self.disable().map_err(|_| Failure::Transport)
    }
    fn attached(&self) -> bool {
        self.ready()
    }
}
pub struct Lifecycle {
    store: PrivateStore,
    dataset: Option<Zeroizing<Vec<u8>>>,
    dataset_channel: Option<u8>,
    channel: Option<u8>,
    observed: Option<Instant>,
    active: bool,
    state: State,
    fatal: bool,
    apply_failed: bool,
}
impl Lifecycle {
    pub fn load(store: PrivateStore) -> Result<Self, Failure> {
        let mut bytes = Zeroizing::new([0; MAX_RECORD]);
        let record = store
            .get(DATASET_KEY, &mut bytes[..])
            .map_err(|_| Failure::Storage)?;
        let (dataset, dataset_channel) = if let Some(length) = record {
            if length < 12 || &bytes[..8] != MAGIC {
                return Err(Failure::Storage);
            }
            let channel = bytes[8];
            let size = usize::from(u16::from_le_bytes([bytes[9], bytes[10]]));
            if size + 11 != length {
                return Err(Failure::Storage);
            }
            ThreadDataset::parse(&bytes[11..length], channel).map_err(|_| Failure::Storage)?;
            (
                Some(Zeroizing::new(bytes[11..length].to_vec())),
                Some(channel),
            )
        } else {
            (None, None)
        };
        Ok(Self {
            store,
            dataset,
            dataset_channel,
            channel: None,
            observed: None,
            active: false,
            state: State::AwaitingCoordinator,
            fatal: false,
            apply_failed: false,
        })
    }
    fn fresh(&self, now: Instant) -> bool {
        self.observed
            .is_some_and(|time| now.saturating_duration_since(time) < OBSERVATION_TTL)
    }
    fn stop(&mut self, radio: Option<&dyn Radio>) -> Result<(), Failure> {
        if self.active {
            if let Some(radio) = radio {
                if radio.disable().is_err() {
                    self.fatal = true;
                    self.state = State::Error;
                    return Err(Failure::Transport);
                }
            }
        }
        self.active = false;
        Ok(())
    }
    pub fn fatal(&self) -> bool {
        self.fatal
    }
    pub fn tick(&mut self, radio: Option<&dyn Radio>, now: Instant) -> Result<(), Failure> {
        if !self.fresh(now) {
            self.channel = None;
            self.stop(radio)?;
            self.state = State::AwaitingCoordinator;
        } else if self.active {
            self.state = if radio.is_some_and(|r| r.attached()) {
                State::Attached
            } else {
                State::Attaching
            };
        }
        Ok(())
    }
    pub fn observe(
        &mut self,
        channel: u8,
        online: bool,
        radio: Option<&dyn Radio>,
        now: Instant,
    ) -> Result<Snapshot, Failure> {
        if (online && !(11..=26).contains(&channel))
            || (!online && channel != 0 && !(11..=26).contains(&channel))
        {
            return Err(Failure::Malformed);
        }
        self.observed = Some(now);
        self.channel = online.then_some(channel);
        if !online {
            self.stop(radio)?;
            self.state = State::AwaitingCoordinator;
            return Ok(self.snapshot());
        }
        if self.dataset_channel.is_some_and(|stored| stored != channel) {
            self.stop(radio)?;
            self.state = State::ChannelMismatch;
            return Err(Failure::NotReady);
        }
        // A failed apply stays stopped; heartbeats must not repeatedly write OT settings.
        if self.apply_failed || self.fatal {
            self.state = State::Error;
            return Err(Failure::Transport);
        }
        if let Some(radio) = radio {
            if self.dataset.is_some() && !self.active {
                self.restore(radio, now)?;
            }
        } else {
            self.state = State::Detached;
        }
        self.tick(radio, now)?;
        Ok(self.snapshot())
    }
    /// Call before initializing OpenThread, which otherwise restores local settings.
    pub fn boot_channel(&self, now: Instant) -> Option<u8> {
        self.channel.filter(|channel| {
            self.fresh(now)
                && !self.apply_failed
                && !self.fatal
                && self.state != State::Error
                && self.dataset_channel.is_none_or(|stored| stored == *channel)
        })
    }
    pub fn snapshot(&self) -> Snapshot {
        Snapshot {
            enabled: true,
            available: true,
            configured: self.dataset.is_some(),
            attached: self.state == State::Attached,
            state: self.state,
            zigbee_channel: self.channel,
            dataset_channel: self.dataset_channel,
            linux_ipv6_or_tun: false,
        }
    }
    pub fn dataset_copy(&self) -> Option<Zeroizing<Vec<u8>>> {
        self.dataset.clone()
    }
    fn activate(&mut self, radio: &dyn Radio, now: Instant) -> Result<(), Failure> {
        let channel = self.boot_channel(now).ok_or(Failure::NotReady)?;
        let bytes = self.dataset.as_ref().ok_or(Failure::NotReady)?;
        if radio.apply(bytes, channel).is_err() {
            // The record is complete and valid. Keep it for explicit retry/restart,
            // but ensure partial radio configuration cannot remain enabled.
            if radio.disable().is_err() {
                self.fatal = true;
            }
            self.active = false;
            self.state = State::Error;
            self.apply_failed = true;
            return Err(Failure::Transport);
        }
        self.active = true;
        self.state = State::Attaching;
        self.tick(Some(radio), now)
    }
    pub fn restore(&mut self, radio: &dyn Radio, now: Instant) -> Result<(), Failure> {
        if self.dataset.is_some() {
            self.activate(radio, now)?;
        }
        Ok(())
    }
    pub fn import(
        &mut self,
        bytes: &[u8],
        channel: u8,
        radio: &dyn Radio,
        now: Instant,
    ) -> Result<Snapshot, Failure> {
        if self.fatal {
            return Err(Failure::Transport);
        }
        if self.channel != Some(channel) || !self.fresh(now) {
            return Err(Failure::NotReady);
        }
        ThreadDataset::parse(bytes, channel).map_err(|_| Failure::Malformed)?;
        if let Some(existing) = &self.dataset {
            if existing.as_slice() != bytes {
                return Err(Failure::Busy);
            }
            if self.active {
                return Ok(self.snapshot());
            }
            // Explicit retry can clear the error gate; heartbeat alone cannot.
            self.state = State::Detached;
            self.apply_failed = false;
        } else {
            let mut record = Zeroizing::new(Vec::with_capacity(bytes.len() + 11));
            record.extend(MAGIC);
            record.push(channel);
            record.extend((bytes.len() as u16).to_le_bytes());
            record.extend(bytes);
            self.store
                .set(DATASET_KEY, Some(&record))
                .map_err(|_| Failure::Storage)?;
            self.dataset = Some(Zeroizing::new(bytes.to_vec()));
            self.dataset_channel = Some(channel);
        }
        self.activate(radio, now)?;
        Ok(self.snapshot())
    }
    pub fn create<R: TryRng + TryCryptoRng>(
        &mut self,
        channel: u8,
        entropy: &mut R,
        radio: &dyn Radio,
        now: Instant,
    ) -> Result<Snapshot, Failure> {
        if self.channel != Some(channel) || !self.fresh(now) {
            return Err(Failure::NotReady);
        }
        if self.dataset.is_some() {
            return Err(Failure::Busy);
        }
        let bytes = generate(channel, entropy)?;
        self.import(&bytes, channel, radio, now)
    }
}
fn generate<R: TryRng + TryCryptoRng>(
    channel: u8,
    entropy: &mut R,
) -> Result<Zeroizing<Vec<u8>>, Failure> {
    if !(11..=26).contains(&channel) {
        return Err(Failure::Malformed);
    }
    let mut pan = 0xffffu16;
    for _ in 0..4 {
        pan = entropy.try_next_u32().map_err(|_| Failure::Transport)? as u16;
        if pan != 0 && pan != 0xffff {
            break;
        }
    }
    if pan == 0 || pan == 0xffff {
        return Err(Failure::Transport);
    }
    let mut random = Zeroizing::new([0; 47]);
    entropy
        .try_fill_bytes(&mut random[..])
        .map_err(|_| Failure::Transport)?;
    let prefix = [
        0xfd, random[40], random[41], random[42], random[43], random[44], random[45], random[46],
    ];
    let mask = (1u32 << channel).to_be_bytes();
    let mut bytes = Zeroizing::new(Vec::new());
    for (kind, value) in [
        (0u8, &[0, 0, channel][..]),
        (1, &pan.to_be_bytes()[..]),
        (2, &random[..8]),
        (3, &b"NativeThread"[..]),
        (4, &random[8..24]),
        (5, &random[24..40]),
        (7, &prefix[..]),
        (12, &[2, 160, 0xff, 0xf8][..]),
        (14, &[0, 0, 0, 0, 0, 1, 0, 1][..]),
        (53, &[0, 4, mask[0], mask[1], mask[2], mask[3]][..]),
    ] {
        bytes.extend([kind, value.len() as u8]);
        bytes.extend(value);
    }
    ThreadDataset::parse(&bytes, channel).map_err(|_| Failure::Transport)?;
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::entropy::OsEntropy;
    use std::cell::Cell;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU32, Ordering};
    static NEXT: AtomicU32 = AtomicU32::new(0);
    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!(
                "matter-thread-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            )))
        }
    }
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    #[derive(Default)]
    struct FakeRadio {
        applies: Cell<u32>,
        disables: Cell<u32>,
        attached: Cell<bool>,
        fail: Cell<bool>,
    }
    impl Radio for FakeRadio {
        fn apply(&self, _: &[u8], _: u8) -> Result<(), Failure> {
            self.applies.set(self.applies.get() + 1);
            if self.fail.get() {
                Err(Failure::Transport)
            } else {
                Ok(())
            }
        }
        fn disable(&self) -> Result<(), Failure> {
            self.disables.set(self.disables.get() + 1);
            Ok(())
        }
        fn attached(&self) -> bool {
            self.attached.get()
        }
    }
    #[test]
    fn fresh_observation_gates_creation_and_expiry_stops_radio_without_flash_writes() {
        let dir = Directory::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        let mut lifecycle = Lifecycle::load(store).unwrap();
        let radio = FakeRadio::default();
        let now = Instant::now();
        let mut entropy = OsEntropy::open().unwrap();
        assert!(matches!(
            lifecycle.create(15, &mut entropy, &radio, now),
            Err(Failure::NotReady)
        ));
        assert_eq!(radio.applies.get(), 0);
        lifecycle.observe(15, true, Some(&radio), now).unwrap();
        lifecycle.create(15, &mut entropy, &radio, now).unwrap();
        assert!(matches!(lifecycle.snapshot().state, State::Attaching));
        let committed = std::fs::read(dir.0.join("credentials.bin")).unwrap();
        radio.attached.set(true);
        lifecycle.tick(Some(&radio), now).unwrap();
        assert!(lifecycle.snapshot().attached);
        lifecycle
            .tick(Some(&radio), now + Duration::from_secs(5))
            .unwrap();
        assert_eq!(radio.disables.get(), 1);
        assert!(matches!(
            lifecycle.snapshot().state,
            State::AwaitingCoordinator
        ));
        assert_eq!(
            std::fs::read(dir.0.join("credentials.bin")).unwrap(),
            committed
        );
        assert!(lifecycle
            .boot_channel(now + Duration::from_secs(5))
            .is_none());
    }
    #[test]
    fn mismatch_malformed_and_replacement_requests_do_not_mutate_state() {
        let dir = Directory::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        let mut lifecycle = Lifecycle::load(store).unwrap();
        let radio = FakeRadio::default();
        let now = Instant::now();
        lifecycle.observe(15, true, Some(&radio), now).unwrap();
        let bytes = crate::commissioning::fixture_dataset();
        assert!(matches!(
            lifecycle.import(&bytes, 11, &radio, now),
            Err(Failure::NotReady)
        ));
        assert!(matches!(
            lifecycle.import(&bytes[..3], 15, &radio, now),
            Err(Failure::Malformed)
        ));
        assert_eq!(radio.applies.get(), 0);
        lifecycle.import(&bytes, 15, &radio, now).unwrap();
        let before = std::fs::read(dir.0.join("credentials.bin")).unwrap();
        let other = generate(15, &mut OsEntropy::open().unwrap()).unwrap();
        assert!(matches!(
            lifecycle.import(&other, 15, &radio, now),
            Err(Failure::Busy)
        ));
        assert!(matches!(
            lifecycle.observe(11, true, Some(&radio), now),
            Err(Failure::NotReady)
        ));
        assert!(matches!(lifecycle.snapshot().state, State::ChannelMismatch));
        assert_eq!(radio.applies.get(), 1);
        assert_eq!(
            std::fs::read(dir.0.join("credentials.bin")).unwrap(),
            before
        );
    }
    #[test]
    fn restart_needs_fresh_matching_channel_and_idempotent_import_does_not_write() {
        let dir = Directory::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        let radio = FakeRadio::default();
        let now = Instant::now();
        let bytes = crate::commissioning::fixture_dataset();
        {
            let mut lifecycle = Lifecycle::load(store).unwrap();
            lifecycle.observe(15, true, Some(&radio), now).unwrap();
            lifecycle.import(&bytes, 15, &radio, now).unwrap();
        }
        let before = std::fs::read(dir.0.join("credentials.bin")).unwrap();
        let store = PrivateStore::open(&dir.0).unwrap();
        let mut lifecycle = Lifecycle::load(store).unwrap();
        assert!(lifecycle.boot_channel(now).is_none());
        assert!(matches!(
            lifecycle.observe(11, true, None, now),
            Err(Failure::NotReady)
        ));
        assert!(lifecycle.boot_channel(now).is_none());
        lifecycle.observe(15, true, None, now).unwrap();
        assert_eq!(lifecycle.boot_channel(now), Some(15));
        lifecycle.restore(&radio, now).unwrap();
        lifecycle.import(&bytes, 15, &radio, now).unwrap();
        assert_eq!(
            std::fs::read(dir.0.join("credentials.bin")).unwrap(),
            before
        );
        lifecycle.observe(0, false, Some(&radio), now).unwrap();
        assert!(matches!(
            lifecycle.snapshot().state,
            State::AwaitingCoordinator
        ));
    }
    #[test]
    fn failed_apply_is_stopped_and_heartbeats_do_not_retry_or_rewrite() {
        let dir = Directory::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        let radio = FakeRadio::default();
        let now = Instant::now();
        let mut lifecycle = Lifecycle::load(store).unwrap();
        lifecycle.observe(15, true, Some(&radio), now).unwrap();
        radio.fail.set(true);
        let bytes = crate::commissioning::fixture_dataset();
        assert!(matches!(
            lifecycle.import(&bytes, 15, &radio, now),
            Err(Failure::Transport)
        ));
        assert_eq!(radio.disables.get(), 1);
        assert!(matches!(lifecycle.snapshot().state, State::Error));
        let before = std::fs::read(dir.0.join("credentials.bin")).unwrap();
        assert!(matches!(
            lifecycle.observe(15, true, Some(&radio), now),
            Err(Failure::Transport)
        ));
        assert_eq!(radio.applies.get(), 1);
        assert_eq!(
            std::fs::read(dir.0.join("credentials.bin")).unwrap(),
            before
        );
        let later = now + Duration::from_secs(6);
        lifecycle.tick(Some(&radio), later).unwrap();
        assert!(matches!(
            lifecycle.observe(15, true, Some(&radio), later),
            Err(Failure::Transport)
        ));
        assert_eq!(radio.applies.get(), 1);
        radio.fail.set(false);
        lifecycle.import(&bytes, 15, &radio, later).unwrap();
        assert_eq!(radio.applies.get(), 2);
    }
    #[test]
    fn corrupt_private_record_is_not_silently_replaced() {
        let dir = Directory::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        store.set(DATASET_KEY, Some(b"bad dataset record")).unwrap();
        let before = std::fs::read(dir.0.join("credentials.bin")).unwrap();
        assert!(Lifecycle::load(store).is_err());
        assert_eq!(
            std::fs::read(dir.0.join("credentials.bin")).unwrap(),
            before
        );
    }
}
