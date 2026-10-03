# Plan: Rust Matter service runtime

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
| 2 | Pending | Optional launcher child, restart budget and packaging still need wiring. |
| 3 | Complete | Mode 0600, flock duplicate exclusion, nonblocking stale socket probe, unsafe path refusal and inode-specific orderly cleanup; host checks passed. |
| 4 | Complete | Four clients, four accepts per turn, 4096-byte read/write budgets, eight queued replies, two-second idle expiry; malformed/overload/idle checks passed. |
| 5 | Complete | Empty requests for opcodes 1/2 return bounded JSON; synthetic host reports `awaiting_fabric` and `detached`, without credentials. |
| 6 | Partial | Matter/Thread/IPC pumps and SIGTERM/SIGINT shutdown implemented. Startup/failure exits are defined; supervisor retry and radio reconnection remain. |
| 7 | Partial | All 19 Rust library tests pass, including six listener tests; synthetic runtime test passes. Factory build is static big-endian MIPS-I/o32, stripped 1,228,412 bytes with no interpreter. Target execution and full native API round trip remain unverified. |
| 8 | Partial | Invocation, ownership, logs, private settings, bounds and current failure behavior are documented in `ziggurat-compilation/docs/matter-service-runtime.md`; launcher recovery documentation remains. |

This checkpoint supplies a running transport/status service. It does not yet
create a controller fabric, attach a Thread network, commission an accessory,
or expose operational device commands. No deployment or physical radio test
was performed; plan 08 remains excluded.

Host socket tests require execution outside the workspace sandbox (otherwise
Unix sockets return EPERM). The factory link additionally required an `accept4`
shim for old uClibc, using `accept` and immediate descriptor flags in the
single-threaded process.

## Depends on

The existing bounded Rust/C++ frame format and status opcode assignments. The
Thread radio endpoint must be supplied by the Thread network lifecycle work
before claiming radio readiness.
