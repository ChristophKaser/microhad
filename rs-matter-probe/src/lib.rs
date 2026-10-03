//! Controller building blocks for the factory gateway; with an optional userspace OpenThread transport.
pub mod ble;
pub mod commissioning;
#[cfg(feature = "control-ipc")]
pub mod control;
pub mod entropy;
#[cfg(feature = "controller-identity")]
pub mod identity;
pub mod ipc;
#[cfg(feature = "control-ipc")]
pub mod sessions;
pub mod store;

#[cfg(feature = "controller-service")]
pub mod service;

#[cfg(feature = "openthread")]
pub mod openthread;
