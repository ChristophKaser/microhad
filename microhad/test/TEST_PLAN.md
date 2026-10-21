# Verify MicroHAD changes

Run host fixtures on the development computer. They use temporary state and
synthetic serial/radio/accessory traffic; they require no gateway or production
credentials. Physical interoperability is a separate acceptance activity.

## Required checks

From the repository root:

```sh
make -C microhad host test
make -C microhad mips
```

Run both after every C++ source/header change. Defaults select Ziggurat and
Matter. Rebuild the CASE probe before tests that use it:

```sh
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
```

Use the wrapper's repository target path; plain Cargo builds can leave an old
fixture binary in use. For changes affecting both Zigbee backends:

```sh
make -C microhad test-backends
```

Run variants sequentially. The Makefile is authoritative for tests included in
`make host test`; the focused scripts below are separate entrypoints.

## Focused integration checks

| Area | Command from repository root |
| --- | --- |
| Actual Ziggurat stack/lifecycle | `python3 microhad/scripts/test_ziggurat_service.py` |
| Backup/conversion | `python3 microhad/scripts/test_network_migration.py` |
| Stopped/running import | `python3 microhad/scripts/test_ziggurat_import.py` |
| Paired supervision | `python3 microhad/scripts/test_paired_supervisor.py` |
| Matter worker/relay | `python3 microhad/scripts/test_matter_native_worker.py` |
| Real CASE/report handoff | `python3 microhad/scripts/test_matter_native_report_integration.py` |
| Matter/shared-radio supervision | `python3 microhad/scripts/test_matter_spinel_supervisor.py --native-worker` |
| CBOR codec/vectors | `python3 microhad/scripts/test-matter-ipc-codec.py` |
| Deployment guards | `python3 microhad/scripts/test_deployment_tools.py` |
| Matter package deployment | `python3 microhad/scripts/test_matter_service_deployment.py` |
| Matter archive preflight/extraction | `python3 microhad/scripts/test_matter_archive.py` |

Prepare the patched host Ziggurat fixture as described in its
[README](../../ziggurat/README.md). Matter report integration builds an isolated
service with test-only hooks; production binaries must exclude those hooks.
Read each script's prerequisite checks when selecting a focused test.

## Web UI

After every change under `microhad/webui/`:

```sh
make -C microhad webui
```

For Matter client behavior, use `python3 microhad/scripts/test_matter_webui.py`.
Rendered controls are checked by `microhad/webui/test/matter-devices-browser.cjs`
and `gateway-settings-browser.cjs` under Node/Chromium with simulated API replies.
Keep browser fixtures pointed at their fake API rather than a live gateway.

## Extend coverage

Assert observable outcomes: responses, emitted frames/requests, persisted state,
device snapshots and bounded recovery. Cover failure/deadline paths with
controlled inputs. Use temporary runtime directories and synthetic credentials;
never require cloud push, a real radio or production TLS material.

Host checks do not establish physical report timing, RF coexistence, target
resource headroom or power-loss durability for a new accessory. The deployed
Matter path receives temperature and air-quality reports. [Zigbee migration
acceptance](../../docs/ziggurat-deployment-plan.md#accept-the-migrated-network)
covers the Zigbee boundary.
