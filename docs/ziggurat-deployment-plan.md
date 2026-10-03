# Deploy Ziggurat on the existing Tuya gateway

Mirrored from `microhad-zigbee-native` commit `03c828bce9a7ab0b07a9da31c4c89b827905f071`.
Host commands run from that sibling checkout unless a block changes directory.


## Current state and next action — 2026-10-03

**This is a plan, not an executed deployment.** Use the existing gateway at
`192.168.178.62`; no spare gateway or radio is required. This plan supersedes
older recommendations to rehearse on spare hardware.

Production currently uses the native EZSP service and EmberZNet 7.5.1.0 / EZSP
v13 on `/dev/ttyS1`. The optional native Ziggurat backend, COBS Unix API and
native-owned persistence pass host tests; both native variants and Ziggurat
build for MIPS. Ziggurat-only device simulations pass. Multi-PAN HDLC firmware
is built but **has not been flashed**.

**Next action:** complete the offline recovery tools in step 1, then stage files
and repeat a synthetic test before scheduling the radio maintenance window.
The first flash is step 7. Steps 1–4 leave production running. Steps 5–10 cause
Zigbee downtime on this same gateway. Do not make startup permanent until the
manual cutover and rollback entry checks succeed.

The objective of this deployment is existing-network Zigbee operation through
native → COBS Unix socket → Ziggurat → HDLC RCP. Thread and Matter follow later;
use the dual-PAN firmware now, but leave IID 2 unused during initial cutover.

## Rollback rules and checkpoint map

Keep an immutable private pre-cutover archive on the host and a working recovery
copy outside the gateway. Application settings/device inventory and radio network
security state are different backups; retain both. Do not print keys or put
backups/configuration in Git. Use mode 0700 directories and 0600 private files.

| Checkpoint | What changed | Available return path |
| --- | --- | --- |
| A: before maintenance | Only staging/test files | Remove staging; production continues |
| B: services stopped, original NCP installed | Process/UART ownership | Stop bridge, resume original launcher/service |
| C: bootloader entered, no upload | Radio temporarily in bootloader | Select run; confirm EZSP; resume original service |
| D: RCP uploaded, Zigbee not started | Radio application replaced | Enter bootloader, restore pinned NCP GBL, verify/restore final pre-cutover network, resume EZSP |
| E: Ziggurat network has transmitted | Current keys/counters live in host stack | Freeze/export latest stack; prepare rollback backup; restore NCP application and latest logical network |
| F: permanent startup installed | Launcher, binaries/config and radio changed | Quiesce launcher, follow E, atomically restore saved EZSP launcher/binary/config |

After checkpoint E, the pre-cutover backup is **not** sufficient on its own:
devices may have accepted higher counters or newer keys. Never simply reinstall
the old daemon while the radio is RCP, or restore stale security counters.

Rollback is an available procedure, not a guarantee against every failure.
A responding RCP supports compiled Spinel bootloader entry. Recovery from an
unresponsive application without verified hardware bootloader entry/SWD remains
unproven; no spare device is introduced as a prerequisite. If neither the
application nor bootloader is reachable, stop software retries and diagnose
physical recovery on this gateway. Do not erase the chip or replace its bootloader.

Unless a block says it runs on the gateway, commands run on the host from
`../microhad-zigbee-native`. SSH commands can use this host helper:

```sh
gateway_ssh() {
    sshpass -f ../ssh_password.txt ssh \
      -o ConnectTimeout=8 -o StrictHostKeyChecking=yes \
      -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
      -o PreferredAuthentications=password -o PubkeyAuthentication=no \
      root@192.168.178.62 "$@"
}
```

## 1. Finish recovery tooling and pin all artifacts — host only

Record exact native/experiment commits, build flags, firmware and executable
SHA256 hashes in a private deployment record. Rebuild and verify sequentially:

