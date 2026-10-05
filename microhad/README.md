# MicroHAD Native

A standalone local Zigbee dashboard and automation service for the
Lidl/Silvercrest RTL8196E + EFR32MG1 gateway. The statically linked C++03 daemon
runs on factory MIPS/uClibc Linux, without Node.js or low.js.

**Start with the [fresh-device installation guide](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/installation.md).**
It explains root access, backups, SSH, firmware, deployment and first pairing.
It also covers Home Assistant setups, which use ZHA instead of this service.
The linked experiment repository requires access to the private project.

## Choose the native backend

| Backend | Build | Radio firmware | Connection | Installation |
| --- | --- | --- | --- | --- |
| EZSP (default) | `make ZIGGURAT=0 mips` | Zigbee NCP, 115200 baud / RTS-CTS | Native directly owns `/dev/ttyS1` | [Mode A](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/installation.md#mode-a-native-service-without-ziggurat) |
| Ziggurat | `make ZIGGURAT=1 mips` | Two-instance HDLC RCP, 460800 baud / RTS-CTS | Native → private COBS Unix socket → Ziggurat → UART | [Mode B](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/installation.md#mode-b-native-service-with-ziggurat) |

Ziggurat's durable network state is owned by native. A fresh native Ziggurat
installation currently bootstraps an empty network using EZSP, exports its state,
then migrates before pairing devices. An absent state file does not create a
network, and the Ziggurat dashboard cannot create one.

Do not run the native coordinator alongside ZHA, serialgateway or another
UART owner. Home Assistant + EZSP uses a TCP bridge; Home Assistant + Ziggurat
uses its WebSocket API. Both are covered in the shared guide.

## Build and deploy

An x86_64 Linux host needs the stock
`rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129` toolchain, CMake and a C/C++
compiler; building the dashboard also needs Node.js/npm. See
[build setup](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/building.md)
for checkout layout, toolchain acquisition limits and firmware builds.

From this checkout, with `RSDK` set to the absolute toolchain directory:

```sh
make RSDK_ROOT="$RSDK" ZIGGURAT=0 host test
make RSDK_ROOT="$RSDK" ZIGGURAT=0 mips
make RSDK_ROOT="$RSDK" ZIGGURAT=1 host test
make RSDK_ROOT="$RSDK" ZIGGURAT=1 mips
make webui
```

Matter is an optional build selection, `MATTER=0|1`, defaulting to `0`.
`MATTER=1` requires `ZIGGURAT=1`; enabled builds currently fail the documented
factory C++ runtime gate. The Rust Matter service and common device/report
integration are implemented and synthetic-tested, but gateway CASE reports,
physical Thread attachment and deployment acceptance remain open. See [Matter
build flags, compiler findings, and remaining integration](MATTER_INTEGRATION.md)
and the [open-point plans](docs/matter-plans/README.md).

Outputs are `build/mips/microhad-zigbee-native` (EZSP) and
`build/mips-ziggurat/microhad-zigbee-native` (Ziggurat). Pass the chosen executable
explicitly: the deployment script defaults to the EZSP output.

```sh
./deploy.sh --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
  --binary build/mips/microhad-zigbee-native \
  --config "$CONFIG_FILE" --target /tuya/native
```

Transfers use SSH stdin on port 22, without SCP. Build the web UI first; default
deployment includes it. Configurations and network state must stay outside Git.
`--activate` installs the **EZSP native launcher only**, preserving the previous
launcher. It neither stops existing processes nor installs the Ziggurat pair.
Follow the backend's installation procedure before activation or restart.

## Use the gateway

The dashboard handles joining, sensors, lights, scenes, remotes and local
Shelly/Fritz!Box devices. HTTPS enables Browser Push; Android/FCM notifications
need separate credentials. These are optional for basic local Zigbee operation.
See [dashboard and application usage](docs/usage.md).

Use `/api/status` for backend/network readiness and `/api/logs` for the bounded
RAM log. Wait for `online` and `network_formed` before pairing. Existing-network
migration requires radio security state in addition to the application inventory.

The paired supervisor restores a one-use kernel RNG seed from
`/tuya/native/ziggurat-rng.seed` before starting Ziggurat, then saves a refreshed
seed after 180 seconds once the kernel RNG is ready. This removes the usual cold
boot entropy wait after the first successful seed save while retaining the
readiness check. The seed is 44 bytes, mode `0600`, on the `/tuya` JFFS2 volume;
it adds one small persistent write per boot. See
[RNG seed deployment and recovery](RECOVERY_TOOLING.md#paired-startup-and-maintenance-control).

## Current state — 2026-10-03

The development gateway runs native + Ziggurat. Startup/reboot recovery and
existing-light controls are verified. Thermometer profile, cached-rejoin,
parent-announcement and fresh-join replay fixes are deployed, but sustained TH01
reporting and live acceptance remain unresolved. One cold boot needed 121 seconds;
broadcast verdicts can be intermittent. Thread/Matter is future work.

- [Latest fix, tests and remaining acceptance](TH01_HERDSMAN_FIX.md)
- [Thermometer investigation](THERMOMETER_RECOVERY.md)
- [Protocol and state ownership](ZIGGURAT_INTEGRATION.md)
- [Existing-network deployment and rollback](ZIGGURAT_DEPLOYMENT_PLAN.md)
- [Recovery commands](RECOVERY_TOOLING.md)
- [Real-radio measurements and limitations](REAL_RADIO_MIGRATION.md)

Original backups remain private. The restart counter margin is not an
unconditional guarantee after arbitrary crashes; reverse NCP restoration has
not been exercised live.
