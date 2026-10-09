# Persistent fabric identity

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

The service persists its controller authority, fabric, node allocator, Thread
dataset and catalog under the configured private state directory. Reads do not
write; interrupted writes must retain a valid identity. Failed commissioning
may leave deliberate node-ID gaps. Corrupt or inconsistent records fail closed.

Use the [identity/store contract](../matter-identity-store-v1.md),
[state validation](../../../docs/matter-state-validation.md),
[catalog rules](../../../docs/matter-node-catalog.md) and
[backup/recovery runbook](../matter-backup-recovery.md).

Acceptance requires identity-preserving service/gateway restart, CASE recovery
without recommissioning, bounded interrupted-node recovery, corrupt-state
rejection, and a demonstrated rollback. Preserve all private state and never
commit or print its contents. Physical restart and power-loss testing belongs to
[real-device acceptance](08-real-device-acceptance.md).
