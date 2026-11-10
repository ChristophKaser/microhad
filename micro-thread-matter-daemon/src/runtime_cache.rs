//! Disposable operational state on RAM storage; never credentials or flash.
use serde::{de::DeserializeOwned, Serialize};
use std::{fs::{self, OpenOptions}, io::{self, Read, Write}, os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt}, path::{Path, PathBuf}};
const MAX_BYTES: u64 = 2 * 1024 * 1024;
#[derive(Clone)]
pub struct RuntimeCache { directory: PathBuf }
fn invalid() -> io::Error { io::Error::other("invalid Matter runtime cache") }
fn private(path: &Path, directory: bool) -> io::Result<()> {
    let m=fs::symlink_metadata(path)?;
    if m.uid()!=unsafe{libc::geteuid()} || m.mode()&0o077!=0 ||
        (directory && !m.is_dir()) || (!directory && (!m.is_file() || m.nlink()!=1)) {return Err(invalid());}
    Ok(())
}
impl RuntimeCache {
    pub fn open(fabric: u64) -> io::Result<Self> {
        Self::open_at(&PathBuf::from(format!("/tmp/mthread-matter-runtime-{fabric:016x}")))
    }
    pub(crate) fn open_at(directory: &Path) -> io::Result<Self> {
        match fs::DirBuilder::new().mode(0o700).create(directory) {
            Ok(())=>(),Err(e) if e.kind()==io::ErrorKind::AlreadyExists=>(),Err(e)=>return Err(e),
        }
        private(directory,true)?;
        Ok(Self{directory:directory.into()})
    }
    pub fn read<T:DeserializeOwned>(&self,name:&str)->io::Result<Option<T>> {
        if !matches!(name,"devices"|"addresses"){return Err(invalid());}
        private(&self.directory,true)?;
        let path=self.directory.join(name);
        match private(&path,false) {Ok(())=>(),Err(e) if e.kind()==io::ErrorKind::NotFound=>return Ok(None),Err(e)=>return Err(e)}
        let mut bytes=Vec::new();
        OpenOptions::new().read(true).custom_flags(libc::O_NOFOLLOW).open(path)?.take(MAX_BYTES+1).read_to_end(&mut bytes)?;
        if bytes.len() as u64>MAX_BYTES{return Err(invalid());}
        serde_json::from_slice(&bytes).map(Some).map_err(|_|invalid())
    }
    pub fn write<T:Serialize>(&self,name:&str,value:&T)->io::Result<()> {
        if !matches!(name,"devices"|"addresses"){return Err(invalid());}
        private(&self.directory,true)?;
        let bytes=serde_json::to_vec(value).map_err(|_|invalid())?;
        if bytes.len() as u64>MAX_BYTES{return Err(invalid());}
        let next=self.directory.join(format!("{name}.next"));
        if fs::symlink_metadata(&next).is_ok(){private(&next,false)?;}
        let mut file=OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).custom_flags(libc::O_NOFOLLOW).open(&next)?;
        file.write_all(&bytes)?;
        // Atomic RAM replacement; deliberately no flash synchronization.
        fs::rename(next,self.directory.join(name))
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn private_bounded_atomic_cache_refuses_links_and_corruption() {
        let path=std::env::temp_dir().join(format!("matter-runtime-cache-{}",std::process::id()));
        let cache=RuntimeCache::open_at(&path).unwrap();
        cache.write("devices",&vec![1u64,2]).unwrap();
        assert_eq!(cache.read::<Vec<u64>>("devices").unwrap(),Some(vec![1,2]));
        cache.write("devices",&vec![3u64]).unwrap();
        assert_eq!(cache.read::<Vec<u64>>("devices").unwrap(),Some(vec![3]));
        fs::write(path.join("devices"),b"{").unwrap();assert!(cache.read::<Vec<u64>>("devices").is_err());
        fs::remove_file(path.join("devices")).unwrap();
        std::os::unix::fs::symlink("/dev/null",path.join("devices")).unwrap();assert!(cache.read::<Vec<u64>>("devices").is_err());
        std::os::unix::fs::symlink("/dev/null",path.join("addresses.next")).unwrap();assert!(cache.write("addresses",&vec![1]).is_err());
        assert!(cache.write("../escape",&vec![1]).is_err());
        fs::remove_dir_all(path).unwrap();
    }
}
