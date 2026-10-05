# Plan: Rust Matter service runtime

## Gateway flash headroom observation — 2026-10-05

A read-only `df -k /tuya` check reported 12,160 KiB total, 8,628 KiB used,
3,532 KiB available (71% used). No files were uploaded or removed, and the
production service was not restarted. This is enough space for the current
2,162,372-byte Matter service as an additive file in principle, but does not
prove the full native/service/supervisor package can be staged on the current
partition: a test-target install also duplicates the 1.7 MiB native binary and
requires atomic temporary files. Check available space again immediately before
any staging; the current snapshot is not a reservation. No CPU/RSS or runtime
deadline measurement was made.

## Composed daemon-to-native report rerun — 2026-10-05

`python3 scripts/test_matter_native_report_integration.py` passed outside the
sandbox. Its first stage received real encrypted CASE sensor `ReportData` in
the production Manager fixture and verified all five values through native
status/history. Its second stage ran the separate host `thread-matter` test
service beside the native host gateway; the daemon's test-seeded Manager queue
published all five values through its IPC loop into the same device/history
path. The native report-before-snapshot buffering and online reconciliation
were exercised in the first stage.

This confirms the process boundary and daemon publisher path. The daemon's
Manager queue was seeded by a host-only test hook, so this is not evidence that
the separate daemon itself received CASE `ReportData`. No MIPS service or
gateway workload was run. The remaining runtime deployment gate is to drive
the daemon's own CASE receive loop from a separate synthetic accessory; physical
acceptance remains excluded in plan 08.

## Rust service test storage checkpoint — 2026-10-05

Compilation repository commit `48a5dbc` disables Cargo incremental artifacts
for the pinned Rust project and directs its default target directory to ignored
`build/rs-matter/target/`. The service-library test command is now explicit in
`ziggurat-compilation/docs/matter-sensors.md`. This addresses host disk growth
during the controller-service test workflow; it changes no service runtime or
deployment behavior.

Validation passed with `cargo +nightly-2026-10-02 test --locked
--manifest-path rs-matter-probe/Cargo.toml --features controller-service
--lib`: 81/81 tests. The sandbox attempt could not create Unix sockets
(`EPERM`); the same command passed outside the sandbox. Step 7 remains partial:
the installed daemon's own active CASE report path and device/target acceptance
remain open.

## Synthetic ICD gateway resource profile — 2026-10-05

Rust commit `6a5c430` implements the receive path measured below.

The sessionless LIT Check-In path is now exercised end to end by the guarded
MIPS commissioning fixture. The gateway reached the synthetic accessory over
OpenThread, registered an ICD client, authenticated a Check-In and completed a
CASE read within the wake window. This profile uses a direct fixture resolver;
it does not test real Thread radio access or gateway SRP discovery.

The 2,666,212-byte MIPS fixture (SHA-256
`e6515d61d398c45a50c35195cc12d117bb3bac28f56d55a4808a0dfe79ad041f`)
completed commissioning in 12,530 ms. Its sampled peak RSS was 2,172 KiB
against a 3,072 KiB cap; sampled CPU averaged 31.2% of one core during the active
run. Minimum `MemFree` was 9,792 KiB and minimum `MemFree` plus clean inactive
file cache was 19,152 KiB. The production gateway stayed online and formed
before and after the run; uptime advanced and no reset was observed. The
production MIPS service independently cross-builds to 2,162,372 bytes
(`008581db1c45cec74e962a37e681f502b84cac9fdd42a49cd7604a730acc1370`), and
81/81 controller-service library tests pass.

Reproduce the bounded target profile from `ziggurat-compilation`:

```sh
bash scripts/build-rs-matter-transport.sh mips commissioning
python3 scripts/test-openthread-matter-device.py \
  --host 192.168.178.62 --password-file ../ssh_password.txt \
  --commissioning --ble --service-owner --icd
```

The test drops cache only for its uniquely staged fixture files, enforces an
8 MiB virtual-memory limit, 3 MiB RSS ceiling and 160-second wall deadline,
and checks production status before and after. A separate attempt including
synthetic SRP discovery timed out before ICD registration; the passing profile
therefore uses direct fixture address injection. These measurements represent
one registered client, not the maximum 64-key authentication scan or the
installed daemon's ongoing CASE report loop. No deployment or production
service restart was performed.

## Current open work — 2026-10-05

