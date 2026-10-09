# Matter device model and reports

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

The service discovers operational addresses through SRP/DNS-SD and reads
Descriptor capabilities through CASE. The bounded catalog and endpoint snapshots
feed native devices, capability-gated commands, typed readings and reports.
Product names do not imply capabilities. Cached metadata may remain while a
node is offline; unavailable values stay unknown.

Use the [catalog](../../../docs/matter-node-catalog.md),
[descriptor discovery](../../../docs/matter-device-discovery.md),
[snapshot contract](../../../docs/matter-device-snapshot-ipc.md),
[sensor values](../../../docs/matter-sensors.md) and
[operation jobs](../../../docs/matter-operational-transactions.md).

Acceptance includes stable IDs, bounded pagination/revision checks, operational
address invalidation and recovery, interrupted commissioning recovery, command
readback, subscription renewal, nullable/signed sensor values, button/contact
mapping, reports before endpoint registration, and device presence/history.
The host CASE/report fixtures exercise the production manager and separate service
handoff. Physical accessory recovery, RF behavior and sustained resource use
remain part of [real-device acceptance](08-real-device-acceptance.md).
