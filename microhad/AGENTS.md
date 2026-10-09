# MicroHAD — Micro Home Automation Daemon

This component is the native C++03 gateway service for the Tuya MIPS device.
Build target binaries with the shared compiler settings in
`../config/toolchains.mk`; `RSDK_ROOT` can override the configured RSDK.

```sh
make host test
make mips
```

Builds default to Ziggurat and Matter, matching the current deployment.
`make mips` builds the vendored Mbed TLS dependency and produces the static,
big-endian MIPS binary at `build/mips-ziggurat-matter/microhad`.
Use `make ZIGGURAT=0 mips` for the legacy EZSP binary or
`make ZIGGURAT=1 MATTER=0 mips` for Ziggurat without Matter APIs.

The host Matter CASE fixture uses the commissioning probe at
`build/thread-matter/target/release/ot-matter-commissioning-probe`.
Rebuild it from the repository root with
`bash thread-matter/scripts/build.sh host commissioning`. Do not use a
plain Cargo build for this fixture: without the repository target directory it
writes under `thread-matter/target/`, leaving the integration harness on an
older binary. This host command does not deploy or access the gateway radio.

## Mandatory verification and commits

After every change to a C++ source or header (`.cpp` or `.h`), run both
`make -C microhad host test` and `make -C microhad mips` before considering
that change successful.
Do not defer these checks until later changes are grouped together.

After every change to files under `microhad/webui/`, run `make -C microhad webui` before
considering that change successful.

After each successfully verified change, commit the files changed for that
task with a descriptive commit message. If the worktree contains unrelated
changes, stage and commit only the files changed for the current task. Never
commit device configuration, credentials, production TLS keys, persistent
network state, or Zigbee keys.

Before starting Ziggurat or the Matter service, `paired_start.sh` checks each
configured Unix socket path. It removes a leftover socket pathname only when
`/proc/net/unix` has no entry for that path. If the path is active, is not a
socket, is a symlink, or the kernel socket table cannot be read, startup leaves
it untouched and reports the problem. This handles socket files left after a
crash without unlinking a live service's socket.

From the repository root, deploy updates with `./deploy.sh` and the password file by pathname. The default
copies the selected binary and Web UI; `--binary-only`, `--webui-only`, and
`--both` select those assets. Pass `--config` only when intentionally replacing
the device configuration. Files are transferred through SSH stdin and each
file is moved into place from a temporary file after transfer. A Matter package
contains the native binary, its `.matter-manifest`, the Rust `thread-matter`
service, and `paired_start.sh`:

```sh
# Native gateway, from the repository root
make -C microhad mips

# Rust service, from the repository root
bash thread-matter/scripts/build.sh mips service

# Stage the Matter package without changing the active launcher or processes
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native --binary-only \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --matter-service build/thread-matter/thread-matter-mips
```

Staging does not activate Matter or restart a process. To install the
Matter-aware paired launcher together with a full package upload, add
`--activate-paired`; the original `/tuya/tuya_start.sh` is kept at
`/tuya/tuya_start.sh.lowjs-backup`. If the package is already staged and flash
space cannot accommodate another binary copy, use `--activate-paired-only` to
verify the staged Matter files and switch the launcher without re-uploading
them. Both activation modes require `/tuya/native` and only affect the next
stack start.

On an existing gateway, stop the old paired supervisor completely before
launching the updated paired launcher. It checks that no process still uses
`/tuya/native/tuya-zigbee-native`, then removes that legacy executable and its
old manifest without keeping a copy. EZSP startup applies the same check on its
next launch.

The Matter flash preflight checks `/proc/*/exe` before atomically replacing the
native or Rust service executable. It keeps the old executable size in the
staging peak while that process is running, because the kernel retains the old
inode until process exit. If a process already runs an executable marked
`(deleted)`, preflight fails closed; restart the owning service before another
update. Keep the configured 512 KiB reserve unless an explicit maintenance
decision changes it. Never remove application backups or network state just to
make preflight pass.

