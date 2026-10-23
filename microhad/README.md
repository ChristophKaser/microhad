# MicroHAD

MicroHAD is the C++03 daemon serving the dashboard and REST API on the factory
MIPS gateway. It owns the shared device model, Zigbee application state,
sensor history, alerts and external HTTP integrations.

Ziggurat owns the Zigbee stack and radio UART; the separate Rust Matter service
owns Thread and Matter sessions/identity. MicroHAD reaches both through private
Unix sockets. The alternative EZSP build opens the NCP UART directly.

## Build and test

Run from the repository root:

```sh
make -C microhad host test
make -C microhad mips
make -C microhad webui
```

Defaults are `ZIGGURAT=1 MATTER=1`; Matter requires Ziggurat. Use
`ZIGGURAT=1 MATTER=0` for Zigbee only, or `ZIGGURAT=0` for EZSP.
The default target binary is `microhad/build/mips-ziggurat-matter/microhad`.
[Build setup](../docs/building.md) covers dependencies and artifact paths;
[toolchain selection](../config/TOOLCHAINS.md) covers RSDK and crosstool-NG.

After every C++ source/header change, run host tests and the MIPS build. After
Web UI changes, build the UI. See [test entrypoints](test/TEST_PLAN.md) and
[repository instructions](AGENTS.md) for verification and commit requirements.

## Configuration and state

The launcher reads `/tuya/native/config.json`; `runtime_dir` normally points
to `/tuya/native`. Choose the binary to match the radio firmware. A Ziggurat
socket failure stays offline rather than falling back to UART access.

| State | Purpose |
| --- | --- |
| `devices.json` | Inventory, bindings and completed reporting configuration |
| `ziggurat-state.json` | Private Zigbee network/security state for the Ziggurat backend |
| `gateway-settings.json` | Saved runtime settings overriding configuration defaults |
| `notifications.json` | Global push setting |
| `history.values` | Committed sensor history pages |
| `matter/` | State owned by the Rust Matter service |

Live device values are cached in RAM at `/tmp/microhad-values.cbor`. Changes
are batched for five seconds, then an owned snapshot is encoded and atomically
written by the existing `history-write` thread. Graceful shutdown flushes the
latest snapshot; an abrupt daemon crash can lose unflushed values. A busy history
writer can delay cache completion beyond the five-second handoff interval.
Legacy `/tmp/microhad-values.json` is read only when CBOR is absent and removed
after the first successful CBOR write. Persistent `devices.json` remains JSON and takes precedence for saved settings
when the RAM snapshot is older.
Cache writes do not change the history flash layout or persistence schedule.

Preserve configuration, identity and network state during application updates.
[Installation](../docs/installation.md) supplies initial configuration;
[deployment](../docs/deployment.md) explains staged and complete releases.

## Developer boundaries

The coordinator thread owns `Gateway`, the Zigbee connection and all common
state mutations. HTTP workers use protected snapshots and the command queue;
the notification worker owns outbound delivery. Never call `Gateway` directly
from HTTP or notification threads. The Matter worker owns its persistent IPC
connection and hands common-device updates to the coordinator.

Dashboard and Lua reads share immutable per-device records from one published
inventory. Normal value updates replace only the affected records and reuse
unchanged metadata and the name index. Automation preserves each accepted
report and intermediate value edge; it builds Lua-shaped tables on the Lua
worker rather than rebuilding maps for the whole inventory on the coordinator.
Dashboard reads use those same device records, with no second inventory batch.
Held views and queued events retain their original values after later updates.
Status/log readers copy immutable record references under short locks and format
JSON after releasing them. Idle Lua scripts avoid calendar and ready-queue work;
script status is sampled every 100 ms, with errors and reloads refreshed promptly.

Completed bindings/reporting signatures survive announcements and restarts.
Reconfiguration is required for new/changed endpoint inputs or an explicit
leave without rejoin. Use `POST /api/devices/<ieee-address>/reconfigure` to
request discovery, bindings and reporting setup; it requires an online network
and active device address. `/refresh` requests current attributes. Increment
the reporting-policy signature version when changing that policy.

`GET /api/settings` exposes broadcast quorum, `applied_value`,
`application_state` and `application_error`. Form-encoded `POST /api/settings`
saves `broadcast_passive_ack_quorum` from 1–255. Persistence can succeed while
stack application is pending. The saved override is reapplied on reconnect;
a corrupt override prevents startup. To return to config defaults, stop the
service and remove the override deliberately.

## Web UI development

```sh
cd microhad/webui
npm ci
npm run dev
```

Vite proxies `/api/` to `GATEWAY_IP` from the root `workspace.local.env` or the
environment. See [shared host settings](../docs/workspace-settings.md). Browser Push enrollment is tested
from the deployed HTTPS dashboard. `make -C microhad webui` builds
`microhad/webui/dist/` as gzip-only `.gz` artifacts; deployment uploads those
files beneath the runtime directory and the gateway serves their canonical URLs
with `Content-Encoding: gzip`. No uncompressed fallback is included.

## References

- [Dashboard usage](../docs/native-usage.md) and [diagnostics](../docs/recovery-tooling.md).
- [Ziggurat protocol/state ownership](../docs/native-ziggurat-integration.md).
- [Matter integration](MATTER_INTEGRATION.md), [HTTP API](docs/matter-native-api-v1.md) and [dashboard](docs/matter-dashboard.md).
- [Sensor history format](SENSOR_HISTORY_FORMAT.md), [syslog IPC](docs/syslog-ipc.md) and [third-party notices](THIRD_PARTY.md).

## Android companion

[MicroHAD Companion](../io.github.microhad/README.md) supports configurable gateway
setup, LAN discovery, Matter commissioning and FCM push notifications. See
[companion configuration](../docs/companion-app.md) for public Firebase client
settings and the `/api/companion/config` endpoint. Private sending credentials
remain on the gateway.

## Remote logging

The paired launcher reads `syslog_remote` from the device's native JSON config
(`NATIVE_CONFIG`, normally `/tuya/native/config.json`). Omit it or set it to
`""` for local logging only. To forward logs over UDP, set a hostname or IPv4
address and explicit port, for example `"syslog_remote": "192.0.2.10:514"`.
The local 512 KiB RAM log ring remains enabled in either mode. Restart the paired
stack after changing this setting; restarting only MicroHAD does not reload it.
Diagnostic logging can contain network secrets, so choose a trusted collector.

Query an explicitly configured string or integer without starting the gateway:

```sh
/tuya/native/microhad --print-config syslog_remote --config /tuya/native/config.json
/tuya/native/microhad --print-config http_port --config /tuya/native/config.json
```

The command validates the configuration, prints only the selected value and
exits. Strings are decoded and integers use decimal notation. A missing field
prints an empty line; implicit defaults are not included. With no `--config`,
it reads `/tuya/native/config.json`. This is a local command and can print
private configuration values, so do not expose it through a public endpoint.

## Thermostats

Thermostat controls, device schedules, advanced settings, and automatic external
temperature sensor linking are described in [thermostat support](docs/thermostats.md).

## Licenses and attribution

Original project code is MIT licensed; upstream components retain their own terms.
See the repository-wide [third-party inventory](../THIRD_PARTY.md) for attribution,
notice generation and release requirements.
