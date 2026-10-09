# Garage BLE relay

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

The phone owns the accessory GATT link and carries BTP packets through the
native owned WebSocket relay. The service owns PASE, provisioning and CASE;
Thread credentials and controller secrets remain in the service.

Every callback checks session and generation. Indications are sequenced, only
one write is outstanding, and completion must match its write ID. Failed writes,
timeouts, phone disconnect and cancellation retire the attempt. Reconnect starts
a new generation and never replays a mutation.

Acceptance requires commissioning through the actual phone/accessory path,
correct MTU handling and write completion, recoverable permission/GATT/network
failures, cancellation on phone loss, and operation after the phone disconnects.
Use the [session lifecycle](../../../docs/matter-session-lifecycle.md) and
[public API](../matter-native-api-v1.md). The Android project is separate and
ignored by this repository; do not stage its contents here.
