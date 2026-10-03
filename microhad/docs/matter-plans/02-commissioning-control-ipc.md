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

## Depends on

[Rust service runtime](01-rust-service-runtime.md). Final body shapes should be
settled before the API and Garage relay depend on them.
