# Plan: Commissioning and device-control IPC

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
| 2 | Partial | Strict requests, session/Thread replies and typed commissioning/BLE/Thread events implemented; device replies/events and native validators remain. |
| 3 | Complete | Production owner uses the bounded session manager and pinned real commissioner future with cancellation/reconnect cleanup. |
| 4 | Partial | Status, Thread lifecycle and commissioning/relay opcodes 3–8 dispatch; owned encrypted PASE, provision and CASE path verified in fixture. Production operational discovery and device read/control dispatch remain in plan 07. |
| 5 | Partial | Native observer/status worker implemented; persistent event consumer and queued commissioning/control requests remain pending. |
| 6 | Partial | Rust body vectors, callback/reconnect/timeout tests and actual service IPC tests pass; full native cross-language body/event vectors remain pending. |
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
5. [ ] Once plan 07 supplies discovery/catalog operations, finish typed device
   snapshot/read/command replies and dispatch. Retain explicit unsupported or
   not-ready results until each operation is available.
6. [ ] Update IPC v1 and compatibility notes, run both native build variants
   sequentially plus Rust tests/factory build, then record and commit evidence.

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
