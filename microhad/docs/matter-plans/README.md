# Matter implementation and acceptance

The current component is `micro-thread-matter-daemon/`, executable `mthread-matter`.
Private messages use [CBOR IPC v2](../matter-ipc-v2.md); public APIs remain JSON.

The implementation has synthetic host and target-build verification. Physical
accessory interoperability, sustained resource/RF behavior and full restart
acceptance remain open. CBOR v2 requires a coordinated native/service release.

1. [Service runtime and release checks](01-rust-service-runtime.md)
2. [Commissioning and control IPC](02-commissioning-control-ipc.md)
3. [Persistent identity and recovery](03-persistent-fabric-identity.md)
4. [Native API and dashboard](04-native-api-dashboard.md)
5. [Phone BLE relay](05-garage-ble-relay.md)
6. [Thread lifecycle](06-thread-network-lifecycle.md)
7. [Device model and reports](07-device-model-and-reports.md)
8. [Physical acceptance](08-real-device-acceptance.md)

Use the component build/test commands and current contracts when changing the
implementation. Keep device configuration, identities and network state private.
Device deployment requires an explicit request and the documented maintenance
flow; host tests do not authorize device access.
