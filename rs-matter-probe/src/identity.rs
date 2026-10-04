//! Persisted controller authority and monotonic node allocation, never logged.
use crate::store::PrivateStore;
use rs_matter::cert::gen::VALID_FOREVER;
use rs_matter::cert::{CertRef, MAX_CERT_TLV_AND_ASN1_LEN, MAX_CERT_TLV_LEN};
use rs_matter::crypto::{
    CanonAeadKey, CanonPkcPublicKey, CanonPkcSecretKey, Crypto, PublicKey, Rng, SecretKey,
    SigningSecretKey,
};
use rs_matter::dm::clusters::time_sync::UtcTime;
use rs_matter::error::{Error, ErrorCode};
use rs_matter::fabric::FabricPersist;
use rs_matter::onboard::cac::{IcacGenerator, RcacGenerator};
use rs_matter::onboard::noc::NocGenerator;
use rs_matter::tlv::TLVElement;
use rs_matter::Matter;
use std::num::NonZeroU8;
use zeroize::Zeroizing;

pub const AUTHORITY_KEY: u16 = 0x1000;
pub const MAX_NODE: u64 = 0xffff_ffef_ffff_ffff;
const MAGIC: &[u8; 8] = b"TMID\x01\0\0\0";
const MAX_RECORD: usize = 152 + 3 * MAX_CERT_TLV_LEN;
pub struct Identity {
    fabric_id: u64,
    controller: u64,
    next_node: u64,
    vendor: u16,
    root_secret: CanonPkcSecretKey,
    signing_secret: CanonPkcSecretKey,
    operational_secret: CanonPkcSecretKey,
    ipk: CanonAeadKey,
    root: Vec<u8>,
    intermediate: Vec<u8>,
    noc: Vec<u8>,
}
fn invalid() -> Error {
    ErrorCode::InvalidData.into()
}
fn storage(_: std::io::Error) -> Error {
    ErrorCode::Failure.into()
}
struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}
impl<'a> Reader<'a> {
    fn take(&mut self, size: usize) -> Result<&'a [u8], Error> {
        let end = self.offset.checked_add(size).ok_or_else(invalid)?;
        let bytes = self.bytes.get(self.offset..end).ok_or_else(invalid)?;
        self.offset = end;
        Ok(bytes)
    }
    fn u64(&mut self) -> Result<u64, Error> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    fn u16(&mut self) -> Result<u16, Error> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn cert(&mut self) -> Result<Vec<u8>, Error> {
        let size = usize::from(self.u16()?);
        if size == 0 || size > MAX_CERT_TLV_LEN {
            return Err(invalid());
        }
        Ok(self.take(size)?.to_vec())
    }
}
impl Identity {
    pub fn open<C: Crypto>(store: &PrivateStore, crypto: &C, vendor: u16) -> Result<Self, Error> {
        if vendor == 0 || vendor == 0xffff {
            return Err(invalid());
        }
        let mut bytes = Zeroizing::new([0; MAX_RECORD]);
        if let Some(size) = store.get(AUTHORITY_KEY, &mut bytes[..]).map_err(storage)? {
            let identity = Self::decode(&bytes[..size])?;
            if identity.vendor != vendor {
                return Err(invalid());
            }
            identity.validate(crypto)?;
            return Ok(identity);
        }
        // Never silently create a different authority for an existing fabric.
        if store.contains_in(0..256).map_err(storage)?
            || store
                .contains_in(crate::catalog::CATALOG_KEY..crate::catalog::CATALOG_KEY + 1)
                .map_err(storage)?
        {
            return Err(invalid());
        }
        let identity = Self::create(crypto, vendor)?;
        identity.validate(crypto)?;
        store
            .set(AUTHORITY_KEY, Some(&identity.encode(identity.next_node)))
            .map_err(storage)?;
        Ok(identity)
    }
    fn create<C: Crypto>(crypto: &C, vendor: u16) -> Result<Self, Error> {
        let mut fabric_id = 0;
        for _ in 0..4 {
            fabric_id = crypto.rand()?.next_u64();
            if fabric_id != 0 {
                break;
            }
        }
        if fabric_id == 0 {
            return Err(invalid());
        }
        let mut root_buffer = [0; MAX_CERT_TLV_AND_ASN1_LEN];
        let mut root_generator = RcacGenerator::new(&mut root_buffer);
        let (root_secret, root) = root_generator.generate(crypto, fabric_id, VALID_FOREVER)?;
        let mut intermediate_buffer = [0; MAX_CERT_TLV_AND_ASN1_LEN];
        let mut intermediate_generator = IcacGenerator::new(&mut intermediate_buffer);
        let (signing_secret, intermediate) = intermediate_generator.generate(
            crypto,
            root_secret.reference(),
            root,
            VALID_FOREVER,
        )?;
        let key = crypto.generate_secret_key()?;
        let mut operational_secret = CanonPkcSecretKey::new();
        key.write_canon(&mut operational_secret)?;
        let mut csr_buffer = [0; 256];
        let csr = key.csr(&mut csr_buffer)?;
        let mut noc_buffer = [0; MAX_CERT_TLV_AND_ASN1_LEN];
        let mut noc_generator = NocGenerator::create(
            signing_secret.reference(),
            root,
            intermediate,
            &mut noc_buffer,
        )?;
        let noc = noc_generator
            .generate(crypto, csr, 1, &[], VALID_FOREVER)?
            .to_vec();
        let mut ipk = CanonAeadKey::new();
        crypto.rand()?.fill_bytes(ipk.access_mut());
        Ok(Self {
            fabric_id,
            controller: 1,
            next_node: 2,
            vendor,
            root_secret,
            signing_secret,
            operational_secret,
            ipk,
            root: root.to_vec(),
            intermediate: intermediate.to_vec(),
            noc,
        })
    }
    fn encode(&self, next: u64) -> Zeroizing<Vec<u8>> {
        let mut bytes = Zeroizing::new(Vec::with_capacity(MAX_RECORD));
        bytes.extend(MAGIC);
        bytes.extend(self.fabric_id.to_le_bytes());
        bytes.extend(self.controller.to_le_bytes());
        bytes.extend(next.to_le_bytes());
        bytes.extend(self.vendor.to_le_bytes());
        bytes.extend(self.root_secret.access());
        bytes.extend(self.signing_secret.access());
        bytes.extend(self.operational_secret.access());
        bytes.extend(self.ipk.access());
        for cert in [&self.root, &self.intermediate, &self.noc] {
            bytes.extend((cert.len() as u16).to_le_bytes());
            bytes.extend(cert);
        }
        bytes
    }
    fn decode(bytes: &[u8]) -> Result<Self, Error> {
        if bytes.len() > MAX_RECORD {
            return Err(invalid());
        }
        let mut reader = Reader { bytes, offset: 0 };
        if reader.take(8)? != MAGIC {
            return Err(invalid());
        }
        let identity = Self {
            fabric_id: reader.u64()?,
            controller: reader.u64()?,
            next_node: reader.u64()?,
            vendor: reader.u16()?,
            root_secret: CanonPkcSecretKey::try_from(reader.take(32)?)?,
            signing_secret: CanonPkcSecretKey::try_from(reader.take(32)?)?,
            operational_secret: CanonPkcSecretKey::try_from(reader.take(32)?)?,
            ipk: CanonAeadKey::try_from(reader.take(16)?)?,
            root: reader.cert()?,
            intermediate: reader.cert()?,
            noc: reader.cert()?,
        };
        if reader.offset != bytes.len() {
            return Err(invalid());
        }
        Ok(identity)
    }
    fn validate<C: Crypto>(&self, crypto: &C) -> Result<(), Error> {
        if self.fabric_id == 0
            || self.controller != 1
            || !(2..=MAX_NODE + 1).contains(&self.next_node)
            || self.vendor == 0
            || self.vendor == 0xffff
        {
            return Err(invalid());
        }
        let root = CertRef::new(TLVElement::new(&self.root));
        let intermediate = CertRef::new(TLVElement::new(&self.intermediate));
        let noc = CertRef::new(TLVElement::new(&self.noc));
        for (cert, secret) in [
            (&root, &self.root_secret),
            (&intermediate, &self.signing_secret),
            (&noc, &self.operational_secret),
        ] {
            if cert.get_fabric_id()? != self.fabric_id {
                return Err(invalid());
            }
            let key = crypto.secret_key(secret.reference())?;
            let mut public = CanonPkcPublicKey::new();
            key.pub_key()?.write_canon(&mut public)?;
            if cert.pubkey()? != public.access() {
                return Err(invalid());
            }
        }
        if noc.get_node_id()? != self.controller {
            return Err(invalid());
        }
        let mut scratch = [0; MAX_CERT_TLV_AND_ASN1_LEN];
        noc.verify_chain_start(crypto, UtcTime::LastKnown(0))
            .add_cert(&intermediate, &mut scratch)?
            .add_cert(&root, &mut scratch)?
            .finalise(&mut scratch)?;
        Ok(())
    }
    pub fn install<C: Crypto>(
        &self,
        matter: &Matter<'_>,
        crypto: &C,
        store: &PrivateStore,
    ) -> Result<NonZeroU8, Error> {
        let existing = matter.with_state(|state| {
            let mut fabrics = state.fabrics.iter();
            let Some(fabric) = fabrics.next() else {
                return Ok(None);
            };
            if fabrics.next().is_some()
                || fabric.fabric_id() != self.fabric_id
                || fabric.node_id() != self.controller
                || fabric.vendor_id() != self.vendor
                || fabric.root_ca() != self.root
                || fabric.icac() != self.intermediate
                || fabric.noc() != self.noc
                || fabric.secret_key().access() != self.operational_secret.access()
                || fabric.ipk().epoch_key().access() != self.ipk.access()
            {
                return Err(invalid());
            }
            Ok(Some(fabric.fab_idx()))
        })?;
        if let Some(index) = existing {
            return Ok(index);
        }
        // An unloaded persisted fabric is corruption, not an interrupted install.
        if store.contains_in(0..256).map_err(storage)? {
            return Err(invalid());
        }
        let index = matter.with_state(|state| {
            state
                .fabrics
                .add(
                    crypto,
                    self.operational_secret.reference(),
                    &self.root,
                    &self.noc,
                    &self.intermediate,
                    Some(self.ipk.reference()),
                    self.vendor,
                    self.controller,
                )
                .map(|f| f.fab_idx())
        })?;
        let kv = matter.kv(store.clone());
        let mut persist = FabricPersist::new(&kv);
        matter.with_state(|state| persist.store(state.fabrics.get(index).unwrap()))?;
        persist.run()?;
        Ok(index)
    }
    pub fn fabric_id(&self) -> u64 {
        self.fabric_id
    }
    pub fn next_node(&self) -> u64 {
        self.next_node
    }
    pub fn allocate_node(&mut self, store: &PrivateStore) -> Result<u64, Error> {
        if self.next_node > MAX_NODE {
            return Err(ErrorCode::NoSpace.into());
        }
        let allocated = self.next_node;
        let next = allocated + 1;
        store
            .set(AUTHORITY_KEY, Some(&self.encode(next)))
            .map_err(storage)?;
        self.next_node = next;
        Ok(allocated)
    }
    pub fn signer<'a>(&'a self, scratch: &'a mut [u8]) -> Result<NocGenerator<'a>, Error> {
        NocGenerator::create(
            self.signing_secret.reference(),
            &self.root,
            &self.intermediate,
            scratch,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::entropy::OsEntropy;
    use rs_matter::crypto::{default_crypto, CryptoSensitiveRef};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU32, Ordering};
    static NEXT: AtomicU32 = AtomicU32::new(0);
    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!(
                "matter-identity-{}-{}",
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
    #[test]
    fn create_reopen_validate_no_read_writes_and_monotonic_allocation() {
        let directory = Directory::new();
        let store = PrivateStore::open(&directory.0).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[1; 32]),
        );
        let mut identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        let original = identity.encode(2);
        assert_eq!(identity.allocate_node(&store).unwrap(), 2);
        drop(identity);
        drop(store);
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        let store = PrivateStore::open(&directory.0).unwrap();
        let mut identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        assert_eq!(identity.encode(2).as_slice(), original.as_slice());
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
        assert_eq!(identity.allocate_node(&store).unwrap(), 3);
        assert!(Identity::open(&store, &crypto, 0xfff2).is_err());
        identity.next_node = MAX_NODE;
        assert_eq!(identity.allocate_node(&store).unwrap(), MAX_NODE);
        assert!(identity.allocate_node(&store).is_err());
        drop(identity);
        drop(store);
        let store = PrivateStore::open(&directory.0).unwrap();
        let mut identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        assert!(identity.allocate_node(&store).is_err());
    }

