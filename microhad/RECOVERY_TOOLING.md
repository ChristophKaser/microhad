# Prepared deployment/recovery tools — 2026-10-03

Recovery tools are deployed under `/tuya/native`. Both NCP and RCP bootloader
entry/run round trips passed on this gateway; the pinned RCP application was
flashed without changing bootloader or NVM3. Fresh complete frozen rollback
state is saved privately on the host. No live reverse NCP restoration was
performed. Recovery from an unresponsive RCP or arbitrary stale counters remains
unproven. See [current checkpoint](ZIGGURAT_DEPLOYMENT_PLAN.md).

Remote freeze examples below that use the former staging path must now specify
`--bridge /tuya/native/gateway-maintenance`. Quiesce with
`/tuya/native/paired_control.sh quiesce`, export complete frozen state, then
stop with `paired_control.sh stop --frozen-snapshot-saved`. Start a fresh pair
with `/tuya/tuya_start.sh`; do not merely remove the quiesce marker.

## Artifacts and offline packaging

From `../ziggurat-compilation`:

```sh
bash scripts/install-commander.sh
bash scripts/package-hdlc-gbl.sh
bash scripts/build-maintenance.sh
```

Commander is pinned to official Silicon Labs Arduino release `2.0.0`, package
`Commander_linux_x86_64_1v16p4b1564.tar.zst`, version `1v16p4b1564`:
archive SHA256 `0d1a2136a824c2f3e9953573502cb80ff459647f41df04d17a30e42140432f8a`.
The installer needs curl, tar and zstd; the executable uses its bundled libraries.
The official website download returned HTTP 403; the official GitHub asset worked.
Packaging runs `commander gbl create OUTPUT --app ot-rcp-multipan.out`, with no
compression, encryption, signing, bootloader or secure-element update.

RCP GBL: `build/firmware-hdlc/build/debug/ot-rcp-multipan.gbl`, 103800 bytes,
SHA256 `5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48`;
program range `[0x4000,0x1d52c)`. Both its CRC/ranges and the retained NCP GBL's
CRC/ranges pass offline validation. The existing private logical recovery backup
also passes `ezsp_restore.py` offline validation. Keep the new GBL build output
and pinned NCP image available on the host; neither is flashed by packaging.

