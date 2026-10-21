# Matter-over-Thread integration

The component is `micro-thread-matter-daemon/`, executable `mthread-matter`.
MicroHAD owns the HTTP/WebSocket APIs and common device state. The Matter
service owns commissioning, controller identity, catalog, subscriptions and
userspace OpenThread. Ziggurat owns the radio UART and exposes the Thread
Spinel socket. No second UART owner or Linux IPv6/TUN interface is required.

The private socket `/tmp/microhad-matter.sock` carries
[CBOR IPC v2](../micro-thread-matter-daemon/docs/matter-ipc-v2.md). Only the length prefix is outside CBOR;
unknown fields are skipped with safe warnings. Public APIs remain JSON.

## Build and verification

From the repository root:

```sh
git submodule update --init --recursive
make -C microhad host test
make -C microhad mips
bash micro-thread-matter-daemon/scripts/build.sh host service
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
bash micro-thread-matter-daemon/scripts/build.sh mips service
python3 micro-thread-matter-daemon/scripts/test-matter-service.py
python3 microhad/scripts/test_matter_native_worker.py
python3 microhad/scripts/test_matter_native_report_integration.py
python3 microhad/scripts/test_matter_spinel_supervisor.py --native-worker
```

The target commands above use RSDK by default. After building crosstool-NG
with `python3 scripts/build-toolchain.py`, select `TOOLCHAIN=crosstool-ng` for
both native and Rust target builds. Crosstool-NG artifacts are isolated under each
component's `build/crosstool-ng/`; host fixtures keep their paths. See
[toolchain selection](../config/TOOLCHAINS.md) for commands and runtime
limits. Deployment examples use RSDK artifact paths.

`MATTER=1` requires `ZIGGURAT=1` and is the default for that backend.
`MATTER=0` retains disabled status behavior and unavailable control routes.
TinyCBOR is pinned at `microhad/dependencies/tinycbor`; Rust uses Ciborium.
The host report fixture uses an isolated service-test-hook build. Production
artifacts must exclude that hook.

## Device removal

Controls lists incomplete Zigbee interviews and Matter commissioning/recovery
records. Their Delete action and the Zigbee/Matter settings popup share a
confirmation flow. Unpairing has a 15-second deadline; failed or unreachable
accessories offer an explicit local-only removal. Active commissioning blocks
Matter removal. Shelly and FRITZ removal remains in connection settings.

`POST /api/devices/{encoded-id}/delete` accepts form data `mode=unpair` or
`mode=local`. Zigbee returns `state=pending`; poll
`GET /api/devices/{encoded-id}/delete` for `pending`, `succeeded`, or `failed`.
Matter also returns a numeric `job`; poll the existing
`GET /api/matter/jobs/{job}` endpoint until its service operation completes.
A successful HTTP submission alone does not mean removal succeeded.

Matter deletion uses opcode 10, action `delete`, with `schema`, `node`, and a
boolean `local`. It removes the entire node, including endpoint aliases and ICD
Check-In registrations. Unpairing reads the accessory's CurrentFabricIndex and
removes only that fabric. An authoritative `matter_inventory_nodes` event
precedes inventory revisions and removes stale native records after reconnect;
legacy catalog migration finishes before this manifest is emitted.

Removal cleans device settings and scene/group/binding/dashboard references.
It retains sensor history, controller identity, network state and other
accessories. Native and Matter service changes belong in the same release.

The host-only two-fabric removal regression is:

```sh
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py \
  --commissioning --ble --service-owner --delete-device
node microhad/webui/test/device-deletion-browser.cjs
```

## Runtime and release

The paired supervisor starts and monitors MicroHAD, Ziggurat and Matter with
bounded retries. The service validates fresh coordinator observations and
matching channels before enabling Thread or commissioning. Dataset and identity
state survive radio/service restart. Mutations are never automatically replayed.

Both daemons must use v2 together. Use the documented
[maintenance deployment](../docs/deployment.md#maintenance-release) for a coordinated release. The installed
service destination is `/tuya/native/mthread-matter`; configuration, sockets,
Matter identity and network state retain their existing paths and formats.
Deployment requires an explicit request. Physical accessory acceptance remains pending.

See the [runtime](../docs/matter-service-runtime.md),
[public API](docs/matter-native-api-v1.md),
[backup/recovery runbook](docs/matter-backup-recovery.md) and
[acceptance requirements](docs/matter-plans/README.md).

## Subscription health and recovery

Matter device settings include **Refresh subscription health** and **Restore
subscriptions**. Health shows the controller's tracked subscription IDs,
endpoint/cluster/attribute paths, negotiated maximum report intervals, time
since the last accepted report (including empty keepalives), retry timing and
failure counts. These are local controller records, not an accessory-side list.

`POST /api/matter/devices/subscriptions` accepts
`{"schema":1,"node":"0000000000000019","restore":false,"cursor":0}`.
It uses the existing HTTP job response/polling mechanism. Results contain at
most six subscriptions plus `next_cursor`; pass that cursor with `restore:false`
to read subsequent pages. `restore:true` is accepted only on the first page and
schedules replacement subscriptions for that node. Completion of this request
means recovery is scheduled; refresh health to observe establishment and failures.
Other devices' active subscription IDs are retained. Recovery during
commissioning or an active operation is rejected as busy.

The ALPSTUGA air quality monitor profile requests a 15-minute maximum report
interval. Other models retain their existing one-hour request. Recovery uses
the accessory's actual negotiated maximum plus ten seconds without a report,
not the age of the last changed measurement. Duplicate values remain suppressed.

A successful authenticated inventory refresh or operation after a device was
marked offline also schedules immediate subscription recovery when the device
has neither ICD Management nor Power Source capabilities and is not registered
as an ICD client. This conservative rule excludes known sleep-capable devices
and devices whose Power Source cluster does not prove continuous availability.
Registered ICD devices outside their active window are shown as sleeping;
requests wait for their normal wake handling. This is an expected sleep state,
not proof that the battery is healthy. A brief reboot that never produces an
offline observation is handled by the report deadline rather than inferred
from a discovery advertisement.
