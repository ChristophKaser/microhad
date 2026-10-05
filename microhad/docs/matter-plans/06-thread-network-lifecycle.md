# Plan: Thread network lifecycle on the shared radio

## Current open work — 2026-10-05

Guarded datasets, fresh coordinator observations, and active commissioning
cancellation on readiness loss are host-verified. The native gateway now
reconnects through a Zigbee channel change while the Matter service refuses the
incompatible Thread dataset and resumes after the original channel returns.
The actual service process also passes its five-second coordinator-observation
expiry gate while preserving the configured dataset and making no additional
radio channel writes. It also recovers a dataset committed immediately before
the synthetic RCP is lost during apply. The native worker now renews the
coordinator observation past the five-second expiry while Thread remains
attached, with no extra radio channel writes. The `apply_failed` suppression
and explicit retry behavior has Rust FakeRadio and host process-level gates
that inject at the OpenThread adapter boundary. A wire-level non-OK Spinel
status from the synthetic RCP now exercises the production OpenThread adapter;
that service process exits, preserves its committed dataset, and restores it
after a fresh process receives a matching coordinator observation. The native
API and dashboard expose a bounded retry action for recoverable in-process
failures. A host composition now confirms the paired supervisor restarts the
real Matter service after the wire-level Spinel apply error, preserves the
dataset across service restart, and leaves the native/Zigbee pair running.
Physical RCP error behavior and target RF remain open.

Next deliverable: Verify native-worker recovery against the restarted service,
including Thread reattachment and restored device operations. The native worker
now recovers the persisted dataset through the restarted daemon; the synthetic
RCP reaches `attaching` but has no mesh peer to demonstrate attachment. Diagnose
the gateway reset before further target profiles. Keep physical coexistence and
RF acceptance in plan 08.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

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
| 5 | Complete | OS-random create, bounded persisted import/restore, no implicit replacement; recoverable in-process apply failures require explicit retry, while wire-level RCP status errors exit and restore the persisted dataset after service restart. |
| 6 | Partial | Redacted lifecycle states, deferred boot, expiry detach, cached-channel guard, bounded stored-dataset retry and host radio-loss/restart reconciliation implemented. A failed Spinel dataset property transaction proves process exit and dataset restore. `scripts/test_matter_spinel_supervisor.py --native-worker` composes the error with the real host gateway, Matter worker, service and paired supervisor; the worker re-observes the coordinator, sees the restored dataset and receives a protected `busy` response to duplicate create. Thread remains `attaching` without a synthetic mesh peer; physical RCP behavior remains open. |
| 7 | Partial | Rust library and host process tests pass, including wire-level synthetic-RCP apply rejection/restart. Physical coexistence/reception and target resource gates remain unverified. |

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

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [ ] Extend synthetic integration coverage for coordinator stalls/restarts,
   channel changes and Thread socket loss while the native worker remains live.
   The Rust service fixture now covers active-session RCP loss/restart and fresh
   channel gating (`34b4839`), plus repeated incompatible observations and
   matching-channel recovery (`77d70f0`). The native process integration now
   covers coordinator loss, a live channel change, Thread `channel_mismatch`,
   and restoration to `attached`. The service process test verifies five-second
   expiry, detached state, omitted stale Zigbee channel, retained dataset
   channel and no additional radio channel write (`37325c0` Rust). Stale-
   observation renewal remains open.
2. [ ] Define and verify explicit recovery when the observed Zigbee channel
   differs from the persisted dataset. Reject incompatible apply before any
   store/radio mutation; never silently replace credentials or reset the UART.
3. [x] Exercise failed dataset apply, idempotent create/import, corrupt cached
   settings and service/RCP restarts. Verify counters survive and all channel
   writes follow fresh observations of the permitted channel. The process test
   now interrupts the first active-dataset apply after its credential commit,
   restarts the service, and verifies fresh-observation gating plus restoration
   on the permitted channel (`987316f` Rust). Direct `apply_failed` heartbeat
   suppression and explicit retry are covered by FakeRadio and service-process
   gates; failure injection inside the real RCP remains open.
4. [ ] Repeat these scenarios with an active commissioning job after plans
   02/04 are wired. Confirm readiness loss terminates work and stale BLE events
   cannot resume a failed attempt.