```sh
cd ../microhad-zigbee-native
make test-backends
make ZIGGURAT=0 mips
make ZIGGURAT=1 mips
cd ../ziggurat-compilation
BUILD_JOBS=4 bash scripts/build-hdlc-firmware.sh
ZIGGURAT_MULTIPAN=1 bash scripts/build-ziggurat.sh
```

The experiment README lists the pinned Rust, RSDK, GSDK, ARM compiler and SLC
prerequisites. The migration venv pins Bellows, zigpy and the flasher as described
in [NETWORK_MIGRATION.md](network-migration.md).

Complete these missing pieces **before any radio upload**:

1. Package `ot-rcp-multipan.out` as an application-only, uncompressed GBL using
   a pinned Silicon Labs packaging tool. The existing build only emits ELF/raw
   binary. Record the packaging command/tool version; run `ezsp_preflight.py image`
   and offline `ezsp_flash.py` validation. Verify program ranges stay in
   `[0x4000,0x37000)` and no bootloader/secure-element update exists. Do not send
   the raw `.bin` to the UART bootloader. An exact packaging command is intentionally
   pending tool selection; it is not an existing repository command.
2. Implement/test a bounded **RCP-to-bootloader helper**: exclusively open UART at
   460800 with RTS/CTS, issue Spinel reset reason 3, close it, reopen at 115200
   without flow control, and recognize the Gecko menu. It must support this old
   gateway and refuse ambiguous ownership. `ezsp_flash.py` normally enters through
   EZSP and cannot perform that RCP entry. Its `--already-in-bootloader` path is
   reusable only once the menu and correct bridge settings are established.
3. Provide a tested **freeze/export command** over the COBS socket, using
   `ziggurat_migration.freeze_and_export(client, original_template)`, and private
   output. Use the existing socket probe as the SSH byte-stream bridge. This is a
   library function today, not an existing CLI. Validate complete scan counts;
   do not trust a partial table export. Test native shutdown/freeze followed by
   this export so restart cannot race it.
4. Provide a final exclusive NCP capture/check command after stopping production.
   Reuse the native export's fixed read commands or equivalent Bellows v13 calls;
   read NWK and live APS counters, keys/seed and child state. Compare identity to
   the production export and preserve complete state. Existing `network_backup.py
   collect` requires the running service, so it cannot itself do this final
   exclusive capture. This closes the counter gap before bootloader entry.
5. Prepare the paired startup supervisor described in step 11 and an explicit
   maintenance stop operation. Test them on the host: ordering, child exits,
   stale sockets, bounded shutdown, no restart during rollback and no two UART
   owners. Avoid unbounded console logs and restart loops that repeatedly write
   state or advance counters.

Retain the previously installed recovery GBL:
`diagnostics/ezsp-upgrade-recovery/ncp-uart-hw-7.5.1-115200-hw.gbl`, SHA256
`05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d`.
Validate both forward and reverse images, and the logical restore backup offline.

**Pass:** both images validated, all tools tested with synthetic fixtures, exact
upload/entry/export commands recorded, no unresolved command placeholders in the
execution checklist. **Rollback:** delete build/staging outputs if desired;
production is untouched. If a tool is unfinished, continue implementing it rather
than entering the maintenance window.

## 2. Save current service and application state — production stays online

Over SSH, inspect `ps w`, each candidate `/proc/PID/status`, `/proc/PID/cmdline`,
`/proc/PID/exe` and `/proc/PID/fd` to identify the exact daemon and supervisor.
Do not assume the filename identifies the supervisor, or reuse recorded PIDs.
Use the password file by pathname and the old Dropbear RSA options documented in
[the integration guide](native-ziggurat-integration.md); never expose the password.

Save the current executable, `/tuya/tuya_start.sh`, configuration, application
state, device settings, groups/scenes, subscriptions and required TLS/notification
assets privately on the host. Check archive contents and checksums. Because the
service is active, take a second consistent copy after shutdown in step 5.
Keep the **currently deployed v13-compatible daemon**, not the historical v8-only
executable, as the primary service rollback binary.

