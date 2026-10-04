# Plan: Native Matter API and dashboard

## Current open work — 2026-10-04

Queued commissioning/Thread APIs, the owned WebSocket relay and a feature-aware
dashboard onboarding/progress panel exist. Queued device snapshot pages now have
native validation, HTTP exposure and a dashboard view. Device controls/measurement APIs and views remain open.

Next deliverable: Verify rendered snapshot interaction, then add device read/control and measurements after plan
07 supplies the production device pipeline. Extend the broader API/failure gates
in checklist item 6; rendered browser interaction remains unverified.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

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

Native status, queued commissioning/Thread mutation routes and the owned relay
are host-tested. The Controls tab now hides Matter controls on disabled builds,
gates mutations on validated readiness, creates/imports an absent Thread network,
hands accessory onboarding to Garage and watches/cancels a known commissioning
session. The dashboard client has nine actual localhost HTTP tests and passes
the required production web build. Device views remain pending plan 07.

## Remaining checkpoints

1. Expose production device snapshots/read/control once plan 07 provides them,
   then render advertised controls and measurements with unavailable/null states.
2. Extend broad enabled/disabled, absent/stalled service, malformed reply, queue
   pressure, cancellation, socket-loss and secret-redaction gates. Existing native
   and new frontend tests cover subsets; checklist item 6 remains open.
3. Inspect rendered browser interactions in addition to the host client and
   TypeScript/build evidence. Physical pairing remains exclusively in plan 08.

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
2. [x] Define and implement asynchronous commissioning start/status/cancel
   and Thread create/import routes with bounded input, stable IDs, fixed
   errors and immediate queue acceptance/rejection.
3. [x] Define and implement one bounded WebSocket BLE relay contract with
   attach, indication, write, write-complete and connection-loss behavior.
   Test generations, ordering, cancellation and relay ownership with plan 05.
4. [x] Add feature-aware onboarding/progress/retry UI. Preserve trusted-LAN
   HTTP/WS operation without requiring a trusted gateway certificate; keep
   setup input out of logs and persistent dashboard state.
5. [ ] When plan 07 is ready, expose device snapshots/read/control and render
   advertised controls and measurements, including unavailable/null values.
   Snapshot API/view and bounded HTTP client are implemented; rendered interaction,
   read/control and measurement rendering keep this item open.
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

## WebSocket transport checkpoint — 2026-10-04

`matter_websocket.cpp` supplies the server handshake and incremental frame codec
needed before socket ownership can move from an HTTP request worker to the
Matter worker. It follows the relevant framing/handshake rules from
[RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html). The handshake checks the
exact HTTP/1.1 GET path, required headers/version, a canonical 16-byte base64
nonce and duplicate fields, and computes the accept value using existing Mbed
TLS SHA-1/base64 support. It does not negotiate extensions or a subprotocol.

Inbound frames must be masked with clear reserved bits. Text continuations can
span input chunks and UTF-8 sequences; ping/pong/close frames may occur between
fragments. The application profile rejects binary messages. Frame/message limits
are 2048 bytes, input chunks 4096 bytes and eight queued messages. Invalid framing,
UTF-8, oversized data and queue overflow fail closed with fixed close codes and
clear retained payloads. Outbound frames are unmasked and validate their type,
control limits and text/close payloads. No packet logging is added.

Focused tests cover the standard handshake accept vector, bad/duplicate keys,
wrong version/path/body, byte-fragmented masked input, interleaved ping, split
UTF-8, maximum-size messages, server encoding, missing masks, invalid fragment
ordering, binary input, invalid UTF-8, oversized/fragmented messages, noncanonical
lengths, bad close payloads, queue saturation and post-close input.

This checkpoint is a transport building block. No WebSocket endpoint is active,
no HTTP socket is handed off yet, and no phone relay/session ownership is claimed.
The next checkpoint must integrate the single connection owner, nonblocking
read/write deadlines, typed attach/indication/write-complete dispatch, progress/C1
events, cancellation/reconnect and shutdown with the existing worker. Garage
integration and real-accessory acceptance remain separate work.

Verification passed: default and Matter-enabled `make host test`/`make mips`.
Host suites ran sequentially; enabled peer regression checks pass (13 tests).
Both factory builds produce static stripped big-endian MIPS-I ELF files.
No HTTP handoff, actual phone relay, deployment or plan 08 action occurred.

## Worker-owned relay checkpoint — 2026-10-04

The plain HTTP WebSocket upgrade now hands a duplicated nonblocking socket to
the existing Matter worker; HTTP threads return promptly and no relay thread is
added. One phone owner is allowed. The typed schema/id/opcode/body envelope binds
status first, then permits only session-matching status/cancel/attach/indication/
write-complete calls. Bind requests select the private event subscription; reply
and event validation remains strict. IDs increase on the connection; outgoing
progress is session-filtered and C1 writes also require the active generation.

