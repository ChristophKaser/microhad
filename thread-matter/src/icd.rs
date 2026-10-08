//! Durable ICD Check-In client state and authenticated wake processing.
use crate::store::PrivateStore;
use rs_matter::{
    crypto::{CanonAeadKeyRef, Crypto},
    dm::clusters::icd_mgmt::{ClientTypeEnum, IcdManagementClient},
    error::{Error, ErrorCode},
    sc::checkin::CheckIn,
    tlv::OctetStr,
    transport::exchange::Exchange,
    IcdCheckIn, Matter,
};
use std::num::NonZeroU8;
use zeroize::Zeroizing;

pub const ICD_CLIENTS_KEY: u16 = 0x1003;
pub const MAX_ICD_CLIENTS: usize = 64;
const MAGIC: &[u8; 8] = b"TMICD\x01\0\0";
const RECORD_BYTES: usize = 35;
const MAX_STORE_BYTES: usize = 8 + 1 + MAX_ICD_CLIENTS * RECORD_BYTES;
const REFRESH_OFFSET: u32 = 1 << 31;

/// Exercise the worst-case sessionless key scan in a disposable fixture store.
/// This is included only in the synthetic commissioning probe, never service.
#[cfg(feature = "controller-probe")]
pub fn profile_unknown_identity_lookup<C: Crypto>(
    crypto: &C,
    directory: &std::path::Path,
    trials: usize,
) -> Result<std::time::Duration, Error> {
    use rs_matter::crypto::CanonAeadKeyRef;
    use std::num::NonZeroU8;
    use std::time::Instant;

    if trials == 0 || trials > 64 {
        return Err(invalid());
    }
    let store = PrivateStore::open(directory).map_err(storage)?;
    let mut registry = ClientRegistry::open(store.clone())?;
    for index in 0..MAX_ICD_CLIENTS {
        let mut key = [0; 16];
        key[..8].copy_from_slice(&(index as u64 + 1).to_le_bytes());
        registry.install(
            NonZeroU8::new(1).ok_or_else(invalid)?,
            index as u64 + 1,
            0,
            key,
            0,
        )?;
    }

    let unknown_key = [0xee; 16];
    let check_in = CheckIn::new(CanonAeadKeyRef::new(&unknown_key));
    let mut payload = vec![0; CheckIn::payload_len(2)];
    check_in
        .generate(crypto, 1, &250u16.to_le_bytes(), &mut payload)
        .map_err(|_| invalid())?;
    let message = IcdCheckIn::from_received(0, 0, &payload)?;
    let started = Instant::now();
    for _ in 0..trials {
        match std::hint::black_box(registry.accept(crypto, &message)) {
            Err(error) if error.code() == ErrorCode::NotFound => (),
            _ => return Err(invalid()),
        }
    }
    let elapsed = started.elapsed();
    drop(registry);
    drop(store);
    std::fs::remove_dir_all(directory).map_err(storage)?;
    Ok(elapsed)
}

fn invalid() -> Error {
    ErrorCode::InvalidData.into()
}

fn storage(_: std::io::Error) -> Error {
    ErrorCode::Failure.into()
}

/// One client registration on a single fabric. The key never leaves this
/// object except for constructing the bounded RegisterClient request.
pub struct Registration {
    pub fabric: NonZeroU8,
    pub node: u64,
    pub endpoint: u16,
    key: Zeroizing<[u8; 16]>,
    pub starting_counter: u32,
    pub last_offset: u32,
}

#[derive(Debug)]
pub struct Wake {
    pub fabric: NonZeroU8,
    pub node: u64,
    pub counter: u32,
    pub active_mode_threshold_ms: u16,
    pub refresh_required: bool,
}

/// Bounded registrations for up to 64 operational ICDs. Check-In replay state
/// is committed before a wake is reported to the caller.
pub struct ClientRegistry {
    store: PrivateStore,
    registrations: Vec<Registration>,
}

