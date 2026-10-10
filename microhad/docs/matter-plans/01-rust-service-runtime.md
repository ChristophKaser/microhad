# Rust Matter service runtime

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../../../micro-thread-matter-daemon/docs/matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

`mthread-matter` owns OpenThread, Matter transport, controller identity, catalog,
commissioning and private IPC in one event loop. MicroHAD owns HTTP, device
state and the persistent IPC worker; Ziggurat owns the radio UART. The paired
supervisor starts and monitors these processes with bounded restart budgets.

Build and test with the [component commands](../../../micro-thread-matter-daemon/README.md).
The host report fixture uses an isolated test-hook build; production artifacts
must pass the test-hook absence check. Never replay interrupted mutations.

Release checks:

1. Build native and Rust host/MIPS artifacts, and run the synthetic service,
   worker, report and supervisor fixtures.
2. Update both daemons together using the [maintenance deployment](../../AGENTS.md).
   Preserve configuration, network state, identity and rollback backups.
3. Measure target CPU, peak RSS and flash headroom under sustained traffic.
4. Verify service restart, radio loss, retry exhaustion and socket ownership.
5. Complete [physical acceptance](08-real-device-acceptance.md) before treating
   host fixtures as accessory interoperability evidence.