Host service/native round trips, the generic paired supervisor failure matrix,
and attached-daemon-to-native report delivery pass. A synthetic gateway MIPS
profile now also runs the production owned commissioning/Manager workload
through subscribed CASE sensor reports. A composed test now makes
the actual Matter service fail on Spinel, verifies recovery and dataset
restoration, then kills that recovered service to exhaust the retry budget;
the supervisor and native/Zigbee services remain running and native Matter
status returns 503. The daemon report fixture seeds decoded reports; its live
CASE subscription path remains open. The actual Matter-enabled gateway worker
now reconnects to a supervisor-restarted Rust service after a wire-level
Spinel error and sees the restored dataset.
Current factory MIPS builds for both binaries pass. Target runtime/resource
evidence is partial: a prior isolated service profile was followed by a reboot.
Later idle observations caught another uptime reset, without a target workload
running. In a user-authorized 13-minute reset-frequency observation, one more
reboot occurred during idle cooldown after a clean Matter startup-only profile;
the profile had no reboot across its own before/after uptime samples. A separate
retry passed with 788 KiB RSS, zero sampled CPU ticks, and orderly shutdown.
The production service came back online and formed about 135 seconds after the
reset. `dmesg` contained no watchdog/reset reason, and no conventional Linux
watchdog feeder was found. The reset source and long-term cadence remain
unknown. Two bounded target profiles preserved production Ziggurat and had no
reset: the commissioning/Manager procedure test and an isolated startup-only
run of the actual `thread-matter` MIPS service. Startup-only idle evidence is
796 KiB RSS with zero CPU-tick growth for 10 seconds; it never initialized OT
because no coordinator observation was supplied.

A fresh read-only gateway check on 2026-10-05 observed `/proc/uptime` advancing
from 3856.53 to 3867.77 seconds across the check. The current `dmesg` contains
the boot log only; targeted scans returned no watchdog, panic, OOM, reset,
brownout, thermal, or killed-process lines. The production native bridge and
Ziggurat processes were present. This confirms no reset during this short
sample, but does not explain earlier reboots or establish a longer-term reset
rate. The gateway process list had no `thread-matter` process during this
observation, so it is not evidence of a live Matter service workload.

The same read-only inspection confirmed the gateway is not running the Matter
service: `/tuya/native/thread-matter` is absent, and the deployed
`/tuya/native/paired_start.sh` has no `MATTER_BIN` handling. The checked-in
`ziggurat_start.sh` opts in only when that executable exists, and the current
source supervisor then starts it when the Thread socket is ready. Thus host
and MIPS implementation evidence does not describe the gateway's active
deployment; Matter use requires a separately reviewed staging/launcher update
and service start. No files were changed during this initial inspection.

Deployment staging was then verified without activation. `deploy.sh` copied
the Matter-enabled native binary (1,741,444 bytes), MIPS service (2,129,600
bytes), manifest, and Matter-aware `paired_start.sh` to `/tuya/native-test`.
Device MD5 sums matched the local native and service artifacts, and the
manifest identified `ziggurat=1`, `matter=1` with the expected native SHA-256.
The staging temporarily left 1,508 KiB free (88% partition use). The four
staged files and test directory were removed after verification, and `/tuya`
then reported 3,512 KiB free (71% use). No staged process was started; the
production launcher, Ziggurat, and native bridge remained running. A
post-cleanup idle memory sample
reported 5,432 KiB `MemFree` and 14,364 KiB `Inactive(file)` (19,796 KiB
combined reclaimable estimate); it does not measure Matter daemon usage. CPU
and runtime deadlines were not sampled because no target workload ran.

Latest service-resource checkpoint (`8f69c39`): the production MIPS-I service is 2,154,180
bytes (SHA-256
`4eaf5b551bafa2c3d1382f26b730b61b567e9b4121a5a1f9ca65e46e5b24cfdd`). The
controller-service library suite passes 80/80, and the MIPS-I production
cross-build succeeds. The additional ICD feature-detection, timeout and retry
logic is not represented in the prior gateway profile: no new on-device
profile was run because the synthetic LIT fixture still fails to deliver an
authenticated Check-In. The existing isolated 15-second synthetic-RCP profile
verified startup, Thread dataset
creation, IPC status and orderly shutdown while production Zigbee/Ziggurat
remained online. Startup after IPC socket creation was 1.09 seconds; peak RSS
was 1,492 KiB, sampled CPU ticks did not increase, minimum MemFree was 2,564
KiB, and minimum MemFree plus clean inactive-file cache was 18,968 KiB. Gateway
uptime advanced by 47.58 seconds and no reset was detected. This profile had
no commissioned Matter accessory and did not exercise ICD RegisterClient,
Check-In cryptography or sleepy-node job dispatch; those costs still require a
representative accessory workload.

Next deliverable: run the installed `thread-matter` daemon through attached
Thread startup and its own active CASE report loop into native state/history,
then measure those target deadlines/resource use. Exercise the new ICD path with
a synthetic commissioned LIT accessory, including registration and Check-In,
before treating the resource profile as representative. Continue bounded
reset-frequency observations as authorized, keeping production running. The
one silent isolated-service launch failure did not recur on the bounded retry.
Full physical paired launch remains in excluded plan 08.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Run the production Matter controller as a separate MIPS process and make the
native status routes report its actual state over `/tmp/microhad-matter.sock`.

## Steps

1. Add a long-running service entry point in `ziggurat-compilation` that owns
   the OpenThread instance, Matter `Matter` state, private credential store,
   and IPC listener on one event-loop thread.
2. Integrate service startup, supervision, restart behavior, and shutdown with
   the gateway's existing launcher. The service must never open the Zigbee UART
   or terminate the production coordinator when it fails.