Record HTTP/HTTPS health, formed status, backend, actual current device count,
coordinator identity and a short baseline of existing light/sensor behavior.
Close joining. Avoid new pairings, key rotation and application configuration
changes throughout handover and the initial observation period.

**Pass:** readable private backups and a baseline; inventory includes all current
application files. **Rollback:** no operational change beyond closing joining;
production keeps running. Keep joining closed for a later retry.

## 3. Stage isolated binaries and test synthetic communication

Measure `df -k /tuya` and `MemFree`; budget both binaries, old recovery copies,
state, logs, process RSS and atomic state temporary files. Previous free flash
was only about 5 MiB, so do not assume two additional executables fit. Keep host
recovery copies and remove only verified obsolete experimental artifacts if
needed. Do not delete production/configuration to make space. `/tmp` is ramfs;
executable staging there consumes nonreclaimable RAM. Use temporary flash staging
with explicit cleanup when needed.

Stage the enabled native executable in `/tuya/ziggurat-stage`, without activation:

```sh
cd ../microhad-zigbee-native
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --binary-only --binary build/mips-ziggurat/microhad-zigbee-native \
  --target /tuya/ziggurat-stage
```

Transfer the pinned Ziggurat executable through SSH stdin into the isolated
staging directory with restrictive permissions and atomic rename. Verify hashes.
Do not start the staged native service yet. Repeat the existing Ziggurat-only
host-RCP test with synthetic state:

```sh
cd ../ziggurat-compilation
python3 scripts/test-cobs-unix.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --upload-storage flash
```

This test cleans its own unique uploads; it does not activate the staged native
binary. Check production remains online/formed and its executable unchanged.

**Pass:** staged hashes correct, storage/memory sufficient, test and cleanup pass.
**Rollback:** remove only the isolated staging/test files. No service restart or
radio recovery is needed.

## 4. Prepare private configuration and a preliminary import

Create a staged configuration from the backed-up production configuration. Retain
all application settings and TLS paths; choose the intended persistent runtime
folder and `/tmp/ziggurat/native.sock`. Keep the socket directory mode 0700 and
native/Ziggurat under the same user. `ziggurat-state.json` must be mode 0600 in a
persistent runtime folder; RAM-only storage is not power-loss recovery.

Collect/convert a preliminary fresh export using the commands in
[NETWORK_MIGRATION.md](network-migration.md), then:

```sh
cd ../microhad-zigbee-native
migration_python=../ziggurat-compilation/tools/network-migration-venv/bin/python
"$migration_python" scripts/ziggurat_migration.py prepare-native \
  --state PRIVATE_IMPORT_BUNDLE.json --output PRIVATE_NATIVE_STATE.json
"$migration_python" scripts/test_ziggurat_import.py --state PRIVATE_STATE.json
```

The actual-network import test remains stopped-only on the host simulator.
Do not start actual network state in a simulator. This preliminary state is a
format check, **not the final cutover state** while the original NCP is active.

Review persistent write frequency: state updates follow security notifications
and checkpoints, not individual frames; identical snapshots skip writes. Measure
expected write rate and available flash endurance/storage. Logs and diagnostics
must stay bounded in RAM. If persistence behavior needs changing, fix/test it
before permanent deployment rather than moving security state to ramfs.

**Pass:** private config/import valid and persistent storage selected.
**Rollback:** remove staged private files; production stays on its own runtime.

## 5. Begin maintenance: stop production and finalize the handover backup

1. Disable automated controls/joins and take a new native export through the
   running owner. Use a new private filename for every capture.
2. Reidentify PIDs. SIGSTOP the verified supervisor so it cannot respawn the
   daemon; SIGTERM the exact daemon and wait for exit. Preserve the supervisor
   PID for later SIGCONT. Do not use broad `killall` commands.
3. Confirm no daemon/serialgateway/low.js/Ziggurat owns `/dev/ttyS1`. Capture the
   now-consistent application files and compare them to step 2.
