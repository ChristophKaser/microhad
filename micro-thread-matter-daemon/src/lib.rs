//! Controller building blocks for the factory gateway; with an optional userspace OpenThread transport.
pub mod ble;
#[cfg(feature = "controller-identity")]
pub mod catalog;
pub mod commissioning;
#[cfg(feature = "control-ipc")]
pub mod control;
pub mod entropy;
#[cfg(feature = "controller-identity")]
pub mod identity;
pub mod ipc;
#[cfg(feature = "controller-service")]
pub mod logging;
mod wire_cbor;
mod wire_shapes;
#[cfg(feature = "control-ipc")]
pub mod sessions;
pub mod store;
#[cfg(feature = "controller-service")]
pub mod subscriptions;

#[cfg(feature = "controller-service")]
pub mod controller;
#[cfg(feature = "controller-service")]
pub mod devices;
#[cfg(feature = "controller-service")]
pub mod discovery;
#[cfg(feature = "controller-service")]
pub mod icd;
#[cfg(feature = "controller-service")]
pub mod operations;
#[cfg(feature = "control-ipc")]
pub mod responses;
#[cfg(feature = "controller-service")]
pub mod service;
#[cfg(feature = "controller-service")]
pub mod thread_lifecycle;

#[cfg(feature = "openthread")]
pub mod openthread;

// Retain the shared native shim, including symbols referenced by Rust std.
#[cfg(all(target_arch = "mips", target_env = "uclibc"))]
extern crate ziggurat_stock_uclibc_compat;