3. Create the Unix socket with mode `0600`; refuse unsafe pre-existing paths,
   remove only a stale socket owned by the service user, and remove the socket
   on orderly shutdown. Keep the socket path aligned with the native client.
4. Implement bounded accept/read/write handling with the existing version-1
   frame decoder. Cap active clients and queues, reject malformed requests, and
   close idle or failed peers without logging payloads.
5. Implement status opcodes 1 and 2 with JSON objects that describe controller
   availability and Thread attachment without including datasets, keys, setup
   codes, or other credentials.
6. Integrate IPC polling with the OpenThread and Matter task pumps so requests
   remain responsive while timers and UDP traffic progress. Define startup,
   radio-disconnect, retry, and shutdown states explicitly.
7. Add host tests for socket permissions, fragmented/coalesced frames, stale
   peers, overload, bounded response time, and clean shutdown. Build the
   service for the factory target and verify the ELF and package size.
8. Document invocation, socket ownership, logs, state paths, resource limits,
   and recovery behavior. Keep deployment separate from the production service
   until the later device-acceptance plan passes.

## Acceptance

- A host service answers both status opcodes with valid bounded JSON.
- The factory MIPS service starts and stops without touching `/dev/ttyS1`.
- The native enabled status routes return the service snapshot when connected
  and 503 when it is absent or malformed.
- Socket permissions are owner-only and no credential payload reaches logs.

## Status quo — 2026-10-04

| Step | Status | Evidence / remaining work |
| --- | --- | --- |
| 1 | Complete | `thread-matter` owns Matter, OpenThread, private store and IPC in one event loop; host synthetic-RCP startup/status/shutdown passed. |
| 2 | Complete | Optional `MATTER_BIN` child in `paired_start.sh`, independent bounded retries and stop-before-native/radio ordering pass synthetic tests. The paired launcher now selects the staged service explicitly; `deploy.sh --activate-paired` provides a separate next-boot path and preserves the existing launcher backup. Mocked deployment tests pass. No gateway activation or target execution was performed; those remain in plan 08. |
| 3 | Complete | Mode 0600, flock duplicate exclusion, nonblocking stale socket probe, unsafe path refusal and inode-specific orderly cleanup; host checks passed. |
| 4 | Complete | Four clients, four accepts per turn, 4096-byte read/write budgets, eight queued replies, two-second idle expiry; malformed/overload/idle checks passed. |
| 5 | Complete | Empty requests for opcodes 1/2 return bounded JSON; synthetic host reports `running` with its persistent fabric and Thread `detached`, without credentials. |
| 6 | Partial | Matter/Thread/IPC pumps and SIGTERM/SIGINT shutdown implemented. Supervisor retries failures with backoff and stops/restarts Matter on coordinator loss; complete dataset/radio reconnection remains in plan 06. |
| 7 | Partial | Current native/Rust production artifacts cross-build as static big-endian MIPS-I ELF32; the service is 2,150,084 bytes (`bd1d79d`). Rust/OpenThread ABI flags are aligned (`4c02d8a`), and production binaries exclude test-only report hooks. Service regression and Spinel-restart tests pass; the attached `Manager::poll()` to native IPC/device/history path passes with seeded report values. An isolated 15-second synthetic-RCP gateway profile passed startup, dataset creation and orderly shutdown at 1,492 KiB peak RSS, zero sampled CPU ticks, 2,564 KiB minimum MemFree and 18,968 KiB minimum MemFree plus clean inactive-file cache; production Zigbee remained online and uptime advanced without reset. It did not exercise CASE reports, LIT registration or Check-In. |
| 8 | Complete | Runtime invocation, ownership, bounds and state paths documented in `ziggurat-compilation/docs/matter-service-runtime.md`; optional supervisor configuration, failure markers, retry budget and staging documented in `MATTER_INTEGRATION.md`. |

This checkpoint supplies a running transport/status service and persistent
controller fabric. It does not yet attach a Thread network, commission an accessory,
or expose operational device commands. No deployment or physical radio test
was performed; plan 08 remains excluded.

Host socket tests require execution outside the workspace sandbox (otherwise
Unix sockets return EPERM). The factory link additionally required an `accept4`
shim for old uClibc, using `accept` and immediate descriptor flags in the
single-threaded process.

The deployment checkpoint initially exposed a blocker: `--matter-service`
staged the Rust executable and supervisor, but the paired launcher did not set
`MATTER_BIN`, and `--activate` selected the native-only launcher. The launcher
now opts into Matter when the staged executable exists, while preserving
Matter-disabled behavior when it is absent. `deploy.sh --activate-paired`
separately stages the Matter-aware launcher for next boot; it requires an
explicit Matter-enabled native binary and service path, is restricted to
`/tuya/native`, preserves the existing launcher backup and never restarts live
processes. The MIPS build emits a feature manifest containing the Ziggurat and
Matter selections plus the binary SHA-256; deployment rejects a missing,
disabled or stale manifest before SSH and stages the matching manifest beside
the executable. Mocked SSH tests verify staging and activation commands without
network access. No gateway connection or launcher activation was performed.

