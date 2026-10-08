# Embedded home automation for the Silvercrest gateway

A standalone home automation stack for the Lidl/Silvercrest gateway with an
RTL8196E CPU and EFR32MG1B232 radio. It runs on factory MIPS/uClibc Linux and
provides a local dashboard, device control, sensor history, notifications, and
HTTP device integrations without requiring Home Assistant or a cloud service.

The default stack runs three supervised processes:

| Component | Purpose |
| --- | --- |
| [MicroHAD](microhad/README.md) | C++03 daemon, Web UI and REST API; owns the common device model, history, alerts and integrations with Shelly and Fritz!Box devices. |
| [Ziggurat](ziggurat/README.md) | Rust Zigbee stack and Spinel multiplexer; connects MicroHAD and Thread to the shared radio. |
| [Thread/Matter](thread-matter/README.md) | Rust Matter controller using rs-matter and userspace OpenThread; owns commissioning, fabrics, sessions and Thread state. |
| [Firmware](firmware/README.md) | Rebuildable two-instance HDLC radio firmware, packaging and radio maintenance tools. |

MicroHAD talks to Ziggurat and the Matter service over private Unix sockets.
Ziggurat owns the radio UART and multiplexes Zigbee and Thread over separate
Spinel instances. OpenThread provides IPv6/UDP inside the Matter process; this
path does not need Linux IPv6 or TUN and does not make the gateway an OTBR.
`microhad/paired_start.sh` starts, monitors and recovers the three processes.

The independent Android Garage app can relay Bluetooth commissioning traffic
between an accessory and the gateway. Its local checkout lives in `garage-app/`,
is ignored here, and retains its own Git repository and credentials.

## Build

Use an x86_64 Linux development host. The stock RSDK toolchain must be supplied
separately; shared compiler defaults live in `config/toolchains.mk`.
See [build setup](docs/building.md) for prerequisites and firmware tooling.
From the repository root:

```sh
make -C microhad host test
make -C microhad mips
make -C microhad webui
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash thread-matter/scripts/build.sh mips service
bash firmware/scripts/build-maintenance.sh
bash microhad/scripts/build-logread.sh
```

MicroHAD defaults to `ZIGGURAT=1 MATTER=1`. An EZSP-only setup remains available
with `ZIGGURAT=0`; `ZIGGURAT=1 MATTER=0` builds Zigbee without Matter.
Home Assistant ZHA can alternatively use the radio through a serial bridge or
Ziggurat WebSocket connection. Those setups use ZHA as the coordinator client.

## Repository layout

Each component owns its source, scripts, patches, tests and dependencies.
Upstream Ziggurat is pinned under `ziggurat/dependencies/ziggurat`; rs-matter
under `thread-matter/dependencies/rs-matter`; the firmware dependencies under
`firmware/dependencies/`. Gecko SDK lives in `shared/dependencies/gecko-sdk`
because firmware and the Matter OpenThread adapter both consume it.
MicroHAD keeps its configured Mbed TLS copy under `microhad/third_party/`.

Root `scripts/`, `config/` and `patches/` contain shared build infrastructure,
compatibility patches and cross-service validation. Generated artifacts,
worktrees, tools and logs are ignored. CPC and the superseded compiler/SDK
experiments are no longer part of the maintained build.

## Installation and deployment

Start with the [installation guide](docs/installation.md) to identify the
hardware, obtain root access, preserve factory backups and choose a setup.
The root `./deploy.sh` uploads assets over SSH stdin. Ordinary deployment
stages files; launcher activation and restarts are explicit operations.
For a complete Matter release, follow the maintenance deployment flow in
[microhad/AGENTS.md](microhad/AGENTS.md), including local rollback backups,
flash-space checks and readiness verification.

Keep credentials, device configuration, Zigbee keys, Thread datasets and Matter
identity outside Git. Transient gateway output belongs in `/tmp`.

## Validation and current limits

Repository records describe the paired stack running on the development
gateway with its existing Zigbee network and a persisted Thread dataset.
Host and guarded synthetic fixtures exercise commissioning, CASE transactions,
subscriptions and sensor reports through the native device model.
Those tests do not establish physical accessory interoperability. The recorded
open checks include first physical accessory commissioning and report flow
through the installed daemon; see the [Matter acceptance tracking](microhad/docs/matter-plans/README.md).

EZSP and native Ziggurat remain supported build modes. Home Assistant with
Ziggurat on this particular gateway has not been accepted here. Radio recovery
and sleepy Zigbee device acceptance have separate documented limits.
See [native usage](docs/native-usage.md), [Matter runtime](docs/matter-service-runtime.md),
and [recovery tooling](docs/recovery-tooling.md).

[Development history](docs/development-history.md) and recorded JSON reports
retain their original dates, paths and measurements; they are historical
evidence, not current build instructions.