Read/write work is bounded, with eight-frame/message queues, 30-second input
idle expiry and two-second incomplete-write expiry. Disconnect/protocol failure
cancels only the bound active session; terminal results remain intact. Service
loss drops the phone without replay. Pending handoff and active descriptors are
closed on shutdown. Phone I/O never accesses Gateway or opens/reset the UART.
The public contract and reconnect policy are in
[Native Matter HTTP API v1](../matter-native-api-v1.md#owned-ble-relay-websocket).

Focused tests validate the relay envelope and session/generation event filters.
The actual-native harness adds upgrade, single-owner rejection, fast concurrent
HTTP reads, ping/pong, status bind, BLE attach/C1, stale-generation rejection,
failed write, terminal state preservation, disconnect cancellation and malformed
client-mask cleanup. Actual Rust service evidence remains synthetic BTP/GATT;
Android integration and production operational discovery remain separate work.
No plan 08 physical gateway/accessory acceptance is performed.

Verification passed: default and Matter-enabled `make host test`/`make mips`,
with sequential host suites and 13 enabled native peer regression checks. Both
`python3 scripts/test_matter_native_worker.py` and its `--real-service` variant
pass. The real-service run reaches companion commit `45782d3` and verifies actual
BTP C1 events, successful write confirmation, reattach, stale indication/write
generations, matching failed-write `transport`, terminal preservation, unbound
phone isolation, bound disconnect cancellation, malformed-mask/repeated-ID
cleanup and setup-code log redaction. The initial failed-write test assumed a
success ack; it was corrected to the existing Rust error contract and the fake
peer was aligned. No production code change was needed for that test mismatch.
Both factory builds remain static stripped big-endian MIPS-I ELF files. Android
GATT/permissions, dashboard onboarding, production DNS-SD/catalog/device reports
and broader timeout/backpressure/target recovery evidence remain open. No
physical gateway, real accessory or plan 08 acceptance action was performed.

## Dashboard onboarding/progress checkpoint — 2026-10-04

The Controls tab contains a feature-aware Matter/Thread panel. Disabled builds
hide its controls; unavailable/reconnecting status gates mutations. Users can
create/import only an absent Thread network, hand setup to Garage without
entering PINs into the browser, and watch/cancel a copied session ID. Garage
commit `7540b17` adds selectable session IDs with retained terminal results.
Device controls remain pending plan 07.

The separate bounded frontend client validates Matter/Thread readiness, canonical
session/dataset IDs, correlation and fixed errors. It caps retained replies at
2048 bytes, each request at three seconds and an operation at ten seconds.
Job completion is distinguished from accessory session completion; mutations
are never automatically replayed. Dataset input is masked and cleared on
submission/background/configuration, without dashboard persistence or logs.

Verification: `make webui` passed TypeScript and Vite production build;
`python3 scripts/test_matter_webui.py` passed nine tests against actual localhost
HTTP fixtures. Both Garage flavor suites pass 60 tests and both APKs build after
session display. Browser rendering/interaction and physical accessory behavior
were not tested. No C++ source/header changes or deployment occurred.

Usage, cleanup limits, trusted-LAN policy and evidence:
[dashboard guide](../matter-dashboard.md). This completes checklist item 4's
implementation checkpoint; items 5 and 6 remain open.

## Native snapshot API checkpoint — 2026-10-04

`POST /api/matter/devices/snapshot` now queues opcode 10 through the existing
single-owner worker; HTTP returns 202 and job polling supplies the validated
page. The native parser bounds nesting/arrays and validators enforce exact
fields, canonical node/revision/endpoint IDs, ID/path agreement, cluster bounds,
sorted uniqueness, Descriptor presence and catalog/online consistency. Nonzero
cursors require a revision. Stale revision errors are delivered explicitly;
malformed responses fail the call rather than reaching HTTP unchecked. There
is no synchronous radio wait, flash write or implicit pagination retry.

Verification passed: `make host test`, `make mips`, then `make host test
MATTER=1 ZIGGURAT=1` and `make mips MATTER=1 ZIGGURAT=1`. Both host suites include
unit/integration checks; enabled tests also pass 13 Ziggurat peer tests. Native
socket tests cover valid empty/endpoint/error pages and reject contradictory
IDs/states, duplicate clusters, absent Descriptor and multiple entries. Existing
MIPS toolchain LOAD-USE warnings remain.

`python3 scripts/test_matter_native_worker.py` verifies two synthetic endpoint
pages over actual HTTP/jobs, malformed request rejection and stale revisions.
The `--real-service` variant verifies the real Rust service's empty catalog and
stale revision through HTTP. Both retain commissioning/Thread/relay/radio-loss
and unavailable-service regression coverage. These checks do not claim a
nonempty physical catalog or device control. No plan 08 test was performed.

Device API implementation remains partial: snapshot views, read/control,
measurements, subscriptions, history and alerts still require plan 07 work.

## Dashboard device snapshot checkpoint — 2026-10-04

`MatterDevices` now consumes queued snapshot pages through the production
TypeScript client. Refresh begins a new revision; Load next endpoint advances
one page explicitly. It shows recorded node/endpoint IDs, discovery/setup/recovery
state, availability and advertised features using pinned SDK cluster IDs. Empty
and undiscovered catalogs are distinct. Changed revisions stop pagination and
require explicit refresh. Previous information is labelled cached on error;
service loss/unmount aborts outstanding work. Calls retain the existing 10-second
job deadline, 3-second HTTP deadline and 2048-byte response limit.

The client rejects changed revisions, cursor loops/skips, contradictory IDs,
invalid availability, duplicate/unsorted clusters, excess entries and unknown
fields. The view additionally bounds retained entries to 2048, rejects duplicate
identities and checks ordering between pages. UI state stays in memory; no
background full-catalog traversal or automatic retry is introduced.

Verification: `make webui` passes TypeScript and Vite compilation; production
client tests via `python3 scripts/test_matter_webui.py` pass 14 actual-localhost
HTTP tests. New checks cover two-page revision handoff, stale revision without
replay, contradictory metadata, empty/recovery entries and invalid cursor/token
rejection before HTTP submission. No native C++ source changed in this checkpoint.
Rendered browser interaction remains unverified: local Chromium needs runtime
paths outside the workspace sandbox. This does not close checklist items 5/6.
Controls, measurements, history and alerts remain open; plan 08 is excluded.