4. Start only the existing `/tuya/serialgateway -p 8888`, with bounded RAM logging,
   and use the prepared exclusive capture/check tool from step 1. Keep its
   connection exclusive through the final reads and immediately enter bootloader
   in step 6; do not resume normal NCP operation between final capture and entry.
5. Verify final identity, channel, PAN, keys/seed, tables and live NWK/APS counters.
   Prepare new normalized state, import bundle, native state and coordinator
   recovery backup. The import advances counters by 10000; native cold startup
   adds 1000. Never manually lower either counter or reuse the preliminary files.
   If the radio ran further after capture or capture/entry was interrupted,
   refresh the final evidence before proceeding.

**Pass:** exactly one bridge owner; consistent final application/security backups
on the host; no automatic coordinator restart. **Rollback:** terminate the exact
bridge, verify the original NCP responds to EZSP, SIGCONT the verified supervisor,
and confirm production/network traffic. Nothing has been flashed.

## 6. Verify bootloader entry and return on this gateway

With exclusive bridge ownership, run the existing non-upload round trip:

```sh
"$migration_python" scripts/ezsp_preflight.py bootloader-roundtrip \
  --host 192.168.178.62 --exclusive-radio
```

This selects run after recognizing the menu; it does not upload. Confirm EZSP
v13 returns. Then repeat the final capture/check and enter bootloader for step 7;
the round trip invalidates any assumption that earlier counters are final.

**Pass:** recognizable Gecko menu and working return to the original application.
**Rollback:** if already in the menu, use `bootloader-return`; if already running,
stop the bridge and resume EZSP as in step 5. If neither answers, stop and diagnose
entry/bridge settings; do not upload to compensate for an unverified connection.

## 7. Upload only the RCP application

Use the validated RCP GBL and its recorded SHA256. With the confirmed bootloader
menu, correct 115200/no-flow-control bridge and exclusive ownership:

```sh
"$migration_python" scripts/ezsp_flash.py --image PRIVATE_RCP_GBL_PATH \
  --sha256 RECORDED_RCP_SHA256 --host 192.168.178.62 \
  --exclusive-radio --already-in-bootloader --flash
```

The existing script returns the bridge to hardware flow control afterward, but
it does not change that bridge to the RCP's 460800 baud. Stop the exact bridge
before opening the RCP at its new settings. Do not replace the bootloader, erase
NVM3, or automatically retry an upload with uncertain completion.

**Pass:** bootloader explicitly accepts the application. **Rollback:** if still
in the bootloader, upload the pinned NCP GBL through the same guarded
`--already-in-bootloader` path. If RCP is running, use the prepared RCP entry
helper first. Follow the NCP verification/restoration procedure below even if
old NVM3 appears intact. If neither is reachable, retain evidence and diagnose
physical recovery; there is no honest guaranteed software rollback for that case.

## 8. Start Ziggurat alone and test reverse entry before starting Zigbee

Keep the original launcher suspended. Create the private socket directory and
manually start only Ziggurat, with console output bounded in RAM. The following
command runs **on the gateway**, in a controlled session:

```sh
/tuya/ziggurat-stage/ziggurat --device /dev/ttyS1 --baudrate 460800 \
  --flow-control hardware --spinel-iid 1 --api cobs-unix \
  --listen unix:/tmp/ziggurat/native.sock --log-level error
```

Query Hello/GetInfo/hardware address and confirm RCP/interface compatibility.
Do not load/start the Zigbee network yet; no native client should be connected.
Confirm CPU/RSS and startup deadline. Cold-boot entropy stalls remain a known
issue: retain randomness checks, report a bounded failure and diagnose entropy.

Stop Ziggurat, prove UART release, and test the prepared RCP bootloader entry
helper on this same radio. Confirm the menu, select run **without uploading**, and
restart Ziggurat. This checks the reverse application entry before migrated
Zigbee traffic begins. A protocol probe can reset/configure radio hardware even
without StartNetwork; keep normal services suspended.

