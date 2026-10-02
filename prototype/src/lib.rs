#![no_std]

use core::sync::atomic::{AtomicU32, Ordering};

// Keep externally visible symbols so assembly inspection includes each probe.
#[unsafe(no_mangle)]
pub extern "C" fn probe_add(a: u32, b: u32) -> u32 {
    a.wrapping_add(b)
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_atomic_increment(value: &AtomicU32) -> u32 {
    value.fetch_add(1, Ordering::SeqCst)
}

#[unsafe(no_mangle)]
pub extern "C" fn probe_decode_le(bytes: &[u8; 4]) -> u32 {
    u32::from_le_bytes(*bytes)
}
