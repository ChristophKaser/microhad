//! Controller building blocks for the factory gateway; with an optional userspace OpenThread transport.
#[cfg(feature = "controller-service")]
mod async_logging;
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
#[cfg(feature = "control-ipc")]
pub mod sessions;
pub mod store;
#[cfg(feature = "controller-service")]
pub mod subscriptions;
pub mod protobuf { include!("../../protocol/generated/matter_typed.rs"); }
#[cfg(any(test, feature="controller-probe"))]
pub mod wire_json;
mod wire_protobuf;
#[cfg(any(test,feature="controller-probe"))]
mod wire_shapes;

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
#[cfg(feature = "controller-service")]
pub mod thermostat;
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

#[cfg(feature = "controller-service")]
pub mod time_sync;
