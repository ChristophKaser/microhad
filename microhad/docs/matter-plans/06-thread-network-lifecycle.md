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

The private no-reset transport and userspace UDP path are implemented. The
service now implements persisted dataset create/import/restore, fresh-observation
startup gating and expiry-driven detach. Native observation delivery is implemented in the optional Matter worker;
production activation and target runtime verification remain pending.

| Step | Status | Evidence / remaining work |
| --- | --- | --- |
| 1–2 | Complete | Private no-reset Unix transport and optional supervision; no direct UART owner or Linux IPv6/TUN requirement. |
| 3 | Complete | Typed private online/channel observations expire after five monotonic seconds; native protected observation/status worker implemented with coordinator liveness expiry; host round trips verified. |
| 4 | Complete | Required/duplicate/truncated TLVs, channel/mask, prefix and PAN checks before mutation; malformed/mismatch synthetic and unit checks pass. |
| 5 | Complete | OS-random create, bounded persisted import/restore, no implicit replacement; persist-before-apply failure disables Thread and requires explicit retry. |
| 6 | Partial | Redacted lifecycle states, deferred boot, expiry detach and cached-channel guard implemented; radio-loss/restart reconciliation still needs integrated coverage. |
| 7 | Partial | 36 Rust library tests and real host process/synthetic-RCP lifecycle test pass; physical coexistence/reception remains unverified. |

The process test changes a persisted cached active channel to 20 while the
canonical dataset/channel is 15. Restart waits without radio traffic, rejects
observation 11, then restores on observation 15; every recorded channel write
is 15. Status/reload leave the private credential file unchanged. The test
selects the actual dataset record rather than unrelated POSIX settings records.
The adapter permits readback of validated datasets while suppressing unchecked
boot restore. Malformed settings envelopes fail before backend truncation;
core dumps are disabled before loading credentials.

Implementation checkpoint: `ziggurat-compilation` commit `089a830`.

Validation: `cargo +nightly-2026-10-02 test --locked --manifest-path
rs-matter-probe/Cargo.toml --features controller-service --lib` (36 passed),
`python3 scripts/test-matter-service.py`, host encrypted commissioning fixtures
with/without `--ble` (209/253 ms), and factory service build. The stripped static
big-endian MIPS-I artifact is 1,552,028 bytes; it has not been run on the gateway.
No physical radio test, deployment or plan 08 action occurred.

## Remaining checkpoints

1. Completed: dataset validation, generation and fail-closed apply are verified
   on host; retain these checks when adding commissioning dispatch.
2. Host check complete; extend recovery coverage against cached settings. Preserve security
   counters, reject corrupt settings and ensure no wrong-channel command is
   emitted before a fresh coordinator observation authorizes initialization.
3. Implemented: native channel/online observations and periodic worker delivery.
   Host tests cover readiness and radio loss; extend integrated coverage for
   channel changes and coordinator stalls/restarts when adding job dispatch.
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