For a complete Matter release, build all target artifacts and use the explicit
maintenance deployment mode. It saves installed MicroHAD/Matter application
binaries, launcher and Web UI under a private local rollback directory,
quiesces and stops the paired stack, and removes the legacy executable without
backing it up. It reclaims the old Matter service, Ziggurat and Web UI only
after saving them locally. Because JFFS2 compresses artifacts,
it checks measured flash after each temporary upload and deletes that temporary
file if it would consume the 512 KiB reserve. It then installs
the controller, Ziggurat, Matter service, `gateway-maintenance` helper and Web
UI, activates the paired launcher, starts the stack, and checks the
supervisor, IPC sockets, Web UI, status API, and each JavaScript/CSS file linked
by the deployed `index.html`. Dashboard verification uses HTTP, waits three
seconds after process readiness, then retries the status API for up to thirty
seconds before checking the page, logs and assets.
Detach SSH stdin from asset-list loops so SSH
commands cannot consume later `find` results. It removes stale runtime socket
pathnames only after checking their processes have stopped. It reports available flash and
process RSS. The mode never copies or removes gateway
configuration or network/identity state.

```sh
# Native controller and Matter manifest, from the repository root
make -C microhad ZIGGURAT=1 MATTER=1 mips
make -C microhad webui
bash microhad/scripts/build-logread.sh

# Ziggurat and radio maintenance helper, from the repository root
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash firmware/scripts/build-maintenance.sh

# Matter service, from the repository root
bash thread-matter/scripts/build.sh mips service

# Complete maintenance deployment, from the repository root
./deploy.sh --maintenance-deploy --host 192.168.178.62 \
  --password-file ../ssh_password.txt --target /tuya/native \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --ziggurat build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat \
  --matter-service build/thread-matter/thread-matter-mips \
  --maintenance-helper build/gateway-maintenance \
  --logread microhad/build/logread-mips
```

The script prints the local backup directory and retains it after success.
On transfer or readiness failure it leaves the deployment as-is and does not
restore files or start either release. If recovery is needed, stop the stack,
restore the saved application files and Web UI from the printed directory, then
start `/tuya/tuya_start.sh` manually. Review the backup directory before
removing it.

For an update to an already paired installation, `--restart` signals only the
native daemon; its current supervisor restarts it. It does not reload the
launcher or restart Ziggurat. To apply a changed paired launcher or Matter
service configuration, gracefully stop the current paired supervisor using
the PID in `/tmp/ziggurat-supervisor/supervisor.pid`, wait for its cleanup to
finish, and launch `/tuya/tuya_start.sh` so the configured stack starts with
the selected launcher. Confirm Zigbee health, `/tmp/ziggurat/thread.sock`,
Matter service readiness and resource use before commissioning. Preserve the
device configuration and both protocols' network/identity state. Never commit
device configuration, credentials, production TLS keys, persistent network
state, or Zigbee keys. The non-production localhost key at
`test/fixtures/tls/localhost.key` is only for host integration tests and must
never be installed, packaged, or referenced by deployment code.

For a temporary Ziggurat log-level change while the paired supervisor is
running, use `/tuya/native/paired_control.sh log-level LEVEL`, where `LEVEL` is
`off`, `error`, `warn`, `info`, `debug`, or `trace`. The helper writes the
request into the supervisor's private `/tmp/ziggurat-supervisor` directory,
then sends TERM to Ziggurat. The supervisor restarts only Ziggurat for this
request and sets `NO_COLOR=1` for its process so syslog output has no ANSI
color escapes; the native gateway reconnects, and the Matter child is restarted by
its monitor if its radio connection causes it to exit. Unexpected Ziggurat
failures still use full paired recovery. The requested level lasts for the
current supervisor lifetime and resets to `ZIGGURAT_LOG_LEVEL` (normally
`info`) after a fresh supervisor launch or reboot. Paired deployment stages
`paired_control.sh` with the supervisor.

When the Matter package is already staged but there is not enough flash to
stage another binary copy, activate the paired launcher while checking the
same local artifacts against the staged device files:

```sh
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native --activate-paired-only \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --matter-service build/thread-matter/thread-matter-mips
```

`--activate-paired-only` uploads only the launcher after verifying the staged
Matter manifest, executable files and their MD5 hashes; it requires the
matching local native and service files as references. It preserves the old
launcher before replacing `/tuya/tuya_start.sh`. The boot script starts the
paired supervisor once, so restarting that supervisor does not trigger an init
respawn. After it exits cleanly, invoke `/tuya/tuya_start.sh` over SSH to start
the new paired stack. `--restart` remains for restarting only the native daemon
under the existing supervisor.