5. [ ] Update operator state/recovery documentation and record current host
   tests and factory artifacts. Check polling/heartbeats cause no flash writes.
6. [ ] Hand RF coexistence, reception and target recovery measurements to
   plan 08 with baseline/rollback steps; keep them open until measured.

Validation: synthetic RCP traces, service/native status and unchanged-state
checks establish lifecycle behavior; they do not establish RF reliability.

## Queued Thread HTTP mutation checkpoint — 2026-10-04

Public `POST /api/thread/create` and `/api/thread/import` now use the bounded
Matter job queue. Strict bodies contain schema and, for import, 1–254 decoded
hex bytes. HTTP input cannot supply an action/channel override or private online
observation. Native parsing supports the full 508-character dataset bound while
BLE bodies retain their smaller limits.

The worker derives the confirmed coordinator channel at execution time; a
queued request never retains an earlier channel. Missing/offline/stale observation
returns a fixed `not_ready` result without sending the mutation. Rust remains the
owner of dataset TLV validation, persisted network policy and radio apply. Status
and job replies expose redacted snapshots only. Public API semantics, including
busy create and idempotent identical import, are documented in
[Native Matter HTTP API v1](../matter-native-api-v1.md).

Focused tests cover native public/private body separation, channel override
rejection, invalid hex, full dataset limits, duplicate schema and offline channel
preparation. The actual-binary harness now creates through HTTP rather than a
private test IPC call, tests malformed import and busy create, and exercises
valid identical and wrong-channel imports against the actual Rust lifecycle.
The valid dataset is read only from this harness's temporary synthetic private
store and is never exported/logged. Offline create is rejected before forwarding.
WebSocket/Android BLE, production discovery/devices and broader recovery work
remain open; no plan 08 action is involved.

Verification passed: default and Matter-enabled `make host test`/`make mips`,
with sequential host suites and 13 enabled native peer checks. Both actual-native
harness variants pass: `python3 scripts/test_matter_native_worker.py` and
`python3 scripts/test_matter_native_worker.py --real-service` (companion commit
`45782d3`). The real-service run verifies identical import without credential
changes, refusal of a different valid PAN dataset with `busy`, wrong-channel
import with `malformed`, all recorded channel writes matching the coordinator,
dataset log redaction and offline local rejection. Both factory binaries are
static stripped big-endian MIPS-I ELF files. No physical deployment or accessory
acceptance was performed; WebSocket/Android and production discovery remain open.

## Active commissioning cancellation on coordinator loss — 2026-10-04

Rust commit `42ca014` extends the actual `thread-matter` service process test.
While a commissioning session has an active BLE write, an offline coordinator
observation now has an explicit acceptance check: the session reaches `failed`
with `not_ready`, and completion of the old BLE write is rejected with
`stale_session`. A fresh matching observation is then restored before the
remaining expiry, fabric persistence, and restart checks continue.

`python3 scripts/test-matter-service.py` passes with the new case, including
the commissioning timeout and guarded cached-dataset restart fixture. This
verifies session cancellation and stale callback isolation when readiness is
lost. RCP process death/reconnect while the session is active, repeated channel
changes, and physical recovery remain open; no target deployment or plan 08
test occurred.

## RCP socket-loss restart and dataset gating — 2026-10-04

Rust commit `f33d47c` extends the actual `thread-matter` process fixture to
terminate the synthetic Ziggurat RCP relay while Thread is configured. The
service exits nonzero, its stale IPC socket pathname is reclaimed on restart,
and the restarted service remains `awaiting_coordinator` without radio traffic.
An observation on the wrong channel returns `not_ready` and leaves radio quiet;
a fresh observation matching the stored dataset starts OpenThread and returns
the service to `running`. The test compares credential bytes, inode and mtime
before and after the failure/restart, and verifies the synthetic RCP receives no
reset command and writes only the permitted channel.

`python3 scripts/test-matter-service.py` passes with this scenario, including
the active BLE-session cancellation-on-offline case, bounded commissioning
timeout, state validation and final restart checks. This proves host process
recovery and credential/channel guards against the synthetic RCP. Repeated
channel changes and all physical RF/coexistence behavior remain open. No target
deployment or plan 08 acceptance occurred.

