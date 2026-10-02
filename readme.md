# Ziggurat on the Tuya RTL8196E gateway

This repository investigates compiling and running the open-source Rust
[Ziggurat](https://github.com/zigpy/ziggurat) Zigbee stack on the gateway's
big-endian Lexra CPU. The aim is a standalone host stack using OpenThread RCP
firmware on its EFR32MG1 radio, without Silicon Labs' precompiled host Zigbee
stack libraries.

The intended production client is **`microhad-zigbee-native`**, running on the
gateway and talking to Ziggurat's protocol-v2 API. Its Ziggurat adapter is
future work; the Python client in this experiment only exercises that API.
Direct Zigbee2MQTT integration is not the immediate client plan.

The ultimate goal is **dual-mode Zigbee and Matter-over-Thread**, aiming to
share the gateway's existing EFR32 radio while retaining Zigbee support through
Ziggurat and `microhad-zigbee-native`. Matter requires Thread connectivity and a
Matter controller/application layer; Ziggurat supplies the Zigbee stack.
The production goal is **fully standalone on the Tuya**: the Zigbee client,
Ziggurat, Thread border-router functionality and required Matter application
functionality must run locally. The development computer is only a build and
test tool. Combined memory/CPU demand remains an unvalidated feasibility gate.

Sharing an OpenThread RCP firmware image between alternative Zigbee and Thread
uses does not establish simultaneous operation. We must investigate multi-PAN
firmware, multiplexed radio access, reset ownership, channel constraints and
coexistence. The pinned gateway project's
[radio-mode guide](rtl8196e-gateway/docs/radio-options.md) describes experimental
same-channel Zigbee + Thread on this Series-1 radio through a separate CPC
multi-PAN path. That path uses `zigbeed`; Ziggurat is not a drop-in replacement.
The pinned Ziggurat Spinel client currently uses link/interface ID 0, so
simultaneous shared-radio operation will require further design and likely
code changes. Dual mode is a goal, not a capability validated by these tests.

The [radio firmware and dual-network audit](docs/radio-dual-mode.md), dated
2026-10-02, finds an alternative to CPC: GSDK 4.5.0 already implements
multi-instance Spinel over HDLC and multiple radio address filters. Reusing
that implementation is the preferred next firmware experiment; CPC is not
inherently necessary. The existing HDLC image is configured for one instance.
MG1 source-matching limitations and host ownership still need validation.
The firmware is partly open source and rebuildable from SDK source plus
precompiled RAIL libraries; it is not a completely source-available image.

The [HDLC implementation plan and progress](docs/hdlc-implementation-plan.md)
tracks firmware builds, shared-interface transport changes, simulation and
private GitHub publication. Hardware flashing remains a separate validation gate.

## Source projects

Sources are pinned Git submodules:

- `ziggurat/`: upstream Rust Zigbee stack and Spinel radio transport.
- `rtl8196e-gateway/`: modern Lexra GCC/binutils/musl toolchain, kernel patches,
  and board-specific OpenThread RCP firmware.
- `cpc-daemon/`: Silicon Labs CPCd v4.5.3, pinned for the stock-gateway CPC
  feasibility test below.

Clone this experiment with `git clone --recurse-submodules <repository-url>`.
For an existing checkout, run `git submodule update --init --recursive`.
Upstream sources remain separate from experiment code; any patches will be
recorded in this repository rather than silently changing a source checkout.

## What has already been established

- Silicon Labs' public `zigbeed` SDK depends on architecture-specific binary
  Zigbee stack libraries. No compatible MIPS host stack library was found.
- The referenced gateway project supplies GCC 15.2.0, binutils 2.45.1 and
  musl 1.2.6 patched for big-endian Lexra LX4380, O32 ABI and soft float.
- It supplies OpenThread RCP firmware for EFR32MG1B232F256GM48, including the
  Lidl board's PA0/PA1 UART and PA4/PA5 hardware flow-control wiring. Firmware
  compatibility with this particular device has not yet been tested.
- Read-only inspection of the development gateway at `192.168.178.62`
  reported RTL8196E, Linux 3.10.90, and 27,892 KiB usable RAM. The kernel was
  built with Realtek RSDK 4.6.4. These are observed values, not an assumption
  that the gateway runs the modern repository's kernel.
- Rust's normal `mips-unknown-linux-musl` target uses MIPS32r2. Lexra requires
  different code generation; pointing Cargo's linker at a new GCC is not
  sufficient because Rust normally generates instructions using LLVM.
- A small local Clang/LLVM 18 MIPS-I probe failed during code generation of
  an atomic increment (`Cannot select ... MipsISD::Sync`). This is evidence
  of an obstacle, not proof that every newer Rust/LLVM implementation fails.
- The modern kernel project documents emulation of `ll/sc` and `sync` on
  Lexra. The stock kernel passed the atomic-counter test below; emulation
  performance under a real Zigbee workload remains unmeasured.
- The inspected Ziggurat workspace requires Rust 1.98 and its host server
  uses Tokio. Its protocol core supports `no_std`. Its client protocol is
  different from EZSP; implementing the planned `microhad-zigbee-native` client
  adapter is separate work from compiling the stack.

## First experiment: Rust prototype

Before building Ziggurat, establish a usable Rust target through small tests:

1. Build and run a host baseline with allocation, threads, atomic counters,
   timers, operating-system randomness, and an I/O loop.
2. Try a custom big-endian, soft-float MIPS-I target and rebuild Rust's
   libraries. Inspect generated assembly before considering a device test.
3. Resolve compiler instruction selection, atomics, linking and kernel/libc
   compatibility. A GCC Rust backend is an alternative if LLVM cannot
   generate reliable Lexra code.
4. Only after the prototype works, build Ziggurat and measure its memory,
   CPU and radio performance.

The prototype does not open the coordinator UART, change network state, or
flash firmware. The user explicitly authorized harmless device testing, and
the runs described below used new paths in RAM-backed `/tmp`, leaving existing
files untouched. Radio firmware changes remain a separate, unauthorized step.

Toolchains, caches, generated target specifications and build logs live in
ignored directories. Credentials, device configuration and network keys must
never be copied into this repository.

## Reproduce the builds

The setup assumes an x86_64 Linux build host. Rust is installed entirely under
`tools/`; the installer does not modify the global PATH or shell configuration.

```sh
bash scripts/install-rust.sh
bash scripts/prototype.sh host
bash scripts/prototype.sh codegen
bash scripts/prototype.sh stock-compat
```

`rust-toolchain.toml` pins nightly-2026-10-02 (Rust 1.101.0-nightly, LLVM 23.1.1).
Nightly is needed for custom JSON targets and rebuilding `core`/`std`.
`scripts/make-llvm-target.sh` derives experimental target specifications from
that compiler rather than maintaining an outdated JSON schema by hand.

- `host`: run the full prototype on the development machine.
- `codegen`: rebuild `core` and compile arithmetic, little-endian decoding and
  an atomic increment as a MIPS-I library, with assembly output.
- `stock`: attempt the full static uClibc build without compatibility wrappers;
  this is a diagnostic baseline expected to fail at linking.
- `stock-compat`: build the full static prototype using the stock RSDK located
  in the parent directory, plus the compatibility wrappers below. This does
  not require building the modern GCC toolchain yet.

The stock target disables compiler-provided thread-local storage using
`has-thread-local: false`, selecting Rust's pthread-key implementation. The
experiment's C shim supplies missing `accept4` and `clock_nanosleep` wrappers
using the RSDK headers' syscall numbers, and `pthread_setname_np` for the current
thread using `prctl`. Errors are preserved; socket flags are passed atomically
to the kernel. The shim supports neither naming another thread nor a name
longer than 15 bytes. A diagnostic constructor reports the PC of SIGILL using
an async-signal-safe write and exits without a core dump. The same shim now
supplies `epoll_create1` and `eventfd` through the kernel syscalls for Tokio/Mio.

Output: `prototype/target/lexra-stock-uclibc/release/lexra-rust-prototype`.
The old linker rejects `-no-pie`; rustc automatically retries without it.
That warning does not prevent the successful static build.

## Prototype results — 2026-10-02

Pinned sources: Ziggurat `392d4c7fe5c497d553977492bc02cb305573b7fc` and
RTL8196E gateway `30eb5e5b9ee334d2c6d76812acd4a6cdf2923423`.

1. The host baseline passed all checks.
2. LLVM 23 compiled the MIPS-I library, including the atomic increment. Its
   assembly contains `ll/sc`; the LLVM 18 failure is not reproduced with this
   compiler. Unaligned decoding uses `lwl/lwr` with load-delay padding.
3. Rust's `std` compiled for the stock target. Linking initially failed on the
   three missing libc wrappers listed above; the compatibility shim fixed it.
4. The first device run exited with SIGILL before entering the prototype.
   A diagnostic run identified PC `0x0041cd84` in `std::rt::lang_start_internal`:
   opcode `0x7c03e83b`, `rdhwr $3, $29`, used to obtain the TLS pointer. The old
   Realtek disassembler labels this encoding as `swp.c0`; that label is not the
   standard MIPS32 decoding. The stock kernel rejected it.
5. Rebuilding `std` with native TLS disabled produced a working executable.
   On `192.168.178.62` it printed every checkpoint and exited with status 0:

```text
PASS arithmetic and explicit byte order
PASS heap allocation (64 KiB)
PASS threads and atomic read-modify-write
PASS monotonic timer and sleep
PASS OS random-device read
PASS localhost TCP I/O
Prototype completed; radio and gateway configuration untouched.
Prototype exit status: 0
```

The atomic test runs two threads, each performing 10,000 sequentially consistent
increments, and verifies 20,000. The I/O check binds an ephemeral localhost
port and echoes four bytes. `/dev/urandom` is read without printing entropy;
this does not validate Ziggurat's separate `getrandom` crate.

ELF inspection confirmed a static ELF32 big-endian MIPS-I/O32 executable with
soft-float attributes and no dynamic interpreter. Device tests disabled core
dumps and used a 15-second watchdog plus an outer SSH timeout. No daemon was
stopped or restarted. The passing binary was installed only at
`/tmp/ziggurat-rust-prototype-20261002-pthread-tls`.

Build and run logs are retained locally under ignored `logs/`. Toolchains,
caches, checkouts and prototype artifacts together use about 1 GiB at this
stage; the modern GCC toolchain has not been built.

## Dependency and Ziggurat builds — 2026-10-02

```sh
bash scripts/dependencies.sh host
bash scripts/dependencies.sh stock
bash scripts/build-ziggurat.sh
```

The dependency probe pins Tokio 1.53.1, tokio-serial 5.5.0, getrandom 0.4.3,
rand 0.10.3 and parking_lot 0.12.5, matching the main dependencies investigated
in Ziggurat. Its lockfile records the probe's transitive dependencies.

`build-ziggurat.sh` creates an ignored detached Git worktree at
`worktrees/ziggurat/`, matching the pinned submodule revision. The primary
source submodules stay clean. It applies the recorded server patch there and
builds the `ziggurat-server` package, whose executable is named `ziggurat`.

The scripts make disposable crate copies in `build/patched-crates/` and use
Cargo path overrides. Registry sources are not edited. Reviewable diffs are
recorded under `patches/`:

- `getrandom`: enable its existing old-Linux file fallback on uClibc and bypass
  dynamic symbol lookup for this static target. The first dependency-device
  run crashed before its first checkpoint; bypassing `dlsym` removed the crash.
  The fallback waits for entropy readiness before reading `/dev/urandom`.
- `nix`: fix the uClibc signal-flag type and exclude a duplicate MIPS
  `SIGEV_THREAD_ID` match arm.
- `radium`: use the compiler's reported atomic capabilities, including the
  absence of 64-bit atomics, rather than guessing from our custom target name.
- `ziggurat-stock-compat`: link the experiment's compatibility crate on uClibc.

Do not run the two build scripts concurrently: both refresh shared disposable
crate sources. Rust still labels MIPS-I support experimental.

The host dependency probe passed entropy, Tokio scheduling/timers, loopback
networking and bidirectional anonymous PTY I/O. On the gateway, entropy,
Tokio's two-worker runtime, parking_lot and loopback networking passed. Creating
an anonymous PTY then failed with `No such device or address`; `/dev/ptmx`
exists but `devpts` is not mounted. The probe consequently exits with status 1,
and serial I/O on this device remains unverified. No mounts or UARTs were
changed to work around this limitation.

The full Ziggurat build succeeded with the stock RSDK linker. Output:

`build/ziggurat/lexra-stock-uclibc/release/ziggurat`

It is a stripped, statically linked, big-endian ELF32 MIPS-I executable,
approximately 3.4 MiB. Its MD5 for the tested build is
`4036fee6a048b0bb33451936f6c68ca0` (transfer integrity, not a security signature).
It was copied through SSH stdin into a new RAM-backed path,
`/tmp/ziggurat-server-smoke-20261002`, and the device checksum matched.

`scripts/device-ziggurat.sh` is the bounded smoke harness, passed through SSH
stdin with that temporary executable path as its sole argument. It runs
`--version`, `--help`, then starts the server for three seconds with
`--device tcp://127.0.0.1:9 --listen 127.0.0.1:0`. Upstream opens its radio
transport lazily; no client connected and no radio transport was opened.
The server logged a successful listen, then was stopped by its test harness:

```text
ziggurat-server 0.1.0
Listening for WebSocket clients on 127.0.0.1:0
VmSize: 5720 kB
VmRSS:  1024 kB
Threads: 1
PASS CLI and three-second localhost-only server startup; no radio client connected.
```

These memory figures describe an idle server before a Zigbee stack exists.
They do not establish active-network memory, CPU performance or reliability.
Existing gateway services, radio firmware, configuration and network state
were preserved. Credentials were read only by `sshpass -f` using the existing
password-file pathname and were not added to this repository.

## Host RCP simulation against gateway Ziggurat

```sh
python3 scripts/simulate-rcp.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt \
  --binary /tmp/ziggurat-server-smoke-20261002 --seconds 25
```

Run the host wire-format/error-path checks without contacting a gateway:
`PYTHONDONTWRITEBYTECODE=1 python3 scripts/test-simulate-rcp.py`.

The already-uploaded binary must exist; it disappears if the gateway reboots.
The script creates a Python-standard-library Spinel simulator on this computer,
bound to its gateway-facing LAN address and an ephemeral TCP port. The gateway
Ziggurat process connects directly to that host socket. The test accepts an RCP
connection only from the selected gateway IP. The host's temporary WebSocket
test client reaches Ziggurat's localhost-only API through SSH forwarding:

```text
Host test client -- WebSocket through SSH --> Tuya Ziggurat
Host simulated RCP <-- Spinel/HDLC over LAN TCP -- Tuya Ziggurat

Planned client: microhad-zigbee-native --> Ziggurat --> physical RCP
```

No radio UART is opened. The simulator implements the subset needed for address
lookup, coordinator configuration, raw transmit completions and reset recovery.
It validates HDLC CRCs and acknowledges supported property setters; unsupported
operations report `Unimplemented`. Resets clear its simulated radio settings.
The client generates fresh synthetic coordinator settings with explicit dummy
keys, without reading or restoring any production network configuration.

The gateway test process uses a new log in `/tmp`, core dumps disabled, a
70-second watchdog, and cleanup on SSH termination. It is stopped at the end;
existing services keep running. Logs, protocol results and sampled resources
are saved locally to ignored `logs/simulation-<timestamp>.json`.

The final device run passed address lookup, an injected receive/capture event
including HDLC-escaped bytes, configuration, network startup, ten broadcast
requests and ten successful asynchronous `BroadcastConfirm` notifications.
It also passed simulated RCP reset recovery with cleared properties,
865 network queries during a 25-second soak, channel changes, client reconnect
and stack shutdown. The simulator observed 12 transmitted frames (including
stack-generated traffic), two resets and 47 channel property writes.
Active-stack sampled RSS stayed at 2,240 KiB, compared with 1,712 KiB before
configuration; the final shutdown sample was 2,248 KiB. CPU counters were
sampled only for the main PID and showed no tick increase. The subsequent
`top` measurement below identified the missing LinuxThreads worker accounting;
the original samples did not measure the complete runtime. That run's JSON is
`logs/simulation-1790943782386518400.json` (ignored, retained locally).

The harness also matches its HDLC encoding against the upstream test vector
`7e8102025e807e`. WebSocket reconnects use normal closing handshakes. Expected
simulated reset notifications appear at ERROR level in upstream logs; both
resets were intentional, and recovery was checked.

## CPU and response timing with the host simulator

The harness now runs `top -b -d 1 -n 65` concurrently on the gateway. This
uClibc uses LinuxThreads: the main thread, pthread manager and Tokio worker
appear as three separate PIDs. The report discovers descendants of the test's
main PID in each `top` snapshot and sums their CPU percentages. Sampling just
the main PID misses the worker's CPU use. The first snapshot and intervals
overlapping the request-soak boundaries are excluded from the soak summary.
Shared memory is not summed across these PIDs.

In the final instrumented 25-second soak, **Ziggurat averaged 11.3% CPU**,
with **8–14%** across 22 full one-second `top` samples. The loop completed
856 network-info queries with a 20 ms host pause between queries and 43 channel
changes. Sampled peak RSS remained 2,248 KiB. `top` monitoring, SSH forwarding,
and the existing native gateway daemon ran alongside the test; the percentage
is Ziggurat's process tree, not total gateway CPU usage. BusyBox `top` reports
whole percentages, so these are approximate measurements for this workload,
not a maximum-capacity or real-mesh benchmark.

Host timing uses `time.perf_counter_ns()` from request send to receipt of the
matching protocol response, grouped by command and status. Raw samples and
minimum, mean, median, nearest-rank p95/p99 and maximum are retained in JSON.
These are end-to-end timings including the WebSocket SSH tunnel, gateway
scheduling and, for radio operations, the LAN trip to the simulated RCP.
They do not isolate Ziggurat's internal execution time.

| Request | Samples | Median | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Network info | 858 | 7.43 ms | 17.50 ms | 28.66 ms |
| Set channel | 43 | 10.16 ms | 15.65 ms | 19.69 ms |
| Broadcast admission response | 10 | 9.34 ms | 20.01 ms | 20.01 ms |

The network-info count also includes the two checks outside the soak.
Single-operation measurements were 36.78 ms for address lookup, 13.77 ms
for configuration and 46.43 ms for network startup. Broadcast timings measure
admission responses; asynchronous completion notifications were validated but
their delivery latency was not measured.

The host simulator's HDLC decode, command handling and response socket write
averaged **0.074 ms** across 96 commands (p95 0.127 ms, maximum 0.186 ms).
This excludes network transit and gateway processing. One injected receive
frame took **5.59 ms** from host injection to client capture-event receipt.

Results are retained locally in
`logs/simulation-1790944335723529010.json`; sanitized `top` rows are included.
The complete gateway `top` log remains in that report's `remote_top_log`
temporary path. The harness stops both Ziggurat and `top` after the test.

This simulator does not model RF timing, contention, real peers, joins,
interference, routing load or a Thread network. It checks transport, API and
host-stack lifecycle; it does not validate over-the-air delivery, Zigbee
interoperability, cryptographic correctness or dual-mode Matter operation.

## Increasing transmit/receive load — 2026-10-02

Run the default-policy experiment and then the synthetic capacity experiment:

```sh
python3 scripts/simulate-rcp.py --load
python3 scripts/simulate-rcp.py --load --unlimited-broadcasts --tcp-nodelay
```

Both use six-second stages at 5, 20, 50 and 100 requests/second **in each
direction**. The host pipelines APS broadcast requests with 32-byte synthetic
payloads while a separate reader measures asynchronous `BroadcastConfirm`
latency from submission. Concurrent injected IEEE 802.15.4 beacon requests
exercise the active MAC receive path and generate outgoing beacon replies.
Each offered pair therefore creates an APS transmission, an incoming beacon
request and an outgoing beacon response. This is not encrypted incoming APS
traffic or a simulation of joined devices, routing or passive acknowledgments.

The scheduler records missed host pacing slots; none were observed in these
runs. Its 256-request pending cap was never reached. After each stage it stops
offering work and allows up to four seconds for confirmations and beacon
replies to drain. It records pending admission/completion work and unanswered
beacon requests over time. These are host-observed outstanding counts, not
direct measurements of internal queue depth. Beacon reply latency uses FIFO
association in this isolated one-source test, without packet IDs echoed in
the replies; all injected requests received one reply in the measured runs.

The default broadcast policy refills one token every 675 ms (~1.48/s) after
an initial burst, reserving tokens for critical/forwarded traffic. Across the
four stages it admitted 30/30, 13/120, 9/300 and 9/600 broadcast requests;
the remainder returned explicit `RateLimited` responses. Every admitted
request completed successfully and all 1,050 injected beacon requests were
answered. The six-second 5/s stage fits the burst allowance; this does not
establish 5/s sustained broadcast admission with default policy. Its report:
`logs/simulation-1790944905764309763.json`.

For capacity testing, `--unlimited-broadcasts` sets the temporary stack's
`broadcast_token_refill` tunable to zero. This is a test-only change and does
not alter the running production daemon, stored configuration or radio.
`--tcp-nodelay` disables Nagle buffering on the host simulator and client
sockets. Without this option, a preceding capacity run showed substantially
larger beacon-reply delays; transport settings affect this benchmark.

In the final capacity run, all 1,050 broadcasts were admitted and received
successful completion notifications, and all 1,050 beacon requests produced
replies. There were no disconnects or lost replies; every stage drained to
zero before the next one. The table reports full stage CPU intervals, measured
with `top` across all LinuxThreads PIDs:

| Offered TX + RX rate | Mean CPU | CPU peak | Broadcast completion median / p95 | Beacon reply median / p95 | Sampled peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| 5 + 5/s | 4.8% | 6% | 15.9 / 38.4 ms | 4.5 / 30.3 ms | 2,392 KiB |
| 20 + 20/s | 17.3% | 19% | 42.3 / 54.7 ms | 4.5 / 11.1 ms | 2,392 KiB |
| 50 + 50/s | 51.5% | 54% | 24.6 / 64.4 ms | 6.4 / 494.2 ms | 2,392 KiB |
| 100 + 100/s | 69.8% | 87% | 34.5 / 178.1 ms | 4,691 / 6,583 ms | 2,508 KiB |

CPU averages have only 4–5 full one-second samples per stage and vary between
runs; this is a short load sweep, not a steady-state capacity bound. Overall
sampled RSS peaked at 2,544 KiB, including the post-load samples. The report is
`logs/simulation-1790945205494517613.json`.

Host-observed pending broadcast work peaked at 1, 2, 10 and 30 requests.
Unanswered beacon requests peaked at 1, 2, 36 and 599. At 100/s, 599 beacon
replies were still outstanding when offering stopped, then drained in about
2.49 seconds. Thus the 100/s stage completed everything eventually but did
not maintain prompt beacon replies. Completion latencies include client SSH
forwarding; beacon reply times run from host injection to observation of the
outgoing beacon at the simulated RCP. Neither is an over-the-air measurement.

The pinned driver's [beacon handler](ziggurat/crates/ziggurat-driver/src/zigbee_stack/mac.rs)
assigns background priority to beacon replies when joins are closed, as in
this test. Its documented intent is to keep unexpected beacon requests from
competing with normal traffic. That supports a reply-priority explanation for
the backlog; the experiment does not isolate receive processing from outgoing
reply scheduling, and it does not prove encrypted incoming APS packets would
experience the same delay. Permitting joins changes both priority and the
beacon-spam behavior and requires a separate test.

The practical assessment is encouraging for ordinary automation loads,
especially the 20 + 20/s case, with all work keeping up and modest CPU/memory.
Higher rates expose background-reply delay and increasing CPU demand. We have
not established a safe maximum traffic rate or the remaining capacity for
Thread/Matter. Next steps should include longer realistic APS receive/peer
tests, real radio timing/retries and shared-radio/Thread integration.

Six focused host checks cover HDLC vectors/corruption, property IDs, reset and
unsupported-operation handling, LinuxThreads CPU aggregation, and completion
notifications arriving before admission responses with rate-limit rejections.
All passed. Existing native gateway services and physical radio state were
preserved; both temporary Ziggurat and `top` stopped after each run.

## CPC daemon on the stock Tuya — 2026-10-02

**CPCd builds and runs on the gateway without reflashing its radio.** This
experiment tests the daemon and transport startup, not a real CPC secondary
or a working shared-radio Zigbee/Thread stack.

The added Silicon Labs submodule is pinned to v4.5.3, commit
`0c9fb76e16a4df765132131f26dd292bb9267514`, matching the pinned gateway project's
CPC baseline. It reports daemon version 4.5.3.0, library API 3 and CPC protocol 5.
Its upstream checkout stays clean; patches are applied in a detached worktree.

```sh
git submodule update --init --recursive
bash scripts/build-cpc.sh
python3 scripts/cpc-device-smoke.py
python3 scripts/cpc-device-smoke.py --handshake
```

The CMake build uses the existing stock RSDK GCC 4.4.7/uClibc toolchain and
produces `build/cpc-stock/cpcd` and `build/cpc-stock/libcpc.a`. The daemon is a
static, big-endian ELF32 MIPS-I executable, about 494 KiB without stripping.
No modern toolchain build or system-wide installation is required.

Applied changes and build choices:

- Apply the pinned gateway project's
  [TCP transport patch](rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/25-RCP-UART-HW/cpcd/tcp-bus.patch),
  allowing a host test peer instead of any physical UART or SPI device.
- Disable CPC encryption for this test, matching that project's RCP security
  setting; encrypted CPC and MbedTLS cross-linking are not validated here.
- Compile the legacy GPIO implementation, but configure recovery pins off.
  No GPIO, bootloader entry or firmware-update mode was exercised.
- Supply stock-libc syscall wrappers for `epoll_create1`, `eventfd`, `accept4`,
  `timerfd_*` and `signalfd`. Correct timer/signal nonblocking flags to the MIPS
  `O_NONBLOCK` value; the old headers contain generic Linux constants.
- Replace unsupported `-fstack-protector-strong` with GCC 4.4's
  `-fstack-protector-all`. The RSDK has no libssp, so an isolated runtime
  supplies its global guard from `/dev/urandom` and terminates on stack-check
  failure. Entropy/guard contents are never logged. Its initializer is compiled
  without stack protection so setting the guard cannot invalidate its own frame.
- Thread naming is diagnostic only: the shim names the current thread with
  `prctl`; foreign-thread naming is skipped on old LinuxThreads.
- Use `/tmp` for CPC sockets and record the pinned Git revision explicitly,
  since upstream's CMake Git helper does not identify this detached worktree.

The CMake changes are recorded in `patches/cpc-stock.patch`; compatibility
sources and the toolchain file are under `cpc/`. The static `libcpc.a` was built
but a client linked against it has not yet been exercised. Its stack-protected
objects require the guard runtime when linking a future static client.

The host smoke harness transfers a new binary through SSH stdin, checks its
checksum, and creates a fresh `/tmp/cpc-smoke-<timestamp>/` directory with a
generated configuration and no production keys. It runs `--version` and
`--help`, then starts normal mode against a host TCP listener for five seconds.
Core dumps are disabled and a 15-second watchdog bounds the daemon test.

The first final run set `reset_sequence: false`, verified a live TCP connection
and the daemon's local control socket, and reported 420 KiB RSS / 8,880 KiB
virtual memory for the main PID. Bypassing negotiation deliberately produces
no secondary traffic and does not validate secondary compatibility. Report:
`logs/cpc-smoke-1790949572631664862.json`.

The second final run enabled the normal reset/handshake sequence against the
same kind of silent host peer. The host received **63 CPC bytes**. CPCd logged
normal reboot-mode commands, a two-second command timeout, and a connection
retry; it remained alive. Main-PID RSS was 432 KiB. No secondary replied, so
version negotiation and endpoint traffic were not completed. Report:
`logs/cpc-smoke-1790949581761965860.json`. Both tested copies matched MD5
`79d0dd37396db813546e8cc13c9bbb77` (transfer integrity only).

Memory figures describe startup and are not active-traffic process-tree
measurements. All test sockets/configuration stayed under `/tmp`; no installed
daemon, launcher or configuration was replaced. No radio UART was opened and
no firmware was flashed. Test processes were stopped, and the production
`gateway-main` process remained running.

For the Matter dual-mode goal, this clears the CPC daemon build/startup gate.
Remaining work includes a compatible simulated/real CPC secondary handshake,
client-library validation, a CPC transport adapter for Ziggurat, Spinel
multi-interface routing, and joint Thread/Zigbee radio ownership. CPC frames
and raw Spinel/HDLC are different transports; the previous Spinel simulator
cannot serve as a CPC secondary without another protocol layer.

## Next gates

The measurements so far look sufficient for ordinary home-automation command
traffic: single-digit/low-double-digit millisecond replies and roughly 11%
Ziggurat CPU leave useful room. This is a provisional assessment based on
queries and channel changes, not a capacity measurement for real Zigbee
traffic. Joins, encrypted transmissions, routing, retries and message bursts
still need load testing. Dual-mode Zigbee plus Matter/Thread remains
unproven: Thread resources and shared-radio scheduling may become the limits.

The increasing-load experiment below distinguishes host-side pending requests
and replies from an internal Zigbee queue, which the existing API does not
expose directly.

- Implement the Ziggurat protocol-v2 adapter in `microhad-zigbee-native`, including
  notifications, asynchronous send confirmations, persistent state and
  reconnect behavior. Exercise it against this simulator before physical RCP
  integration.
- Extend simulated traffic and duration, then benchmark real peer/routing load.
  A short synthetic active-stack test is not a production-readiness claim.
- Validate serial I/O on the target without disrupting its coordinator; the
  anonymous-PTY probe cannot run with the current device setup.
- Validate RCP firmware, UART flow control and client integration separately,
  with explicit authorization and a backup before any firmware change.
- Build the modern Lexra/musl toolchain if old-libc compatibility becomes too
  restrictive. Keep the successful stock-RSDK path as a reproducible baseline.
- Evaluate the shared-radio Zigbee + Thread architecture for the ultimate
  Matter dual-mode goal, including Spinel interface IDs/multiplexing, firmware
  compatibility, Series-1 same-channel limits and combined memory demand.

## References

- [Gateway build environment](https://github.com/jnilo1/rtl8196e-gateway/tree/main/1-Build-Environment)
- [OpenThread RCP firmware](https://github.com/jnilo1/rtl8196e-gateway/tree/main/2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP)
- [Rust MIPS target definition](https://github.com/rust-lang/rust/blob/master/compiler/rustc_target/src/spec/targets/mips_unknown_linux_musl.rs)
- [GCC backend for rustc](https://github.com/rust-lang/rustc_codegen_gcc)