## Remaining checkpoints

1. Finish the channel-gated lifecycle in plan 06, then verify status while
   awaiting a coordinator, attaching, attached, disconnected and restarting.
   The guarded lifecycle synthetic service test now passes; native observation
   delivery and integrated radio-loss behavior remain pending.
2. The real service's wire-level Spinel apply failure now restarts under the
   paired supervisor on host while native/Zigbee stay running and the persisted
   dataset restores. Add radio-loss and retry-exhaustion composition and confirm
   bounded IPC latency.
3. Verify a native HTTP-to-service status round trip with valid, malformed,
   absent and stalled service peers. Record the current factory binary hash,
   size and ELF checks rather than reusing earlier build measurements.
4. Keep physical startup/resource measurements and launcher activation in plan
   08; distinguish synthetic evidence from target evidence. Before that gate,
   the deployment package and explicit paired-launcher path are host-verified.

## Dependencies

The existing bounded Rust/C++ frame format and status opcode assignments. The
Thread radio endpoint must be supplied by the Thread network lifecycle work
before claiming radio readiness.

Latest Thread lifecycle checkpoint: 36 Rust checks, synthetic service restart
and encrypted fixture regressions pass. Factory service size is 1,552,028 bytes.
Startup now reports `awaiting_coordinator` before a fresh private channel
observation, then `running`; prior measurements/states above are historical.

Native worker checkpoint: an actual enabled native-to-Rust IPC/HTTP round trip
now passes with a synthetic Ziggurat coordinator and Thread RCP. Native online
channel observations authorize guarded startup and dataset creation, radio loss
produces `awaiting_coordinator`, and service loss clears cached HTTP status to
503. Target execution and broader restart/failure recovery remain pending.

The owned-commissioner checkpoint connects the session task, BTP/UDP transport
and commissioning IPC/events. 42 service-feature checks and the actual running
service lifetime harness, including a real 30-second timeout, pass. Current
factory service is 1,740,456 stripped bytes; target execution remains unverified.
Native event consumption, production discovery and device jobs remain open.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Add integrated failure scenarios for service exit, Thread socket loss,
   coordinator restart and supervisor retry exhaustion. `python3
   scripts/test_matter_spinel_supervisor.py --native-worker --exhaust-retries`
   passed against the real Rust service and native worker on a synthetic RCP:
   Spinel failure restarted Matter and restored its dataset; killing the
   recovered service exhausted the Matter-only budget, returned 503 for Matter
   status, and left native HTTP online and the Zigbee/native pair running. No
   second UART owner started. This is host evidence, not target acceptance.
2. [ ] Define recovery for every startup/radio-loss state and verify bounded
   reconnect/backoff with plan 06. Test IPC responsiveness during recovery.
3. [ ] Repeat the enabled native HTTP-to-service test after persistent IPC is
   added in plan 02; include absent, malformed and stalled peers.
4. [ ] Record reproducible host/factory build commands, hashes, ELF properties
   and artifact sizes for the final implementation. Keep host timings separate
   from target CPU/RSS measurements.
5. [ ] Hand target startup, shutdown and resource verification to plan 08 with
   a concrete test manifest. Close this plan only after its acceptance evidence
   is recorded; a cross-build alone does not verify target execution.

Validation: synthetic supervisor/service/native integration tests and factory
cross-build; physical execution remains a separate acceptance checkpoint.

## SRP/DNS runtime artifact checkpoint — 2026-10-04

Rust commit `e59c3dc` adds the bounded userspace operational resolver to the
existing event loop without blocking IPC. OpenThread now includes SRP/DNS/ECDSA
and Network Data service support. Service-feature tests pass 54; actual-service
IPC and native real-service HTTP/WebSocket regressions pass alongside encrypted
SRP/DNS commissioning/fresh CASE. Both host/MIPS builds pass, with fixture
registration symbols absent from the service.

Latest stripped static big-endian MIPS-I/o32 soft-float artifacts: service
1,924,788 bytes, probe 2,264,780 bytes. Hashes and verification scope are recorded
in `ziggurat-compilation/docs/matter-operational-discovery-validation.json`.
This is a current artifact manifest, not target resource/performance acceptance.
Broader integrated failure/restart work remains open; no physical execution or
plan 08 work occurred.

## Native worker and paired supervisor recovery checkpoint — 2026-10-04

`python3 scripts/test_matter_native_worker.py --real-service` passed against
the actual `thread-matter` process and native Matter worker. It verified native
HTTP status, Thread observation and dataset lifecycle requests, coordinator
loss transitioning the service to `awaiting_coordinator`, continued native
availability, and 503 responses after the service stopped.

`python3 scripts/test_paired_supervisor.py` passed all 10 tests with local Unix
sockets enabled. This covers independent Matter retry exhaustion without
stopping Zigbee/native, radio failure stopping and restarting the Matter/native/
Zigbee pair in order, native crash retry bounds without a second radio owner,
quiesce ordering, missing Thread endpoint behavior, stale socket handling,
duplicate supervisor refusal and unsafe stop refusal. The default sandbox run
was not valid evidence because it denies Unix sockets; the elevated host run
passed in 66.7 seconds.

