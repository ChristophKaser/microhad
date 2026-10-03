# Silvercrest gateway: native Zigbee or Home Assistant

Run a Lidl/Silvercrest RTL8196E gateway locally, retaining its factory Linux.
Choose a standalone native dashboard or use the gateway as a radio for Home
Assistant ZHA. Either can use the radio's EZSP stack or our MIPS Ziggurat build.

**New, store-bought device? Start with the [installation guide](docs/installation.md).**
It covers opening the gateway, root access, backups, SSH and all four setups.
Identify the RTL8196E + EFR32MG1B232 hardware first; the Silvercrest brand alone
is not sufficient to establish compatibility.

## Choose a mode

| Setup | Runs on the gateway | Runs elsewhere | Installation |
| --- | --- | --- | --- |
| Native, without Ziggurat | EZSP NCP radio + native service/dashboard | Nothing required for normal control | [Mode A](docs/installation.md#mode-a-native-service-without-ziggurat) |
| Native, with Ziggurat | HDLC RCP radio + Ziggurat + native service/dashboard | Nothing required for normal control | [Mode B](docs/installation.md#mode-b-native-service-with-ziggurat) |
| Home Assistant, without Ziggurat | EZSP NCP radio + TCP serial bridge | Home Assistant ZHA | [Mode C](docs/installation.md#mode-c-home-assistant-without-ziggurat) |
| Home Assistant, with Ziggurat | HDLC RCP radio + Ziggurat WebSocket server | Home Assistant ZHA | [Mode D](docs/installation.md#mode-d-home-assistant-with-ziggurat) |

Start with EZSP for the simplest setup. Ziggurat moves the Zigbee stack from
the radio into Linux; it requires different radio firmware. Native uses a
private COBS Unix socket, while ZHA uses a WebSocket connection. These are
alternative coordinator clients, not services to enable together.

## Current support — 2026-10-03

| Area | Evidence and limits |
| --- | --- |
| Native + EZSP | Both backend test suites and MIPS builds pass; previously used on the development gateway. |
| Native + Ziggurat | Running on real hardware; paired startup, reboot recovery and existing-light controls verified. TH01 fresh-join replay and parent-announcement fixes are deployed; device acceptance and sustained thermometer reporting still await verification. |
| ZHA + EZSP | Established upstream Silvercrest setup; not newly tested as part of this project. |
| ZHA + Ziggurat | Supported by upstream ZHA/Ziggurat; this gateway combination has not been tested here. Treat Mode D as experimental. |
| Recovery | Original assets and fresh frozen state retained; offline NCP rollback preparation verified. Live reverse restoration and recovery from an unresponsive RCP remain unverified. |
| Startup | One controlled cold boot took 121 seconds from native start to readiness; cause unresolved. |
| Zigbee delivery | Broadcast responses observed with intermittent relay-quorum/CCA failure verdicts. Sleepy-device behavior needs further acceptance testing. |
| Thread/Matter | [OpenThread MIPS CLI and Unix adapter pass a bounded device simulation with internal IPv6/UDP](docs/openthread-feasibility.md). Linux border routing lacks IPv6/TUN; production IID 2 remains unused. No real Thread mesh or Matter deployment is claimed. |

The ultimate goal is standalone Zigbee and Matter-over-Thread on this device,
sharing the radio through HDLC without cpcd. The native build remains C++03;
Ziggurat is Rust. The radio firmware is rebuildable from SDK source plus
precompiled Silicon Labs RAIL libraries.

## Reference

- [Installation, first pairing and rollback](docs/installation.md)
- [Build prerequisites and commands](docs/building.md)
- [Native dashboard and application usage](docs/native-usage.md)
- [Native application](https://github.com/ChristophKaser/microhad-zigbee-native)
- [Native protocol and durable state](docs/native-ziggurat-integration.md)
- [Existing-network migration and rollback](docs/ziggurat-deployment-plan.md)
- [Real-radio findings](docs/real-radio-migration.md)
- [Latest TH01 fix and remaining acceptance checks](docs/th01-herdsman-fix.md)
- [OpenThread build, adapter tests and feasibility](docs/openthread-feasibility.md)
- [HDLC/Thread implementation plan](docs/hdlc-implementation-plan.md)
- [Archived build reference and experiment history](docs/development-history.md)

The repository pins upstream sources as submodules and carries local patches
and build scripts. It does not contain passwords, deployment configuration,
network keys or private coordinator backups. See the installation guide for
checkout layout and required artifacts.
