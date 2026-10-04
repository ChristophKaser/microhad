# Plan: Native Matter API and dashboard

## Goal

Expose commissioning, Thread network status, discovered Matter devices, and
supported controls through the native HTTP service and dashboard.

## Steps

1. Keep the existing `/api/matter/status` and `/api/thread/status` routes, then
   define asynchronous start/status/cancel endpoints and Thread status/create/
   import endpoints against the agreed IPC schema.
2. Validate setup-code or QR input, dataset size and channel compatibility,
   device IDs, endpoint/cluster/attribute IDs, command values, and request
   lengths before forwarding them to the service.
3. Add coordinator-owned commands and protected snapshots. HTTP handlers must
   not call `Gateway` directly, mutate Matter state, or wait on phone BLE
   indications.
4. Return stable public errors and session states. Do not return private keys,
   datasets, setup codes, or other secrets in status JSON or logs.
5. Add feature-aware dashboard setup and device views. Hide onboarding and
   controls when `MATTER=0`; show unavailable/reconnecting state when the
   feature is enabled but the service is absent.
6. Keep the user's trusted-LAN `http://` and `ws://` policy. Do not require an
   installed gateway certificate or disable TLS validation globally. Document
   that session IDs correlate relay traffic and are not administrator
   authentication.
7. Add API integration tests for enabled/disabled builds, unavailable service,
   invalid input, cancellation, queue pressure, and secret redaction. Run the
   native host and MIPS checks after C++ changes and `make webui` after UI
   changes.

## Acceptance

- Onboarding and status work without blocking either HTTP worker for the
  duration of commissioning.
- All state changes reach the Matter service through IPC and Zigbee changes
  remain coordinator-queued.
- Disabled builds do not expose Matter controls; enabled builds clearly report
  service unavailability.
- The dashboard works over the selected trusted-LAN HTTP/WebSocket transport.

## Status quo — 2026-10-04

Read-only Matter/Thread status routes and the `MATTER` compiler flag exist.
Commissioning, relay, Thread mutation, device-control routes and dashboard
onboarding are pending. The current status client is insufficient for the
asynchronous integration: connection deadlines and status/observation reply validation are implemented;
worker scheduling, queues and remaining typed bodies are still pending.

## Remaining checkpoints

1. Implemented: protected snapshots now expose the confirmed Ziggurat channel
   and online/formed state through immutable lease accessors. Persisted network
   configuration never populates this observation; offline/unknown is zero.
2. Observation/status worker implemented: fresh coordinator observations and
   cached status keep IPC outside coordinator/HTTP threads. Mutation job queues
   and asynchronous commissioning/control dispatch remain pending.
3. Implement commissioning start/status/cancel and Thread create/import using
   validated bodies, fixed public errors and asynchronous job/session IDs.
4. Add bounded WebSocket relay routing with generation and sequence checks,
   disconnect cleanup and write-confirmation forwarding. Agree the contract
   with plan 05 before building the app against it.
5. Expose device snapshots/read/control once plan 07 provides them, then build
   feature-aware onboarding and device views with redacted progress/errors.
6. Verify disabled/enabled builds, absent/stalled service, malformed replies,
   queue pressure, cancellation and HTTP/WebSocket behavior. Run required
   native host/MIPS and web UI builds for their respective changes.

## Dependencies

[Commissioning and control IPC](02-commissioning-control-ipc.md) and
[persistent identity](03-persistent-fabric-identity.md). Define the relay
endpoint contract with the Garage workstream so the app can connect when its
plan is implemented.

Native snapshot checkpoint — 2026-10-04: `observed_network_channel` is populated
only by Ziggurat Ready, cleared on Down, and suppressed when offline/unformed.
Legacy EZSP and persisted configuration report zero until a radio observation
exists. `ViewLease::coordinatorOnline()` and `coordinatorChannel()` expose the
immutable data without accessing Gateway from a worker. Tests cover persisted
channel versus observed channel, immutable copies and offline/unformed state.
The periodic Matter worker, queues and IPC forwarding remain pending.