## Device build, deployment, and checks

The development gateway is `192.168.178.62`. From the repository root, use the
untracked password file only by pathname:

```sh
make -C microhad host test
make -C microhad mips
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --target /tuya/native --restart
```

Service restarts are allowed when needed to load a deployed binary, unless the
restart is expected to be destructive. `--restart` is only allowed for
`/tuya/native`; it signals the exact `microhad` daemon and the
permanent launcher restarts it from the newly deployed binary. It does not
reboot the device or alter Zigbee state.

Check the external dashboard rather than using `curl` on the device (it is not
installed there):

```sh
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/status
curl --connect-timeout 5 --max-time 8 -sS http://192.168.178.62/api/logs
curl --connect-timeout 5 --max-time 8 -k -sS https://192.168.178.62/api/status
```

When a `curl` request to `http://192.168.178.62/` or any endpoint beneath it
fails, the cause is almost always the sandbox. Rerun the failed command with
elevated permissions (`sandbox_permissions: "require_escalated"`), even if
previous `curl` invocations succeeded without elevation. Do not conclude that
the gateway is unreachable until the elevated retry has been attempted.

The dashboard's **Restart service** button uses the following endpoint. It
gracefully restarts only the supervised native daemon; it does not reboot the
device or replace Zigbee state:

```sh
curl --connect-timeout 5 --max-time 8 -sS -X POST http://192.168.178.62/api/service/restart
```

The dashboard serves HTTP on port 80 and HTTPS on port 443. The coordinator
thread is the sole owner of the NCP serial connection and `Gateway` state.
Each HTTP listener has an acceptor plus two request workers; all mutations use
the coordinator command queue. Status reads use protected snapshots; log reads
copy the syslog shared-memory ring with bounded semaphore acquisition, without
locking the coordinator.
The single notification worker owns outbound FCM/browser-push calls and keeps
at most 32 pending jobs in RAM. Do not call `Gateway` directly from a web or
notification thread.

The dashboard's global Push notifications switch persists in
`notifications.json` beneath the runtime directory. Turning it off discards
pending RAM jobs, blocks test and automatic push delivery, and retains browser
subscriptions for a later re-enable. Disabled notifications must be rejected
before notification audit/outbox handling and before the notification-worker
queue, so contact events do not exercise delivery-related code while push is
off.

A contact opening is only an alert candidate. Its per-device settings default
to a ten-minute first `Window open` notification and five-minute reminders;
closing it cancels the pending alert.

Secure join preparation waits at most ten seconds for each NCP policy reply.
If one is lost, preparation is cancelled and the dashboard permits another
joining request instead of remaining stuck indefinitely.

Temperature and humidity sensors receive standard report configuration on
discovery (five-minute maximum interval). As a bounded fallback for devices
that ignore it, each known measurement sensor defaults to active-endpoint
refresh every five minutes; its dashboard setting can select another whole
minute interval or `0` to disable fallback polling. The coordinator then
refreshes attributes after the device responds. This is intentionally
serialized for sleepy battery devices.

The paired launcher runs BusyBox `syslogd -C512 -L -R 192.168.178.51:7878`.
Native gateway and Matter service messages use syslog directly; Ziggurat's
tracing output is sent to syslog. The `/api/logs` reader attaches directly to the daemon's 512 KiB RAM ring and
uses bounded, nonblocking semaphore acquisition. It never forks a helper.
The separately deployed BusyBox 1.13.4 `logread` remains a diagnostic tool; it
omits the `syslogd` applet because the stock firmware runs that daemon. See
[syslog IPC compatibility](docs/syslog-ipc.md) for supported layouts and bounds.
Permanent radio captures belong under the
ignored `diagnostics/` directory and must never be committed.

Warning: daily operation must not write logs, diagnostics, or other
high-frequency state beneath `/tuya` (flash). Keep transient output in `/tmp`
and verify new runtime behavior does not introduce flash write cycles.

## Garage Android app

`../garage-app/` is a separate Git repository copied into this workspace and
ignored by the parent repository. Commit changes to the Garage app in
`../garage-app/` separately from changes to this repository. Do not stage or
commit Garage app files in the parent repository.
