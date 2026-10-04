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
