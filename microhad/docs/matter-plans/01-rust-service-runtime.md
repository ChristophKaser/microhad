# Plan: Rust Matter service runtime

## Current open work — 2026-10-04

Runtime recovery and final build evidence remain open; the host service and optional
supervisor exist.

Next deliverable: Add an integrated failure/restart matrix for service exit, coordinator
restart, Thread socket loss and exhausted supervisor retries. Record bounded recovery
and continued Zigbee ownership.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Run the production Matter controller as a separate MIPS process and make the
native status routes report its actual state over `/tmp/microhad-matter.sock`.

## Steps

1. Add a long-running service entry point in `ziggurat-compilation` that owns
   the OpenThread instance, Matter `Matter` state, private credential store,
   and IPC listener on one event-loop thread.
2. Integrate service startup, supervision, restart behavior, and shutdown with
   the gateway's existing launcher. The service must never open the Zigbee UART
   or terminate the production coordinator when it fails.
3. Create the Unix socket with mode `0600`; refuse unsafe pre-existing paths,
   remove only a stale socket owned by the service user, and remove the socket
   on orderly shutdown. Keep the socket path aligned with the native client.
4. Implement bounded accept/read/write handling with the existing version-1
   frame decoder. Cap active clients and queues, reject malformed requests, and
   close idle or failed peers without logging payloads.
5. Implement status opcodes 1 and 2 with JSON objects that describe controller
   availability and Thread attachment without including datasets, keys, setup
   codes, or other credentials.
6. Integrate IPC polling with the OpenThread and Matter task pumps so requests
   remain responsive while timers and UDP traffic progress. Define startup,
   radio-disconnect, retry, and shutdown states explicitly.
7. Add host tests for socket permissions, fragmented/coalesced frames, stale
   peers, overload, bounded response time, and clean shutdown. Build the
   service for the factory target and verify the ELF and package size.
8. Document invocation, socket ownership, logs, state paths, resource limits,
   and recovery behavior. Keep deployment separate from the production service
   until the later device-acceptance plan passes.

## Acceptance

- A host service answers both status opcodes with valid bounded JSON.
- The factory MIPS service starts and stops without touching `/dev/ttyS1`.
- The native enabled status routes return the service snapshot when connected
  and 503 when it is absent or malformed.
- Socket permissions are owner-only and no credential payload reaches logs.

## Status quo — 2026-10-04

| Step | Status | Evidence / remaining work |
| --- | --- | --- |
| 1 | Complete | `thread-matter` owns Matter, OpenThread, private store and IPC in one event loop; host synthetic-RCP startup/status/shutdown passed. |
| 2 | Complete | Optional `MATTER_BIN` child in `paired_start.sh`, independent bounded retries, stop-before-native/radio ordering and opt-in staging; ten synthetic supervisor tests pass. Production launcher activation remains outside this checkpoint. |
| 3 | Complete | Mode 0600, flock duplicate exclusion, nonblocking stale socket probe, unsafe path refusal and inode-specific orderly cleanup; host checks passed. |
| 4 | Complete | Four clients, four accepts per turn, 4096-byte read/write budgets, eight queued replies, two-second idle expiry; malformed/overload/idle checks passed. |
| 5 | Complete | Empty requests for opcodes 1/2 return bounded JSON; synthetic host reports `running` with its persistent fabric and Thread `detached`, without credentials. |
| 6 | Partial | Matter/Thread/IPC pumps and SIGTERM/SIGINT shutdown implemented. Supervisor retries failures with backoff and stops/restarts Matter on coordinator loss; complete dataset/radio reconnection remains in plan 06. |
| 7 | Partial | All 31 Rust library tests pass, including six listener tests; synthetic runtime and process restart pass. Factory build with authority creation is static big-endian MIPS-I/o32, stripped 1,527,452 bytes with no interpreter. Target execution and full native API round trip remain unverified. |
| 8 | Complete | Runtime invocation, ownership, bounds and state paths documented in `ziggurat-compilation/docs/matter-service-runtime.md`; optional supervisor configuration, failure markers, retry budget and staging documented in `MATTER_INTEGRATION.md`. |

This checkpoint supplies a running transport/status service and persistent
controller fabric. It does not yet attach a Thread network, commission an accessory,
or expose operational device commands. No deployment or physical radio test
was performed; plan 08 remains excluded.

Host socket tests require execution outside the workspace sandbox (otherwise
Unix sockets return EPERM). The factory link additionally required an `accept4`
shim for old uClibc, using `accept` and immediate descriptor flags in the
single-threaded process.

The launcher checkpoint also passes a mocked deployment check proving that
`--matter-service` stages only the optional executable and paired supervisor
alongside the selected native binary. It neither transfers credentials nor
starts a process. No gateway connection was made during these tests.

## Remaining checkpoints

1. Finish the channel-gated lifecycle in plan 06, then verify status while
   awaiting a coordinator, attaching, attached, disconnected and restarting.
   The guarded lifecycle synthetic service test now passes; native observation
   delivery and integrated radio-loss behavior remain pending.
2. Exercise service failure, radio loss and supervisor retry exhaustion together;
   confirm bounded IPC latency and that Zigbee/native supervision continues.
3. Verify a native HTTP-to-service status round trip with valid, malformed,
   absent and stalled service peers. Record the current factory binary hash,
   size and ELF checks rather than reusing earlier build measurements.
4. Defer physical startup/resource measurements to an explicitly arranged,
   backed-up gateway test; distinguish synthetic evidence from target evidence.

## Dependencies

The existing bounded Rust/C++ frame format and status opcode assignments. The
Thread radio endpoint must be supplied by the Thread network lifecycle work
before claiming radio readiness.

Latest Thread lifecycle checkpoint: 36 Rust checks, synthetic service restart
and encrypted fixture regressions pass. Factory service size is 1,552,028 bytes.
Startup now reports `awaiting_coordinator` before a fresh private channel
observation, then `running`; prior measurements/states above are historical.

Native worker checkpoint: an actual enabled native-to-Rust IPC/HTTP round trip
now passes with a synthetic Ziggurat coordinator and Thread RCP. Native online
channel observations authorize guarded startup and dataset creation, radio loss
produces `awaiting_coordinator`, and service loss clears cached HTTP status to
503. Target execution and broader restart/failure recovery remain pending.

The owned-commissioner checkpoint connects the session task, BTP/UDP transport
and commissioning IPC/events. 42 service-feature checks and the actual running
service lifetime harness, including a real 30-second timeout, pass. Current
factory service is 1,740,456 stripped bytes; target execution remains unverified.
Native event consumption, production discovery and device jobs remain open.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [ ] Add integrated failure scenarios for service exit, Thread socket loss,
   coordinator restart and supervisor retry exhaustion. Verify the native
   bridge and Zigbee supervision continue and no second UART owner starts.
2. [ ] Define recovery for every startup/radio-loss state and verify bounded
   reconnect/backoff with plan 06. Test IPC responsiveness during recovery.
3. [ ] Repeat the enabled native HTTP-to-service test after persistent IPC is
   added in plan 02; include absent, malformed and stalled peers.
4. [ ] Record reproducible host/factory build commands, hashes, ELF properties
   and artifact sizes for the final implementation. Keep host timings separate
   from target CPU/RSS measurements.
5. [ ] Hand target startup, shutdown and resource verification to plan 08 with
   a concrete test manifest. Close this plan only after its acceptance evidence
   is recorded; a cross-build alone does not verify target execution.

Validation: synthetic supervisor/service/native integration tests and factory
cross-build; physical execution remains a separate acceptance checkpoint.
