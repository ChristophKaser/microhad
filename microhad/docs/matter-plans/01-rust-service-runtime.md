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

## Depends on

The existing bounded Rust/C++ frame format and status opcode assignments. The
Thread radio endpoint must be supplied by the Thread network lifecycle work
before claiming radio readiness.
