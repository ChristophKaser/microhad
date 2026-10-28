# MicroHAD — Micro Home Automation Daemon

This component is the native C++03 gateway service for the Tuya MIPS device.
Build target binaries with the shared compiler settings in
`../config/toolchains.mk`; the gitignored root `workspace.local.env` supplies
local compiler selection, gateway IP, LAN interface and SSH credentials. See
[shared host settings](../docs/workspace-settings.md) and the root
`workspace.local.env.example`. Host tools load it automatically; explicit
arguments and existing environment variables override it. Never commit this
trusted shell file or print credentials. Prefer an existing `SSH_PASSWORD_FILE`
by pathname. `RSDK_ROOT` can override the configured RSDK.
`make TOOLCHAIN=crosstool-ng mips` selects the source-built compiler and writes
under `build/crosstool-ng/`. See [toolchain selection](../config/TOOLCHAINS.md).

```sh
make host test
make mips
```

Artifact paths shown below are RSDK outputs. When the local configuration selects
crosstool-NG, use its isolated artifact paths from the toolchain guide; explicit
`--binary` and service-path arguments override automatic selection.

Builds default to Ziggurat and Matter.
Initialize TLS with `git submodule update --init --recursive microhad/dependencies/mbedtls`
from the repository root before building. `make mips` builds the Mbed TLS v4 submodule and produces the static,
big-endian MIPS binary at `build/mips-ziggurat-matter/microhad`.
Use `make ZIGGURAT=0 mips` for the legacy EZSP binary or
`make ZIGGURAT=1 MATTER=0 mips` for Ziggurat without Matter APIs.

The host Matter CASE fixture uses the commissioning probe at
`build/micro-thread-matter-daemon/target/release/ot-matter-commissioning-probe`.
Rebuild it from the repository root with
`bash micro-thread-matter-daemon/scripts/build.sh host commissioning`. Do not use a
plain Cargo build for this fixture: without the repository target directory it
writes under `micro-thread-matter-daemon/target/`, leaving the integration harness on an
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

From the repository root, deploy updates with `./deploy.sh`, using the configured host and SSH credentials. Explicit `--host` and `--password-file` arguments remain overrides. The default
copies the selected binary and Web UI; `--binary-only`, `--webui-only`, and
`--both` select those assets. Pass `--config` only when intentionally replacing
the device configuration. Files are transferred through SSH stdin and each
file is moved into place from a temporary file after transfer. A Matter package
contains the native binary, its `.matter-manifest`, the Rust `mthread-matter`
service, and `paired_start.sh`:

```sh
# Native gateway, from the repository root
make -C microhad mips

# Rust service, from the repository root
bash micro-thread-matter-daemon/scripts/build.sh mips service

# Stage the Matter package without changing the active launcher or processes
./deploy.sh --target /tuya/native --binary-only \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips
```

Staging does not activate Matter or restart a process. To install the
Matter-aware paired launcher together with a full package upload, add
`--activate-paired`; the original `/tuya/tuya_start.sh` is kept at
`/tuya/tuya_start.sh.lowjs-backup` only when `--rollback-dir` is supplied. If the package is already staged and flash
space cannot accommodate another binary copy, use `--activate-paired-only` to
verify the staged Matter files and switch the launcher without re-uploading
them. Both activation modes require `/tuya/native` and only affect the next
stack start.

The Matter flash preflight checks `/proc/*/exe` before atomically replacing the
native or Rust service executable. It keeps the old executable size in the
staging peak while that process is running, because the kernel retains the old
inode until process exit. If a process already runs an executable marked
`(deleted)`, preflight fails closed; restart the owning service before another
update. Keep the configured 512 KiB reserve unless an explicit maintenance
decision changes it. Never remove application backups or network state just to
make preflight pass.

For service-only updates on an installed paired gateway, select exactly the
services that need uploading. `--services matter`, `--services native`,
`--services ziggurat`, or a comma-separated combination verify local MIPS
artifacts, compare installed hashes, and skip unchanged services. Native
updates require a matching Matter manifest. Only the selected changed
executables (and native manifest when selected) are transferred. A changed
selection quiesces and restarts the paired stack using its installed launcher;
an unchanged selection performs no restart. Configuration, identity, network
state, Web UI and existing backups are preserved.

```sh
bash micro-thread-matter-daemon/scripts/build.sh mips service
./deploy.sh --services matter --target /tuya/native \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips
```

Rollback artifact generation is disabled by default for both selective and
complete maintenance deployments. Supply `--rollback-dir DIR` to explicitly
save previous application artifacts in an empty private local directory.
Selective deployment saves only selected changed services; a complete release
also saves its launcher, support executables and Web UI. Existing backups are
never removed. New legacy launcher backups are also opt-in. On upload or
verification failure, the stack remains stopped and the script reports the
failure without automatic recovery. Keep local release artifacts for manual
recovery when opting out of rollback copies.

