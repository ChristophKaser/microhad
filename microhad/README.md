<img src="webui/public/microhad-icon.svg" width="96" height="96" alt="MicroHAD microchip-house icon" />

# MicroHAD — Micro Home Automation Daemon

A standalone local Zigbee dashboard and automation service for the
Lidl/Silvercrest RTL8196E + EFR32MG1 gateway. The statically linked C++03 daemon
runs on factory MIPS/uClibc Linux, without Node.js or low.js.

**Start with the [fresh-device installation guide](../docs/installation.md).**
It explains root access, backups, SSH, firmware, deployment and first pairing.
It also covers Home Assistant setups, which use ZHA instead of MicroHAD.
This component lives in the same repository as Ziggurat and the Matter service.

## Choose the native backend

| Backend | Build | Radio firmware | Connection | Installation |
| --- | --- | --- | --- | --- |
| EZSP | `make ZIGGURAT=0 mips` | Zigbee NCP, 115200 baud / RTS-CTS | MicroHAD directly owns `/dev/ttyS1` | [Mode A](../docs/installation.md#mode-a-native-service-without-ziggurat) |
| Ziggurat | `make ZIGGURAT=1 mips` | Two-instance HDLC RCP, 460800 baud / RTS-CTS | MicroHAD → private COBS Unix socket → Ziggurat → UART | [Mode B](../docs/installation.md#mode-b-native-service-with-ziggurat) |

Ziggurat's durable network state is owned by native. A fresh native Ziggurat
installation currently bootstraps an empty network using EZSP, exports its state,
then migrates before pairing devices. An absent state file does not create a
network, and the Ziggurat dashboard cannot create one.

Do not run the native coordinator alongside ZHA, serialgateway or another
UART owner. Home Assistant + EZSP uses a TCP bridge; Home Assistant + Ziggurat
uses its WebSocket API. Both are covered in the shared guide.

## Build and deploy

An x86_64 Linux host needs the stock RSDK MIPS toolchain, CMake and a C/C++
compiler; building the dashboard also needs Node.js/npm. Shared compiler
settings are in `../config/toolchains.mk`; see [build setup](../docs/building.md).

From this directory, the RSDK is discovered from the shared toolchain
configuration. Set `RSDK_ROOT` to override it:

```sh
make ZIGGURAT=0 host test
make ZIGGURAT=0 mips
make ZIGGURAT=1 host test
make ZIGGURAT=1 mips
make webui
```

Build the paired Rust Matter service from the repository root with
`bash micro-thread-matter-daemon/scripts/build.sh mips service`; it uses the shared
Rust and MIPS toolchain settings.

Matter is an optional build selection, `MATTER=0|1`, defaulting to `1` when
Ziggurat is enabled and `0` for the EZSP build.
`MATTER=1` requires `ZIGGURAT=1`. The small native C++03 bridge uses the
factory runtime; Matter controller behavior runs in the separate Rust service
because the factory C++ runtime cannot build the pinned C++ Matter SDK. The
Rust Matter service and common device/report
integration are implemented and synthetic-tested. The development gateway now
runs the Matter service, has a persisted Thread dataset attached on the current
Zigbee channel, and retains its existing Zigbee network. A first accessory has
not yet been commissioned; active-daemon CASE sensor reports and physical
device onboarding remain open. See [Matter build flags, compiler findings, and
remaining integration](MATTER_INTEGRATION.md) and the [open-point
plans](docs/matter-plans/README.md).

See the [gateway processes and protocol paths](../readme.md#processes-and-protocol-paths)
for the service architecture, shared radio transport and Android commissioning
relay.

Builds default to `ZIGGURAT=1 MATTER=1`, matching the current gateway
deployment. Set `ZIGGURAT=0` for the legacy EZSP build; that also defaults
Matter off. `MATTER=1` requires `ZIGGURAT=1`.

Outputs are `build/mips-ziggurat-matter/microhad` by default,
`build/mips-ziggurat/microhad` for `ZIGGURAT=1 MATTER=0`, and
`build/mips/microhad` for `ZIGGURAT=0`. The deployment script
defaults to the Matter-capable output; `--binary` selects another build. It
installs the executable at `/tuya/native/microhad` and updates the launcher.

```sh
../deploy.sh --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
  --binary build/mips-ziggurat-matter/microhad \
  --config "$CONFIG_FILE" --target /tuya/native
```

Transfers use SSH stdin on port 22, without SCP. Build the web UI first; default
deployment includes it. Configurations and network state must stay outside Git.
`--activate` installs the **EZSP MicroHAD launcher only**, preserving the
previous launcher. Existing processes continue until the normal supervised
restart or reboot. On its next start, the launcher removes the old
`/tuya/native/tuya-zigbee-native` executable after confirming no process still uses it.
Follow the backend's installation procedure before activation or restart.

## Broadcast confirmation setting

Set `broadcast_passive_ack_quorum` in native `config.json` to an integer from
1 to 255 (default: 8). Ziggurat considers a broadcast confirmed when it hears
rebroadcasts from that many eligible neighbor routers, or from all eligible
neighbors if fewer are present. These confirmations measure mesh propagation;
they do not confirm that each target bulb applied a scene.

The dashboard's **Gateway settings** dialog saves changes atomically in
`runtime_dir/gateway-settings.json`. That saved value overrides `config.json`;
remove the override file while the service is stopped to restore the config
value. Unchanged saves do not rewrite the file. Changes apply immediately using
Ziggurat's `SetTunable` command and are reapplied after reconnects and restarts.
The dialog shows saved and confirmed applied values separately, including
pending, rejected and unsupported states. The setting is unavailable with EZSP.
A corrupt settings override prevents startup and must be corrected.

`GET /api/settings` returns `broadcast_passive_ack_quorum`, `applied_value`
(`null` until confirmed for the current connection), `application_state`, and
`application_error`. Form-encoded `POST /api/settings` accepts
`broadcast_passive_ack_quorum` and returns the same snapshot after saving;
successful persistence can still be pending application to Ziggurat.

Verify the dialog with `node webui/test/gateway-settings-browser.cjs` after
installing its dependencies; this uses headless Chromium and simulated replies.

## Use the gateway

The dashboard handles joining, sensors, lights, scenes, remotes and local
Shelly/Fritz!Box devices. HTTPS enables Browser Push; Android/FCM notifications
need separate credentials. These are optional for basic local Zigbee operation.
See [dashboard and application usage](docs/usage.md).

Use `/api/status` for backend/network readiness and `/api/logs` for the BusyBox
syslogd RAM ring, read directly without spawning helper processes. See
[syslog IPC compatibility](docs/syslog-ipc.md). Wait for `online` and `network_formed` before pairing. Existing-network
migration requires radio security state in addition to the application inventory.

The paired supervisor restores a one-use kernel RNG seed from
`/tuya/native/ziggurat-rng.seed` before starting Ziggurat, then saves a refreshed
seed after 180 seconds once the kernel RNG is ready. This removes the usual cold
boot entropy wait after the first successful seed save while retaining the
readiness check. The seed is 44 bytes, mode `0600`, on the `/tuya` JFFS2 volume;
it adds one small persistent write per boot. See
[RNG seed deployment and recovery](RECOVERY_TOOLING.md#paired-startup-and-maintenance-control).

## Current state — 2026-10-05

The development gateway runs the native paired supervisor, Ziggurat, native
gateway and Matter service. Its Thread dataset is attached on channel 11, while
the Zigbee network remains online and formed. No Matter accessory is paired
yet; the Garage BLE onboarding flow still needs a real accessory and setup
code. Startup/reboot recovery and existing-light controls are verified.
Thermometer profile, cached-rejoin, parent-announcement and fresh-join replay
fixes are deployed, but sustained TH01 reporting and live acceptance remain
unresolved. One cold boot needed 121 seconds; broadcast verdicts can be
intermittent.

- [Latest fix, tests and remaining acceptance](TH01_HERDSMAN_FIX.md)
- [Thermometer investigation](THERMOMETER_RECOVERY.md)
- [Protocol and state ownership](ZIGGURAT_INTEGRATION.md)
- [Existing-network deployment and rollback](ZIGGURAT_DEPLOYMENT_PLAN.md)
- [Recovery commands](RECOVERY_TOOLING.md)
- [Real-radio measurements and limitations](REAL_RADIO_MIGRATION.md)

Original backups remain private. The restart counter margin is not an
unconditional guarantee after arbitrary crashes; reverse NCP restoration has
not been exercised live.

## Device reporting configuration

Successful Zigbee reporting and coordinator bindings are recorded in
`devices.json`. Ordinary device announcements and short-address changes keep
those bindings. A gateway restart also keeps completed light configuration.
Setup runs for new devices, changed endpoint/binding inputs, or after an explicit
leave announcement with `rejoin=false`. Router-reported departures and keepalive
expiry do not invalidate configuration. Ziggurat's current join notification does
not distinguish a factory reset from a connectivity rejoin; a reset without a
preceding leave can therefore require an explicit reconfiguration request.

Existing device records without a success marker receive configuration once;
rejected binding/reporting responses are not recorded as successful. Configuration
completion writes persistent state only when the recorded configuration changes.

Use `POST /api/devices/<ieee-address>/reconfigure` to force endpoint discovery,
binding and reporting setup. It uses the coordinator command queue and requires
an online Zigbee network and an active device address. The existing `/refresh`
request retains its attribute-refresh behavior (and explicit setup for remotes).
When changing reporting policy in the implementation, increment the signature
version so previously configured devices receive the new settings.

To stage the controller, Web UI, and current supervisor without replacing the
installed Matter service, use `../deploy.sh --both --supervisor --binary PATH
--password-file PATH --target /tuya/native`. This validates the Matter manifest,
checks staging flash space with the 512 KiB reserve, and uploads the manifest and
both supervisor/control scripts. Restart the paired supervisor separately to load
the new script. Starting the boot launcher with `ZIGGURAT_LOG_LEVEL=trace` changes
only that running supervisor; a reboot uses the default `info` level again.