    #[test]
    fn staged_envelope_restores_authority_catalog_and_allocator_together() {
        use crate::catalog::{Catalog, Phase};

        let source = Directory::new();
        let store = PrivateStore::open(&source.0).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[4; 32]),
        );
        let mut identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        let fabric = identity.fabric_id();
        assert_eq!(identity.allocate_node(&store).unwrap(), 2);
        let catalog = Catalog::open(store.clone(), fabric, identity.next_node()).unwrap();
        let reservation = catalog.reserve(2).unwrap();
        reservation.fabric_installed().unwrap();
        reservation.thread_provisioned().unwrap();
        reservation.complete().unwrap();
        let source_bytes = std::fs::read(source.0.join("credentials.bin")).unwrap();
        drop(catalog);
        drop(identity);
        drop(store);

        let staged = Directory::new();
        drop(PrivateStore::open(&staged.0).unwrap());
        std::fs::copy(
            source.0.join("credentials.bin"),
            staged.0.join("credentials.bin"),
        )
        .unwrap();
        let staged_store = PrivateStore::open(&staged.0).unwrap();
        let mut restored = Identity::open(&staged_store, &crypto, 0xfff1).unwrap();
        assert_eq!(restored.fabric_id(), fabric);
        assert_eq!(restored.next_node(), 3);
        let restored_catalog = Catalog::open(
            staged_store.clone(),
            restored.fabric_id(),
            restored.next_node(),
        )
        .unwrap();
        let nodes = restored_catalog.nodes();
        assert_eq!(nodes.len(), 1);
        assert_eq!(nodes[0].id, 2);
        assert!(matches!(nodes[0].phase, Phase::Complete));
        assert_eq!(restored.allocate_node(&staged_store).unwrap(), 3);
        drop(restored_catalog);
        drop(restored);
        drop(staged_store);
        assert_eq!(
            std::fs::read(source.0.join("credentials.bin")).unwrap(),
            source_bytes
        );
    }
    #[test]
    fn partial_wrong_keys_and_orphaned_fabric_fail_closed() {
        let directory = Directory::new();
        let store = PrivateStore::open(&directory.0).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[2; 32]),
        );
        let identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        let original = identity.encode(2);
        for size in [0, 7, 100, original.len() - 1] {
            store.set(AUTHORITY_KEY, Some(&original[..size])).unwrap();
            assert!(Identity::open(&store, &crypto, 0xfff1).is_err());
            let mut loaded = [0; MAX_RECORD];
            assert_eq!(store.get(AUTHORITY_KEY, &mut loaded).unwrap(), Some(size));
        }
        let mut corrupt = Zeroizing::new(original.to_vec());
        corrupt[34] ^= 1;
        store.set(AUTHORITY_KEY, Some(&corrupt)).unwrap();
        assert!(Identity::open(&store, &crypto, 0xfff1).is_err());
        store.set(AUTHORITY_KEY, None).unwrap();
        store.set(1, Some(b"orphaned fabric")).unwrap();
        assert!(Identity::open(&store, &crypto, 0xfff1).is_err());
        assert!(!store.contains_in(AUTHORITY_KEY..AUTHORITY_KEY + 1).unwrap());
    }
    #[test]
    fn interrupted_install_restores_same_identity_and_mismatch_never_overwrites() {
        use rs_matter::dm::devices::test::{TEST_DEV_ATT, TEST_DEV_COMM, TEST_DEV_DET};
        use rs_matter::utils::init::InitMaybeUninit;
        let directory = Directory::new();
        let store = PrivateStore::open(&directory.0).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[3; 32]),
        );
        let identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        let original = identity.encode(2);
        assert!(!store.contains_in(0..256).unwrap());
        drop(identity);
        drop(store);
        let store = PrivateStore::open(&directory.0).unwrap();
        let identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        assert_eq!(identity.encode(2).as_slice(), original.as_slice());
        {
            let mut memory = Box::new(std::mem::MaybeUninit::uninit());
            let matter = memory.init_with(Matter::init(
                &TEST_DEV_DET,
                TEST_DEV_COMM,
                &TEST_DEV_ATT,
                5540,
            ));
            matter.startup(&matter.kv(store.clone())).unwrap();
            identity.install(matter, &crypto, &store).unwrap();
        }
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        {
            let mut memory = Box::new(std::mem::MaybeUninit::uninit());
            let matter = memory.init_with(Matter::init(
                &TEST_DEV_DET,
                TEST_DEV_COMM,
                &TEST_DEV_ATT,
                5540,
            ));
            matter.startup(&matter.kv(store.clone())).unwrap();
            identity.install(matter, &crypto, &store).unwrap();
            let other_directory = Directory::new();
            let other_store = PrivateStore::open(&other_directory.0).unwrap();
            let other_identity = Identity::open(&other_store, &crypto, 0xfff1).unwrap();
            assert!(other_identity.install(matter, &crypto, &store).is_err());
        }
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
        store.set(1, Some(b"unloaded broken fabric")).unwrap();
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        let mut memory = Box::new(std::mem::MaybeUninit::uninit());
        let matter = memory.init_with(Matter::init(
            &TEST_DEV_DET,
            TEST_DEV_COMM,
            &TEST_DEV_ATT,
            5540,
        ));
        assert!(identity.install(matter, &crypto, &store).is_err());
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
    }
    #[test]
    fn abandoned_stage_and_failed_allocation_preserve_committed_state() {
        let directory = Directory::new();
        let store = PrivateStore::open(&directory.0).unwrap();
        std::fs::write(directory.0.join(".credentials-abandoned"), b"partial stage").unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[4; 32]),
        );
        let mut identity = Identity::open(&store, &crypto, 0xfff1).unwrap();
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        let moved = Directory::new();
        std::fs::rename(&directory.0, &moved.0).unwrap();
        assert!(identity.allocate_node(&store).is_err());
        assert_eq!(identity.next_node, 2);
        assert_eq!(
            std::fs::read(moved.0.join("credentials.bin")).unwrap(),
            before
        );
        std::fs::rename(&moved.0, &directory.0).unwrap();
        assert_eq!(identity.allocate_node(&store).unwrap(), 2);
    }
    #[test]
    fn orphaned_catalog_never_creates_a_replacement_authority() {
        let directory = Directory::new();
        let store = PrivateStore::open(&directory.0).unwrap();
        store
            .set(crate::catalog::CATALOG_KEY, Some(b"orphaned node catalog"))
            .unwrap();
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        let crypto = default_crypto(
            OsEntropy::open().unwrap(),
            CryptoSensitiveRef::new(&[1; 32]),
        );
        assert!(Identity::open(&store, &crypto, 0xfff1).is_err());
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
    }
}
