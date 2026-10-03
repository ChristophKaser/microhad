//! Controller building blocks for the factory gateway; with an optional userspace OpenThread transport.
pub mod commissioning;

#[cfg(feature = "openthread")]
pub mod openthread;