Verification passed: `make host test`, `make mips`,
`make ZIGGURAT=1 MATTER=1 host test` and
`make ZIGGURAT=1 MATTER=1 mips`. The enabled suite includes 13 native peer
checks. Suites must run sequentially because their legacy tests share `/tmp`
log paths; the initial concurrent enabled run failed a redaction assertion,
then the isolated full rerun passed. No gateway deployment occurred.

Native worker checkpoint — 2026-10-04

Matter-enabled startup creates one `matter-ipc` worker with a 64 KiB stack;
Matter-disabled startup creates no Matter thread. Both HTTP listeners share its
cached, validated status and never connect to the service for those reads.
The worker sends private channel/online observations about once per second,
then fetches Matter/Thread status. Failures clear the cache; snapshots expire
after five monotonic seconds. Shutdown joins the worker before deleting the
coordinator; each IPC operation has the existing one-second total deadline.

Coordinator poll progress is recorded separately from dirty dashboard views.
Progress older than 2.5 seconds, unknown channel, offline/unformed state or
shutdown produces an offline channel-zero observation. Matter-enabled idle
coordinator select sleeps are capped at one second to refresh liveness. A frozen
coordinator therefore cannot keep renewing an old online snapshot. The worker
does not read Gateway or radio state directly, write flash, or log payloads.

Worker tests cover the offline observation body, strict snapshot exchange,
cached reads, duplicate start, stop/restart and absent service. Synthetic native
round-trip coverage uses the actual Matter-enabled binary and scripted Ziggurat
and IPC peers for confirmed channel, radio loss, twenty quick HTTP reads and
absent-service 503. Commissioning/relay mutation routes and queues remain open.

Verification: default and Matter-enabled `make host test`/`make mips` pass,
including the final default rerun after startup cleanup/test edits. Both
`python3 scripts/test_matter_native_worker.py` and the same command with
`--real-service` pass. The latter runs the real Rust service against a synthetic
RCP, authorizes create through the native channel observation, confirms cached
HTTP status/configuration, records only matching channel writes, then observes
Thread return to `awaiting_coordinator` on Zigbee peer loss. It shuts down both
temporary services and removes private synthetic state; no physical gateway
or real accessory is used. The real-service check requires the already built
companion host service and synthetic relay binaries.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Consume plan 02's persistent channel and bounded queue in MatterWorker.
   Publish protected session/job snapshots and events; retain coordinator-only
   ownership of Gateway and keep HTTP handlers independent of IPC waits.
2. [ ] Define and implement asynchronous commissioning start/status/cancel
   and Thread create/import routes with bounded input, stable IDs, fixed
   errors and immediate queue acceptance/rejection.
3. [ ] Define and implement one bounded WebSocket BLE relay contract with
   attach, indication, write, write-complete and connection-loss behavior.
   Test generations, ordering, cancellation and relay ownership with plan 05.
4. [ ] Add feature-aware onboarding/progress/retry UI. Preserve trusted-LAN
   HTTP/WS operation without requiring a trusted gateway certificate; keep
   setup input out of logs and persistent dashboard state.
5. [ ] When plan 07 is ready, expose device snapshots/read/control and render
   advertised controls and measurements, including unavailable/null values.
6. [ ] Verify enabled/disabled builds, unavailable/stalled service, invalid
   input, queue saturation, WebSocket loss and secret redaction. Run default
   and enabled host/MIPS suites sequentially and make webui for UI edits.

Validation: fake and actual-service API tests must show responsive cached reads
and immediate job submission while commissioning waits on BLE or Thread.

## Persistent worker and queued-call checkpoint — 2026-10-04

The native Matter worker now owns one persistent IPC channel. Its private
coordinator observations, Matter/Thread snapshots and commissioning/relay calls
share that channel; the worker polls typed events between calls. Heartbeats
remain approximately one second apart and take scheduling priority over queued
jobs. HTTP status still reads only protected cached snapshots.

