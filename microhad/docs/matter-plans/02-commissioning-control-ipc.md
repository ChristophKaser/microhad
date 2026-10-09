# Commissioning and control IPC

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

The [v2 contract](../matter-ipc-v2.md) and [CDDL schema](../../../protocol/matter-ipc-v2.cddl)
define status, commissioning, BLE callbacks, Thread operations, device jobs and
events. Only the length prefix remains outside CBOR. Unknown fields are skipped
with opcode/count warnings; known types, ranges and authorization remain checked.

The native worker owns one persistent channel. Requests use nonzero u32 IDs,
responses repeat their ID/opcode, and events use zero. Jobs are bounded; a full
queue rejects admission. Commissioning events target the selected relay owner.
Disconnect cancels an active session; reconnect invalidates old generations.

Verification covers fragmentation, joined frames, malformed encoding, structural
bounds, unknown fields, event interleaving, queue pressure, cancellation and
recovery. Public JSON validation remains strict. See the
[HTTP API](../matter-native-api-v1.md) and [session lifecycle](../../../docs/matter-session-lifecycle.md).
Physical acceptance must cover phone loss, radio loss and service restart during
commissioning without disclosing credentials or replaying mutations.