## Active commissioning across RCP process restart — 2026-10-04

Rust commit `34b4839` extends the service fixture with the harder failure: it
queues a BLE commissioning write, kills the synthetic RCP relay, then restarts
`thread-matter`. The allocated node remains in the catalog and is recovered as
`recovery_required`; the Thread dataset stays byte-for-byte unchanged. The old
session's BLE write completion returns `stale_session`. Startup emits no radio
traffic before observation, rejects a wrong channel without radio mutation,
then resumes on the matching channel.

`python3 scripts/test-matter-service.py` passes with this case, including the
existing 30-second commissioning timeout and subsequent fabric/state checks.
This verifies active-session/RCP failure recovery on the host synthetic RCP.
Repeated channel changes and target RF/recovery acceptance remain open; plan
08 was not run.

## Repeated coordinator channel mismatch recovery — 2026-10-05

Rust commit `77d70f0` adds repeated live mismatch observations after the
service restores an interrupted commissioning node. The service rejects
channels 20 and 11 against the stored channel 15, reports `channel_mismatch`,
keeps credentials unchanged, and records no off-channel radio writes. A fresh
channel-15 observation returns the service to `running`.

`python3 scripts/test-matter-service.py` passes with the 30-second commissioning
timeout, active-session RCP-loss recovery, repeated mismatch checks and final
state validation. This covers changing/offline coordinator reports while the
service is alive; actual Zigbee channel migration and RF coexistence remain
target acceptance work in plan 08.

## Native coordinator channel-change recovery — 2026-10-05

The native Ziggurat client previously compared the active channel byte as part
of Zigbee network identity. After a coordinator reconnect on a new channel, it
treated the same PAN, extended PAN, keys and coordinator EUI as a different
network and permanently rejected the coordinator. Identity comparison now
allows the channel to change while retaining checks on the remaining network
identity fields and security counters.

The real Rust service/native gateway integration now disconnects the synthetic
coordinator, reconnects it on an incompatible channel, verifies the service
reports `channel_mismatch` while the gateway process stays alive, and confirms
the Thread radio writes only the permitted dataset channel. It then reconnects
on the original Zigbee channel and verifies Thread returns to `attached`.
Verification passed:
`make ZIGGURAT=1 MATTER=1 host`,
`make ZIGGURAT=1 MATTER=1 mips`,
`python3 scripts/test_matter_native_worker.py`,
`python3 scripts/test_matter_native_worker.py --real-service`, and
`python3 scripts/test_ziggurat_native_peer.py` (13 tests). This is host
synthetic evidence; real coordinator behavior and RF coexistence remain in
plan 08, which was not run. The MIPS artifact is a stripped, static, big-endian
MIPS-I ELF; `size` reports 1,593,436 bytes text, 138,108 data, and 65,768 BSS
(1,797,312 bytes total). This build size is not a substitute for target peak
RSS, available-RAM, CPU, or scheduling-deadline measurements.

## RCP loss during active-dataset apply — 2026-10-05

Rust commit `987316f` extends the real `thread-matter` service process test to
pause the synthetic RCP on the active-dataset Spinel property write, after the
Thread dataset is committed to the private credential envelope. The test drops
the RCP relay during that apply, confirms the service exits, and verifies the
committed dataset survives unchanged. After restart the service reports
`awaiting_coordinator` without radio requests; only a fresh channel-15
observation permits OpenThread restoration. The restored status reports the
dataset configured on channel 15, and every simulated radio channel write uses
that permitted channel.

`python3 scripts/test-matter-service.py` passes with this scenario and the
existing commissioning, RCP restart, mismatch and expiry checks. This covers
process recovery from RCP loss during apply. It does not exercise the separate
in-process `apply_failed` heartbeat suppression and explicit retry path against
the real RCP boundary. No target deployment or plan 08 test was performed.

## Coordinator observation expiry gate — 2026-10-05

Rust commit `37325c0` strengthens the actual `thread-matter` process test after
5.1 seconds without a coordinator observation. It verifies the service returns
to `awaiting_coordinator`, is detached, omits the expired Zigbee channel while
retaining the configured dataset channel, leaves the credential envelope
unchanged, and performs no additional radio channel write.

