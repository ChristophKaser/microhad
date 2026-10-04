//! Bounded durable node identities. Writes occur only at commissioning checkpoints.
use crate::{identity::MAX_NODE, store::PrivateStore};
use rs_matter::error::{Error, ErrorCode};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

pub const CATALOG_KEY: u16 = 0x1002;
pub const MAX_NODES: usize = 64;
const MAGIC: &[u8; 8] = b"TMNC\x01\0\0\0";
const MAX_RECORD: usize = 18 + MAX_NODES * 9;
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Phase {
    Reserved = 1,
    FabricInstalled = 2,
    ThreadProvisioned = 3,
    Complete = 4,
    RecoveryRequired = 5,
}
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Node {
    pub id: u64,
    pub phase: Phase,
}
#[derive(Clone)]
pub struct Catalog {
    store: PrivateStore,
    fabric: u64,
    nodes: Rc<RefCell<Vec<Node>>>,
    fatal: Rc<Cell<bool>>,
}
fn invalid() -> Error {
    ErrorCode::InvalidData.into()
}
impl Catalog {
    pub fn open(store: PrivateStore, fabric: u64, next_node: u64) -> Result<Self, Error> {
        if fabric == 0 || !(2..=MAX_NODE + 1).contains(&next_node) {
            return Err(invalid());
        }
        let mut bytes = [0; MAX_RECORD];
        let size = store.get(CATALOG_KEY, &mut bytes).map_err(|_| invalid())?;
        let mut nodes = Vec::new();
        if let Some(size) = size {
            let bytes = &bytes[..size];
            if size < 18
                || &bytes[..8] != MAGIC
                || u64::from_le_bytes(bytes[8..16].try_into().unwrap()) != fabric
            {
                return Err(invalid());
            }
            let count = usize::from(u16::from_le_bytes(bytes[16..18].try_into().unwrap()));
            if count > MAX_NODES || size != 18 + count * 9 {
                return Err(invalid());
            }
            let mut previous = 1;
            for entry in bytes[18..].chunks_exact(9) {
                let id = u64::from_le_bytes(entry[..8].try_into().unwrap());
                if id <= previous || id >= next_node {
                    return Err(invalid());
                }
                previous = id;
                let phase = match entry[8] {
                    1 => Phase::Reserved,
                    2 => Phase::FabricInstalled,
                    3 => Phase::ThreadProvisioned,
                    4 => Phase::Complete,
                    5 => Phase::RecoveryRequired,
                    _ => return Err(invalid()),
                };
                nodes.push(Node { id, phase });
            }
        }
        let catalog = Self {
            store,
            fabric,
            nodes: Rc::new(RefCell::new(nodes)),
            fatal: Rc::new(Cell::new(false)),
        };
        let mut recovered = catalog.nodes.borrow().clone();
        let mut changed = false;
        for node in &mut recovered {
            if !matches!(node.phase, Phase::Complete | Phase::RecoveryRequired) {
                node.phase = Phase::RecoveryRequired;
                changed = true;
            }
        }
        if changed {
            catalog.commit(recovered)?;
        }
        Ok(catalog)
    }
    pub fn nodes(&self) -> Vec<Node> {
        self.nodes.borrow().clone()
    }
    pub fn fatal(&self) -> bool {
        self.fatal.get()
    }
    pub fn has_capacity(&self) -> bool {
        !self.fatal() && self.nodes.borrow().len() < MAX_NODES
    }
    pub fn endpoint_id(&self, node: u64, endpoint: u16) -> Result<String, Error> {
        if !self.nodes.borrow().iter().any(|entry| entry.id == node) {
            return Err(invalid());
        }
        Ok(format!(
            "matter:{:016x}:{node:016x}:{endpoint:04x}",
            self.fabric
        ))
    }
    pub fn reserve(&self, id: u64) -> Result<Reservation, Error> {
        if self.fatal() {
            return Err(ErrorCode::Failure.into());
        }
        if !self.has_capacity() {
            return Err(ErrorCode::NoSpace.into());
        }
        let mut nodes = self.nodes.borrow().clone();
        if !(2..=MAX_NODE).contains(&id) || nodes.last().map(|node| id <= node.id).unwrap_or(false)
        {
            return Err(invalid());
        }
        nodes.push(Node {
            id,
            phase: Phase::Reserved,
        });
        self.commit(nodes)?;
        Ok(Reservation {
            catalog: self.clone(),
            id,
            armed: true,
        })
    }
    fn advance(&self, id: u64, phase: Phase) -> Result<(), Error> {
        let mut nodes = self.nodes.borrow().clone();
        let node = nodes
            .iter_mut()
            .find(|node| node.id == id)
            .ok_or_else(invalid)?;
        if node.phase == phase {
            return Ok(());
        }
        if !matches!(
            (node.phase, phase),
            (Phase::Reserved, Phase::FabricInstalled)
                | (Phase::FabricInstalled, Phase::ThreadProvisioned)
                | (Phase::ThreadProvisioned, Phase::Complete)
                | (
                    Phase::Reserved | Phase::FabricInstalled | Phase::ThreadProvisioned,
                    Phase::RecoveryRequired
                )
        ) {
            return Err(invalid());
        }
        node.phase = phase;
        self.commit(nodes)
    }
    fn commit(&self, nodes: Vec<Node>) -> Result<(), Error> {
        if self.fatal() {
            return Err(ErrorCode::Failure.into());
        }
        let mut bytes = Vec::with_capacity(MAX_RECORD);
        bytes.extend_from_slice(MAGIC);
        bytes.extend(self.fabric.to_le_bytes());
        bytes.extend((nodes.len() as u16).to_le_bytes());
        for node in &nodes {
            bytes.extend(node.id.to_le_bytes());
            bytes.push(node.phase as u8);
        }
        if self.store.set(CATALOG_KEY, Some(&bytes)).is_err() {
            self.fatal.set(true);
            return Err(ErrorCode::Failure.into());
        }
        *self.nodes.borrow_mut() = nodes;
        Ok(())
    }
}
/// A dropped/cancelled future cannot leave an apparently active catalog entry.
pub struct Reservation {
    catalog: Catalog,
    id: u64,
    armed: bool,
}
impl Reservation {
    pub fn fabric_installed(&self) -> Result<(), Error> {
        self.catalog.advance(self.id, Phase::FabricInstalled)
    }
    pub fn thread_provisioned(&self) -> Result<(), Error> {
        self.catalog.advance(self.id, Phase::ThreadProvisioned)
    }
    pub fn complete(mut self) -> Result<(), Error> {
        self.catalog.advance(self.id, Phase::Complete)?;
        self.armed = false;
        Ok(())
    }
}
impl Drop for Reservation {
    fn drop(&mut self) {
        if self.armed
            && self
                .catalog
                .advance(self.id, Phase::RecoveryRequired)
                .is_err()
        {
            self.catalog.fatal.set(true);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        path::PathBuf,
        sync::atomic::{AtomicU32, Ordering},
    };
    static NEXT: AtomicU32 = AtomicU32::new(0);
    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!(
                "matter-catalog-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            )))
        }
        fn store(&self) -> PrivateStore {
            PrivateStore::open(&self.0).unwrap()
        }
    }
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    #[test]
    fn completed_identity_survives_reload_without_observation_writes() {
        let directory = Directory::new();
        let store = directory.store();
        let catalog = Catalog::open(store.clone(), 17, 2).unwrap();
        let node = catalog.reserve(2).unwrap();
        node.fabric_installed().unwrap();
        node.thread_provisioned().unwrap();
        node.complete().unwrap();
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        assert_eq!(
            catalog.endpoint_id(2, 1).unwrap(),
            "matter:0000000000000011:0000000000000002:0001"
        );
        drop(catalog);
        drop(store);
        let recovered = Catalog::open(directory.store(), 17, 3).unwrap();
        assert!(recovered.nodes()[0].phase == Phase::Complete);
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
        assert!(recovered.endpoint_id(3, 1).is_err());
    }
    #[test]
    fn cancelled_or_crashed_commissioning_requires_explicit_recovery() {
        let directory = Directory::new();
        let store = directory.store();
        let catalog = Catalog::open(store.clone(), 17, 2).unwrap();
        drop(catalog.reserve(2).unwrap());
        assert!(catalog.nodes()[0].phase == Phase::RecoveryRequired);
        let mut crashed = catalog.reserve(4).unwrap();
        crashed.fabric_installed().unwrap();
        crashed.armed = false;
        drop(crashed);
        let recovered = Catalog::open(store.clone(), 17, 5).unwrap();
        assert!(recovered
            .nodes()
            .iter()
            .all(|node| node.phase == Phase::RecoveryRequired));
        assert!(recovered.reserve(4).is_err());
        let before = std::fs::read(directory.0.join("credentials.bin")).unwrap();
        Catalog::open(store, 17, 5).unwrap();
        assert_eq!(
            std::fs::read(directory.0.join("credentials.bin")).unwrap(),
            before
        );
    }
    #[test]
    fn capacity_is_bounded_and_completion_cannot_skip_case() {
        let directory = Directory::new();
        let catalog = Catalog::open(directory.store(), 17, 2).unwrap();
        for id in 2..2 + MAX_NODES as u64 {
            drop(catalog.reserve(id).unwrap());
        }
        assert!(!catalog.has_capacity());
        assert!(catalog.reserve(1000).is_err());
        let other = Directory::new();
        let catalog = Catalog::open(other.store(), 17, 2).unwrap();
        assert!(catalog.reserve(2).unwrap().complete().is_err());
        assert!(catalog.nodes()[0].phase == Phase::RecoveryRequired);
    }
    #[test]
    fn wrong_fabric_unallocated_node_and_corrupt_record_fail_closed() {
        let directory = Directory::new();
        let store = directory.store();
        let catalog = Catalog::open(store.clone(), 17, 2).unwrap();
        drop(catalog.reserve(2).unwrap());
        assert!(Catalog::open(store.clone(), 18, 3).is_err());
        assert!(Catalog::open(store.clone(), 17, 2).is_err());
        let mut bytes = [0; MAX_RECORD];
        let size = store.get(CATALOG_KEY, &mut bytes).unwrap().unwrap();
        for invalid in [bytes[..size - 1].to_vec(), {
            let mut value = bytes[..size].to_vec();
            value[26] = 255;
            value
        }] {
            store.set(CATALOG_KEY, Some(&invalid)).unwrap();
            assert!(Catalog::open(store.clone(), 17, 3).is_err());
        }
    }
    #[test]
    fn cleanup_storage_failure_poison_stops_further_commissioning() {
        let directory = Directory::new();
        let catalog = Catalog::open(directory.store(), 17, 2).unwrap();
        let pending = catalog.reserve(2).unwrap();
        std::fs::remove_dir_all(&directory.0).unwrap();
        drop(pending);
        assert!(catalog.fatal());
        assert!(!catalog.has_capacity());
        assert!(catalog.reserve(3).is_err());
    }