This closes the planned host failure/restart matrix for the paired supervisor
and real-service/native lifecycle checkpoint. Production daemon report delivery
into a registered Matter endpoint now passes for attached `Manager::poll()`
events seeded after decoding; wire-decoded CASE reports inside the daemon remain
open. Target startup,
CPU, RSS, deadlines, physical commissioning and RF behavior remain exclusively
in plan 08. No gateway deployment occurred.

Rust commit `f33d47c` additionally exercises the actual service binary across
synthetic RCP socket death and restart. It verifies stale IPC socket reclamation,
unchanged credentials, startup waiting without radio traffic, wrong-channel
rejection before radio use, and recovery after a fresh matching observation.
`python3 scripts/test-matter-service.py` passes. This closes the host service/
RCP restart checkpoint; active commissioning during RCP death, target startup
and resource deadlines, and physical acceptance remain open.

## Real-service retry exhaustion checkpoint — 2026-10-05

`python3 scripts/test_matter_spinel_supervisor.py --native-worker
--exhaust-retries` passed. The first Matter process failed after a synthetic
RCP Spinel dataset rejection; the paired supervisor restarted it, and the real
Rust service restored its persisted dataset. The harness then killed that
recovered service. With the configured two-launch budget exhausted, no third
launch occurred and `control/matter.failed` was created. The paired supervisor
stayed alive, the native `/api/status` remained online, `/api/matter/status`
returned 503, and event records showed one Zigbee and one native start with no
native stop. This closes the host retry-exhaustion composition; target CPU,
memory, deadlines, startup and reset-source acceptance remain open.

## Current Matter factory cross-build checkpoint — 2026-10-05

The Matter-enabled native bridge builds with
`make ZIGGURAT=1 MATTER=1 mips`; the Rust service builds with
`source scripts/rust-env.sh && CARGO_TARGET_DIR="$PWD/build/rs-matter/target"
bash scripts/build-rs-matter-transport.sh mips service`. The native source was
at `eb451f8`; the Rust source was at `980db77`. Both artifacts are stripped,
static, 32-bit big-endian MIPS-I executables with no `PT_INTERP`: native
1,736,548 bytes (SHA-256
`258fc92c7bb5b588bdef5fce01588ac2b93064dd6fa6c712a5a8d500450a9eee`), service
2,109,120 bytes (SHA-256
`d7e030a815faef3ff56876a375d4419f700a14c93c9f047a80d1f5aae6f66a1c`). The
combined executable payload is 3,845,668 bytes, excluding configuration,
state, runtime files and concurrent-process overhead.

The build emits existing RSDK LOAD-USE and experimental MIPS-I warnings. The
Rust/OpenThread objects now use matching abicalls flags after `4c02d8a`; the
mixed-ABI link warnings are gone. The old linker still reports its unsupported
`-no-pie` option, while `readelf` confirms an ET_EXEC with no dynamic section
or interpreter. Current Rust artifact SHA-256 is
`1ff121679781bfaa59caa99486f54d6ce5c76e0b52f2671c88eddddb866d7a28` at the
same 2,109,120-byte size. `file`, `readelf -h -l`, file sizes and hashes were
checked. Cross-builds establish target code generation and package size only;
they do not establish executable startup, CPU use, peak RSS, scheduling
deadlines, coordinator coexistence or Matter accessory behavior. Those
measurements remain in plan 08.

## Rebuilt MIPS deployment candidates — 2026-10-05

Verified the Matter-enabled native bridge using `make ZIGGURAT=1 MATTER=1
mips`; the output is `build/mips-ziggurat-matter/microhad-zigbee-native`, 1,736,548
bytes, SHA-256
`1f15345fc12125e2c173f9cc82c4a9a9df6fecd7ad3f5ff50bb20b2009184b47`.
Rebuilt the Rust service from `1b7ae98` using
`bash scripts/build-rs-matter-transport.sh mips service`; the output is
`build/rs-matter/thread-matter-mips`, 2,109,120 bytes, SHA-256
`dcd10f1687d5cae0a1633fe9a04398bb0326d28feb0a2d5d3a19a5458c63c675`.
Both are static ELF32 big-endian MIPS-I/o32 executables. `readelf` reports no
interpreter or dynamic section for the Rust service. This updates the older
artifact record above; hashes identify these exact local candidates and are
not a claim that either has run on the gateway. The earlier 1,504,596-byte
artifact was the Matter-disabled Ziggurat binary and is not suitable for this
paired deployment. Staging requires packaging these exact Matter-enabled files.
Matter use and service cutover still require the device startup/resource gate
and completed production report delivery. The service dispatcher now accepts
MoveToLevel through the manager; the composed test verifies bounded
not-ready handling when the fixture has no attached accessory.

## Authorized gateway simulation preflight — 2026-10-05

