# Documentation

Start with [installation](installation.md) for a new gateway, or
[Zigbee migration](ziggurat-deployment-plan.md) to preserve an existing network.
Commands run on the development computer unless marked **gateway**.

- [Binary releases](binary-releases.md): download, verify, install/update over SSH, and rebuild matching sources.

## Use and maintain a gateway

- [Supported devices](supported-devices.md): available features, unsupported functions and compatibility limits.
- [Android companion app](companion-app.md): gateway setup, Matter commissioning, FCM push and LAN discovery.
- [Dashboard usage](native-usage.md): pairing, controls, sensors, history and alerts.
- [Shared host settings](workspace-settings.md): private gateway, LAN, compiler and SSH configuration.
- [Build setup](building.md): prerequisites, toolchains and output paths.
- [Deploy application updates](deployment.md): staging, activation, complete releases and rollback.
- [Recovery and diagnostics](recovery-tooling.md): supervisor control, logs and radio ownership.
- [Crash snapshots](crash-snapshots.md): architecture, builds, host receiver, dump retrieval and GDB analysis.
- [Sensor reporting](sensor-reporting.md): troubleshoot missing Zigbee readings.
- [Automation scripting](scripting.md): write, reload and debug gateway-local Lua scripts.
- [Automation examples](../microhad/automation/examples/): complete Lua scripts for alerts, remotes, scenes and thermostat controls.
- [Lua scripting API](lua_scripting_api.md) and [script lifecycle](lua_script_lifecycle.md): gateway-local automation runtime and reload semantics.
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

- [Stock userspace in QEMU](stock-qemu.md): download a Malta kernel and emulator, prepare local backups, and boot with no LAN or Internet access.

Protocol and storage references:

- [Matter protobuf IPC v3](../micro-thread-matter-daemon/docs/matter-ipc-v3.md) and [CDDL schema](../protocol/matter-ipc-v3.proto).
- [Matter HTTP/WebSocket API](../microhad/docs/matter-native-api-v1.md).
- [Commissioning sessions](matter-session-lifecycle.md).
- [Operational discovery](matter-operational-discovery.md), [node catalog](matter-node-catalog.md), [descriptor discovery](matter-device-discovery.md), [snapshot pagination](matter-device-snapshot-ipc.md), [operations](matter-operational-transactions.md) and [sensor values](matter-sensors.md).
- [Matter identity store](../microhad/docs/matter-identity-store-v1.md) and [offline validation](matter-state-validation.md).
- [Sensor history](../microhad/SENSOR_HISTORY_FORMAT.md) and [syslog IPC](../microhad/docs/syslog-ipc.md).
- [Toolchain selection](../config/TOOLCHAINS.md) and [third-party notices](../microhad/THIRD_PARTY.md).
