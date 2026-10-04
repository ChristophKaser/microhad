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
