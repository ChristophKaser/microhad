# Thread/Matter controller service

`thread-matter` owns the gateway's Matter fabrics, commissioning, sessions,
subscriptions and Thread state. It uses the pinned rs-matter dependency and an
OpenThread C ABI adapter built from the shared Gecko SDK. MicroHAD consumes its
private IPC API and applies devices and reports to the common dashboard model.

From the repository root:

```sh
bash thread-matter/scripts/build.sh host service
bash thread-matter/scripts/build.sh mips service
bash thread-matter/scripts/build.sh host commissioning
bash thread-matter/scripts/build.sh host transport
```

The default is `host service`. Production outputs are
`build/thread-matter/target/release/thread-matter` and
`build/thread-matter/thread-matter-mips`. Synthetic fixtures retain the names
`ot-matter-commissioning-probe` and `ot-matter-transport-probe`.
Always use the build wrapper before integration checks; plain Cargo builds can
leave the fixture harness using a stale executable.

Build scripts prepare `build/thread-matter/service-source/` from
`dependencies/rs-matter/` and apply the controller patch series. OpenThread
sources remain under `../shared/dependencies/gecko-sdk/`; generated adapter
archives and test fixtures are under `build/openthread-unix/` at the root.

Host checks include:

```sh
python3 thread-matter/scripts/test-matter-service.py
python3 thread-matter/scripts/test-openthread-matter-transport.py
python3 thread-matter/scripts/test-rs-matter-controller.py
python3 microhad/scripts/test_matter_native_report_integration.py
```

The native integration builds a separate service artifact with test-only report
hooks. Production builds check that those hooks are absent.
See [runtime](../docs/matter-service-runtime.md) and
[physical acceptance tracking](../microhad/docs/matter-plans/README.md).