The host attempted `python3 scripts/test-openthread-device.py --host
192.168.178.62 --password-file ../ssh_password.txt --profile
--profile-load-seconds 60` using the synthetic RCP path. The harness refused to
stage or launch its test workload because MemFree was 2,980 KiB, below its
3,072 KiB preflight requirement. It removed its unique temporary paths and
confirmed production remained `online=true`, `network_formed=true`, and on the
Ziggurat backend. A later read-only sample showed 2,960 KiB free. No CPU/RSS or
deadline measurements were taken in this attempt. The later retry used the
separate constrained-profile guardrails below and kept the production service
running.

## Constrained gateway OpenThread profile — 2026-10-05

Rust harness commit `1a75661` adds an explicit constrained mode without
changing the standard profile guard. The retry used that mode after MemFree
reached 3,004 KiB. The MIPS OpenThread CLI ran for 100 seconds against the
synthetic RCP while the existing production service remained online and formed.
It completed 1,166
cycles (19.42/s); loaded CPU averaged 11.80% of one core, sampled CLI RSS was
748–808 KiB, and sampled system MemFree bottomed at 1,544 KiB against a
1,536 KiB abort guard. Median/p95/p99/max cycle latency was
25.58/52.42/86.71/168.75 ms. Cleanup and before/after production status passed.

This is OpenThread CLI plus synthetic UDP/RCP traffic, not the Matter service,
CASE, a real radio, or combined Zigbee/Matter load. The 8 KiB sampled margin
over the free-memory guard is narrow, and one-second sampling may miss peaks.
Full Matter-service CPU/RSS/deadline feasibility remains unverified; see the
[profile record](../../ziggurat-compilation/docs/openthread-resource-profile.md).

## Guarded MIPS service startup — 2026-10-05

Rust commit `f809024` adds a bounded startup sampler. Using unique volatile
state/IPC paths and a private radio URL, the factory MIPS service logged startup
and shutdown and remained alive for 15 seconds waiting for coordinator
observation; it did not initialize OpenThread, commission a device, or access
the production UART. Sampled service RSS was 788 KiB and CPU ticks did not
increase during the sampling window. Raw `MemFree` was 928–944 KiB; clean
inactive file cache was 10,484 KiB, yielding 11,412–11,428 KiB effective free.
The test-only raw-free abort floor was 512 KiB, while the effective-free floor
was 1,536 KiB. The deployment gate still requires at least 1 MiB raw `MemFree`,
so the profile fails that gate despite the reclaimable cache.

The legacy SSH channel closed after the service's orderly stop marker, and the
initial harness did not save a JSON report; its error path now retains samples.
At the next gateway check, uptime had reset, service PIDs had changed, and
status was temporarily offline/unformed. The existing supervisor returned it
to online/formed within the next four-second poll. The post-reset kernel log
does not retain the prior boot's cause, so this may be related to the profile
but cannot be attributed to it or classified as OOM. No further target workload
should run until the reset risk and memory headroom are addressed. The existing
service was never intentionally stopped; all unique test files were removed.

## Gateway memory accounting update — 2026-10-05

Rust harness commit `eb04f96` now counts clean inactive file cache alongside
`MemFree` for effective-availability preflight and runtime guards. It reports
both separately, subtracts dirty/writeback/shared pages conservatively, and
retains an independent 1 MiB raw-`MemFree` floor. The earlier 100-second
OpenThread profile used the stricter raw-`MemFree` guard and remains valid as
historical evidence; it has not been reclassified under the new formula.

A later read-only sample showed 2,932 KiB `MemFree`, 8,768 KiB inactive file
cache, no dirty/writeback/shared pages, and no swap. Process resident samples
were 2,756 KiB for the native bridge and 2,044 KiB for Ziggurat; kernel slab was
4,340 KiB, of which 3,980 KiB was unreclaimable. This explains why raw free RAM
looks low while clean inactive cache remains reclaimable. It is one snapshot,
not Matter-service headroom evidence; full Matter-service CPU, RSS and deadline
measurements remain open.

## Keep report fixture hooks out of production artifacts — 2026-10-05

Rust commit `6e44f33` adds a build-time binary-marker check for the
`MICROHAD_MATTER_TEST_SEED_SENSOR_REPORT_NODE` injection hook. The dedicated host
test-service build must contain the marker; host production and stripped MIPS
production builds must not. All three artifact checks passed. The dedicated
test-service build passed, both production service build paths passed, and
`python3 scripts/test-matter-service.py` passed its commissioner, Thread
lifecycle, and Spinel-failure recovery checks. The current MIPS service remains
2,109,120 bytes. This closes the test-fixture/production artifact boundary; it
does not close the live CASE-report path or the gateway reset investigation.

## Repeated reset and watchdog investigation — 2026-10-05

