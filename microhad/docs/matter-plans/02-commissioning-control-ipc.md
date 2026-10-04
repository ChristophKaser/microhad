# Plan: Commissioning and device-control IPC

## Current open work — 2026-10-04

Native commissioning/relay dispatch and bounded jobs exist. Rust now dispatches
bounded asynchronous OnOff reads/commands and job polling; native validation,
HTTP admission and worker-side operation polling carry jobs through confirmed
readback. Rust also dispatches Level Control and advertised sensor reads with
typed readbacks. Native validators accept OnOff, Level and sensor-typed replies;
sensor UI/model projection and initial Matter report-event consumption now exist;
broader recovery and complete cross-language vectors remain open.
Snapshot API remains verified (`237d110`).

Native commit `c22f38a` fixes Matter opcode 10 dispatch: the worker previously
treated asynchronous read/command job replies as device snapshot pages, which
could throw while parsing missing snapshot fields. Snapshot handling now runs
only for snapshot requests, and the page decoder rejects a valid non-page job
reply safely. A synthetic IPC peer now sends an unsolicited `matter_report`
event after endpoint reconciliation and verifies that it updates the shared
native device state. The full native host suite, the synthetic-peer worker
fixture, the isolated Rust-service/native-worker fixture, and the Matter-enabled
MIPS build pass. The Rust-service fixture still does not generate a sensor
report through a commissioned accessory; the report-event proof uses the
synthetic IPC peer. Follow-up native commit `6fe31b3` expands that peer test to
send both OnOff and measured Celsius temperature reports, then verifies the
shared device state and normalized sensor value through `/api/status`. Full
cross-language vectors, event backpressure, and broader recovery remain open.

Native commit `750c4c9` adds a stalled-peer timeout regression: a fake IPC peer
accepts the request and withholds its response past the supplied deadline; the
client returns `Matter service response timed out` within the bounded test
window. `make ZIGGURAT=1 MATTER=1 host test` and
`make ZIGGURAT=1 MATTER=1 mips` pass. Fragmentation/coalescing, cross-language
event vectors, broader recovery, and the remaining failure matrix stay open.

## Composed Rust-to-native report delivery — 2026-10-04

Rust commit `53c49dd` adds a host-only gate to the encrypted commissioning
probe. It serves Matter status, Thread observation, and real revisioned device
snapshot pages over the production Rust `IpcServer`, then publishes its
notifier-backed CASE sensor reports on that connection. Native test
`scripts/test_matter_native_report_integration.py` starts the production
`MatterWorker` and native gateway, registers the endpoint using snapshot
pagination, and verifies all five readings in the shared device status and
history API. The composed run passed against the synthetic RCP and synthetic
accessory. This proves the production Rust Manager/IpcServer and native
worker/Gateway paths together; the producer is still the commissioning probe,
not the installed `thread-matter` service process. Service-daemon report
delivery, disconnect/restart recovery, backpressure, and broader malformed
cross-language vectors remain open. No gateway or physical radio was used.