**Pass:** RCP communication and reverse bootloader menu/run work at their proper
rates. **Rollback:** enter the menu and restore the pinned NCP application, then
verify/restore the final pre-cutover logical backup as described below. If startup
is merely a process/config issue, stop Ziggurat and correct it without reflashing.

## 9. Start the staged native client with final state

Install only the **final** prepared private native state and consistent application
files in the chosen runtime directory, verifying ownership/checksum. Keep host
copies immutable. Start the staged enabled native daemon manually, using the
staged configuration. It should connect to the COBS socket, save advanced counters
before Configure, load tables and start the existing network. It must not form a
replacement network or access the UART itself.

Check HTTP/HTTPS: backend `ziggurat`, online/formed, expected current inventory;
compare identity, PAN/channel and key metadata privately. Observe that only
Ziggurat owns the UART. Verify private state updates and no secret-bearing logs.
This is checkpoint E as soon as the stack sends its first frame.

**Pass:** state loads, counters advance and existing identity is retained.
**Rollback:** stop the staged native daemon gracefully while keeping Ziggurat
alive; confirm freeze and capture a fresh normalized final snapshot through the
prepared export tool. Use the orderly rollback procedure below. A native shutdown
has a five-second bound, so exit alone does not prove freeze/export succeeded.
If no frames were sent, prove that before considering the pre-cutover backup.

## 10. Validate the existing network during a reversible observation window

Keep joining and security rotation closed. Test one already-paired light with
an explicit reversible command, then restore its previous state. Verify real
APS success rather than HTTP admission alone. Check group/broadcast behavior,
existing routers, temperature/humidity/contact reports and a naturally waking
sleepy child. Preserve existing pairings; do not re-pair to hide failures.

Observe for at least 30 minutes of active traffic and until representative sleepy
devices have reported (extend the window for their actual reporting periods).
Measure `top` summed LinuxThreads CPU, RSS, MemFree, control latency, UART errors,
state-write rate and reconnect behavior. Use current EZSP observations as the
baseline; synthetic 5.27 ms query timing is not a required radio latency target.

Test graceful native restart/freeze-save and attachment/reload, one change at a
time. Delay deliberate process crashes and gateway power-cycle tests until
orderly recovery is confirmed and fresh backups exist. Do not claim the 1000-frame
margin covers unlimited notification loss or state lag.

**Pass:** existing devices communicate, no sustained memory growth, missing keys,
counter regressions, repeated reconnects or unacceptable flash writes.
**Rollback:** freeze/export latest state while responsive, then orderly NCP return.
If the problem is limited to application settings, restore those settings and
retry without changing radio firmware or rolling back security state.

## 11. Install permanent paired supervision only after manual validation

Prepare an atomic launcher update retaining the original `/tuya/tuya_start.sh`
and v13-compatible binary/config privately. The current `native_start.sh` only
supervises the native daemon: it is **not sufficient** for the two-process backend.
Do not use `deploy.sh --activate` unchanged for this cutover.

The new launcher must create the private RAM socket directory, start Ziggurat,
wait for API readiness with a deadline, then start the native daemon. Native
failure must freeze/save before restart when possible; Ziggurat failure must
make native offline and trigger a controlled recovery using persisted state.
A maintenance stop must disable respawn, freeze/export, stop native, then stop
Ziggurat. Only Ziggurat owns reset/UART. Keep startup retries bounded with backoff,
logs bounded in RAM, and no automatic EZSP fallback on RCP firmware.

Install intended binaries/config atomically with enough room for rollback and
state temporary files; recheck all hashes. Test supervised process startup first,
then a controlled gateway reboot. Preserve the host's latest private snapshot
before reboot; `/tmp` sockets/logs are ephemeral, network state is not.

**Pass:** startup order, reboot, service health, persistence and maintenance stop
work. **Rollback:** quiesce the new supervisor; freeze/export if possible; perform
orderly NCP return; atomically restore the saved EZSP launcher/binary/config and
resume it. Restoring a launcher alone does not undo a radio migration.