impl ClientRegistry {
    pub fn open(store: PrivateStore) -> Result<Self, Error> {
        let mut bytes = Zeroizing::new([0; MAX_STORE_BYTES]);
        let Some(size) = store
            .get(ICD_CLIENTS_KEY, &mut bytes[..])
            .map_err(storage)?
        else {
            return Ok(Self {
                store,
                registrations: Vec::new(),
            });
        };
        let mut reader = Reader {
            bytes: &bytes[..size],
            offset: 0,
        };
        if reader.take(MAGIC.len())? != MAGIC {
            return Err(invalid());
        }
        let count = usize::from(reader.byte()?);
        if count > MAX_ICD_CLIENTS || size != MAGIC.len() + 1 + count * RECORD_BYTES {
            return Err(invalid());
        }
        let mut registrations = Vec::with_capacity(count);
        for _ in 0..count {
            let fabric = NonZeroU8::new(reader.byte()?).ok_or_else(invalid)?;
            let node = reader.u64()?;
            let endpoint = reader.u16()?;
            let mut key = Zeroizing::new([0; 16]);
            key.copy_from_slice(reader.take(16)?);
            let starting_counter = reader.u32()?;
            let last_offset = reader.u32()?;
            if registrations
                .iter()
                .any(|entry: &Registration| entry.fabric == fabric && entry.node == node)
            {
                return Err(invalid());
            }
            registrations.push(Registration {
                fabric,
                node,
                endpoint,
                key,
                starting_counter,
                last_offset,
            });
        }
        if reader.offset != size {
            return Err(invalid());
        }
        Ok(Self {
            store,
            registrations,
        })
    }

    pub fn len(&self) -> usize {
        self.registrations.len()
    }

    pub fn is_empty(&self) -> bool {
        self.registrations.is_empty()
    }

    pub fn is_registered(&self, fabric: NonZeroU8, node: u64) -> bool {
        self.registration(fabric, node).is_some()
    }

    pub fn registered_nodes(&self, fabric: NonZeroU8) -> Vec<u64> {
        self.registrations
            .iter()
            .filter(|registration| registration.fabric == fabric)
            .map(|registration| registration.node)
            .collect()
    }

    /// Register a permanent client with one ICD and atomically retain the
    /// returned starting counter together with the key used in RegisterClient.
    pub async fn register<C: Crypto>(
        &mut self,
        matter: &Matter<'_>,
        crypto: &C,
        fabric: NonZeroU8,
        node: u64,
        endpoint: u16,
        local_node: u64,
        entropy: &mut crate::entropy::OsEntropy,
    ) -> Result<(), Error> {
        use rs_matter::crypto::TryRng;

        let mut key = Zeroizing::new([0; 16]);
        entropy
            .try_fill_bytes(&mut key[..])
            .map_err(|_| Error::from(ErrorCode::Failure))?;
        let handle = Exchange::initiate(matter, crypto, fabric, node)
            .await?
            .icd_management()
            .register_client(endpoint, |request| {
                request
                    .check_in_node_id(local_node)?
                    .monitored_subject(local_node)?
                    .key(OctetStr::new(&key[..]))?
                    .verification_key(None)?
                    .client_type(ClientTypeEnum::Permanent)?
                    .end()
            })
            .await?;
        let starting_counter = handle.response()?.icd_counter()?;
        handle.complete().await?;
        self.install(fabric, node, endpoint, *key, starting_counter)?;
        Ok(())
    }

    pub fn registration(&self, fabric: NonZeroU8, node: u64) -> Option<&Registration> {
        self.registrations
            .iter()
            .find(|entry| entry.fabric == fabric && entry.node == node)
    }

    /// Commit a new key and the RegisterClientResponse counter together. An
    /// existing record is replaced atomically after a successful response.
    pub fn install(
        &mut self,
        fabric: NonZeroU8,
        node: u64,
        endpoint: u16,
        key: [u8; 16],
        starting_counter: u32,
    ) -> Result<(), Error> {
        if key.iter().all(|byte| *byte == 0) {
            return Err(invalid());
        }
        let existing = self
            .registrations
            .iter()
            .position(|entry| entry.fabric == fabric && entry.node == node);
        if existing.is_none() && self.registrations.len() == MAX_ICD_CLIENTS {
            return Err(ErrorCode::ResourceExhausted.into());
        }
        let registration = Registration {
            fabric,
            node,
            endpoint,
            key: Zeroizing::new(key),
            starting_counter,
            last_offset: 0,
        };
        if let Some(index) = existing {
            let previous = std::mem::replace(&mut self.registrations[index], registration);
            if let Err(error) = self.persist() {
                self.registrations[index] = previous;
                return Err(error);
            }
        } else {
            self.registrations.push(registration);
            if let Err(error) = self.persist() {
                self.registrations.pop();
                return Err(error);
            }
        }
        Ok(())
    }

