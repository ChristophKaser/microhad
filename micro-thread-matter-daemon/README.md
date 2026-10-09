# Thread/Matter controller service

`mthread-matter` owns the gateway's Matter fabrics, commissioning, sessions,
subscriptions and Thread state. It uses the pinned rs-matter dependency and an
OpenThread C ABI adapter built from upstream OpenThread v2026.10.0.
MicroHAD consumes its private IPC API and applies devices and reports to the common dashboard model.

From the repository root:

```sh
bash micro-thread-matter-daemon/scripts/build.sh host service
bash micro-thread-matter-daemon/scripts/build.sh mips service
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
bash micro-thread-matter-daemon/scripts/build.sh host transport
```

The default is `host service`. Production outputs are
`build/micro-thread-matter-daemon/target/release/mthread-matter` and
`build/micro-thread-matter-daemon/mthread-matter-mips`. Synthetic fixtures retain the names
`ot-matter-commissioning-probe` and `ot-matter-transport-probe`.
Always use the build wrapper before integration checks; plain Cargo builds can
leave the fixture harness using a stale executable.

MIPS builds select RSDK by default, or experimental crosstool-NG without RSDK.
After `python3 scripts/build-toolchain.py`, run
`TOOLCHAIN=crosstool-ng bash micro-thread-matter-daemon/scripts/build.sh mips service`.
Its output is `build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips`;
OpenThread target artifacts are under `build/crosstool-ng/openthread-unix/`.
RSDK OpenThread uses Clang 18; the candidate uses its native GCC/G++.
Host builds need neither MIPS SDK and keep their fixture paths. See
[toolchain selection](../config/TOOLCHAINS.md) for source credits and limits.

Build scripts prepare `build/micro-thread-matter-daemon/service-source/` from
`dependencies/rs-matter/` and apply the controller patch series. OpenThread
sources remain under `dependencies/openthread/`; generated adapter
archives and test fixtures are under `build/openthread-unix/` at the root.

Host checks include:

```sh
python3 micro-thread-matter-daemon/scripts/test-openthread-compatibility.py
python3 micro-thread-matter-daemon/scripts/test-matter-service.py
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py
python3 micro-thread-matter-daemon/scripts/test-rs-matter-controller.py
python3 microhad/scripts/test_matter_native_report_integration.py
```

The native integration builds a separate service artifact with test-only report
hooks. Production builds check that those hooks are absent.
See [runtime](../docs/matter-service-runtime.md) and
[physical acceptance tracking](../microhad/docs/matter-plans/README.md).

OpenThread is pinned to release `v2026.10.0`, commit
`53e8322140dde7b26827332100504507cec9cc4e`. Its build initializes the nested
Mbed TLS dependency without requiring the Silicon Labs firmware installer.
Existing clean GSDK-derived source and CMake directories are preserved as
`source-mailbox-gsdk-backup`, `host-gsdk-backup` and `mips-gsdk-backup` on migration;
modified legacy sources are refused rather than overwritten.

The release and the unchanged GSDK 4.5.0 radio source both declare Spinel RCP API
version 11, minimum supported RCP API version 4, and protocol version 4.3.
Runtime compatibility checks remain enabled. Host synthetic tests validate the
shared-radio no-reset attachment; physical radio compatibility requires a
separately authorized gateway acceptance run.

The private protocol is [length-prefixed CBOR v2](../microhad/docs/matter-ipc-v2.md).
Only the length prefix is outside the CBOR envelope.