Sources: [Silicon Labs packaging commands](https://docs.silabs.com/simplicity-commander/latest/simplicity-commander-commands/gbl-commands)
and [official release asset](https://github.com/SiliconLabs/arduino/releases/download/2.0.0/Commander_linux_x86_64_1v16p4b1564.tar.zst).

## Device helper — commands for later maintenance only

Stage `../ziggurat-compilation/build/gateway-maintenance` in
`/tuya/ziggurat-stage` during step 3. Commands below run on the gateway:

```sh
/tuya/ziggurat-stage/gateway-maintenance check-free /dev/ttyS1
/tuya/ziggurat-stage/gateway-maintenance check-owner /dev/ttyS1 VERIFIED_BRIDGE_PID
/tuya/ziggurat-stage/gateway-maintenance bootloader-enter /dev/ttyS1 --exclusive-radio
/tuya/ziggurat-stage/gateway-maintenance bootloader-run /dev/ttyS1 --exclusive-radio
```

`bootloader-enter` sends one CRC-protected HDLC Spinel RESET reason 3, IID 0, at
460800/RTS-CTS, drains/closes UART, waits three seconds, reopens at 115200/no-flow,
and recognizes the Gecko menu. It sends neither upload nor run. `bootloader-run`
recognizes the menu at 115200/no-flow and sends only `2` (run). All serial actions
have a 25-second watchdog; menu reads are bounded to 4096 bytes and ten seconds.
After menu entry the original serialgateway can provide the 115200 upload bridge.

Production accepts only `/dev/ttyS1`, requires root, scans `/proc` for owners,
fails on incomplete inspection and uses TIOCEXCL while opened. `check-owner`
requires exactly the supplied PID and checks its executable argv is
`/tuya/serialgateway`. Stop verified supervisors first; the check cannot stop an
uncooperative root process opening the UART later. Host test builds additionally
allow PTYs and tolerate inaccessible root processes because tests run unprivileged;
that exception is absent from the MIPS binary.

`cobs-hello /tmp/ziggurat/native.sock` checks the initial protocol-v2 Hello with a
five-second deadline; `cobs-bridge` forwards stdin/stdout with a 120-second watchdog.
Both require a same-user, mode-0600 Unix socket. Neither opens UART.

## Final exclusive NCP capture

Host command, only after step 5 suspends the original supervisor and stops native:

```sh
migration_python=../ziggurat-compilation/tools/network-migration-venv/bin/python
"$migration_python" scripts/ezsp_final_capture.py \
  --compare PRIVATE_RUNNING_EXPORT.json --output PRIVATE_FINAL_EXPORT.json \
  --host 192.168.178.62 --password-file ../ssh_password.txt \
  --bridge-pid VERIFIED_BRIDGE_PID --supervisor-pid VERIFIED_SUSPENDED_SUPERVISOR_PID \
  --exclusive-radio --capture --enter-bootloader
```

Without `--capture` it only validates the comparison export. Live mode checks the
supervisor is SIGSTOPped and the exact bridge owns UART; Bellows connection performs
ASH initialization, so this is a maintenance action, not an online second client.
It reuses bounded read commands, handles status-only errors, validates identity,
keys/seed and counter monotonicity against the prior export, and saves a new 0600
raw export. `--enter-bootloader` then requests entry immediately on that connection.
It never flashes, joins or forms a network. This narrows the capture-to-stop gap;
it is not an atomic NCP freeze. The preparation margin remains a bounded freshness
policy, and interruption requires fresh capture. Errors may leave the saved export
or radio in bootloader; verify actual state before retrying. Overall deadline is
160 seconds, each response ten seconds, capture 120 seconds.

## Frozen rollback export

After the native client is quiesced, while Ziggurat is still running:

```sh
"$migration_python" scripts/ziggurat_freeze.py --template PRIVATE_FINAL_STATE.json \
  --host 192.168.178.62 --password-file ../ssh_password.txt \
  --quiesced --execute --output PRIVATE_FINAL_FROZEN_STATE.json
```

The default bridge is `/tuya/ziggurat-stage/gateway-maintenance`; after permanent
installation supply `--bridge /tuya/native/gateway-maintenance`. Local host tests
use `--socket PRIVATE_LOCAL_SOCKET`. Without `--execute`, only the template is
validated. The command checks stopped state, every scan count/key count, stable
final network info, keys, children and address cache, then saves normalized private
state including post-freeze evidence. Overall deadline is 120 seconds. Use this
output with `prepare-rollback`; native wire-state JSON is not a substitute.

## Paired startup and maintenance control

`paired_start.sh` is a foreground POSIX-sh supervisor; `ziggurat_start.sh` is the
candidate boot launcher. Both are prepared but **not installed**. The launcher
expects binaries, configuration, helper and paired supervisor in `/tuya/native`.
Do not use unchanged `deploy.sh --activate` to install this two-process launcher.

The supervisor atomically claims a private control directory, checks UART is free,
starts Ziggurat, verifies its Hello, then starts native. Restarts have backoff and
a finite budget (default five attempts across both children). Native failure
restarts native against the existing stack; Ziggurat failure stops native before
restarting the pair. Startup deadline is 15 seconds plus at most the five-second
probe in progress. Shutdown bounds each child to eight seconds and reports forced
termination; this does not certify freeze. Marker files use POSIX shell redirection because the device has no `touch`.
Child console output is discarded;
native's existing bounded RAM log remains available. No unbounded file is appended.

On the gateway, during an orderly maintenance stop:

```sh
/tuya/native/paired_control.sh quiesce
# Run host freeze/export and verify that the new private snapshot was saved.
/tuya/native/paired_control.sh stop --frozen-snapshot-saved
```

Quiescence stops native, disables respawn and retains Ziggurat for export. The stop
flag is an explicit operator assertion that export succeeded; it does not inspect
the host file. A fresh supervisor start is required to resume after quiescence.

Before starting Ziggurat, the supervisor restores a one-use kernel RNG seed from
`/tuya/native/ziggurat-rng.seed` through `rng-seed` and credits only the entropy
count recorded when that seed was saved. The previous file is consumed before it
is credited, preventing replay after an interrupted boot. Three minutes after
supervisor startup, a background helper waits for kernel RNG readiness, saves a
replacement atomically with mode `0600`, and removes the consumed marker. On a
first boot, or if restore fails, Ziggurat retains its existing readiness wait;
the helper does not bypass it. The seed contains no gateway identity or Zigbee
state.

Build and stage the helper plus its supervisor integration without replacing
the native daemon or web UI:

```sh
make rng-seed
./deploy.sh --password-file ../ssh_password.txt --target /tuya/native \
  --rng-seed-only --rng-seed-helper build/rng-seed
```

The supervisor writes seed-helper diagnostics to `/tmp/ziggurat-rng-seed.log`.
Ziggurat logging defaults to `error` with output discarded. For a diagnostic
start, set `ZIGGURAT_LOG_LEVEL=info` and `ZIGGURAT_LOG=/tmp/ziggurat.log` in
`ziggurat_start.sh`; both values are configurable. A deployment does not restart
the active supervisor, so changes take effect on its next start.
Stale control directories are refused and require PID inspection before cleanup.
SIGTERM to the supervisor is bounded emergency shutdown, not an orderly rollback.

## Verified host checks

Both native host suites (including 13 scripted peer cases) and both MIPS builds
pass. HDLC firmware and MIPS Ziggurat rebuild successfully. Additional checks:

| Test | Result |
| --- | --- |
| `python3 ../ziggurat-compilation/scripts/test-maintenance.py` | 4 PTY cases: guards, ownership, reset CRC, baud/flow switch, menu-only run |
| venv Python `scripts/test_deployment_tools.py` | 6 capture/freeze/permission/Hello cases |
| `python3 scripts/test_paired_supervisor.py` | 6 fake-process cases: order, quiesce, duplicates, busy UART, restart budget, readiness/stale endpoint |
| `python3 scripts/test_ziggurat_service.py` | 8 actual-stack host cases, including recovery CLI after native freeze and live/stale socket handling |
| venv Python `scripts/test_network_migration.py` | 15 migration/restore cases |
| venv Python `scripts/test_ziggurat_import.py --freeze-running` | Synthetic freeze/rollback regression passes |
| experiment `python3 scripts/test-cobs-unix.py` | 7 actual-server host COBS checks |

The supervisor fake-process stale endpoint test checks lifecycle ordering;
actual socket reclamation is also tested against the patched real server. All
UART tests use host PTYs. Real device timing/menu/ownership compatibility still
needs the later maintenance checks. No code here guarantees recovery if both
application and bootloader cannot be reached.

### Real-device capture behavior (2026-10-03)

An exclusive Bellows connection resets this NCP and leaves `networkState` at
`NO_NETWORK`. The capture helper sets runtime security level 5 and Zigbee PRO stack profile 2,
then calls `networkInit(0)` to resume the saved
network and requires `JOINED_NETWORK` before reading. It never calls formation
or joining commands. Initialization can transmit and advance counters, so final
capture and bootloader entry must use the same connection. If initialization
fails, stop; do not substitute network formation. Seven focused tool tests pass.

A daemon whose supervisor is SIGSTOPped can remain as a zombie after graceful
exit. Confirm state `Z`, zero file descriptors and the device-side UART-free
check; disappearance from `/proc` cannot be required until its parent reaps it.

The first real RCP reverse-entry check exposed a helper addressing error: IID 0
is reserved in this SDK's two-instance build, and `IidToInstance(0)` returns null.
The corrected helper sends `90 01 03` (IID 1, RESET, BOOTLOADER), with HDLC FCS.
The previous `80 01 03` frame was rejected; firmware flashing itself succeeded.
Four host PTY tests pass for the corrected helper, including framing and baud/flow
switches. See the step 8 execution record for the physical verification result.
