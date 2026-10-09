# Experimental HDLC multi-PAN implementation

This implementation shares one HDLC transport in the Ziggurat process, with
Zigbee on RCP IID 1 and a local Thread consumer on IID 2. It adds no cpcd.
`microhad`, local Thread border routing and Matter remain the
standalone product goal; the latter integrations are not implemented here.

## Reproduce the firmware build

```sh
bash firmware/scripts/install-silabs.sh
bash firmware/scripts/build-hdlc-firmware.sh
```

The installer keeps tools under ignored `tools/silabs/`. It uses GSDK v4.5.0
at `90fc93e1f95a10c981f0e49ca6787192e82fd8f2`, ARM GCC 12.2.rel1 and the official
Silicon Labs public SLC package at
`a2cef4434600379695bef23bfbdcc8e604f7f305` (SLC 5.6.3). Java 17 must be installed;
the environment script currently selects the Ubuntu Java 17 path.

The gateway installer's old SLC download URL returned HTTP 403. A full SDK
archive download timed out. A sparse Git checkout succeeded; `util/plugin`
must be included for the security-manager dependency. Project generation then
succeeded without unresolved dependency warnings. No Commander is needed to
produce the ELF and raw binary; a bootloader-update GBL is not produced.

The experimental project is `firmware/hdlc-multipan/ot-rcp-multipan.slcp`.
It keeps the Lidl USART0 PA0/PA1/PA4/PA5 wiring, 460800 baud, UARTDRV and hardware
flow control. Two static instances are enabled; ordinary upstream NCP glue is
selected with vendor hooks disabled. Both multi-instance initialization
symbols are present in the linked ELF. Submodules are unchanged.

Artifacts are under `build/firmware-hdlc/build/debug/`:

- `ot-rcp-multipan.out`: ELF.
- `ot-rcp-multipan.bin`: raw application bytes; **base address `0x4000`**.
- `ot-rcp-multipan.map`: allocation evidence.

The recorded build has text 103,088 bytes, data 636 bytes and BSS 16,688 bytes
according to `arm-none-eabi-size`. The map reserves a 4,096-byte stack, places
the heap base at `0x200043a0` and heap limit at `0x20007c00`, and provides
14,432 bytes between them. These numbers describe linker allocation, not
measured runtime headroom. Flash is configured as `0x4000..0x40000`; RAM is
`0x20000000..0x20007c00`. Link-time fit and initialization symbols are verified;
actual radio operation, stack high-water use and the installed bootloader's
compatibility are not. Do not treat the raw `.bin` as a complete chip image.
Versions, artifact sizes and SHA-256 hashes are recorded in
[the build manifest](hdlc-build-manifest.json).

The radio implementation still links the precompiled MG1 RAIL archive. See
[licensing and source audit](radio-dual-mode.md).

## Build the host and MIPS transport changes

`ziggurat/patches/ziggurat-hdlc-multipan.patch` begins an ordered mailbox series against
the pinned Ziggurat revision. The build script applies each mailbox with
`git am` in `worktrees/ziggurat-multipan`; it leaves the upstream checkout untouched.
Stock and multipan builds use separate compatibility and syslog mailboxes where
their source context differs.

```sh
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash ziggurat/scripts/build-hdlc-probe.sh
```

The default RSDK MIPS output is `build/ziggurat-hdlc/lexra-stock-uclibc/release/ziggurat`;
the build also creates `ziggurat.stripped` for bounded device tests.
Multipan is the default. Set `ZIGGURAT_MULTIPAN=0` for the single-PAN baseline.
The HDLC build uses `worktrees/ziggurat-multipan`. `TOOLCHAIN=crosstool-ng`
selects the candidate and writes
`build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat`; see
[toolchain selection](../config/TOOLCHAINS.md).

To run the tests on the build computer after the worktree has been prepared:

```sh
source scripts/rust-env.sh
CARGO_TARGET_DIR="$experiment_root/build/host" cargo test --offline \
  --manifest-path worktrees/ziggurat-multipan/Cargo.toml -p ziggurat-spinel
CARGO_TARGET_DIR="$experiment_root/build/host" cargo build --offline \
  --manifest-path worktrees/ziggurat-multipan/Cargo.toml -p ziggurat-server
python3 ziggurat/scripts/test-simulate-rcp.py
python3 ziggurat/scripts/test-hdlc-multipan.py
```

Host networking must be permitted for the final socket integration test.
Host builds need the same Cargo dependency cache as the existing experiment.

## Transport ownership and local Thread endpoint

Example against the host simulator, replacing HOST/PORT with its printed
address (this is not a production radio invocation):

```sh
ziggurat --device tcp://HOST:PORT --spinel-iid 1 \
  --thread-socket /tmp/ziggurat-hdlc-thread.sock --listen 127.0.0.1:9999
```

`--spinel-iid 0` remains the default for existing single-PAN firmware.
`--thread-socket` requires IID 1 and opens the radio link at startup. The local
socket is created with mode 0600 and refuses an existing path; after a crash,
remove only the known stale experiment socket before restarting.

Each `SpinelClient` has independent transaction IDs, pending replies and
property subscriptions. `new_for_interface` owns the physical transport;
`interface(2)` registers a second client sharing its reader, writer and radio
lock. Duplicate/invalid IIDs are rejected. Incoming messages are routed by IID
before transaction/property dispatch; a response for another IID cannot
complete a pending request. Reader startup is idempotent across logical clients.