## 12. Close the Zigbee deployment, then add Thread/Matter separately

Save a fresh private frozen recovery snapshot during a controlled stop, resume
Zigbee and archive the deployment hashes/validation record. Retain the NCP GBL,
original service files and recovery tools on the host. Mark unresolved device or
recovery issues explicitly; do not delete rollback assets after one successful day.

Only then plan Thread on IID 2 and Matter on the same device: common channel,
one radio reset owner, coordinated scans, separate private HDLC endpoint and
combined memory/CPU measurements. No `cpcd` is needed for the chosen HDLC design.

**Rollback:** while adding Thread, first stop/disable the Thread/Matter consumer
and return to the validated Zigbee-only setup. A full Zigbee rollback remains the
orderly NCP procedure below; Thread commissioning/data needs its own backup.

## Orderly radio-and-network rollback after Ziggurat transmitted

1. Disable respawn and new application sends. Gracefully stop native, keep
   Ziggurat alive, then run the prepared COBS freeze/export tool. Verify stopped
   state and complete keys/children/counters; save it privately on the host.
   The native wire-state JSON alone is not accepted as the normalized frozen
   rollback input: it lacks the explicit post-freeze evidence required by the tool.
2. Prepare and validate the recovery backup offline:

   ```sh
   "$migration_python" scripts/ziggurat_migration.py prepare-rollback \
     --original PRIVATE_FINAL_PRE_CUTOVER_STATE.json \
     --last-stopped-state PRIVATE_FINAL_FROZEN_STATE.json \
     --output PRIVATE_NCP_ROLLBACK_BACKUP.json
   "$migration_python" scripts/ezsp_restore.py \
     --backup PRIVATE_NCP_ROLLBACK_BACKUP.json
   ```

   Changed identity/network key/seed causes refusal; investigate rather than
   bypassing validation. Preserve the newest device keys and both counter spaces.
3. Stop all consumers, verify exclusive UART ownership, use the prepared RCP
   reset-to-bootloader helper at 460800/RTS-CTS, then the verified 115200/no-flow
   bridge. Upload the pinned application-only NCP GBL:

   ```sh
   "$migration_python" scripts/ezsp_flash.py \
     --image diagnostics/ezsp-upgrade-recovery/ncp-uart-hw-7.5.1-115200-hw.gbl \
     --sha256 05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d \
     --host 192.168.178.62 --exclusive-radio --already-in-bootloader --flash
   ```

4. Confirm EZSP v13 with exclusive bridge ownership. Restore the prepared backup:

   ```sh
   "$migration_python" scripts/ezsp_restore.py \
     --backup PRIVATE_NCP_ROLLBACK_BACKUP.json --host 192.168.178.62 \
     --exclusive-radio --restore --output PRIVATE_VERIFIED_NCP_BACKUP.json
   ```

   This intentionally writes network state and further advances counters. If
   verification fails, use `--verify-only` with a new output filename before
   deciding another write is needed. Do not blindly repeat restore.
5. Terminate the exact bridge; atomically restore the saved v13-compatible native
   executable, application configuration and EZSP launcher as needed; resume the
   verified original supervisor or start the restored launcher once. Check
   online/formed, identity and real existing-device reports/controls.

Before any migrated transmission, the final pre-cutover coordinator backup can
be used instead of the frozen-stack rollback backup, with counters advanced by
the restore tool. After transmissions, never assume that earlier state is fresh.

If native crashed but Ziggurat responds, export the live stack after freeze;
do not prefer an older disk snapshot. If both crashed, persisted checkpoints and
the ZHA margin may permit recovery, but do not prove final counters/current keys.
Recover Ziggurat from the latest valid saved state only when its freshness and
counter margin can be justified, then freeze/export; otherwise diagnose before
network restoration. Power loss or an unresponsive RCP can remove the evidence
needed for a safe logical rollback. These limits remain part of this same-device
plan, rather than a requirement for another gateway.