For a complete Matter release, build all target artifacts and use
`--maintenance-deploy`. It quiesces and stops the paired stack, then replaces
the controller, Ziggurat, Matter service,
`gateway-maintenance` and Web UI. It activates the paired launcher,
starts the stack, and checks the supervisor, IPC sockets, dashboard, status API
and linked assets. It never copies or removes configuration or network/identity
state. Both maintenance modes retain the 512 KiB flash reserve, check process
ownership before executable replacement, verify uploaded hashes, and reject
running deleted executables. Selective staging retains installed files until
verified atomic replacement. A complete release can reclaim its stopped old
Matter, Ziggurat and Web UI files before staging, with measured JFFS2 checks
following every upload. Keep SSH stdin detached from asset-list loops.

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
`debug`) after a fresh supervisor launch or reboot. Paired deployment stages
`paired_control.sh` with the supervisor.

For a temporary host-OTBR handover, use
`/tuya/native/paired_control.sh thread-tcp IPv4:PORT` (for example,
`GATEWAY_IPV4:9998`, replacing `GATEWAY_IPV4` with the host-configured gateway
address). This gracefully stops the local Matter consumer,
restarts Ziggurat with `--thread-listen`, suppresses Matter respawn and keeps
MicroHAD running. `thread-tcp off` restores the Unix Thread socket and local
Matter service. The applied selection is in `active-thread-tcp` under the
private runtime control directory. It survives child restarts but always
resets to Unix/local Matter on a fresh supervisor start or reboot. Network
state and identity are preserved. Selective deployment accepts
`--services ziggurat,supervisor` to upload the changed radio executable and
supervisor/control scripts without replacing other services.

The paired supervisor retries native, radio and Matter failures indefinitely,
with `RESTART_BACKOFF` (default three seconds, minimum one second). Quiesce,
explicit stop and ownership checks still prevent unwanted starts. Legacy
`RESTART_LIMIT` and `MATTER_RESTART_LIMIT` settings no longer stop recovery.
Unexpected Matter exits record the shell wait status (128 plus the signal
number for signal deaths), PID, launch count, uptime and memory figures in
`/tmp/ziggurat-supervisor/matter.events`. This private RAM log retains the latest
64 events and is cleared on supervisor shutdown. Rust panics and critical
OpenThread radio diagnostics synchronously reach syslog before process exit.
The launcher sets `RUST_BACKTRACE=full` for both Rust services and forwards
their stderr through private RAM FIFOs to syslog at `daemon.err`, tagged
`ziggurat-stderr` and `mthread-matter-stderr`. Long lines are split into
160-byte messages to fit stock syslog limits. Forwarders drain before reuse
or supervisor cleanup; the supervisor retains the actual service PID and
exit status. Stack detail still depends on target backtrace support and symbols.

Diagnostic defaults are `debug` for Ziggurat and Matter, with
`ZIGGURAT_RAW_FRAMES` and `MATTER_RAW_FRAMES` both `on`.
Firmware-level `MATTER_RADIO_TRACE` stays `off`: enabling it disrupted
operational CASE recovery on the shared radio.
These defaults apply to fresh supervisor starts and reboots; explicit environment
settings still override them. Temporary control requests last for that supervisor
lifetime. Both Spinel interfaces, Zigbee radio frames, Thread Spinel frames and
Matter UDP payloads are captured through bounded 256-entry logging queues
(up to 4 MiB each, allocated only as entries arrive); check drop
markers before treating missing packets as evidence. Packet logs include keys.

The coordinator uses normal nice priority (0). `COORDINATOR_SLOW` reports work
turns above 100 ms, rate-limited to one every five seconds, with command, poll,
publish and thread CPU timings. `MATTER_SLOW` reports stages above 50 ms and
`MATTER_LOOP` summarizes maximum turn/gap times every five seconds. Ziggurat's
`radio_timing` reports writer-lock, UART-write, reply-wait, Thread arbitration
and consumer-reply-write durations in microseconds.

The host syslog collector keeps 50 full-log rotations of 100 MB and independently
keeps 50 event-log rotations of 10 MB. `gateway-events.log` contains warnings,
errors, lifecycle and loop timing so packet volume cannot displace that evidence.
Measure the deployed capture rate before estimating full-packet retention.

