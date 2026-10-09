# Deploy Ziggurat on the existing Tuya gateway

> **Current fix and plan (2026-10-03):** the herdsman TH01 profile and scoped
> fresh-join replay fix are host-tested and deployed. The replay defect failed
> before and passes after the patch in the actual host server. Production is
> online; joining is closed and temporary captures are stopped. Device acceptance
> and sustained reporting await user availability. No radio flash was performed
> for these fixes. Next: capture, REST alert, one pairing attempt, then observe.
> [Implementation, tests, deployment and rollback](th01-herdsman-fix.md).

> **Reporting correction (2026-10-03):** cached-endpoint rejoins previously
> skipped binding/reporting setup. The native fix is tested for both backends
> and deployed. Küche accepted setup and sent four reports, but sustained
> reporting after a later rejoin remains unresolved.
> [Current thermometer status](thermometer-recovery.md).

> **Post-deployment correction (2026-10-03):** investigation of Küche TH01
> reproduced and fixed child loss on relayed own parent announcements, and fixed
> rejection of TH01 signed humidity reads. Both fixes are deployed; the child
> survives startup. [Evidence and current verification](thermometer-recovery.md).


## Current state and next action — 2026-10-03

**All 12 Zigbee deployment steps are complete, with the limits below.** Use the existing gateway at
`192.168.178.62`; no spare gateway or radio is required. This plan supersedes
older recommendations to rehearse on spare hardware.

The existing Zigbee network is online through native → COBS Unix socket →
Ziggurat → real HDLC RCP (IID 1), with all 23 inventory devices retained.
Permanent paired supervision is installed and running from `/tuya/native`;
the original EZSP supervisor was retired with its files retained for rollback.
Network identity/security match the final private backup; NWK/APS counters advanced.
Steps 1–12 completed. Controlled reboot recovered automatically with a
121-second native readiness delay; final frozen recovery state is archived.
Sleepy-child stimulation was waived and remains physically unverified; broadcast
relay-quorum/CCA verdicts are intermittent. See [measured findings](real-radio-migration.md). Thread IID 2 remains unused.

**Step 1 is complete:** images are packaged/validated, binaries rebuilt, recovery
commands and candidate paired launcher implemented, and host tests pass. Exact
commands and limits are in [RECOVERY_TOOLING.md](recovery-tooling.md). Nothing was
installed on the gateway during step 1.

**Step 2 is complete:** private live service/application archive and fresh logical
network export are saved and validated. Production stayed online; its binary and
processes were not replaced/restarted. Joining was already closed.

**Execution checkpoint:** steps 1–12 completed.

