# Plan: Rust Matter service runtime

## Current open work — 2026-10-05

Host service/native round trips and the paired supervisor failure matrix pass.
Current factory MIPS builds for both binaries pass. Target runtime/resource
evidence and report delivery from the installed daemon to a registered native
device remain open.

Next deliverable: Verify the complete paired launch on the gateway under plan 08,
including CPU, memory, startup/IPC deadlines, and Matter reports from a real
commissioned device. Continue host coverage for production-daemon report delivery.

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
| 7 | Partial | Current Matter-enabled native/Rust factory builds are static big-endian MIPS-I ELF32 files: 1,736,548-byte native bridge and 2,109,120-byte Rust service. Rust/OpenThread ABI flags are aligned (`4c02d8a`); host suites and process tests pass. Target execution, peak runtime resources and full production-daemon report delivery remain unverified. |
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
2. Exercise service failure, radio loss and supervisor retry exhaustion together;
   confirm bounded IPC latency and that Zigbee/native supervision continues.
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

1. [ ] Add integrated failure scenarios for service exit, Thread socket loss,
   coordinator restart and supervisor retry exhaustion. Verify the native
   bridge and Zigbee supervision continue and no second UART owner starts.
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
into a registered Matter endpoint remains a host-fixture gap; target startup,
CPU, RSS, deadlines, physical commissioning and RF behavior remain exclusively
in plan 08. No gateway deployment occurred.

Rust commit `f33d47c` additionally exercises the actual service binary across
synthetic RCP socket death and restart. It verifies stale IPC socket reclamation,
unchanged credentials, startup waiting without radio traffic, wrong-channel
rejection before radio use, and recovery after a fresh matching observation.
`python3 scripts/test-matter-service.py` passes. This closes the host service/
RCP restart checkpoint; active commissioning during RCP death, target startup
and resource deadlines, and physical acceptance remain open.

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