Matter uses `MATTER_LOG_LEVEL` (normally `debug`) and
`/tuya/native/paired_control.sh matter-log-level LEVEL` with the same six levels.
This gracefully restarts only Matter. The level lasts for the supervisor lifetime and resets to
`MATTER_LOG_LEVEL` on a fresh launch or reboot. Debug shows protocol activity;
trace includes decrypted payloads. Matter syslog bodies use 174-byte limits and
the same fragment markers as Ziggurat to fit the stock 255-byte receive limit.

Enabling firmware radio tracing causes unstable radio behavior on this
shared-radio gateway, including failed Matter CASE sessions and RCP timeouts.
It is only for debugging a specific problem during a short, explicitly
requested investigation. Keep it disabled for normal operation and routine
software packet capture, and disable it immediately after that investigation.

The EFR32 firmware radio trace is disabled by default. Use
`/tuya/native/paired_control.sh radio-trace on` to restart only Ziggurat with
RCP debug logging enabled; use `radio-trace off` to turn it off. This captures
post-encryption Thread frames and emits the active MAC key once per key change,
so the syslog archive contains decryptable frames. for development purposes, logging and storing keys is okay.

When the Matter package is already staged but there is not enough flash to
stage another binary copy, activate the paired launcher while checking the
same local artifacts against the staged device files:

```sh
./deploy.sh --target /tuya/native --activate-paired-only \
  --binary microhad/build/mips-ziggurat-matter/microhad \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips
```

`--activate-paired-only` uploads only the launcher after verifying the staged
Matter manifest, executable files and their MD5 hashes; it requires the
matching local native and service files as references. With `--rollback-dir`, it preserves the old
launcher before replacing `/tuya/tuya_start.sh`. The boot script starts the
paired supervisor once, so restarting that supervisor does not trigger an init
respawn. After it exits cleanly, invoke `/tuya/tuya_start.sh` over SSH to start
the new paired stack. `--restart` remains for restarting only the native daemon
under the existing supervisor.

## Device build, deployment, and checks

Use the private workspace settings for the selected gateway, compiler and SSH
credentials. A configured gateway is not authorization to deploy to
it: device modifications still require an explicit request. From the repository root:

```sh
make -C microhad host test
make -C microhad mips
./deploy.sh --target /tuya/native --restart
```

Service restarts are allowed when needed to load a deployed binary, unless the
restart is expected to be destructive. `--restart` is only allowed for
`/tuya/native`; it signals the exact `microhad` daemon and the
permanent launcher restarts it from the newly deployed binary. It does not
reboot the device or alter Zigbee state.

Check the external dashboard rather than using `curl` on the device (it is not
installed there):

```sh
. ./scripts/workspace-env.sh
workspace_load "$PWD"
: "${GATEWAY_IP:?Set GATEWAY_IP in workspace.local.env}"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/status"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/logs"
curl --connect-timeout 5 --max-time 8 -k -sS "https://$GATEWAY_IP/api/status"
```

When a `curl` request to the configured gateway or any endpoint beneath it
fails, the cause is almost always the sandbox. Rerun the failed command with
elevated permissions (`sandbox_permissions: "require_escalated"`), even if
previous `curl` invocations succeeded without elevation. Do not conclude that
the gateway is unreachable until the elevated retry has been attempted.

The dashboard's **Restart service** button uses the following endpoint. It
gracefully restarts only the supervised native daemon; it does not reboot the
device or replace Zigbee state:

```sh
curl --connect-timeout 5 --max-time 8 -sS -X POST "http://$GATEWAY_IP/api/service/restart"
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

The paired launcher reads `syslog_remote` from `NATIVE_CONFIG` using the native
daemon's `--print-config syslog_remote --config PATH` mode before changing any processes.
It runs BusyBox `syslogd -C512 -L -m 0`, adding `-R HOST:PORT` only when
`syslog_remote` is nonempty. Missing or empty values disable forwarding.
The development gateway's private device configuration retains its collector;
never hard-code that destination in the launcher or public example config.
Native gateway and Matter service messages use syslog directly; Ziggurat's
tracing output is sent to syslog. The `/api/logs` reader attaches directly to the daemon's 512 KiB RAM ring and
uses bounded, nonblocking semaphore acquisition. It never forks a helper. See
[syslog IPC compatibility](docs/syslog-ipc.md) for supported layouts and bounds.
Permanent radio captures belong under the
ignored `diagnostics/` directory and should never be committed.

Warning: daily operation must not write logs, diagnostics, or other
high-frequency state beneath `/tuya` (flash). Keep transient output in `/tmp`.
The explicitly authorized exception is `history.values`: its 255 flash slots
are stable, full pages are persisted by a background worker, and dirty sparse
pages are written only every four hours. Never compact flash history or clear
slots without a replacement. See `SENSOR_HISTORY_FORMAT.md`.