After a prior read-only observation of about 3,247 seconds uptime, a later
pre-profile check found only 390.26 seconds. Subsequent idle monitoring found
one more confirmed backward uptime step: 722.42 seconds, then 252.93 seconds
about a minute later. This occurred when no target Matter workload was running.
Other readings of 285.50 and 411.18 seconds were separated by uncertain elapsed
time and do not establish another reset. A later 10-second device sleep
increased uptime by 10.04 seconds, calibrating the counter. During the
bounded repeat profile uptime advanced from 390.26 to 410.11 seconds over
17.75 seconds of host time, `/api/status` stayed online/formed, and no reset
occurred during that run. The profile itself exited before its first resource
sample and emitted no service log. A five-second retry passed with five samples,
788 KiB peak RSS, zero CPU-tick growth, 11,508 KiB minimum raw `MemFree`, and
19,884 KiB minimum effective free. Both profiles preserved production Zigbee
and showed no reboot; the initial silent launch failure did not reproduce.

The running image exposes no `/dev/watchdog*` or `/sys/class/watchdog`, no
watchdog module, process, or init feeder was found, and the current boot's
kernel log has no watchdog initialization or reset message. The old init script
and `/etc`/`/tuya` searches likewise showed no feeder. `/proc/watchdog_reboot`
does exist, but reading it blocked the SSH session; it was not read again. The
custom RTL8196E watchdog sources in the local firmware tree are not evidence
that this driver is present in the running image. There is currently no
identified watchdog control to disable safely. The cause may be a vendor
hardware watchdog or another reset source; the current boot has no persistent
prior-boot log, so attribution remains open. The production supervisor restored
Zigbee after a reset and the API reported online/formed. Keep the service up and
do not run more Matter load profiles until the reset source is diagnosed.

The RTL8196E datasheet documents a hardware watchdog reset indicator at
`WDTCNR` (`0xB800311C`), independent of whether Linux exposes `/dev/watchdog`.
The target has no `/dev/mem` or `devmem` utility, and `/proc/watchdog_reboot`
reads blocked SSH, so the reset indicator could not be inspected safely. A
latest read-only observation found 3,173 seconds uptime and healthy Zigbee; a
later ten-second interval advanced uptime from 3,198.43 to 3,208.47 seconds.
This rules out a reset during that check but does not resolve the earlier
resets. There is no known safe way to
disable the hardware watchdog from the current image. See the [resource-profile
record](../../ziggurat-compilation/docs/openthread-resource-profile.md) for the
observation and datasheet reference.

A read-only live check at 2026-10-05 10:03 UTC found uptime advancing from
1,990.12 to 2,001.07 seconds during the SSH checks, with the paired supervisor,
Ziggurat and native daemon still running. The current `dmesg` ring buffer shows
kernel and board initialization messages but no reset reason or watchdog
initialization. Its final vendor messages read and write `0xb8000038`; that is
not the documented watchdog control register, and this observation does not
identify it as related to resets. This confirms only that no reset occurred
during this brief read-only check; the reset cause remains undetermined.

## Constrained synthetic-RCP repeat profile — 2026-10-05

After the DNS-SD legacy migration MIPS cross-build, the authorized constrained
profile ran an OpenThread CLI against a synthetic RCP over an isolated host
relay. It did not open the production UART, restart any service, or issue radio
reset commands. The production status API reported online/formed on Ziggurat
before and after. The full 45-second run used 20 seconds disabled idle, five
seconds of load, and 20 seconds enabled idle; it completed 98 cycles at 19.57/s.

Sampled CLI RSS peaked at 808 KiB. Raw MemFree reached a minimum of 12,696 KiB
and effective free memory (including clean inactive file cache) 19,608 KiB.
Loaded CPU averaged about 10.8% of one core; p95 cycle latency was 54.20 ms
and maximum was 76.60 ms. There were 39 resource samples. Uptime increased
through the profile and to 5,109.76 seconds after it; current-boot `dmesg` had
no watchdog, reset, panic or brownout matches. This did not reproduce a reset.

This measures only the OpenThread CLI's synthetic RCP/UDP path; it does not
measure the Rust Matter service, CASE, DNS Browse callback, real RCP or combined
Zigbee/Thread load. The existing service remained healthy, and cleanup removed
temporary binaries/settings/socket. See the detailed
[resource profile](../../ziggurat-compilation/docs/openthread-resource-profile.md)
and [machine-readable samples](../../ziggurat-compilation/docs/openthread-resource-profile.json).

## MIPS CASE sensor-report workload — 2026-10-05

The Matter commissioning MIPS probe was rebuilt from current source and run
with `--service-owner --sensor-reports` against a synthetic accessory. Production
Ziggurat remained `online=true`, `network_formed=true` before and after; the
UART was never opened. Encrypted commissioning, production Manager discovery,
reads/commands, seven CASE subscriptions and five sensor reports completed in
10.94 seconds. The 2,580,192-byte probe reached 2,204 KiB RSS; minimum raw
MemFree was 10,080 KiB, and minimum free plus clean inactive file cache was
19,216 KiB. Sampled process CPU averaged about 37.5% of one core over the
11.75-second sampled interval. It stayed within the 3,072 KiB RSS, 512 KiB raw
MemFree and 4,096 KiB effective-free limits, cleaned temporary artifacts and
did not reset the gateway. The machine-readable report is
`ziggurat-compilation/build/rs-matter/openthread-ble-commissioning-device-validation.json`.