Verification: `python3 scripts/test_matter_native_report_integration.py`
(host-only; local UDP and Unix sockets required) passed; the underlying
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery --sensor-reports` fixture also passed.

Next deliverable: exercise the installed `thread-matter` service process
through registered native device reports, then extend disconnect/restart and
failure coverage. Keep physical acceptance in plan 08.

## Active-session stale BLE rejection — 2026-10-04

Rust commit `42ca014` adds a service-process regression that starts a
commissioning session, queues a phone BLE write, reports coordinator loss,
waits for the controller to fail the session with `not_ready`, then submits
the delayed write completion. The service rejects it as `stale_session`.
The fixture restores a fresh matching coordinator observation and completes its
existing timeout, credentials, and restart checks.

`python3 scripts/test-matter-service.py` passes. This covers a real service
session/lifecycle race over production IPC. Installed-daemon reports reaching
a registered native device, process restart during active commissioning, and
the broader malformed/backpressure matrix remain open.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Extend the status-only private IPC contract into a versioned interface for
commissioning, phone BLE relay, Thread dataset operations, discovery, reads,
and commands.

## Steps

1. Write a protocol table for every opcode, request and response body, error,
length limit, correlation rule, and event direction. Reserve opcode ranges and
version changes before implementing either side.
2. Define typed bounded bodies for commissioning start/status/cancel, BLE
   attach/indication/write-complete, Thread dataset status/create/import,
   device snapshot/read, and supported commands. Reject unknown fields,
   invalid lengths, invalid UTF-8, and incompatible versions.
3. Add a single active commissioning-session state machine with opaque session
   IDs, deadlines, cancellation, BLE connection generations, and explicit
   failure states. Correlation IDs identify IPC calls; they are not user
   authentication.
4. Implement dispatch in the Rust service. Keep PASE, CASE, dataset validation,
   and Matter state in the service. Keep BLE payloads opaque and never log
   setup codes, credentials, or encrypted session traffic.
5. Add matching native request/response/event dispatch. Keep HTTP workers away
   from `Gateway`; route Zigbee mutations through `GatewayRunner` and serialize
   Matter state through the service's single owner.
6. Add cross-language golden vectors and tests for fragmentation, coalescing,
   wrong correlations, cancellation races, stale BLE generations, malformed
   bodies, queue limits, and peer disconnects.
7. Update the protocol document and compatibility rules whenever an opcode or
   body changes.

## Acceptance

- Rust and C++ agree on every golden vector and reject malformed inputs the
  same way.
- A commissioning session can start, report state, cancel, time out, and be
  followed by a new session without stale callbacks affecting it.
- No synchronous command holds an HTTP worker while commissioning waits on
  the phone or radio.
- IPC errors and logs contain no setup code, dataset, or key material.

## Status quo — 2026-10-04

Step 1 is complete: [Matter IPC v1](../matter-ipc-v1.md) assigns all 12 current
opcodes, JSON schema/version rules, request/reply/event directions, fixed error
codes, length/numeric limits, correlations, relay ordering and session lifecycle.
The document explicitly distinguishes reserved contracts from implemented
status behavior. Step 2 is partial: Rust now has strict typed requests for all
assigned actions, schema checks, canonical IDs/hex, PIN/range limits and rejection
of duplicate/unknown fields, invalid UTF-8 and oversized bodies. Typed successful
response/event bodies and matching native validation remain pending.

Step 4 is partial: the running service dispatches status and fixed `malformed`
or `unsupported` errors for new bodies without echoing input; it does not accept
commissioning/control jobs yet. Steps 5 and 6 remain pending; step 7 continues
with every body implementation. The strict decoder checkpoint passed all 22
Rust library checks and the synthetic real-service test; its static stripped
factory MIPS build was 1,343,120 bytes.
No commissioning/control API is claimed at this checkpoint.

Step 3 is now complete as a service building block: the single-owner session
manager owns its pinned commissioning future, uses unpredictable IDs and
monotonic deadlines, drops work before cancellation/reconnect cleanup, clears
zeroizing PIN storage on terminal states and rejects stale BLE generation,
sequence and write callbacks. Typed redacted session snapshots are implemented.
Five focused lifecycle tests pass; the full Rust service-feature library suite
has 27 passing tests. Actual commissioner/fabric/network dispatch wiring remains
in step 4; the running IPC service still does not accept these jobs.
Implementation details are in
`ziggurat-compilation/docs/matter-session-lifecycle.md`.

## Remaining checkpoints

1. Define typed successful replies and events, including Thread coordinator
   observations, session progress and relay writes. Fix schema and size limits
   in the shared contract before app/native consumers depend on them.
2. Connect the session manager to the production commissioner, persisted
   authority, node allocator and Thread dataset. Verify start/cancel/timeout
   without accepting a second concurrent attempt.
3. Dispatch BLE attach, indication, write completion and disconnect through the
   session owner; reject obsolete generation/sequence callbacks and clear
   secret-bearing state on every terminal path.
4. Dispatch device snapshots, reads and supported commands after plan 07 adds
   operational discovery. Return explicit unsupported/not-ready errors until
   each operation is implemented.
5. Extend the native client with bounded nonblocking connection deadlines and
   strict response/event validation; never forward unchecked service JSON.
6. Publish matching Rust/C++ body vectors and exercise partial frames,
   correlations, stale events, queue pressure and disconnect races. Record
   host and factory build results before marking dispatch complete.

## Dependencies

[Rust service runtime](01-rust-service-runtime.md). Final body shapes should be
settled before the API and Garage relay depend on them.

Native client checkpoint — 2026-10-04

The client now supports private opcode-9 coordinator observations and uses one
one-second CLOCK_MONOTONIC deadline across nonblocking connect/send/receive.
Unix backlog EAGAIN is a failed connection, not an in-progress connection;
MSG_NOSIGNAL prevents a disconnected service from terminating the caller.
Descriptors are close-on-exec and checked against select's descriptor limit.

Status and observation replies are validated against their implemented schema
before returning JSON. Duplicate/unknown fields, missing fields, non-boolean
flags, unknown states/errors, invalid channels, trailing data and secret-bearing
fields fail closed, leaving an empty result. The limited parser accepts the
canonical ASCII fields/enums emitted by Rust, booleans, small unsigned integers
and the single result object; it rejects strings with escapes and arrays. This
is not yet the validator for device or commissioning replies.

Focused tests cover fragmented replies, schema failures, valid observation
success/error envelopes, invalid channel input and bounded connect against a
full Unix listen backlog. The private observer worker and other opcode dispatch
remain pending. No gateway deployment is involved.

Verification passed in sequence: `make host test`, `make mips`,
`make ZIGGURAT=1 MATTER=1 host test`, and
`make ZIGGURAT=1 MATTER=1 mips`. Native linking includes `-lrt` for the
monotonic-clock interface on older libc platforms. Full commissioning/device
body vectors and asynchronous dispatch remain open.

Targeted event checkpoint — 2026-10-04

Step 2 now includes typed bounded success/error reply envelopes and typed
Thread, commissioning-snapshot and opaque BLE-write events, with fixed Rust
serialization vectors. Thread replies use the new serializer. BLE-write bounds
are 1–244 decoded bytes and nonzero IDs; only the dedicated event contains
opaque relay data. Device response/event schemas and native event/body vectors
remain pending.

Step 4 includes targeted event queues with stable connection IDs, no broadcast,
no event-driven idle extension and per-peer backpressure/disconnect isolation.
The running service supports opt-in Thread snapshot events. Commissioning/BLE
event production will be wired with the owned commissioner task; jobs 3–8
and 10–11 still return `unsupported` rather than simulating successful work.

Evidence: 41 service-feature Rust tests and 24 control-ipc-only tests pass.
Focused tests cover reply/event order, isolated subscriber delivery, queue
overflow, retired peer IDs, expiry, malformed event framing and ID exhaustion.
The real synthetic service test receives the initial awaiting-coordinator event
and the subsequent observed-channel event. Native-to-real-Rust synthetic
observation/HTTP regression also passes; no gateway deployment is involved.

Factory service build passes: `bash scripts/build-rs-matter-transport.sh mips
service`; stripped artifact is static big-endian MIPS-I/o32, 1,556,124 bytes.
It was not deployed or executed on the gateway. This checkpoint supplies the
event-delivery prerequisite for commissioning, not a commissioned accessory.

Implementation is committed in `ziggurat-compilation`; see its latest
`Add bounded targeted Matter events and typed outgoing bodies` checkpoint.
The next step remains owned commissioner/session dispatch and persistent native
event consumption; Thread subscription does not replace that work.

## Owned commissioner dispatch status — 2026-10-04

| Step | Current status | Evidence / remaining work |
| --- | --- | --- |
| 1 | Complete | Opcodes, optional subscription and concrete callback result fields documented in IPC v1. |
| 2 | Partial | Strict requests, session/Thread replies, device job requests/replies, typed commissioning/BLE/Thread events, and typed Matter report events implemented; broader report vectors remain. |
| 3 | Complete | Production owner uses the bounded session manager and pinned real commissioner future with cancellation/reconnect cleanup. |
| 4 | Partial | Status, Thread lifecycle, commissioning/relay and native/Rust OnOff job path verified through confirmed readback. Broader failure/recovery gates remain. |
| 5 | Partial | Native observer/status worker consumes Matter report events into shared device state; queued commissioning/control requests remain pending. |
| 6 | Partial | Rust body vectors, callback/reconnect/timeout, Matter broadcast, and native report parser tests pass; full native cross-language body/event vectors remain pending. |
| 7 | Current | Contract and implementation evidence updated at this checkpoint. |

The running service accepts start only with an active canonical dataset and a
fresh matching native channel observation. Start/status do not write credentials.
Attach installs the real task; it commits a monotonic node allocation before
using the persisted signer, authenticating PASE, provisioning Thread and trying
CASE completion. Reattach drops the old task and authority borrow first. It
publishes typed progress and C1 events, validates generation/sequence/write IDs,
and rejects stale callbacks. Thread readiness loss fails active work rather than
continuing on a mismatched network. No producer logs or echoes setup secrets.

42 service-feature tests and 24 control-only tests pass. The actual service
harness proves waiting-BLE/busy/status, BTP write generation, PASE reconnect,
stale callbacks, cancel/idempotence, failed write, a real 30-second timeout and
no status/heartbeat store writes. The native-to-real-Rust HTTP regression passes.

`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner` uses this production owner/future with the encrypted synthetic
accessory; it passes PASE, Thread provisioning, CASE completion/controls and fresh
CASE after committed authority/fabric reopen with the phone detached. It retains
synthetic GATT and fixture-only rendezvous, not production DNS-SD or real Android
BLE. Owner/direct BTP/direct UDP host timings are 206/199/246 ms.

Factory service and fixture builds pass, static big-endian MIPS-I/o32: stripped
service 1,740,456 bytes; commissioning fixture 2,076,352 bytes. These are package
sizes, not target RSS/performance measurements; no updated target execution or
plan 08 action occurred. Production commissioning completion remains dependent
on plan 07 DNS-SD/catalog work and native/Garage wiring from plans 04/05.

Implementation checkpoint: `ziggurat-compilation` commit
`Wire production owned commissioner into service IPC and BTP transport`.
Detailed callback/lifetime and fixture scope are recorded in its
`docs/matter-session-lifecycle.md` and
`docs/rs-matter-openthread-commissioning.md`.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Implement one worker-owned persistent native IPC channel. Preserve
   private-socket checks, bounded connect/read/write deadlines and unique live
   correlations; demultiplex replies and opcode-12 events between requests.
2. [x] Add strict native validators for session replies, progress, BLE writes
   and callback acknowledgements. Bound hex payloads, counters and event queues;
   reject unknown fields, stale correlations and malformed service data.
3. [ ] Add a bounded mutation queue and reconnect policy. Discard interrupted
   calls explicitly and resubscribe retained sessions without replaying
   commissioning start, dataset mutations or GATT writes blindly.
4. [ ] Test cross-language body/event vectors, fragmented/coalesced frames,
   interleaved events, wrong IDs, queue pressure, disconnects and stale BLE
   callbacks against both fake peers and the actual Rust service.
5. [x] Once plan 07 supplies discovery/catalog operations, finish typed device
   snapshot/read/command replies and dispatch. Snapshot pages and OnOff/Level
   jobs are implemented; retain explicit unsupported or not-ready results for
   operations whose service implementation is still open.
6. [ ] Update IPC v1 and compatibility notes, run both native build variants
   sequentially plus Rust tests/factory build, then record and commit evidence.

## Rust operational job dispatch checkpoint — 2026-10-04

The Rust service now accepts supported OnOff attribute reads and on/off/toggle
commands as bounded asynchronous jobs. Opcode 10 also polls job status. Admission
requires attached Thread, no active commissioning, a completed catalog entry and
an online endpoint advertising OnOff. The service rechecks catalog and capability
before execution, serializes operations with commissioning, runs one CASE
operation at a time, caps the queue at eight and applies a 30-second deadline
including queue wait. Radio loss fails queued and active jobs; mutations are
never retried. The synthetic encrypted accessory fixture exercises queued CASE
commands and confirmed readback.

Verification: 59 service-feature Rust tests pass; the production-owned synthetic
fixture passes commissioning, discovery and OnOff job readback; host and MIPS
service and commissioning builds pass. MIPS linking reports existing experimental
MIPS-I and abicalls/non-abicalls warnings. No gateway deployment or plan 08 test
occurred.

This closes the Rust service-dispatch portion of remaining checkpoint 4 and adds
the Rust side of plan 07 OnOff operations. Native typed job validation, HTTP
read/control routes, dashboard controls, Level operations, sensors, reporting,
and extended interruption/restart recovery remain open. Implementation and
executor behavior are documented in
`ziggurat-compilation/docs/matter-operational-transactions.md`.

## Native operational job validator checkpoint — 2026-10-04

The native Matter channel now validates opcode-10 OnOff read and job-poll
requests, opcode-11 on/off/toggle requests, Rust asynchronous acceptance/status
replies, fixed failure codes and completed typed boolean readbacks. Unsupported
cluster paths and Level commands are rejected before queueing. This is strict
wire validation only; public HTTP read/control routes are still open.

Verification: default `make host test` and `make mips`, then
`make ZIGGURAT=1 MATTER=1 host test` and `make ZIGGURAT=1 MATTER=1 mips` all
pass. Host integration includes the native IPC/HTTP suite; MIPS emits the
existing assembler LOAD-USE warnings. The Rust fixture remains the encrypted
CASE evidence for actual job completion. No gateway deployment or plan 08 test
occurred.

## Native OnOff HTTP admission checkpoint — 2026-10-04

The native server now validates and accepts `/api/matter/devices/read` and
`/api/matter/devices/command` requests, converts them to the private opcode-10/11
contract and submits them through the worker queue. It rejects unsupported
cluster paths and Level commands before queueing. The worker retains the Rust
job ID, polls status without replaying the operation, and keeps its native job
running until confirmed readback or a fixed failure reaches the public job API.

Default and Matter-enabled host unit/integration suites and MIPS builds pass.
The Matter-enabled HTTP tests confirm accepted OnOff request validation and
pre-queue rejection for unsupported Level commands. Device deployment remains
out of scope.

## Native Matter operation polling checkpoint — 2026-10-04

The native worker now follows Rust operational jobs after IPC acceptance. It
polls one Rust job at a time over the persistent channel, exposes the native job
as running while Rust reports queued/running, and publishes completion only
after Rust's separate CASE readback. Rust operation failures become bounded
native error codes; transport loss fails pending work as `transport` and clears
it without replay. Heartbeats, status refresh and Thread coordinator ownership
remain on the same Matter worker.

The synthetic native worker harness exercises On, read, toggle and off through
the HTTP endpoints and a fake peer's queued/running/complete responses, checking
confirmed boolean values. The `--real-service` harness also passes against the
actual Rust service and synthetic RCP, preserving existing status, Thread and
relay behavior. Default and Matter-enabled host tests plus both MIPS builds
pass; the target assembler emits its existing LOAD-USE warnings. No physical
gateway or plan 08 test was performed.

Validation: golden vectors plus actual-service request/event round trips; no
HTTP worker may wait for a commissioning lifetime or forward unchecked JSON.

## Native persistent channel checkpoint — 2026-10-04

`MatterChannel` provides a single-owner persistent Unix stream, with private
socket checks, nonblocking descriptors and one-second total call deadlines.
Each call has a new nonzero 32-bit correlation; exhaustion refuses further calls
rather than wrapping. It consumes interleaved events while waiting for replies
and can poll events between calls. Fragmented data remains in the bounded frame
decoder. Wrong replies, malformed events, transport failure and overflow close
the connection and discard queued events; interrupted mutations are not replayed.

Native schema validation now covers commissioning start/status/cancel replies,
BLE attach/indication/write-complete acknowledgements and typed commissioning,
Thread and BLE-write events. It validates canonical IDs, states/errors, deadlines,
u32 counters and opaque lowercase hex payloads of at most 244 bytes. There are
at most eight queued events. Payloads are never logged.

Focused socket tests cover successive correlated calls on one connection,
byte-fragmented events before replies, idle event consumption, wrong reply IDs,
u32 overflow, invalid hex, unexpected fields and event queue saturation. Exact
Rust cancelled-session and BLE-write vectors are included. Worker integration,
mutation queues, subscriptions/reconnect recovery and public APIs remain open;
the running bridge still uses its existing observation/status client.

Verification passed: `make host test`, `make mips`,
`make ZIGGURAT=1 MATTER=1 host test` and
`make ZIGGURAT=1 MATTER=1 mips`. Default/enabled test suites ran sequentially;
the enabled suite includes 13 native peer tests. Both factory binaries are
stripped static 32-bit big-endian MIPS-I ELF files. Host IPC tests require
execution outside the workspace sandbox, which blocked the initial socket
write. No gateway deployment, physical pairing or plan 08 action occurred.

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

The bounded handshake/frame codec and focused host tests are implemented as a
building block for the native relay. Details, message limits and current scope
are recorded in [plan 04](04-native-api-dashboard.md#websocket-transport-checkpoint--2026-10-04).
There is no public upgrade route yet. Socket handoff, typed session/BLE dispatch,
connection ownership, deadlines and recovery remain open before Garage can
consume a stable native relay API. This is neither Android implementation nor
real-accessory acceptance evidence.

Verification passed: default and Matter-enabled `make host test`/`make mips`.
Host suites ran sequentially; enabled peer regression checks pass (13 tests).
Both factory builds produce static stripped big-endian MIPS-I ELF files.
No HTTP handoff, actual phone relay, deployment or plan 08 action occurred.

## Native relay ownership checkpoint — 2026-10-04

The native plain-HTTP WebSocket endpoint and typed session/BLE dispatch are now
wired to the persistent Matter worker. Contract, queue/deadline bounds, single
phone ownership, session/generation filtering and disconnect/reconnect policy
are documented in [the native API](../matter-native-api-v1.md#owned-ble-relay-websocket)
and [plan 04](04-native-api-dashboard.md#worker-owned-relay-checkpoint--2026-10-04).
The app can now target a concrete relay contract; Garage code and fake-GATT/app
lifecycle tests remain open. Native synthetic peer evidence is not Android or
real-device acceptance evidence.

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

## Rust Level Control executor checkpoint — 2026-10-04

Rust commit `6ec569a` adds opcode-10 CurrentLevel reads and opcode-11
MoveToLevel to the bounded operational job manager. Start-time capability checks
require an online completed catalog node and endpoint advertising cluster 8.
The command validates a Matter level of 0–254 and `u16` transition time, then
performs a separate CurrentLevel read; a null readback cannot report mutation
success. Typed results retain cluster/attribute identity and return a tagged
`u8` value. Unsupported cluster paths fail before an exchange starts.

Verification passed: 59 Rust service-feature library tests; host service and
commissioning-probe builds; MIPS service and commissioning-probe builds; and
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery`. The encrypted synthetic accessory confirms
CurrentLevel read, MoveToLevel to 124, readback 124, and rejection of a root
endpoint without cluster 8. The command marker is emitted only after those checks.
The stripped MIPS service is 2,010,808 bytes (SHA-256
`97d2125621658f93121cf35b1ac4bed87ce1b659bb832f80489c0fb1bb0a2b75`); the
commissioning probe is 2,440,920 bytes (SHA-256
`d290dce4c9fd24704267a485d426e8699dc3d56e4d7cec95698e5d55f675c239`). Expected
MIPS-I/ABI linker warnings remain. No target execution or plan 08 action occurred.

