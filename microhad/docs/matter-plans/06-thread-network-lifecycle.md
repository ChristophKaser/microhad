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

## Status quo — 2026-10-04

The private no-reset OpenThread transport, IID mapping and userspace UDP path
are implemented. Dataset lifecycle and channel-gated startup changes are
currently uncommitted in `ziggurat-compilation`. Their full synthetic runtime
test is not passing yet: the conflicting cached-settings test needs correction
and verification. Native coordinator observations are not wired to the service.
Do not treat these changes as an accepted production attach path.

## Remaining checkpoints

1. Finish and verify dataset record validation, secure generation, persistence
   before apply and explicit fail-closed recovery. Test mismatched/malformed
   imports without radio or store mutation.
2. Verify boot against conflicting cached OpenThread settings. Preserve security
   counters, reject corrupt settings and ensure no wrong-channel command is
   emitted before a fresh coordinator observation authorizes initialization.
3. Implement native channel/online observations and bounded periodic delivery
   through plan 04's worker. Test no observation, stale observation, offline
   coordinator, changed channel and native restart.
4. Test create/import/restore, idempotent requests, attach/detach, apply failure,
   radio socket loss and restart. Confirm heartbeats/status do not rewrite
   persistent credentials and failed apply does not trigger uncontrolled retries.
5. Document current states, observation expiry, channel constraints and operator
   recovery; build the host and factory service and record verified artifacts.
6. Leave physical coexistence/reception measurements to arranged gateway
   testing, with baseline and rollback; synthetic tests cannot establish RF
   reliability or resource usage on the target.

## Dependencies

[Rust service runtime](01-rust-service-runtime.md),
[commissioning/control IPC](02-commissioning-control-ipc.md), and native
channel reporting.