The worker accepts strict schema-1 commissioning/relay requests (opcodes 3–8)
into a RAM queue of at most eight pending calls. Job IDs are nonzero u32 and do
not wrap; at most sixteen job snapshots are retained, with only terminal jobs
eligible for eviction. Submission returns immediately; a completed IPC call's
validated reply is available separately from the commissioning session lifetime.
A service-level error is a completed call with an `ok:false` reply, while a
transport failure marks the call failed. Pending request bodies are cleared on
execution/failure/shutdown and never returned in snapshots or logs.

Eight validated events may await the relay consumer. Event overflow or service
loss closes the IPC channel, clears stale cached status/events and fails pending
calls. The worker reconnects for the next heartbeat but never automatically
replays a mutation or BLE callback. A later relay implementation must explicitly
recover session state and reattach rather than assume a lost C1 write was sent.
No new flash writes are introduced.

Public commissioning/job/Thread mutation APIs and the WebSocket event consumer
remain open in plan 04; Thread create/import queue support is also still open.

Focused worker tests use a single persistent fake peer to verify offline
heartbeat/status correlations, asynchronous cancellation submission, cached
reads during a delayed reply and an event arriving between calls. A failed
peer test blocks one call, fills all eight pending slots, rejects overflow and
checks all nine jobs fail on disconnect without returning request bodies.
Restart preserves monotonic IPC correlations; worker shutdown clears job/event
caches. Invalid setup PINs and unavailable-service submissions fail early.

Verification passed: default `make host test`/`make mips` and
`make ZIGGURAT=1 MATTER=1 host test`/`make ZIGGURAT=1 MATTER=1 mips`.
Host suites ran sequentially; enabled peer checks pass (13 tests). Both
factory binaries remain stripped static big-endian MIPS-I ELF files.
`python3 scripts/test_matter_native_worker.py` passes with its updated persistent
fake peer, and `python3 scripts/test_matter_native_worker.py --real-service`
passes against companion commit `45782d3` with synthetic RCP/Ziggurat peers.
The actual native binary renews confirmed channel observations, reads cached
HTTP snapshots, observes radio loss and returns 503 after service exit.
Queue/cancellation/event delivery evidence is from the focused fake-peer tests;
public API-to-commissioner integration remains open. No physical gateway test,
deployment, accessory pairing or plan 08 action was performed.

## Queued commissioning HTTP checkpoint — 2026-10-04

Start/status/cancel now have native POST routes that validate strict schema-1
bodies and return 202 with an ephemeral job ID. GET job reads use protected RAM
snapshots only. `complete` means receipt of a validated service reply, not
successful accessory commissioning; service errors remain in that reply.
Invalid input returns 400; unavailable/full queues return fixed 503 errors;
unknown jobs and disabled control routes return 404. The 202 reason phrase is
`Accepted`. Input/setup codes are not reflected in replies or logs.

The route contract is [Native Matter HTTP API v1](../matter-native-api-v1.md).
Native integration tests cover disabled routes and malformed/overflowing input.
The actual-binary harness adds queued start, busy/status, cancel/idempotence,
stale-session and job-ID validation, plus fast HTTP acceptance and cached reads
while the synthetic service delays a reply. Actual Rust service verification
uses synthetic coordinator/RCP peers and the existing private dataset-create
setup; this checkpoint does not add public Thread create/import or BLE transport.
WebSocket relay, dashboard onboarding, Garage and production discovery/device
operations remain open. Plan 08 remains excluded.

Verification passed: default and Matter-enabled `make host test`/`make mips`,
with host suites run sequentially; enabled peer checks pass (13 tests). Both
`python3 scripts/test_matter_native_worker.py` and its `--real-service` variant
pass, including fixed 503 `not_ready` rejection after service exit and setup-PIN
log redaction. The latter reaches companion commit `45782d3` through the actual
native HTTP/worker/IPC path. Both factory binaries remain static stripped
big-endian MIPS-I ELF files; target execution is not inferred from these builds.