Native C++ typed reply checks, HTTP route, worker execution and dashboard do not
yet support Level. Plan 02 device-control integration and plan 04 dashboard work
therefore remain partial; sensors, subscriptions and report delivery remain
open in plan 07.

## Native Level request/reply checkpoint — 2026-10-04

Native commit `1ab6e91` extends the strict opcode-10 read validator to accept only
OnOff cluster 6 or Level Control cluster 8 attribute 0. Opcode 11 accepts
`move_to_level` only with level 0–254 and transition time 0–65535; unknown and
extra fields remain rejected. Completed operation validation checks the
cluster-specific boolean/U8 tag and value, preserving null for unknown
CurrentLevel reads. The generic Matter worker carries these operation jobs
through confirmed Rust readback without replay.

Verification passed: `make host test`,
`make ZIGGURAT=1 MATTER=1 host test`, `make mips`,
`make ZIGGURAT=1 MATTER=1 mips`, and
`python3 scripts/test_matter_native_worker.py`. Unit tests cover valid OnOff and
Level bodies, malformed ranges and typed result validation; HTTP integration
checks accept valid Level reads/commands for service admission and reject level
255. The synthetic worker fixture verifies CurrentLevel read, MoveToLevel and
confirmed value 124 over the native HTTP/job path. The MIPS builds pass with the
toolchain's expected LOAD-USE warnings. No target execution occurred.

