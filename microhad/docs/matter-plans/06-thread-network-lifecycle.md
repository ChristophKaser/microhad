# Plan: Thread network lifecycle on the shared radio

## Goal

Attach OpenThread through Ziggurat's private RCP socket while preserving
Ziggurat as the only Zigbee UART and reset owner.

## Steps

1. Confirm and document the production Ziggurat Thread socket path, logical
   Spinel IID, physical IID mapping, and single-owner startup/shutdown contract.
   Do not open `/dev/ttyS1` from the Matter service.
2. Add service configuration for the private Thread socket and ensure
   OpenThread initialization uses the no-reset URL and the supported MIPS
   platform shims.
3. Observe the active Zigbee channel from the native coordinator and pass it to
   the Matter service through a bounded status/configuration message.
4. Validate every imported or generated active Thread dataset before changing
   radio state: required TLVs, duplicate/truncated fields, channel, channel
   mask, mesh prefix, PAN identifiers, and unsupported pending datasets.
5. Implement create/import/apply with an explicit rollback or fail-closed path.
   Do not apply a dataset on a channel incompatible with the shared Zigbee
   network.
6. Expose Thread detached/attaching/attached/error state and radio socket
   failures. Reconcile startup order, RCP restart, and service restart without
   issuing radio reset commands from the Matter process.
7. Run host synthetic-RCP tests for attach, detach, channel mismatch, malformed
   import, and socket loss. Then measure coexistence and reception effects
   under safe test conditions on the gateway.

## Acceptance

- OpenThread uses the Ziggurat-owned Thread socket and cannot reset or open the
  Zigbee UART.
- Incompatible datasets are rejected before any radio or persistent-state
  mutation.
- Zigbee remains online and formed through Matter startup, shutdown, and Thread
  errors.
- No Linux IPv6/TUN setup is required by the selected userspace UDP transport.

## Depends on

[Rust service runtime](01-rust-service-runtime.md),
[commissioning/control IPC](02-commissioning-control-ipc.md), and native
channel reporting.