`python3 scripts/test-matter-service.py` passes with this process scenario and
the existing commissioning, RCP restart and mismatch recovery checks. This
closes the host process expiry checkpoint; stale-observation renewal semantics,
failed dataset apply at the process/RCP boundary and physical acceptance remain
open. No target deployment or plan 08 test was performed.

## Failed dataset apply suppression and explicit retry unit gate — 2026-10-05

The existing lifecycle unit test verifies that an active-dataset apply failure
stops the radio and leaves the lifecycle in `Error`. Repeated coordinator
observations and a later heartbeat do not retry the failed apply or rewrite the
committed credential envelope. A subsequent explicit import retries the apply
and succeeds after the radio recovers.

Verification passed with the project-local toolchain and cache:
`source scripts/rust-env.sh && cargo +nightly-2026-10-02 test --offline
--manifest-path rs-matter-probe/Cargo.toml --features controller-service
thread_lifecycle::tests::failed_apply_is_stopped_and_heartbeats_do_not_retry_or_rewrite
--lib --locked`. This is a FakeRadio unit gate; reliable real-RCP injection for
the same in-process failure path remains open.

## Failed dataset apply service-process gate — 2026-10-05

Rust commit `f04d043` adds a host-only `controller-service-test-hooks` Cargo
feature. Its one-shot marker injects a failure at `OpenThread::set_dataset`
after dataset validation and before entering the C OpenThread adapter. The
real `thread-matter` process remains alive, reports `Error`, refuses to retry
on a fresh coordinator heartbeat without changing the committed credential
envelope, and accepts an explicit import retry after the one-shot failure is
consumed. The normal `controller-service` build excludes the hook.

Verification passed:
`source scripts/rust-env.sh && CARGO_TARGET_DIR="$PWD/build/rs-matter/target"
cargo +nightly-2026-10-02 build --offline --manifest-path
rs-matter-probe/Cargo.toml --features controller-service-test-hooks --bin
thread-matter --locked`, `python3 scripts/test-matter-service.py`, and the
targeted lifecycle unit test with `controller-service-test-hooks` (1/1). The
full service script also passes its existing commissioning, timeout, mismatch,
expiry, and restart gates. This does not test the OpenThread C API or a failure
reported by the real RCP; reliable real-RCP injection remains open.

## Native observation renewal while attached — 2026-10-05

Native commit `dc811fe` extends the actual `thread-matter`/native worker
synthetic-RCP integration. After coordinator recovery returns Thread to
`attached`, the test holds the system online for six seconds, longer than the
five-second observation TTL. The native worker's periodic online/channel
observations keep the service attached on the same Zigbee channel, with no
additional radio channel writes. The existing loss, mismatched-channel and
same-channel recovery gates still pass in the same run.

Verification passed with `python3 scripts/test_matter_native_worker.py
--real-service` using the host synthetic RCP, plus
`python3 -m py_compile scripts/test_matter_native_worker.py`. This verifies
host liveness renewal and bounded channel behavior; target scheduling, RF
coexistence and real-RCP failed-apply injection remain open. No deployment or
plan 08 test was performed.

## Bounded operator recovery after dataset apply failure — 2026-10-05

Rust commit `1603086` adds a strict `{"schema":1,"action":"retry"}` service
request. Retry requires a fresh coordinator observation matching the retained
dataset channel and reapplies that persisted dataset only after an earlier
apply failure; it does not accept channel or credential overrides. A retry
while observations are stale returns `not_ready`, and ordinary observations
or heartbeats still do not trigger another apply. Native commit `cf81b2a`
routes this through `POST /api/thread/retry` and adds a dashboard recovery
control for the configured `error` state, so operators can recover without
re-entering or exposing the dataset.