**Next action:** investigate cold-boot Configure latency and verify sleepy devices
when available, then plan local Thread/Matter integration and combined resource
measurements. IID 2 remains unused; no Thread/Matter deployment is claimed.

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
`./microhad`. SSH commands can use this host helper:

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
cd ./microhad
make test-backends
make ZIGGURAT=0 mips
make ZIGGURAT=1 mips
cd ..
BUILD_JOBS=4 bash firmware/scripts/build-hdlc-firmware.sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
```

The [build guide](../docs/building.md) lists the pinned Rust, GSDK, ARM compiler
and SLC prerequisites. [Toolchain selection](../config/TOOLCHAINS.md) covers RSDK
and experimental crosstool-NG; deployment examples here use default RSDK paths. The migration venv pins Bellows, zigpy and the flasher as described
in [NETWORK_MIGRATION.md](network-migration.md).

Completed recovery components:

1. Pinned Commander 1.16.4 and application-only RCP GBL packaging script.
   RCP GBL SHA256: `5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48`.
   Both RCP and retained NCP images pass CRC/program-range checks.
2. `gateway-maintenance`: bounded UART ownership checks, Spinel reset-to-bootloader,
   baud/flow switch and verified-menu run. Host and MIPS binaries built; PTY tests pass.
3. `scripts/ziggurat_freeze.py`: guarded COBS freeze/export with complete scan-count
   validation and private output. Tested after actual host native shutdown/freeze.
4. `scripts/ezsp_final_capture.py`: exclusive v13 read-only capture, comparison,
   stopped-supervisor/bridge checks and optional immediate bootloader entry.
5. `paired_start.sh`, `ziggurat_start.sh` and `scripts/paired_control.sh`: candidate
   paired supervisor/launcher and quiesce/export/stop workflow. Six host lifecycle
   cases pass. They are not installed or activated.

Reproduce packaging and helper builds from the experiment checkout:

```sh
bash firmware/scripts/install-commander.sh
bash firmware/scripts/package-hdlc-gbl.sh
bash firmware/scripts/build-maintenance.sh
```

See [tool commands, prerequisites and verified results](recovery-tooling.md).
Hardware entry/return still needs later same-device checks. Final NCP capture plus
immediate entry narrows the counter gap; it does not supply an atomic NCP freeze.

Retain the previously installed recovery GBL:
`diagnostics/ezsp-upgrade-recovery/ncp-uart-hw-7.5.1-115200-hw.gbl`, SHA256
`05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d`.
Validate both forward and reverse images, and the logical restore backup offline.

**Pass:** completed: both images validated, all tools tested with synthetic fixtures,
and exact packaging/entry/export commands recorded. Execution-time private paths
and inspected PIDs are deliberately supplied at maintenance time. **Rollback:** delete build/staging outputs if desired;
production is untouched. If a tool is unfinished, continue implementing it rather
than entering the maintenance window.

## 2. Save current service and application state — production stays online


**Completed 2026-10-03.** Private host backup directory:
`diagnostics/ziggurat-deployment/step2-1791013232746596827/` (ignored by Git,
mode 0700; every file mode 0600). It contains:

- `service-and-application.tar`: 2,171,392 bytes, 31 entries, with the deployed
  daemon, launcher, configuration/application files, TLS/notification assets,
  serial bridge and time script. The configured external Firebase account file
  was included. Archive members/read lengths were checked and all regular-file
  MD5 hashes matched the corresponding live files after transfer.
- `deployed-v13-daemon` and `deployed-launcher.sh`: independent simple rollback
  copies. Daemon MD5 remains `7a508ac880516642572a70e87bdc5014`.
- `running-network-export.json`, `normalized-network-state.json` and
  `coordinator-backup.json`: fresh online export (65 records), normalized state
  and logical recovery backup. Offline restore validation and stopped-only actual
  host Ziggurat import passed. The import transmitted zero frames.
- Private process/fd records, HTTP/HTTPS status snapshots, bounded log snapshot,
  inventories, resource readings and `verified-baseline.json`.

The observed main daemon PID was 655 and parent launcher PID 126. LinuxThreads
PIDs share its descriptors; they are not separate coordinator services. These
PIDs are observations only: resolve them again before future maintenance.
HTTP/HTTPS were online, network formed, and device count remained 23. The fresh
radio export has zero populated link-key records and two direct children; raw
NCP token export remains unavailable. These counts are not the device inventory.
Passive observation recorded nine light-capable, four measurement-capable and
four contact-capable entries, with seven entries receiving a newer `last_seen`
during preparation. No device control commands were sent.

At the resource snapshot, MemFree was 10,092 KiB and `/tuya` had 5,380 KiB free.
Step 3 must budget both staged binaries and temporary space; these readings are
not permanent capacity guarantees. The gateway lacks `touch`, `find`, `tr` and
`readlink`. Backup used raw `/proc` reads, tar-member inventories and md5sum;
prepared supervisor/control scripts now create marker files with POSIX shell
redirection and their six host cases pass again.

This is an **online snapshot**, not a final exclusive handover. No archived file
changed during its immediate checksum comparison, but take another consistent
application copy and a fresh counter/key capture in step 5. Do not use this
backup's counter values unchanged after continued production operation.

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

**Completed.** The inactive `/tuya/native-test` copy was privately archived (13 files), checksum-verified and removed; no process used it. Free flash increased from 5,380 to 6,148 KiB. The synthetic COBS/Tuya test passed all eight checks and cleaned its temporary uploads before permanent staging. Enabled native, Ziggurat and the maintenance helper are now staged in private `/tuya/ziggurat-stage`, with transfer checksums verified. Production stayed online/formed with 23 devices and its original binary checksum. No staged daemon has been started on the real radio.


Measure `df -k /tuya` and `MemFree`; budget both binaries, old recovery copies,
state, logs, process RSS and atomic state temporary files. Previous free flash
was only about 5 MiB, so do not assume two additional executables fit. Keep host
recovery copies and remove only verified obsolete experimental artifacts if
needed. Do not delete production/configuration to make space. `/tmp` is ramfs;
executable staging there consumes nonreclaimable RAM. Use temporary flash staging
with explicit cleanup when needed.

Stage the enabled native executable in `/tuya/ziggurat-stage`, without activation:

```sh
cd ./microhad
./deploy.sh --host 192.168.178.62 --password-file ../ssh_password.txt \
  --binary-only --binary build/mips-ziggurat/microhad \
  --target /tuya/ziggurat-stage