This closes native Level IPC/API admission; plan 04 dashboard control remains
open, as do reports, subscriptions and broad restart/failure coverage.

## Sensor read IPC checkpoint — 2026-10-04

Rust commit `ca0019e` adds advertised-cluster CASE reads for Air Quality (91),
Temperature Measurement (1026), Relative Humidity Measurement (1029), CO2
Concentration (1037), and PM2.5 Concentration (1066), returning typed values,
units, and measured/unknown quality. Native read admission and completed-job
validation now accept these clusters and reject bad cluster-specific units,
ranges, malformed decimals, and inconsistent null/quality states.

Verification passed: 60 Rust service library tests; the production-owned
synthetic encrypted fixture including SRP/DNS discovery and all five sensor
reads; and `make ZIGGURAT=1 MATTER=1 host test` including native unit,
integration, and Ziggurat peer tests. Details and value scaling are in
`ziggurat-compilation/docs/matter-sensors.md` and
`docs/matter-native-api-v1.md`. The generic worker can carry the typed results,
and native commit `02f170f` now applies successful results to matching
registered Matter devices before completing their job. The shared status view
exposes those values to the common device/dashboard path; persistence, history,
subscriptions, reports and broad restart/failure coverage remain open. No
device deployment or plan 08 acceptance occurred.

