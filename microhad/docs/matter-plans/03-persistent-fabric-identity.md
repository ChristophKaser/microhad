# Plan: Persistent Matter fabric identity

## Goal

Preserve the gateway's Matter authority, fabric, node allocation, and device
state safely across service and gateway restarts.

## Steps

1. Define the production key/value schema and reserve stable keys for the
   root-authority private key and certificate, intermediate authority
   materials if used, fabric metadata, node-allocation counter, and version.
2. Reuse the bounded atomic `PrivateStore` implementation. Confirm that all
   files and directories remain owner-only on the target filesystem, that
   symlinks and corrupt envelopes fail closed, and that writes do not happen
   during normal status polling.
3. Implement first-start identity creation with cryptographically secure
   entropy. Persist the complete authority record atomically before exposing a
   commissioning session; recover safely from an interrupted first write.
4. Load and validate the same authority on restart. Refuse mismatched fabric
   metadata or partial records instead of silently creating a new fabric.
5. Allocate unique operational node IDs monotonically. Persist allocation
   before use so crashes cannot reuse an ID; define exhaustion and migration
   behavior.
6. Add tests for first start, reload, interrupted/corrupt state, concurrent
   owner exclusion, permission failures, node allocation across restart, and
   allocator exhaustion.
7. Document backup/restore and recovery without exposing private keys or
   operational credentials. Keep any export flow explicitly encrypted and
   outside routine logs.

## Acceptance

- A restart restores the identical fabric and authority and can establish CASE
  with an already commissioned node.
- Repeated allocation and restart never reuse a node ID.
- Corrupt or partially written state fails closed and does not overwrite the
  last committed identity.
- Flash writes are limited to actual state changes and commissioning events.

## Depends on

[Rust service runtime](01-rust-service-runtime.md) and its single-owner store.
Complete this before real accessory commissioning.
