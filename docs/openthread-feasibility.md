# OpenThread on factory MIPS Linux — 2026-10-03

Current builds use upstream OpenThread v2026.10.0 from the Thread/Matter
dependency checkout; the results below record the earlier GSDK-based build.
See [building](building.md) for current commands.

## Result

The original GSDK-matched OpenThread POSIX CLI built for the gateway using Clang 18
for C/C++ and the existing RSDK 4.4.7 linker, headers and uClibc libraries.
The Unix HDLC adapter passes host tests and a bounded MIPS device simulation
including internal IPv6/UDP delivery. This establishes a cross-build and
selected target runtime paths; it does not establish a working Thread network,
simultaneous Zigbee/Thread operation, or Matter control on the gateway.

Read-only SSH checks on 192.168.178.62 found:

- Running kernel: Linux 3.10.90, factory build dated 2020-04-28.
- `/proc/sys/net/ipv6` absent: no active host IPv6 stack.
- `/dev/net/tun` absent and no `tun` entry in `/proc/misc`.
- `/lib/modules` and `/proc/modules` absent: no available module-loading path
  was found to supply the missing features.
- MemTotal 27,892 KiB; MemFree 4,084 KiB at the sampled instant. Cached memory
  was 16,284 KiB, but this is not an established reclaimable capacity budget.

**The Linux interface/border-router path is blocked on this factory kernel.**
This is not a requirement for direct device communication through OpenThread's
own IPv6/UDP APIs. OpenThread implements IPv6 in userspace. TUN exposes those
packets to the Linux network stack; it is needed when Linux applications or
Ethernet routing use a `wpan0` interface.

**Correction after reviewing the exact build:** `OT_DAEMON=OFF` in this CLI
build. The POSIX platform enables platform NETIF/UDP by default only in daemon
mode; the built `netif.cpp` object contains interface-name/index accessors but
no TUN initialization. Thus the built CLI does not require TUN to initialize
its own Thread stack. The earlier statement that missing IPv6/TUN prevented
this CLI from running was too broad. The executable subsequently passed the bounded target simulation below.
Complete ABI coverage and actual shared-radio behavior remain unverified.

