# Deploy application updates

Use this guide for an already prepared gateway. For first installation or radio
replacement, use [installation](installation.md) or [Zigbee migration](ziggurat-deployment-plan.md).
Run host commands from the repository root. Set `GATEWAY_IP` and `PASSWORD_FILE`
to your gateway address and private password-file path. Deployment requires an
explicit request; the examples below are procedures, not actions run by reading
this document.

## Choose the update procedure

| Need | Procedure |
| --- | --- |
| Upload binary/UI without restarting | Ordinary staging |
| Load a staged native binary under the existing supervisor | `--restart` |
| Install a paired boot launcher for the next start | `--activate-paired` or `--activate-paired-only` |
| Update the complete Matter stack | Maintenance release |

The script uses SSH stdin on port 22 and atomically replaces transferred files.
Default staging includes the native binary and Web UI. `--binary-only`,
`--webui-only` and `--both` select assets. Pass `--config` only to intentionally
replace configuration. Staging does not activate the paired launcher or restart
services. The default binary is the Ziggurat/Matter build.

## Ordinary staging

```sh
make -C microhad mips
make -C microhad webui
./deploy.sh --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
  --target /tuya/native
```

To include the paired supervisor/control scripts while retaining the installed
Matter service, pass `--both --supervisor --binary PATH`. The script checks the
Matter manifest and flash headroom. Restart the supervisor separately to load
changed scripts. A native-only `--restart` signals MicroHAD; the existing
supervisor restarts that child. It does not reload Ziggurat or the launcher.

A staged Matter package needs the native binary, matching `.matter-manifest`,
Rust service and supervisor:

```sh
./deploy.sh --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" \
  --target /tuya/native --binary-only \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips
```

Add `--activate-paired` to install the paired launcher for the next stack start.
If the matching package is already staged, `--activate-paired-only` with the
same explicit local artifact paths verifies remote files/hashes and uploads
only the launcher. Both preserve `/tuya/tuya_start.sh.lowjs-backup` and require
`/tuya/native`. `--activate` selects the EZSP launcher instead.

## Selected services and supervisor

To update Ziggurat and its supervisor/control scripts without uploading native,
Matter, configuration or Web UI assets:

```sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
./deploy.sh --services ziggurat,supervisor --host "$GATEWAY_IP" \
  --password-file "$PASSWORD_FILE" --target /tuya/native \
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
See [runtime Thread ownership](hdlc-multipan-usage.md#tcp-endpoint-for-host-otbr).

## Maintenance release

Build all artifacts and update native/Matter together; their private IPC versions
must match. This procedure assumes the paired installation and network state
already exist. It does not install radio firmware or create network state.

```sh
make -C microhad ZIGGURAT=1 MATTER=1 mips
make -C microhad webui
bash microhad/scripts/build-logread.sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash firmware/scripts/build-maintenance.sh
bash micro-thread-matter-daemon/scripts/build.sh mips service
./deploy.sh --maintenance-deploy --host "$GATEWAY_IP" \
  --password-file "$PASSWORD_FILE" --target /tuya/native \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --ziggurat build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips \
  --maintenance-helper build/gateway-maintenance \
  --logread microhad/build/logread-mips
```

The script saves installed application binaries, launcher and Web UI in a private
local rollback directory, quiesces/stops the stack, installs the complete bundle,
activates the launcher and starts it. It checks processes, IPC sockets, status,
Web UI and linked JS/CSS assets, then reports flash and RSS. It preserves
configuration, Zigbee network state and Matter identity/state.

Keep the 512 KiB flash reserve. Preflight accounts for executable inodes still
held by running processes; a process running a deleted executable causes refusal.
Maintenance measures flash after temporary uploads because JFFS2 compression
changes actual space use. Never delete state or backups to make a release fit.
The unused legacy `tuya-zigbee-native` executable is removed only after verifying
no process uses it; it is not included in rollback backups.

## Restart and verify

To apply a changed launcher or service configuration, stop the existing paired
supervisor gracefully using its verified PID from
`/tmp/ziggurat-supervisor/supervisor.pid`, wait for cleanup, and run
`/tuya/tuya_start.sh` on the gateway. Never start a second supervisor beside it.
For a rollback-ready maintenance stop, use the freeze/export sequence in
[recovery tooling](recovery-tooling.md#orderly-stop-and-frozen-zigbee-export).

From the host:

```sh
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/status"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/logs"
```

Require Zigbee online/formed, expected backend and preserved identity. For
Matter, check Thread socket and service readiness before commissioning. Verify
an actual existing-device report/control and the dashboard assets.

## Recover a failed release

The script prints and retains the local application backup directory. A transfer
or readiness failure does not automatically restore or restart either release.
Inspect current processes, stop the stack, restore the saved application files
and Web UI, then start the saved launcher manually. Review the backup before
removing it. Application rollback does not rewind network counters or Matter
identity; [Matter state recovery](../microhad/docs/matter-backup-recovery.md)
and [radio/network rollback](ziggurat-deployment-plan.md#rollback) are separate procedures.