The Unix endpoint serves one consumer at a time. It accepts ordinary IID 0
HDLC Spinel requests, maps them to IID 2 with an independent upstream TID, and
restores the consumer's IID/TID in replies. IID 2 notifications become IID 0
notifications. Requests wait for the shared radio lock so they cannot bypass
Ziggurat's exclusive radio operations. The endpoint runs inside Ziggurat;
the tiny C bridge used over SSH is only a development test helper.

An [OpenThread Unix adapter and MIPS CLI proof build](openthread-feasibility.md)
are now available. Its URL is `spinel+hdlc+unix:///path/to/thread.sock?no-reset`;
the adapter uses logical IID 0, with Ziggurat mapping it to radio IID 2.
Host framing/reset tests and the bounded MIPS simulated-RCP/UDP test pass.
The Unix path attaches without waiting for a boot reset notification. Devpts
is not required by this approach.

This endpoint is **not yet an OTBR integration**. The adapter has not been
validated through a complete startup handshake via actual Ziggurat; a synthetic
RCP handshake on the MIPS device passed. `otbr-agent`
has not been built with it. The running factory kernel lacks IPv6/TUN. No real
Thread mesh, IPv6 border routing, commissioning or Matter application has been
validated. Direct internal OpenThread UDP can avoid Linux IPv6/TUN; standard
Linux sockets and border routing require them. The built CLI uses the internal
stack, with target execution verified against the synthetic RCP. Physical
radio startup and simultaneous PAN operation remain untested.

## Shared-radio policy in this version

- The physical owner alone may send an RCP reset; secondary resets receive
  `LastStatus=InvalidState` and never reach the RCP.
- A physical reset notification cancels both interfaces' pending commands and
  notifies both consumers. Ziggurat reprograms its radio configuration; a real
  Thread consumer must restore its own configuration too.
- The first shared channel setting fixes the channel for the process lifetime.
  Conflicting channel settings are rejected. Changing both networks' channel
  together is future work; a reset does not release this policy.
- Promiscuous mode, disabling source matching, disabling the radio or idle RX,
  and starting scans are rejected while both interfaces are registered.
  This also means capture and scan-based commissioning are not usable in this
  multi-PAN mode yet. Use explicit common-channel configuration for simulation.
- Raw Thread notifications have a bounded 256-message queue. Long-duration
  load, overflow behavior and Thread recovery need testing before production.

The radio remains one transmitter. MG1 source-match/filter-mask limitations
described in the audit still require real sleepy-child/address-collision tests.

## Device test safeguards and current evidence

`ziggurat/scripts/test-hdlc-multipan.py --host 192.168.178.62` uploads only experiment
binaries to unique `/tmp/ziggurat-hdlc-*` paths and uses host-simulated RCP
traffic. It never opens the physical UART or reads production state.
The harness now checks available memory before RAM-backed uploads, leaves an
8 MiB margin beyond file sizes using `MemFree` (ramfs cache is not safely
reclaimable), limits the remote test to 45 seconds, and
removes its uploaded binaries/socket/log after collecting results.

The early harness left uploaded binaries behind. The first device run completed
seven traffic checks and logged successful radio recovery, but its last
assertion incorrectly expected another reset command. The corrected check
verifies reprogramming. The next upload timed out and new SSH/dashboard
requests stopped responding, while ping still worked. Accumulated RAM-backed
test files are a suspected cause; memory could not be inspected through SSH.
Memory preflight and cleanup were added after this incident.

After the user power-cycled the gateway, SSH and HTTP recovered. The final
safeguarded run passed all eight checks. Its local report is
`logs/hdlc-multipan-1790970047159156188.json`; the host pass after policy
hardening is `logs/hdlc-multipan-1790969568063990203.json`. Logs remain ignored;
the [validation summary](hdlc-validation.json) preserves the relevant results.

| Final Tuya measurement | Result |
| --- | --- |
| Free RAM before upload | 17,300 KiB |
| Required free RAM, files plus margin | 11,659 KiB |
| Ziggurat VmRSS after the test workload | 2,068 KiB |
| Thread endpoint queries | 30 |
| Query round-trip median / p95 | 7.21 / 7.82 ms |
| Free RAM in subsequent cleanup/health check | 17,216 KiB |
| Production dashboard after cleanup | HTTP 200 |

Query timing includes the SSH Unix-socket bridge and host simulator. The RSS
sample describes Ziggurat plus the shared transport, not a real Thread or
Matter stack. No capacity or CPU conclusion follows from this short test.

Ten upstream codec tests, five new multi-PAN tests and seven Python simulator
checks pass. Formatting, patch application and shell syntax were checked.
Clippy was not available in the isolated Rust toolchain.

One run soon after reboot stalled during Zigbee configuration. `State::new`
requests OS randomness; the patched getrandom fallback polls `/dev/random`
before opening `/dev/urandom`. The pool was small after reboot and the test
passed later after recovery; low entropy is a suspected cause, not a traced
proof. Do not bypass the readiness check. Reliable cold-boot RNG readiness,
an explicit startup timeout/error and avoiding a blocked async worker need
attention before production.

### Physical-radio reset correction (2026-10-03)

In this SDK build, IID 0 is reserved and does not dispatch commands. Both the
maintenance helper and Ziggurat transport must send RESET to active Zigbee IID 1.
The physical transport owner remains the only reset issuer; the Thread client
still cannot reset. Reset notifications on broadcast IID 0 are still distributed
to consumers. The simulator now rejects commands on reserved IID 0 before
processing RESET, preventing the false positive that hid this startup failure.
Six Multi-PAN Rust tests (16 total transport tests), seven simulator checks and
eight host scenarios pass after this correction. Firmware need not be reflashed.