The attempted `--service-discovery` variant timed out at its 45-second synthetic
SRP-registration deadline because the device-side RCP simulator does not
provide an SRP registrar. It peaked at 1,772 KiB RSS and left production
online; no sensor-report results are inferred from it. Host SRP/DNS-SD
integration passes separately. The successful MIPS run does not include the
installed `thread-matter` daemon, native worker on target, physical radio, or a
real accessory.

## Installed service startup-only gateway profile — 2026-10-05

The actual 2,129,600-byte `thread-matter-mips` executable ran with unique
private state, IPC and an unused radio socket while Ziggurat remained
online/formed. The harness waited for a coordinator observation, so the service
did not initialize OpenThread or touch a radio. It created the IPC socket and
logged `Matter controller service started` 1.07 seconds later, then handled
SIGTERM with the clean stop marker. Peak RSS was 796 KiB, CPU ticks stayed flat
over 10 seconds, minimum raw MemFree was 10,896 KiB and minimum effective free
was 19,620 KiB. Gateway uptime advanced 34.89 seconds over transfer/profile;
production status was unchanged and all isolated paths were removed. The
machine report is
`ziggurat-compilation/logs/matter-service-device-profile.json`.

This verifies MIPS process startup and shutdown only. Active coordinator attach,
CASE subscriptions inside the installed daemon, native-worker coexistence on
the gateway, radio deadlines and real device interaction remain unverified.

## Installed service active Thread lifecycle on the gateway — 2026-10-05

The user-authorized `--active-thread-simulation` harness now runs the installed
2,129,600-byte MIPS daemon against a private synthetic Spinel RCP and sends
coordinator observation, dataset-create and status requests over its actual
Unix IPC socket. The latest run completed cleanly: the service created a
channel-15 dataset and reported `configured=true`, `state=attaching`, and
`attached=false`. It reached the service-start log 0.05 seconds after socket
creation, sampled 1,468 KiB peak RSS and one CPU tick, and left at least
11,516 KiB raw MemFree and 19,756 KiB MemFree plus clean inactive file cache.
The production Zigbee service stayed online/formed before and after. The
synthetic RCP has no peer mesh, so physical Thread attachment and the daemon's
CASE report loop remain open.

Three such active runs each passed their own before/after uptime check, but a
subsequent gateway check found a reboot and temporary Zigbee outage after each
run. In the latest case, uptime advanced to 206.05 seconds at test cleanup and
was 37.75 seconds at the next SSH sample. The paired supervisor recovered
Ziggurat online/formed within about 138–149 seconds. Current-boot kernel-log
checks showed no watchdog, panic or OOM matches. Read-only inspection found no
Linux watchdog device/sysfs entry, feeder process, or watchdog reference in the
active init/paired-start scripts; `/proc/watchdog_reboot` was not read because
it previously blocked SSH. No Tuya watchdog feeder or safe disable control is
known. The timing association merits investigation but does not prove these
Matter tests caused the reboots or establish their rate. Detailed evidence is
in `ziggurat-compilation/docs/openthread-resource-profile.md`. The user
explicitly authorized continued guarded tests after an earlier recommendation
to pause.

A later two-minute read-only window advanced uptime from 403.33 to 525.97
seconds over 12 samples while production remained online/formed. No reset
occurred in that window; it is too short to rule out the earlier behavior.

At 12:23 UTC, read-only checks found uptime advancing from 1,686.69 to
1,716.06 seconds, with production online and formed. Current-boot `dmesg`
showed normal initialization and no watchdog, reset, panic, OOM, brownout, or
reboot-cause message. Since the ring buffer contains only the current boot, it
does not reveal why earlier resets occurred. No reset occurred during this
brief check; the cause remains unknown. See the detailed
[resource-profile record](../../ziggurat-compilation/docs/openthread-resource-profile.md).

## Check-In responder filtering and debugger availability — 2026-10-05

Rust commit `d3092ac` changes the ICD responder to accept only Secure Channel
Check-In exchanges. The generic responder had accepted unrelated exchanges
and the handler then discarded them, which could consume CASE `ReportData`
before the subscription manager. The composed report integration now passes
with the filtered acceptor. The controller-service suite passes 81/81 and the
static MIPS-I service build passes at 2,158,276 bytes.

The updated service also passed a guarded 10-second gateway profile with a
private synthetic RCP, state and IPC. It created the private Thread dataset
and stopped cleanly; peak RSS was 1,508 KiB, sampled CPU ticks did not change,
minimum MemFree was 9,272 KiB and minimum MemFree plus clean inactive file cache
was 18,116 KiB. Uptime advanced 46.85 seconds, production stayed online/formed,
and no reboot was detected. The gateway has no `gdb` or `gdbserver`. The host
toolchain's Realtek MIPS GDB 7.6.2 is available, and the target core pattern is
`core`; a core could help with a userspace crash if process limits permit, but
it cannot explain an abrupt hardware reset. See the detailed runtime and
[resource-profile record](../../ziggurat-compilation/docs/openthread-resource-profile.md).
The live CASE report loop within the installed daemon remains a separate joined
process validation gate.