## Matter report-event IPC checkpoint — 2026-10-04

Rust commit `3aa4600` adds a strictly validated `matter_report` event carrying
the typed common operation value, broadcasts it to all live IPC clients, and
retains queued readings until at least one client accepts each event. The native
client validates node, endpoint, attribute, cluster, value type, units, ranges,
and known/unknown quality. Native commit `5ec72cb` routes these events directly
to `Gateway::applyMatterReading`, so common device state, history, and value
alerts receive them; they do not enter the phone relay event queue.

Verification passed: all 65 Rust service-feature library tests including event
serialization and multi-client IPC broadcast; controller-service release build;
`make ZIGGURAT=1 MATTER=1 host test`; and `make ZIGGURAT=1 MATTER=1 mips` (with
the toolchain's existing LOAD-USE warnings). Native unit coverage checks valid
OnOff/Level report events and rejects out-of-range CurrentLevel. The manager's
synthetic production-owner fixture verifies subscription and report correlation
separately; a single live Rust-service-to-native-process report fixture remains
open. Sensor reports are decoded and correlated in the production Manager, but
live service-to-native delivery, remote-node recovery, sleepy-device policy,
broader backpressure/restart tests, and physical
acceptance remain open. No device deployment or plan 08 action occurred.

### Sensor report cross-language vectors — 2026-10-04

Rust commit `23b8f19` asserts exact serialized opcode-12 event payloads for
temperature, humidity unknown/null, air quality, CO2 and PM2.5. Matching native
client vectors parse the same payloads and assert the shared typed values,
units and quality. Verification passed with 70/70 Rust library tests using
`source scripts/rust-env.sh && cargo +nightly-2026-10-02 test
--manifest-path rs-matter-probe/Cargo.toml --features controller-probe --lib`
and `make ZIGGURAT=1 MATTER=1 host test` (including all native unit,
integration and Ziggurat peer tests). This closes the report wire-vector gap;
it does not replace the still-open live Rust-service-to-native-process fixture.

## Production report-path revalidation — 2026-10-04

The host-only `python3 scripts/test_matter_native_worker.py --real-service`
fixture passes with the real Rust service process and native worker. Separately,
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery --sensor-reports` passes against the
synthetic RCP/accessory and production-owned Manager, including normalized
subscribed sensor reports over CASE. These are adjacent but separate fixtures:
the real-service fixture does not commission the synthetic accessory or
produce a report, while the report fixture does not launch the native worker.
They do not close the live Rust-service-to-native report-delivery checkpoint.
No gateway or radio was used. The next deliverable remains a composed host
fixture that drives a production Manager report through service IPC and
verifies native shared state.

## Rust Manager-to-IPC delivery step — 2026-10-04

Rust commit `146cfbd` moves the service report-drain path onto
`Manager::publish_reports`, which serializes and broadcasts queued readings
through the production `IpcServer`, and only removes a report after at least
one connected client accepts it. A Rust socket test enters through
`record_report` with a matching fabric/node/subscription, verifies that the
report remains queued with no clients, then checks the exact sensor event
payload received by a Unix IPC client. The production service now calls this
same method. Verification passed: 71/71 controller-probe library tests and the
release `thread-matter` build. The C++ native parser has matching vectors in
native commit `3a763df` and its full host suite passes. A process-level test
where the live `thread-matter` binary delivers an accessory report into native
shared device state remains open.

Revalidation after the manager refactor: the rebuilt release service passes
`python3 scripts/test_matter_native_worker.py --real-service` with the native
worker. This covers process startup, IPC status/snapshot and Thread lifecycle
traffic after the change, but still has no commissioned accessory or report.

## Live accessory report through production IPC — 2026-10-04

Rust commit `db459aa` extends the notifier-backed encrypted accessory fixture:
each temperature, humidity, air-quality, CO2 and PM2.5 ReportData value enters
the production Manager, is correlated by fabric/node/subscription/path, passes
through `Manager::publish_reports` and the production `IpcServer`, and is read
from a connected Unix socket client. The fixture checks the full typed event
payload for each measurement. Verification passed with
`bash scripts/build-rs-matter-transport.sh host commissioning` followed by
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery --sensor-reports`. This composes accessory,
Manager and IPC delivery in one host fixture. It still does not start the
`thread-matter` daemon or a native gateway process; delivery into registered
native shared state, history and alerts remains open. No gateway or hardware
radio was used.