Verification passed: the targeted Rust lifecycle test (1/1),
`python3 scripts/test-matter-service.py` including adapter-boundary injected
apply failure and explicit retry, Matter-enabled `make ZIGGURAT=1 MATTER=1
host test` including 13 native peer checks, `npm run build`, the Chromium
dashboard fixture including the retry button, `python3 scripts/test_matter_webui.py`
(21 tests), `python3 scripts/test_matter_native_worker.py`, Python bytecode
compilation, and `git diff --check`. This closes bounded host operator
recovery. Failure injection at the real RCP boundary and all target acceptance
remain open; CPU, peak memory, startup/scheduling deadlines and RF behavior
have not been measured on the gateway. No deployment or plan 08 test was
performed.

## Constrained synthetic-radio gateway profile — 2026-10-05

The Rust harness in `ziggurat-compilation` commit `1a75661` ran the MIPS
OpenThread CLI for 100 seconds against a synthetic RCP while the installed
service remained online and formed. It completed 1,166 radio-query/UDP cycles
at 19.42/s; loaded CLI CPU averaged 11.80% of one core, sampled RSS peaked at
808 KiB, and system MemFree bottomed at 1,544 KiB against a 1,536 KiB abort
limit. The test used internal Thread IPv6/UDP and did not exercise Matter,
physical RF, or the shared coordinator. Its 8 KiB sampled margin is narrow;
full Matter-service and combined Zigbee/Thread feasibility remain open. See
the [resource profile](../../ziggurat-compilation/docs/openthread-resource-profile.md).

The profile's harness now uses `MemFree` plus conservatively counted clean
inactive file cache for effective-availability checks, while preserving a
separate 1 MiB raw-`MemFree` floor (`ziggurat-compilation` commit `eb04f96`).
The 2026-10-05 measurements above predate this accounting update and retain
their original raw-`MemFree` interpretation. No Matter-service workload was run.

The later isolated MIPS service startup sampled 788 KiB RSS while waiting for a
coordinator, without initializing OpenThread or touching the radio. Raw
`MemFree` fell to 928–944 KiB, below the 1 MiB deployment gate. A device reboot
was observed in the following status interval, followed by supervisor recovery;
its cause is unknown. Later idle monitoring caught repeated reboots, including
two within roughly two minutes, with no Matter target workload running. Bounded
repeat profiles did not reset the device and preserved Zigbee health; after one
transient silent launch failure, a five-second service profile passed. No
watchdog node, feeder, or driver was identified; the reset cause remains open.
This is not Thread attach or coexistence evidence. Suspend further target
profiles pending the runtime-plan reset investigation; details are recorded in
[plan 01](01-rust-service-runtime.md).

## Wire-level Spinel apply rejection and service recovery — 2026-10-05

Rust commit `68d8e21` removes the service-adapter marker hook and injects a
non-OK `LAST_STATUS` response from the synthetic RCP to an active-dataset
property transaction. The production OpenThread/Spinel path causes the service
process to exit before replying to that IPC request. The test verifies the
validated dataset was persisted first, then starts a fresh service process on a
new synthetic RCP connection, supplies a fresh matching channel observation,
and confirms the retained dataset is restored without a radio reset or a
credential rewrite. The complete `python3 scripts/test-matter-service.py` suite
passes. This validates wire-level error handling and process recovery on a
synthetic RCP; it does not prove physical-RCP behavior or that the paired
launcher restarts this specific failure.

Native test `scripts/test_matter_spinel_supervisor.py` now composes this
wire-level failure with the actual paired supervisor and host `thread-matter`
service. It verifies the dataset is persisted before failure, the supervisor
relaunches the service, a fresh matching coordinator observation restores the
dataset without rewriting credentials, and native/Zigbee each remain at one
start. `python3 scripts/test_matter_spinel_supervisor.py` passes against the
synthetic RCP. Physical RCP behavior and service-to-mesh attachment remain open.

The same test's `--native-worker` mode starts the actual Matter-enabled native
gateway under the paired supervisor and uses a synthetic Ziggurat peer. After
the Spinel failure and service restart, the native worker reconnects, renews
the coordinator observation, and sees Thread configured on the persisted
channel. The restored service remains available through the Thread control API;
a duplicate create returns `busy`. The simulated RCP reaches `attaching`, not
`attached`, because the fixture has no Thread mesh peer. Both test modes pass;
the actual worker reattachment path is verified, while mesh attachment and
device operations after recovery remain unverified.