    #[test]
    fn staged_backup_restores_catalog_and_rejects_bad_candidate_without_touching_source() {
        let source = Directory::new();
        let source_store = source.store();
        let catalog = Catalog::open(source_store.clone(), 17, 2).unwrap();
        let reservation = catalog.reserve(2).unwrap();
        reservation.fabric_installed().unwrap();
        reservation.thread_provisioned().unwrap();
        reservation.complete().unwrap();
        let source_bytes = std::fs::read(source.0.join("credentials.bin")).unwrap();
        drop(catalog);
        drop(source_store);

        let restored = Directory::new();
        drop(restored.store());
        std::fs::copy(
            source.0.join("credentials.bin"),
            restored.0.join("credentials.bin"),
        )
        .unwrap();
        let restored_store = restored.store();
        let recovered = Catalog::open(restored_store.clone(), 17, 3).unwrap();
        assert_eq!(recovered.nodes().len(), 1);
        assert_eq!(recovered.nodes()[0].id, 2);
        assert!(matches!(recovered.nodes()[0].phase, Phase::Complete));
        assert_eq!(
            recovered.endpoint_id(2, 1).unwrap(),
            "matter:0000000000000011:0000000000000002:0001"
        );
        drop(recovered);
        drop(restored_store);

        let candidate = Directory::new();
        drop(candidate.store());
        std::fs::copy(
            source.0.join("credentials.bin"),
            candidate.0.join("credentials.bin"),
        )
        .unwrap();
        let candidate_path = candidate.0.join("credentials.bin");
        let mut corrupt = std::fs::read(&candidate_path).unwrap();
        *corrupt.last_mut().unwrap() ^= 1;
        std::fs::write(&candidate_path, corrupt).unwrap();
        assert!(PrivateStore::open(&candidate.0).is_err());
        assert_eq!(
            std::fs::read(source.0.join("credentials.bin")).unwrap(),
            source_bytes
        );

        std::fs::write(
            source.0.join(".credentials-abandoned"),
            b"partial replacement",
        )
        .unwrap();
        let reopened = source.store();
        let catalog = Catalog::open(reopened.clone(), 17, 3).unwrap();
        assert_eq!(catalog.nodes().len(), 1);
        assert!(matches!(catalog.nodes()[0].phase, Phase::Complete));
        drop(catalog);
        drop(reopened);
    }
}
