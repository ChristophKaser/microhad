# Deploy application updates

Use this guide for an already prepared gateway. For first installation or radio
replacement, use [installation](installation.md) or [Zigbee migration](ziggurat-deployment-plan.md).
Run host commands from the repository root. Configure gateway, compiler and
SSH settings in the private [workspace env file](workspace-settings.md).
`deploy.sh` reads it automatically; `--host`, `--user` and `--password-file`
remain explicit overrides. Deployment requires an explicit request; these
examples are procedures, not actions run by reading this document.

Artifact paths below show RSDK outputs. When selecting `TOOLCHAIN=crosstool-ng`,
use the crosstool-NG paths listed in [toolchain selection](../config/TOOLCHAINS.md);
explicit `--binary` and service paths must match the selected build.

## Choose the update procedure

| Need | Procedure |
| --- | --- |
| Upload binary/UI without restarting | Ordinary staging |
| Load a staged native binary under the existing supervisor | `--restart` |
| Install a paired boot launcher for the next start | `--activate-paired` or `--activate-paired-only` |
| Update the complete Matter stack | Maintenance release |

The script uses SSH stdin with `SSH_PORT` (default 22) and atomically replaces transferred files.
Default staging includes the native binary and Brotli-only Web UI. `--binary-only`,
`--webui-only` and `--both` select assets. Pass `--config` only to intentionally
replace configuration. Staging does not activate the paired launcher or restart
services. The default binary is the Ziggurat/Matter build.

Build the Web UI with `make -C microhad webui`; its `dist/` directory contains
only `.br` assets. The gateway serves their normal URLs with `Content-Encoding:
br`, with no gzip or uncompressed fallback, so deploy the matching native
binary and restart it with any Web UI update.

## Ordinary staging

```sh
make -C microhad mips
make -C microhad webui
./deploy.sh --target /tuya/native
```

To include the paired supervisor/control scripts while retaining the installed
Matter service, pass `--both --supervisor --binary PATH`. The script checks the
Matter manifest and flash headroom. Restart the supervisor separately to load
changed scripts. A native-only `--restart` signals MicroHAD; the existing
supervisor restarts that child. It does not reload Ziggurat or the launcher.

A staged Matter package needs the native binary, matching `.matter-manifest`,
Rust service and supervisor:

```sh
./deploy.sh --target /tuya/native --binary-only \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips
```

Add `--activate-paired` to install the paired launcher for the next stack start.
If the matching package is already staged, `--activate-paired-only` with the
same explicit local artifact paths verifies remote files/hashes and uploads
only the launcher. Both require `/tuya/native`; legacy launcher backup creation
is opt-in through `--rollback-dir DIR`. `--activate` selects the EZSP launcher instead.

## Selected services and supervisor

To update Ziggurat and its supervisor/control scripts without uploading native,
Matter, configuration or Web UI assets:

```sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
./deploy.sh --services ziggurat,supervisor --target /tuya/native \
  --ziggurat build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat.stripped
```

`supervisor` selects `paired_start.sh` and `paired_control.sh` as a pair. Either
changed hash causes that pair to be uploaded. Unchanged selections are skipped;
if everything matches, no service restarts. A changed selection quiesces/stops
the current stack, verifies staged files and restarts the installed launcher.
The 512 KiB reserve, ownership checks and identity/network preservation apply.
Rollback artifact generation remains opt-in through `--rollback-dir DIR`.

For temporary host OTBR access after this update, use
`paired_control.sh thread-tcp IPv4:PORT`; `thread-tcp off` restores local Matter.
This is a runtime request, not a boot-launcher setting. Every fresh supervisor
starts with Unix/local Matter, including after reboot. Use the corrected
supervisor that preserves diagnostic restart intent across transient UART/socket
release failures. See [runtime Thread ownership](hdlc-multipan-usage.md#tcp-endpoint-for-host-otbr).

## Maintenance release

Build all artifacts and update native/Matter together; their private IPC versions
must match. This procedure assumes the paired installation and network state
already exist. It does not install radio firmware or create network state.

```sh
make -C microhad ZIGGURAT=1 MATTER=1 mips
make -C microhad webui
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash firmware/scripts/build-maintenance.sh
bash micro-thread-matter-daemon/scripts/build.sh mips service
./deploy.sh --maintenance-deploy --target /tuya/native \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --ziggurat build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips \
  --maintenance-helper build/gateway-maintenance
```

The script quiesces/stops the stack, installs the complete bundle, activates
the launcher and starts it. Supply `--rollback-dir DIR` to save installed
application binaries, launcher and Web UI in an empty private local directory;
rollback artifact generation is disabled by default. It checks processes, IPC sockets, status,
Web UI and linked JS/CSS assets, then reports flash and RSS. It preserves
configuration, Zigbee network state and Matter identity/state.

Keep the 512 KiB flash reserve. Preflight accounts for executable inodes still
held by running processes; a process running a deleted executable causes refusal.
Maintenance measures flash after temporary uploads because JFFS2 compression
changes actual space use. Never delete state or backups to make a release fit.
## Restart and verify

To apply a changed launcher or service configuration, stop the existing paired
supervisor gracefully using its verified PID from
`/tmp/ziggurat-supervisor/supervisor.pid`, wait for cleanup, and run
`/tuya/tuya_start.sh` on the gateway. Never start a second supervisor beside it.
For a rollback-ready maintenance stop, use the freeze/export sequence in
[recovery tooling](recovery-tooling.md#orderly-stop-and-frozen-zigbee-export).

From the host, load settings for these raw shell commands:

```sh
. ./scripts/workspace-env.sh
workspace_load "$PWD"
: "${GATEWAY_IP:?Set GATEWAY_IP in workspace.local.env}"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/status"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/logs"
```

Require Zigbee online/formed, expected backend and preserved identity. For
Matter, check Thread socket and service readiness before commissioning. Verify
an actual existing-device report/control and the dashboard assets.

## Recover a failed release

When `--rollback-dir DIR` was supplied, the script prints and retains that
application backup directory. Otherwise use your retained release artifacts.
A transfer or readiness failure does not automatically restore or restart either
release. Inspect current processes, stop the stack, restore the saved application
files and Web UI, then start the saved launcher manually. Review any backup
before removing it. Application rollback does not rewind network counters or Matter
identity; [Matter state recovery](../microhad/docs/matter-backup-recovery.md)
and [radio/network rollback](ziggurat-deployment-plan.md#rollback) are separate procedures.
