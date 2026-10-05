//! Private, bounded, atomically replaced controller credential envelope.
//! No Debug/Display implementations expose persisted blobs or keys.
use rs_matter::error::{Error, ErrorCode};
use rs_matter::persist::KvBlobStore;
use std::cell::{Cell, RefCell};
use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::rc::Rc;

const MAX_FILE: usize = 65536;
const MAX_BLOB: usize = 4096;
const MAX_ENTRIES: usize = 128;
const MAGIC: &[u8; 8] = b"TMKV\x01\0\0\0";
unsafe extern "C" {
    fn geteuid() -> u32;
    fn flock(fd: i32, operation: i32) -> i32;
}
struct Inner {
    directory: PathBuf,
    values: RefCell<BTreeMap<u16, Vec<u8>>>,
    sequence: Cell<u32>,
    poisoned: Cell<bool>,
    #[cfg(test)]
    fail_after_rename: Cell<bool>,
    read_only: bool,
    _lock: Option<File>,
}
#[derive(Clone)]
pub struct PrivateStore(Rc<Inner>);
fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid controller state")
}
fn private(path: &Path, directory: bool) -> io::Result<()> {
    let meta = fs::symlink_metadata(path)?;
    if meta.uid() != unsafe { geteuid() }
        || meta.mode() & 0o077 != 0
        || (directory && !meta.is_dir())
        || (!directory && !meta.is_file())
    {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "controller state must be private",
        ));
    }
    Ok(())
}
fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for byte in bytes {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb88320u32 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}
fn encode(values: &BTreeMap<u16, Vec<u8>>) -> io::Result<Vec<u8>> {
    if values.len() > MAX_ENTRIES {
        return Err(invalid());
    }
    let mut payload = Vec::new();
    payload.extend((values.len() as u16).to_le_bytes());
    for (key, data) in values {
        if data.len() > MAX_BLOB || payload.len() + data.len() + 20 > MAX_FILE {
            return Err(invalid());
        }
        payload.extend(key.to_le_bytes());
        payload.extend((data.len() as u16).to_le_bytes());
        payload.extend(data);
    }
    let mut bytes = MAGIC.to_vec();
    bytes.extend((payload.len() as u32).to_le_bytes());
    bytes.extend(crc32(&payload).to_le_bytes());
    bytes.extend(payload);
    Ok(bytes)
}
fn decode(bytes: &[u8]) -> io::Result<BTreeMap<u16, Vec<u8>>> {
    if bytes.len() < 18 || bytes.len() > MAX_FILE || &bytes[..8] != MAGIC {
        return Err(invalid());
    }
    let length = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
    let crc = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
    if length != bytes.len() - 16 || crc != crc32(&bytes[16..]) {
        return Err(invalid());
    }
    let count = u16::from_le_bytes(bytes[16..18].try_into().unwrap()) as usize;
    if count > MAX_ENTRIES {
        return Err(invalid());
    }
    let mut values = BTreeMap::new();
    let mut offset = 18;
    for _ in 0..count {
        if bytes.len() - offset < 4 {
            return Err(invalid());
        }
        let key = u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap());
        let size = u16::from_le_bytes(bytes[offset + 2..offset + 4].try_into().unwrap()) as usize;
        offset += 4;
        if size > MAX_BLOB || size > bytes.len() - offset || values.contains_key(&key) {
            return Err(invalid());
        }
        values.insert(key, bytes[offset..offset + size].to_vec());
        offset += size;
    }
    if offset != bytes.len() {
        return Err(invalid());
    }
    Ok(values)
}
impl PrivateStore {
    pub fn open(directory: &Path) -> io::Result<Self> {
        match fs::DirBuilder::new().mode(0o700).create(directory) {
            Ok(()) => (),
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => (),
            Err(error) => return Err(error),
        }
        private(directory, true)?;
        let lock_path = directory.join("owner.lock");
        match fs::symlink_metadata(&lock_path) {
            Ok(_) => private(&lock_path, false)?,
            Err(error) if error.kind() == io::ErrorKind::NotFound => (),
            Err(error) => return Err(error),
        }
        let lock = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .open(&lock_path)?;
        private(&lock_path, false)?;
        // Linux LOCK_EX | LOCK_NB. One daemon owns the directory across all clones.
        if unsafe { flock(lock.as_raw_fd(), 2 | 4) } != 0 {
            return Err(io::Error::last_os_error());
        }
        let path = directory.join("credentials.bin");
        let values = match fs::symlink_metadata(&path) {
            Ok(meta) => {
                private(&path, false)?;
                if meta.len() > MAX_FILE as u64 {
                    return Err(invalid());
                }
                let mut bytes = Vec::new();
                File::open(&path)?
                    .take(MAX_FILE as u64 + 1)
                    .read_to_end(&mut bytes)?;
                decode(&bytes)?
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => BTreeMap::new(),
            Err(error) => return Err(error),
        };
        Ok(Self(Rc::new(Inner {
            directory: directory.into(),
            values: RefCell::new(values),
            sequence: Cell::new(0),
            poisoned: Cell::new(false),
            #[cfg(test)]
            fail_after_rename: Cell::new(false),
            read_only: false,
            _lock: Some(lock),
        })))
    }
    /// Open an existing candidate without creating paths or permitting writes.
    pub fn open_read_only(directory: &Path) -> io::Result<Self> {
        private(directory, true)?;
        let lock_path = directory.join("owner.lock");
        let lock = match fs::symlink_metadata(&lock_path) {
            Ok(_) => {
                private(&lock_path, false)?;
                let file = File::open(&lock_path)?;
                if unsafe { flock(file.as_raw_fd(), 2 | 4) } != 0 {
                    return Err(io::Error::last_os_error());
                }
                Some(file)
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => None,
            Err(error) => return Err(error),
        };
        let credentials = directory.join("credentials.bin");
        private(&credentials, false)?;
        let metadata = fs::symlink_metadata(&credentials)?;
        if metadata.len() > MAX_FILE as u64 {
            return Err(invalid());
        }
        let mut bytes = Vec::new();
        File::open(&credentials)?
            .take(MAX_FILE as u64 + 1)
            .read_to_end(&mut bytes)?;
        let values = decode(&bytes)?;
        Ok(Self(Rc::new(Inner {
            directory: directory.into(),
            values: RefCell::new(values),
            sequence: Cell::new(0),
            poisoned: Cell::new(false),
            #[cfg(test)]
            fail_after_rename: Cell::new(false),
            read_only: true,
            _lock: lock,
        })))
    }
    pub fn get(&self, key: u16, bytes: &mut [u8]) -> io::Result<Option<usize>> {
        if self.0.poisoned.get() {
            return Err(invalid());
        }
        let values = self.0.values.borrow();
        let Some(value) = values.get(&key) else {
            return Ok(None);
        };
        if value.len() > bytes.len() {
            return Err(invalid());
        }
        bytes[..value.len()].copy_from_slice(value);
        Ok(Some(value.len()))
    }
    pub fn contains_in(&self, range: std::ops::Range<u16>) -> io::Result<bool> {
        if self.0.poisoned.get() {
            return Err(invalid());
        }
        Ok(self.0.values.borrow().range(range).next().is_some())
    }
    /// Commit one whole envelope. A crash before rename retains the previous envelope.
    /// A post-rename durability error poisons this owner; restart must reload the file.
    pub fn set(&self, key: u16, data: Option<&[u8]>) -> io::Result<()> {
        if self.0.read_only || self.0.poisoned.get() {
            return Err(invalid());
        }
        if data.map(|d| d.len() > MAX_BLOB).unwrap_or(false) {
            return Err(invalid());
        }
        let mut candidate = self.0.values.borrow().clone();
        if let Some(data) = data {
            candidate.insert(key, data.to_vec());
        } else {
            candidate.remove(&key);
        }
        let bytes = encode(&candidate)?;
        let mut staged = None;
        for _ in 0..32 {
            let sequence = self.0.sequence.get().checked_add(1).ok_or_else(invalid)?;
            self.0.sequence.set(sequence);
            let path = self
                .0
                .directory
                .join(format!(".credentials-{}-{sequence}", std::process::id()));
            match OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .open(&path)
            {
                Ok(file) => {
                    staged = Some((path, file));
                    break;
                }
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
        let (path, mut file) = staged.ok_or_else(invalid)?;
        let result = (|| {
            file.write_all(&bytes)?;
            file.sync_all()?;
            fs::rename(&path, self.0.directory.join("credentials.bin"))?;
            self.0.poisoned.set(true);
            #[cfg(test)]
            if self.0.fail_after_rename.replace(false) {
                return Err(io::Error::other("injected directory sync failure"));
            }
            File::open(&self.0.directory)?.sync_all()?;
            *self.0.values.borrow_mut() = candidate;
            self.0.poisoned.set(false);
            Ok(())
        })();
        if result.is_err() {
            let _ = fs::remove_file(&path);
        }
        result
    }
}
impl KvBlobStore for PrivateStore {
    fn load<'a>(&mut self, key: u16, buf: &'a mut [u8]) -> Result<Option<&'a [u8]>, Error> {
        let size = self.get(key, buf).map_err(|_| ErrorCode::Failure)?;
        Ok(size.map(|length| &buf[..length]))
    }
    fn store(&mut self, key: u16, data: &[u8], _: &mut [u8]) -> Result<(), Error> {
        self.set(key, Some(data))
            .map_err(|_| ErrorCode::Failure.into())
    }
    fn remove(&mut self, key: u16, _: &mut [u8]) -> Result<(), Error> {
        self.set(key, None).map_err(|_| ErrorCode::Failure.into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;
    use std::sync::atomic::{AtomicU32, Ordering};
    static NEXT: AtomicU32 = AtomicU32::new(0);
    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            Self(std::env::temp_dir().join(format!(
                "matter-store-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            )))
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    #[test]
    fn survives_reopen_preserves_other_keys_and_excludes_another_owner() {
        let dir = Fixture::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        store.set(1, Some(b"public fixture one")).unwrap();
        store.set(2, Some(b"public fixture two")).unwrap();
        assert!(PrivateStore::open(&dir.0).is_err());
        store.set(1, None).unwrap();
        drop(store);
        let store = PrivateStore::open(&dir.0).unwrap();
        let mut bytes = [0; 64];
        assert!(store.get(1, &mut bytes).unwrap().is_none());
        let length = store.get(2, &mut bytes).unwrap().unwrap();
        assert_eq!(&bytes[..length], b"public fixture two");
        assert_eq!(
            fs::metadata(dir.0.join("credentials.bin")).unwrap().mode() & 0o777,
            0o600
        );
    }
    #[test]
    fn corrupt_or_truncated_credentials_fail_closed() {
        for truncated in [false, true] {
            let dir = Fixture::new();
            let store = PrivateStore::open(&dir.0).unwrap();
            store.set(1, Some(b"public fixture")).unwrap();
            drop(store);
            let path = dir.0.join("credentials.bin");
            let mut bytes = fs::read(&path).unwrap();
            if truncated {
                bytes.pop();
            } else {
                *bytes.last_mut().unwrap() ^= 1;
            }
            fs::write(path, bytes).unwrap();
            assert!(PrivateStore::open(&dir.0).is_err());
        }
    }
    #[test]
    fn rejected_update_retains_previous_committed_value() {
        let dir = Fixture::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        store.set(1, Some(b"public fixture")).unwrap();
        assert!(store.set(1, Some(&[0; MAX_BLOB + 1])).is_err());
        let mut bytes = [0; 64];
        let length = store.get(1, &mut bytes).unwrap().unwrap();
        assert_eq!(&bytes[..length], b"public fixture");
        assert!(store.get(1, &mut [0; 1]).is_err());
    }
    #[test]
    fn orphaned_synced_stage_before_rename_preserves_committed_envelope() {
        let dir = Fixture::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        store.set(1, Some(b"committed identity")).unwrap();
        drop(store);

        let mut candidate = BTreeMap::new();
        candidate.insert(1, b"replacement identity".to_vec());
        candidate.insert(2, b"new node counter".to_vec());
        let staged_path = dir
            .0
            .join(format!(".credentials-{}-99", std::process::id()));
        let mut staged = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(staged_path)
            .unwrap();
        staged.write_all(&encode(&candidate).unwrap()).unwrap();
        staged.sync_all().unwrap();
        drop(staged);

        let reopened = PrivateStore::open(&dir.0).unwrap();
        let mut bytes = [0; 64];
        let length = reopened.get(1, &mut bytes).unwrap().unwrap();
        assert_eq!(&bytes[..length], b"committed identity");
        assert!(reopened.get(2, &mut bytes).unwrap().is_none());
        reopened.set(2, Some(b"next committed counter")).unwrap();
        drop(reopened);

        let restarted = PrivateStore::open(&dir.0).unwrap();
        let length = restarted.get(1, &mut bytes).unwrap().unwrap();
        assert_eq!(&bytes[..length], b"committed identity");
        let length = restarted.get(2, &mut bytes).unwrap().unwrap();
        assert_eq!(&bytes[..length], b"next committed counter");
    }
    #[test]
    fn post_rename_sync_failure_poisons_owner_and_restart_loads_complete_envelope() {
        let dir = Fixture::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        store.set(1, Some(b"old complete identity")).unwrap();
        store.0.fail_after_rename.set(true);

        assert!(store.set(1, Some(b"new complete identity")).is_err());
        assert!(store.get(1, &mut [0; 64]).is_err());
        drop(store);

        let restarted = PrivateStore::open(&dir.0).unwrap();
        let mut bytes = [0; 64];
        let length = restarted.get(1, &mut bytes).unwrap().unwrap();
        assert_eq!(&bytes[..length], b"new complete identity");
    }
    #[test]
    fn read_only_open_refuses_live_owner_and_never_creates_paths() {
        let dir = Fixture::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        store.set(7, Some(b"synthetic state")).unwrap();
        assert!(PrivateStore::open_read_only(&dir.0).is_err());
        drop(store);
        fs::remove_file(dir.0.join("owner.lock")).unwrap();
        let before = fs::read(dir.0.join("credentials.bin")).unwrap();
        let readonly = PrivateStore::open_read_only(&dir.0).unwrap();
        assert!(readonly.set(7, Some(b"changed")).is_err());
        drop(readonly);
        assert!(!dir.0.join("owner.lock").exists());
        assert_eq!(fs::read(dir.0.join("credentials.bin")).unwrap(), before);
    }
    #[test]
    fn refuses_public_directory_or_symlink_credentials() {
        let dir = Fixture::new();
        let store = PrivateStore::open(&dir.0).unwrap();
        drop(store);
        fs::set_permissions(&dir.0, fs::Permissions::from_mode(0o755)).unwrap();
        assert!(PrivateStore::open(&dir.0).is_err());
        fs::set_permissions(&dir.0, fs::Permissions::from_mode(0o700)).unwrap();
        std::os::unix::fs::symlink("owner.lock", dir.0.join("credentials.bin")).unwrap();
        assert!(PrivateStore::open(&dir.0).is_err());
    }
}