```

Transfer the pinned Ziggurat executable through SSH stdin into the isolated
staging directory with restrictive permissions and atomic rename. Verify hashes.
Do not start the staged native service yet. Repeat the existing Ziggurat-only
host-RCP test with synthetic state:

```sh
cd ..
python3 ziggurat/scripts/test-cobs-unix.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --upload-storage flash
```

This test cleans its own unique uploads; it does not activate the staged native
binary. Check production remains online/formed and its executable unchanged.

**Pass:** staged hashes correct, storage/memory sufficient, test and cleanup pass.
**Rollback:** remove only the isolated staging/test files. No service restart or
radio recovery is needed.

## 4. Prepare private configuration and a preliminary import

**Completed.** A private staged configuration preserves `/tuya/native` application state and all TLS/notification paths, and selects `/tmp/ziggurat/native.sock`. A fresh preliminary export was converted to normalized state, import bundle and checksum-protected native wire state. Actual-state stopped host import passes with zero transmitted frames. The private state is only on the host at this stage; final counters/keys will be recaptured during maintenance. Security state remains persistent on flash; identical snapshots skip writes, counter events occur every 100 sends, and changed tables are checkpointed every 60 seconds. Hardware write-rate/endurance remains an observation gate in step 10; logs remain in RAM. No alternate native service was started.


Create a staged configuration from the backed-up production configuration. Retain
all application settings and TLS paths; choose the intended persistent runtime
folder and `/tmp/ziggurat/native.sock`. Keep the socket directory mode 0700 and
native/Ziggurat under the same user. `ziggurat-state.json` must be mode 0600 in a
persistent runtime folder; RAM-only storage is not power-loss recovery.

Collect/convert a preliminary fresh export using the commands in
[NETWORK_MIGRATION.md](network-migration.md), then:

```sh
cd ./microhad
migration_python=../tools/network-migration-venv/bin/python
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

**Completed 2026-10-03:** fresh 65-record native export, consistent private
application archive and exclusive normalized/security backups saved. Supervisor
126 is suspended; exited daemon 655 is a zombie with no descriptors; bridge 1344
is the verified sole UART owner. Existing saved network initializes after setting
runtime Zigbee PRO profile 2 (boot default is 0). The guarded capture helper and
seven focused tests cover this requirement; no formation/flash was performed.
Repeat capture immediately before upload after the step 6 round trip.


Direct user approval permits maintenance and flashing. The earlier rejected
attempts executed no maintenance commands.

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

**Completed 2026-10-03:** bootloader entry acknowledged, direct UART helper
verified its menu and selected run without uploading. EZSP v13 returned and the
fresh final capture retained identity/security. The same capture connection then
entered bootloader. Final normalized/import/native/recovery files are saved
privately. TCP OOB flow switching failed to expose the menu; restarting the exact
bridge with explicit `-f` at 115200 made the menu verifiable. Use that no-flow
bridge for upload, not an assumed OOB setting.


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

**Completed 2026-10-03:** bootloader accepted the pinned 103800-byte RCP
GBL at 100% and run was requested. Only application range `[0x4000,0x1d52c)`
was programmed; bootloader/NVM3 were excluded. Exact serial bridge stopped and
UART-free check passed. Original supervisor remains suspended. No migrated
Zigbee network has started yet (checkpoint D).


Use the validated RCP GBL and its recorded SHA256. With the confirmed bootloader
menu, correct 115200/no-flow-control bridge and exclusive ownership:

