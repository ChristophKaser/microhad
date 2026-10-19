# Documentation

Start with [installation](installation.md) for a new gateway, or
[Zigbee migration](ziggurat-deployment-plan.md) to preserve an existing network.
Commands run on the development computer unless marked **gateway**.

## Use and maintain a gateway

- [Supported devices](supported-devices.md): available features, unsupported functions and compatibility limits.
- [Android companion app](companion-app.md): gateway setup, Matter commissioning, FCM push and LAN discovery.
- [Dashboard usage](native-usage.md): pairing, controls, sensors, history and alerts.
- [Shared host settings](workspace-settings.md): private gateway, LAN, compiler and SSH configuration.
- [Build setup](building.md): prerequisites, toolchains and output paths.
- [Deploy application updates](deployment.md): staging, activation, complete releases and rollback.
- [Recovery and diagnostics](recovery-tooling.md): supervisor control, logs and radio ownership.
- [Sensor reporting](sensor-reporting.md): troubleshoot missing Zigbee readings.
- [Matter dashboard](../microhad/docs/matter-dashboard.md): Thread setup and phone-assisted commissioning.
- [Matter backup and recovery](../microhad/docs/matter-backup-recovery.md): consistent identity/state backups.

## Develop the stack

| Component | Entry point | Main references |
| --- | --- | --- |
| Android companion | [README](../io.github.microhad/README.md) | [Setup and API](companion-app.md), MIT license |
| MicroHAD | [README](../microhad/README.md) | [Ziggurat integration](native-ziggurat-integration.md), [tests](../microhad/test/TEST_PLAN.md) |
| Ziggurat | [README](../ziggurat/README.md) | [Shared-radio transport](hdlc-multipan-usage.md), [network conversion](network-migration.md) |
| Thread/Matter | [README](../micro-thread-matter-daemon/README.md) | [Runtime](matter-service-runtime.md), [transport](rs-matter-openthread-transport.md) |
| Radio firmware | [README](../firmware/README.md) | [Build setup](building.md), [recovery](recovery-tooling.md) |

Protocol and storage references:

- [Matter CBOR IPC v2](../micro-thread-matter-daemon/docs/matter-ipc-v2.md) and [CDDL schema](../protocol/matter-ipc-v2.cddl).
- [Matter HTTP/WebSocket API](../microhad/docs/matter-native-api-v1.md).
- [Commissioning sessions](matter-session-lifecycle.md).
- [Operational discovery](matter-operational-discovery.md), [node catalog](matter-node-catalog.md), [descriptor discovery](matter-device-discovery.md), [snapshot pagination](matter-device-snapshot-ipc.md), [operations](matter-operational-transactions.md) and [sensor values](matter-sensors.md).
- [Matter identity store](../microhad/docs/matter-identity-store-v1.md) and [offline validation](matter-state-validation.md).
- [Sensor history](../microhad/SENSOR_HISTORY_FORMAT.md) and [syslog IPC](../microhad/docs/syslog-ipc.md).
- [Toolchain selection](../config/TOOLCHAINS.md) and [third-party notices](../microhad/THIRD_PARTY.md).

[Physical Matter acceptance](../microhad/docs/matter-plans/08-real-device-acceptance.md)
describes the remaining hardware checks. Synthetic tests establish software
behavior; they do not establish accessory compatibility or sustained RF reliability.
