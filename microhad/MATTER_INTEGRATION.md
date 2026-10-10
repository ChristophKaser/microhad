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

The target commands above use RSDK by default. After building the candidate
with `python3 scripts/build-toolchain.py`, select `TOOLCHAIN=crosstool-ng` for
both native and Rust target builds. Candidate artifacts are isolated under each
component's `build/crosstool-ng/`; host fixtures keep their paths. See
[toolchain selection](../config/TOOLCHAINS.md) for commands and experimental
limits. Deployment examples use RSDK artifact paths.

`MATTER=1` requires `ZIGGURAT=1` and is the default for that backend.
`MATTER=0` retains disabled status behavior and unavailable control routes.
TinyCBOR is pinned at `microhad/dependencies/tinycbor`; Rust uses Ciborium.
The host report fixture uses an isolated service-test-hook build. Production
artifacts must exclude that hook.

## Runtime and release

The paired supervisor starts and monitors MicroHAD, Ziggurat and Matter with
bounded retries. The service validates fresh coordinator observations and
matching channels before enabling Thread or commissioning. Dataset and identity
state survive radio/service restart. Mutations are never automatically replayed.

Both daemons must use v2 together. Use the documented
[maintenance deployment](AGENTS.md) for a coordinated release. The installed
service destination is `/tuya/native/mthread-matter`; configuration, sockets,
Matter identity and network state retain their existing paths and formats.
Deployment requires an explicit request. The v2 release has local build and
synthetic verification; physical accessory acceptance remains pending.

See the [runtime](../docs/matter-service-runtime.md),
[public API](docs/matter-native-api-v1.md),
[backup/recovery runbook](docs/matter-backup-recovery.md) and
[acceptance requirements](docs/matter-plans/README.md).
