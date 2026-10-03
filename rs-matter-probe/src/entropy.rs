//! OS entropy for the production controller. Read failure aborts rather than weakens keys.
use core::convert::Infallible;
use rs_matter::crypto::{TryCryptoRng, TryRng};
use std::fs::File;
use std::io::{self, Read};

pub struct OsEntropy(File);
impl OsEntropy {
    pub fn open() -> io::Result<Self> {
        File::open("/dev/urandom").map(Self)
    }
}
impl TryRng for OsEntropy {
    type Error = Infallible;
    fn try_next_u32(&mut self) -> Result<u32, Infallible> {
        let mut bytes = [0; 4];
        self.try_fill_bytes(&mut bytes)?;
        Ok(u32::from_ne_bytes(bytes))
    }
    fn try_next_u64(&mut self) -> Result<u64, Infallible> {
        let mut bytes = [0; 8];
        self.try_fill_bytes(&mut bytes)?;
        Ok(u64::from_ne_bytes(bytes))
    }
    fn try_fill_bytes(&mut self, bytes: &mut [u8]) -> Result<(), Infallible> {
        self.0.read_exact(bytes).expect("OS entropy read failed");
        Ok(())
    }
}
impl TryCryptoRng for OsEntropy {}