An application can use `otUdpOpen`, `otUdpBind` with the internal Thread
interface, and `otUdpSend` to communicate through OpenThread without Linux
IPv6 sockets. A Matter controller would need a transport/platform integration
with those APIs; ordinary Linux Matter socket code would still need kernel
IPv6. Direct UDP connectivity alone does not provide Matter control.
See [OpenThread UDP APIs](https://openthread.io/reference/group/api-udp).
Creating `wpan0` requires a TUN driver; IPv6 sockets/routing require kernel IPv6.
Creating a device node alone cannot provide either capability. No device files,
services, radio configuration, firmware, or network state were changed by
those read-only checks. The later simulated device test used temporary isolated
binaries and generated test state; all test files were removed afterward.

## Build and compiler

From the experiment repository:

```sh
bash micro-thread-matter-daemon/scripts/build-openthread.sh host
bash micro-thread-matter-daemon/scripts/build-openthread.sh mips
```

Prerequisites: the already-pinned Silicon Labs SDK, CMake, Python 3, host C++,
Clang/LLVM 18 (`clang-18`, `clang++-18`, `llvm-ar-18`, `llvm-strip-18`), and the
sibling stock RSDK. The script checks SDK commit
`90fc93e1f95a10c981f0e49ca6787192e82fd8f2` and copies OpenThread into an ignored
build directory before patching. It does not reset the SDK or download sources.
The build banner reports `b74cf17-dirty` after the adapter changes; use the SDK
commit and local patch as the source identity rather than treating that
worktree-derived package string as an upstream OpenThread revision.

Ziggurat uses Rust nightly 2026-10-02 and LLVM with big-endian MIPS-I/o32,
soft float, static relocation, and the same stock linker/uClibc runtime. Its
Rust target disables native TLS. Rust is not a C++ compiler; this OpenThread
proof build separately uses Clang's C++11 frontend and RSDK C++ headers.
The actual installed RSDK compiler reports GCC 4.4.7.

Important cross-build details:

- `-march=mips1 -mabi=32 -msoft-float`; static executable.
- `-fno-pic -mno-abicalls -G0` for the Clang objects; `-G0` avoids an LLVM
  MIPS-I GP-relative instruction-selection failure encountered in this build.
- Target C++ headers use the RSDK's `mips-linux-uclibc/4181` configuration.
- A forward declaration of `std::type_info` accommodates the old
  `exception_ptr.h`; standard integer format/limit macros are enabled.
- POSIX backtrace support is disabled because the target libc lacks that API.
- `-mllvm -mno-check-zero-division` matches the existing Ziggurat LLVM setting.
  It removes `teq` division checks unsupported on Lexra; dataset generation
  failed with SIGILL before this change and passed afterward.
- Static library grouping resolves OpenThread's circular link dependencies.
- LLVM stripping explicitly preserves `.gnu.attributes` for soft-float checks.
- The stock linker warns about mixing its abicalls runtime with Clang's static
  non-abicalls objects. The executable links; target call/ABI behavior still
  is only exercised on selected paths by the target simulation, not established
  comprehensively by link success.

Output: `build/openthread-unix/ot-cli-mips` (919,456 bytes in this build).
ELF verification: 32-bit big-endian MIPS-I/o32, statically linked, soft float,
with no program interpreter or dynamic shared-library dependencies.
SHA256: `27a55270564874449e0bff93a4c941da01deded960c94e32139fde68e22ae08f`.
Build and validation logs are under `build/openthread-unix/`.

A MIPS-I ELF header alone is not full Lexra instruction validation. Disassembly
contains unaligned load/store instructions emitted by LLVM, and `ll/sc` in
linked runtime code. The existing LLVM-built Ziggurat also contains unaligned
instructions. The bounded device simulation now exercises OpenThread startup,
dataset generation, channel/state calls, and internal IPv6/UDP. This still does
not independently prove every runtime helper or instruction path. Final
disassembly has no `teq` or `rdhwr`; `ll/sc` and unaligned instructions remain.
The longer [resource profile](openthread-resource-profile.md) measures sampled
RSS and CPU under synthetic load. Long-running entropy behavior, broader ABI
coverage, and real radio load need further measurements before production deployment.

## Unix adapter

Patch: `micro-thread-matter-daemon/patches/openthread-hdlc-unix.patch`.

The adapter extends OpenThread's existing HDLC interface with:

```text
spinel+hdlc+unix:///path/to/thread.sock?no-reset
```

This is an interface URL, not a command to run on the current gateway.
The endpoint must be supplied by Ziggurat's `--thread-socket` option; the socket
path's existence/enabling was not checked or changed in production.

The adapter connects an AF_UNIX/SOCK_STREAM socket, then uses nonblocking I/O
and close-on-exec. Existing HDLC encoding/decoding, select integration and
partial-write handling are reused. Socket sends use `MSG_NOSIGNAL`. The socket
must have the Unix file type and the URL must explicitly request `no-reset`.
The Unix-only startup path also skips the boot/reset notification wait when
attaching to an already-running RCP, verifying readiness through protocol,
version and capability queries instead. Other transports retain their previous
startup behavior. Reset commands are rejected before sending, including commands with nonzero
transaction IDs. Closing the socket does not wait for unrelated subprocesses.
A disconnected socket retains OpenThread's existing fail/exit behavior; this
patch does not implement automatic reconnection or radio recovery.

The normal logical interface is IID 0. Ziggurat maps its IID/TID to physical
Thread IID 2 and restores the consumer's identifiers on replies/notifications.
OpenThread must not open `/dev/ttyS1`; Ziggurat remains the UART/reset owner.
The existing Zigbee COBS socket has a different protocol and cannot be used.

`micro-thread-matter-daemon/tests/openthread-unix.cpp` exercises the actual patched OpenThread transport:
missing no-reset rejection, reset suppression, escaped bytes, fragmented frame
reassembly, exact decoded round trip, and closing without waiting for an
unrelated child. Both the host CLI and the MIPS CLI compile with the adapter.
These focused tests cover the transport. The subsequent target simulation
also completes the RCP/OpenThread startup handshake against a synthetic RCP;
startup through actual Ziggurat and real-device acceptance remain untested.

## Remaining feasibility gates

1. Choose a networking path: direct OpenThread internal UDP APIs for local
   device communication, a kernel with IPv6/TUN for Linux sockets/border routing,
   or an external OTBR host
   with a private forwarded radio endpoint. An embedded userspace Thread/Matter
   platform is another possible design, but requires its own networking port;
   it is not provided by this POSIX build.
2. Extend the documented synthetic CPU/RSS profile to real mesh load and broader
   ABI coverage. Binary size is not RSS, and sampled RSS is not an absolute peak.
3. Test the full OpenThread startup handshake against the multiplexer. Its
   shared policy currently rejects scans, promiscuous mode, disabling source
   matching, disabling the radio, and idle RX. OpenThread initialization and
   commissioning may require changes; transport tests do not establish this.
4. Coordinate the existing Zigbee channel, sole reset owner, network restoration
   after reset, and commissioning scans. Both networks must share a channel.
5. Verify simultaneous PAN filtering/ACKs, sleepy children, address collisions,
   and radio load on the MG1, preserving Zigbee state.
6. Add a Matter controller and commissioning path, including Thread dataset
   provisioning and secure fabric storage. OpenThread alone supplies network
   connectivity, not Matter device commands.

No kernel flash, production service restart, real Thread commissioning or
Matter implementation is included or claimed by these builds and simulations.

## Verification record

The successful reproducibility checks were:

```sh
bash micro-thread-matter-daemon/scripts/build-openthread.sh host > build/openthread-unix/host-validation.log 2>&1
bash micro-thread-matter-daemon/scripts/build-openthread.sh mips > build/openthread-unix/mips-validation.log 2>&1
```

The host log ends with `Unix HDLC adapter: reset guard, escaped/fragmented round
trip, independent close passed`. The MIPS log includes `Target verification
passed: big-endian MIPS-I/o32, soft float, static` and the artifact SHA256 above.
The tests cover both zero and nonzero transaction IDs on reset commands.
Compilation alone does not execute the MIPS program.

Other local evidence is in `configure.log`, `host-configure.log`,
`host-build.log`, `test-build.log`, `mips-build.log`, `mips-link.log` and
`mips-disassembly.txt` under the ignored output directory. Earlier attempt logs
may contain resolved compilation failures; the two validation logs are the final
build/test record. The patch, build script, test source and this report are the
reviewable source artifacts; generated executables and logs remain ignored.

The gateway checks used password-file SSH with RSA host-key compatibility and
strict host-key checking. They only read `/proc/version`, `/proc/meminfo`,
`/proc/misc`, module paths, and existence of the IPv6 and TUN paths. No password
contents or Zigbee keys were read into output. The old shell lacks `uname` and
rejected `head -5`, so kernel/memory evidence was obtained with `cat` instead.

The initial sandbox blocked local/network sockets and legacy compiler syscalls;
the authorized read-only SSH checks, local socket tests and stock linking ran
outside it. The initial stock compiler invocation failed in the sandbox, but
its successful version check reported GCC 4.4.7. The Clang path was selected for
C++11 support; the installed Ziggurat build configuration was inspected and not
changed or rebuilt.

## Bounded target simulation — passed

The requested device run passed on 2026-10-03. The host provided synthetic Spinel
RCP replies; a tiny stock-toolchain relay on the gateway exposed them as a Unix
HDLC socket. The real MIPS OpenThread CLI used its internal IPv6/UDP stack.
The production Ziggurat socket and `/dev/ttyS1` were never opened by the test.

Reproduce only for an explicitly authorized device test:

```sh
bash micro-thread-matter-daemon/scripts/build-openthread.sh host
bash micro-thread-matter-daemon/scripts/build-openthread.sh mips
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh host
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh mips
python3 micro-thread-matter-daemon/scripts/test-openthread-device.py
python3 micro-thread-matter-daemon/scripts/test-openthread-device.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt
```

The relay build uses the stock RSDK for MIPS. The test runner requires Python 3,
`sshpass`, the password file by pathname, and a reachable simulator host IPv4
address. Without `--host` it tests only local executables/sockets. Neither mode
reads production keys, device configuration or coordinator state.

Passing target calls were: `version`, `eui64`, `state`, `channel`, `channel 15`,
`channel`, `dataset init new`, `dataset networkname OpenThreadSim`,
`dataset commit active`, `ifconfig up`, `thread start`, `ipaddr mleid`,
`udp open`, `udp bind :: 12345`, `udp send <own-mleid> 12345 simulator-loopback`,
`udp close`, `thread stop`, and `ifconfig down`. The synthetic dataset was newly
generated in the private test directory; its keys were not printed or retained.
Channel changes affected only the synthetic radio.

The receive callback printed `18 bytes from <own-mleid> 12345 simulator-loopback`.
This establishes internal UDP delivery, not delivery to a separate Thread
radio device. The harness checks the received-byte line, not just send success.
The final simulator handled 9 GET and 13 SET requests; no unsupported request
or reset command was recorded. OpenThread exited with status 0.

Safeguards and observations:

- At least 3 MiB MemFree and 2 MiB free flash are required before device staging.
- New binaries were staged in a unique `/tuya/native-test/ot-sim-*` directory,
  avoiding the pinned RAM cost of large `/tmp` uploads. Socket/settings were in
  a private `/tmp/ot-sim-*` directory. Logs stayed on the development host.
- CLI virtual address space capped at 4 MiB; watchdog aborts above 2 MiB RSS,
  below 1 MiB MemFree, or after 35 watchdog samples. Relay has a 45-second alarm.
  The later `--profile` mode extends these time bounds; see the resource report.
- Final preflight MemFree: 3,624 KiB. One startup sample reported RSS 280 KiB
  and MemFree 2,280 KiB. This single sample is not peak memory measurement.
- Production reported `online=true`, `network_formed=true`, backend `ziggurat`
  before and after. No production restart, radio flash or coordinator operation
  was performed. Cleanup verified both unique test directories were absent.
- The parent `/tuya/native-test` staging directory remains available; no test
  binary, settings file or socket is retained on the gateway.

A shareable, key-free record is in
[openthread-device-validation.json](openthread-device-validation.json).
Raw local evidence remains ignored under `logs/`:
`openthread-device-1791042973080072191.json` and `.log` for the passing target run;
`openthread-device-1791043132927651299.json` and `.log` for the final host run.

Resolved failures, retained for reproducibility:

1. Initial host startup timed out on protocol-version replies because the
   driver still awaited a reset notification in `no-reset` mode. The adapter
   now selects an explicit attach-to-running-RCP path for Unix transport.
2. Host harness fixes accounted for EUI64 byte order, the `> Done` prompt, and
   enabling the Thread stack before UDP self delivery. Unsupported synthetic
   properties still return explicit errors rather than fabricated success.
3. First target startup exited cleanly before processing commands because the
   old shell backgrounded it with closed stdin. Saving SSH input as fd 3 and
   explicitly giving that fd to the child fixed command delivery.
4. The next target run initialized and handled CLI identity/state/channel, then
   exited 132 (SIGILL) at `dataset init new`. Disassembly contained LLVM `teq`
   division checks. Applying Ziggurat's existing no-zero-division-check setting
   removed them; the complete target call sequence then passed. No PC/core dump
   was captured, so the instruction attribution is supported by disassembly and
   the successful controlled rebuild rather than a captured fault address.

Physical RCP startup through Ziggurat, combined-PAN policy, another Thread
endpoint, mesh stability, real device control, and Matter remain future work.