```sh
"$migration_python" scripts/ezsp_flash.py --image ../build/firmware-hdlc/build/debug/ot-rcp-multipan.gbl \
  --sha256 5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48 --host 192.168.178.62 \
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

**Completed 2026-10-03:** real IID 1 RCP identity matches the final NCP
backup; COBS v2 readiness/firmware queries passed (5.34/9.65 ms through SSH;
RSS 1252 KiB before network load). First reverse-entry attempt exposed reserved
IID 0 addressing in the helper. Corrected to IID 1; host/MIPS rebuild and four
PTY tests passed, then physical Gecko menu and run were verified without upload.
Ziggurat restarted and real hardware identity matched again. No network Start.


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

**Completed 2026-10-03:** corrected static MIPS Ziggurat deployed; native
online/formed on backend `ziggurat`, 23 existing devices retained, HTTP/HTTPS
healthy. Live stack identity/security match the final backup privately and both
NWK/APS counters are advanced. State persists privately under `/tuya/native`.
Checkpoint E now applies: rollback requires a fresh frozen stack snapshot.
Eight actual native/host-Ziggurat/simulator lifecycle scenarios pass after fix.


**In progress 2026-10-03:** final private state installed, original 23-device
inventory retained. First network start failed before reset completion because
Ziggurat's owner reset also targeted reserved IID 0. Fixed to active IID 1;
16 Rust transport tests, seven simulator checks and eight host multi-PAN
scenarios pass. Rebuilding the MIPS binary before retry. Native remains offline;
permanent startup has not been installed.


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

**Completed 2026-10-03 with explicit limits:** user shortened the window
to 15 minutes; actual observation was 1215 seconds / 113 online snapshots with
zero offline/error samples. Six Zigbee devices (lights and temperature/humidity)
reported; six non-Zigbee inventory entries are excluded from that count. Real
native light toggle, APS-acknowledged readback and restore passed. Group delivery
returned success after settling; broadcasts obtained actual descriptor responses
but relay quorum/CCA verdicts remained intermittent. Retain this limitation.
User waived sleepy-child stimulation: physically unverified, not a claimed pass.
Summed top CPU was native 0–13%, Ziggurat 0–1%; RSS about 3600/2012 KiB and memory
stable. Security checkpoint state changes approximately once/minute. Native
SIGTERM completed, complete frozen snapshot and offline rollback preparation
passed, and manual native restart returned online with 23 inventory entries.


**Observation details 2026-10-03:** real native light toggle/readback/restore passed
with 35–38 ms APS acknowledgements. A later group read returned delivery success;
broadcast descriptor reads reached the router but relay quorum/CCA verdicts were
intermittent. The user shortened the window to 15 minutes; it has completed. The user said no sleepy device
can be stimulated now and authorized assuming that check; retain it as physically
unverified. See [hardware findings and measurements](real-radio-migration.md).
The inventory contains 17 Zigbee plus six non-Zigbee entries; count radio reports
separately. Memory/RSS is stable, and high top `%MEM` reflects virtual allocation.


Keep joining and security rotation closed. Test one already-paired light with
an explicit reversible command, then restore its previous state. Verify real
APS success rather than HTTP admission alone. Check group/broadcast behavior,
existing routers, temperature/humidity/contact reports and a naturally waking
sleepy child. Preserve existing pairings; do not re-pair to hide failures.

Observe for at least 15 minutes of active traffic (user reduced the window on
2026-10-03). The user waived physical sleepy-child verification for this cutover;
retain it as unverified rather than extending the window until a remote is pressed.
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

**Completed 2026-10-03 with a startup latency limit:** permanent paired launcher, private configuration/state and pinned binaries installed under `/tuya/native`. Supervised startup, quiesce, complete freeze/export and clean UART release passed. Controlled `/bin/reboot` automatically started the pair (supervisor 126, Ziggurat 134, native 149); HTTP/HTTPS returned online/formed with 23 inventory entries and real existing-light descriptor, bind, reporting and read-attribute replies, plus a humidity report. Native logged readiness 121 seconds after its start, following lifecycle/hello timeouts; the initial host deadline expired before verification. No manual restart was needed. Cold-boot Configure blocking remains a follow-up investigation, with RNG readiness suspected but unproven. Device binary MD5s match the installed artifacts. Free RAM after reboot was 12844 KiB, cached 8136 KiB. Step 12 must save the newest frozen state before closure.


**In progress 2026-10-03:** fresh pre-install frozen export saved, staged
processes stopped, UART release verified. Original launcher/binary/config retained
as `.ezsp-backup` files plus immutable host archives. Verified binaries/config
moved atomically into `/tuya/native`; paired launcher installed. Supervised startup
returned online with 23 inventory entries. Next test quiesce/freeze/stop and reboot.


Prepare an atomic launcher update retaining the original `/tuya/tuya_start.sh`
and v13-compatible binary/config privately. The prepared `ziggurat_start.sh` uses the paired supervisor. The old `native_start.sh` only
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
then a controlled gateway reboot (`gateway_ssh /bin/reboot`; this device has
no `/sbin/reboot`). Preserve the host's latest private snapshot
before reboot; `/tmp` sockets/logs are ephemeral, network state is not.

**Pass:** startup order, reboot, service health, persistence and maintenance stop
work. **Rollback:** quiesce the new supervisor; freeze/export if possible; perform
orderly NCP return; atomically restore the saved EZSP launcher/binary/config and
resume it. Restoring a launcher alone does not undo a radio migration.

## 12. Close the Zigbee deployment, then add Thread/Matter separately

**Completed 2026-10-03:** permanent supervisor quiesced; latest complete frozen stack and native persisted state archived privately. Network/security identity and nonregressing NWK/APS counters passed rollback preparation; the advanced standard NCP recovery backup passed offline validation. Pair stopped cleanly and a fresh permanent launcher returned online/formed with 23 inventory entries; HTTP and HTTPS passed. Final private artifact hashes/validation record saved, original network/application/service files and pinned NCP GBL retained. No reverse NCP restore was executed. Zigbee deployment is closed with the documented sleepy-child, intermittent broadcast, cold-boot latency and crash-recovery limits. Thread/Matter remains a separate future phase on this same gateway.


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