    /// Authenticate one Check-In, enforce the spec's strictly increasing
    /// offset rule, and durably save that offset before returning a wake.
    pub fn accept<C: Crypto>(&mut self, crypto: &C, message: &IcdCheckIn) -> Result<Wake, Error> {
        let payload_len = message.payload().len();
        let mut authenticated = None;
        for (index, registration) in self.registrations.iter().enumerate() {
            if (message.fabric != 0 && registration.fabric.get() != message.fabric)
                || (message.node != 0 && registration.node != message.node)
            {
                continue;
            }
            let mut payload = Zeroizing::new([0; rs_matter::MAX_ICD_CHECK_IN_BYTES]);
            payload[..payload_len].copy_from_slice(message.payload());
            if let Ok(parsed) = CheckIn::new(CanonAeadKeyRef::new(&registration.key))
                .parse(crypto, &mut payload[..payload_len])
            {
                if parsed.app_data.len() != 2 {
                    return Err(invalid());
                }
                authenticated = Some((
                    index,
                    parsed.counter,
                    u16::from_le_bytes(parsed.app_data.try_into().unwrap()),
                ));
                break;
            }
        }
        let (index, counter, active_mode_threshold_ms) =
            authenticated.ok_or_else(|| Error::from(ErrorCode::NotFound))?;
        let (fabric, node, starting_counter, previous_offset) = {
            let registration = &self.registrations[index];
            (
                registration.fabric,
                registration.node,
                registration.starting_counter,
                registration.last_offset,
            )
        };
        let offset = counter.wrapping_sub(starting_counter);
        if offset <= previous_offset {
            return Err(ErrorCode::Duplicate.into());
        }
        self.registrations[index].last_offset = offset;
        if let Err(error) = self.persist() {
            self.registrations[index].last_offset = previous_offset;
            return Err(error);
        }
        Ok(Wake {
            fabric,
            node,
            counter,
            active_mode_threshold_ms,
            refresh_required: offset >= REFRESH_OFFSET,
        })
    }

    fn persist(&self) -> Result<(), Error> {
        let mut bytes =
            Vec::with_capacity(MAGIC.len() + 1 + self.registrations.len() * RECORD_BYTES);
        bytes.extend_from_slice(MAGIC);
        bytes.push(self.registrations.len() as u8);
        for registration in &self.registrations {
            bytes.push(registration.fabric.get());
            bytes.extend_from_slice(&registration.node.to_le_bytes());
            bytes.extend_from_slice(&registration.endpoint.to_le_bytes());
            bytes.extend_from_slice(registration.key.as_ref());
            bytes.extend_from_slice(&registration.starting_counter.to_le_bytes());
            bytes.extend_from_slice(&registration.last_offset.to_le_bytes());
        }
        self.store
            .set(ICD_CLIENTS_KEY, Some(&bytes))
            .map_err(storage)
    }
}

struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, size: usize) -> Result<&'a [u8], Error> {
        let end = self.offset.checked_add(size).ok_or_else(invalid)?;
        let value = self.bytes.get(self.offset..end).ok_or_else(invalid)?;
        self.offset = end;
        Ok(value)
    }

    fn byte(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }

    fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }

    fn u32(&mut self) -> Result<u32, Error> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }

    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rs_matter::crypto::{default_crypto, CryptoSensitiveRef};
    use std::os::unix::fs::PermissionsExt;

    fn fixture_dir() -> std::path::PathBuf {
        let directory = std::env::temp_dir().join(format!(
            "matter-icd-registry-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&directory).unwrap();
        std::fs::set_permissions(&directory, std::fs::Permissions::from_mode(0o700)).unwrap();
        directory
    }

    fn wire_check_in<C: Crypto>(crypto: &C, key: &[u8; 16], counter: u32) -> Vec<u8> {
        let check_in = CheckIn::new(CanonAeadKeyRef::new(key));
        let mut payload = vec![0; CheckIn::payload_len(2)];
        check_in
            .generate(crypto, counter, &250u16.to_le_bytes(), &mut payload)
            .unwrap();
        payload
    }

    fn incoming(node: u64, payload: Vec<u8>) -> IcdCheckIn {
        IcdCheckIn::from_received(1, node, &payload).unwrap()
    }

    #[test]
    fn authenticated_check_in_is_persisted_before_wake_and_replay_is_rejected() {
        let directory = fixture_dir();
        let store = PrivateStore::open(&directory).unwrap();
        let mut registry = ClientRegistry::open(store.clone()).unwrap();
        let key = [0x42; 16];
        registry
            .install(NonZeroU8::new(1).unwrap(), 0x1234, 0, key, u32::MAX - 2)
            .unwrap();
        let crypto = default_crypto(
            crate::entropy::OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[0x61; 32]),
        );
        let message = incoming(0x1234, wire_check_in(&crypto, &key, 1));
        let wake = registry.accept(&crypto, &message).unwrap();
        assert_eq!(wake.counter, 1);
        assert_eq!(wake.active_mode_threshold_ms, 250);
        assert!(!wake.refresh_required);
        let refresh = incoming(0x1234, wire_check_in(&crypto, &key, 0x8000_0000));
        assert!(registry.accept(&crypto, &refresh).unwrap().refresh_required);

        drop(registry);
        let mut restarted = ClientRegistry::open(store).unwrap();
        assert_eq!(
            restarted
                .registration(NonZeroU8::new(1).unwrap(), 0x1234)
                .unwrap()
                .last_offset,
            0x8000_0003
        );
        assert_eq!(
            restarted.accept(&crypto, &message).unwrap_err().code(),
            ErrorCode::Duplicate
        );
        drop(restarted);
        std::fs::remove_dir_all(directory).unwrap();
    }

    #[test]
    fn sessionless_check_in_uses_the_key_to_identify_its_registered_fabric_and_node() {
        let directory = fixture_dir();
        let store = PrivateStore::open(&directory).unwrap();
        let mut registry = ClientRegistry::open(store).unwrap();
        let first_key = [0x31; 16];
        let second_key = [0x72; 16];
        registry
            .install(NonZeroU8::new(1).unwrap(), 0x1234, 0, first_key, 10)
            .unwrap();
        registry
            .install(NonZeroU8::new(2).unwrap(), 0x1234, 0, second_key, 20)
            .unwrap();
        let crypto = default_crypto(
            crate::entropy::OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[0x61; 32]),
        );
        let payload = wire_check_in(&crypto, &second_key, 21);
        let sessionless = IcdCheckIn::from_received(0, 0, &payload).unwrap();

        let wake = registry.accept(&crypto, &sessionless).unwrap();
        assert_eq!(wake.fabric.get(), 2);
        assert_eq!(wake.node, 0x1234);
        assert_eq!(wake.counter, 21);
    }

    #[test]
    fn bad_mic_and_unregistered_peers_never_produce_wakes() {
        let directory = fixture_dir();
        let store = PrivateStore::open(&directory).unwrap();
        let mut registry = ClientRegistry::open(store).unwrap();
        let key = [0x24; 16];
        registry
            .install(NonZeroU8::new(1).unwrap(), 0x2233, 1, key, 10)
            .unwrap();
        let crypto = default_crypto(
            crate::entropy::OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[0x16; 32]),
        );
        let mut invalid_payload = wire_check_in(&crypto, &key, 11);
        invalid_payload[13] ^= 1;
        assert!(registry
            .accept(&crypto, &incoming(0x2233, invalid_payload))
            .is_err());
        let mut invalid_sessionless = wire_check_in(&crypto, &key, 11);
        invalid_sessionless[13] ^= 1;
        let unknown_identity = IcdCheckIn::from_received(0, 0, &invalid_sessionless).unwrap();
        assert!(registry.accept(&crypto, &unknown_identity).is_err());
        assert!(registry
            .accept(&crypto, &incoming(0x9988, wire_check_in(&crypto, &key, 11)))
            .is_err());
        assert_eq!(
            registry
                .registration(NonZeroU8::new(1).unwrap(), 0x2233)
                .unwrap()
                .last_offset,
            0
        );
        drop(registry);
        std::fs::remove_dir_all(directory).unwrap();
    }
}
