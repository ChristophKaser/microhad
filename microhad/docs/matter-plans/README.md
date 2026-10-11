# Matter development and acceptance

The implementation references live in the [documentation index](../../../docs/README.md#develop-the-stack).
Use [physical acceptance](08-real-device-acceptance.md) for the remaining
phone/accessory, restart and sustained RF/resource checks. Host tests and target
builds do not establish hardware interoperability.

Development entrypoints:

- [Runtime/builds](../../../micro-thread-matter-daemon/README.md).
- [CBOR IPC](../../../micro-thread-matter-daemon/docs/matter-ipc-v2.md).
- [HTTP/WebSocket API](../matter-native-api-v1.md) and [dashboard](../matter-dashboard.md).
- [Identity/store](../matter-identity-store-v1.md) and [backup/recovery](../matter-backup-recovery.md).
- [Commissioning sessions](../../../docs/matter-session-lifecycle.md).
- [Catalog](../../../docs/matter-node-catalog.md), [descriptors](../../../docs/matter-device-discovery.md) and [reports](../../../docs/matter-sensors.md).
- [Coordinated release](../../../docs/deployment.md#maintenance-release).
