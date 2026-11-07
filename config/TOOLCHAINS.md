# Gateway toolchain

Gateway builds use the pinned source-built crosstool-NG compiler. The former
`TOOLCHAIN` setting is ignored, regardless of its value. RSDK is no longer used.
Host builds use host compilers; Silicon Labs firmware keeps its separate ARM tools.

The compiler targets Lexra LX4380, big-endian MIPS-I/O32 and soft float, using
GCC 15.2.0, binutils 2.45.1, uClibc-ng 1.0.54 LinuxThreads and stock Linux
3.10.90 headers. Native TLS, time64 and FENV remain disabled. Interface enumeration
and SUSv3/SUSv4 legacy APIs are enabled for OpenThread and BusyBox.

## Build

Build the compiler explicitly before building target applications:

```sh
python3 scripts/build-toolchain.py
make -C microhad host test
make -C microhad mips
ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
bash micro-thread-matter-daemon/scripts/build.sh mips service
bash firmware/scripts/build-maintenance.sh
```

Sources, checksums and configurations are pinned under `config/crosstool-ng/`.
See [configuration provenance and credits](crosstool-ng/README.md) for upstream
Lexra patches and local adaptations. Installation is under `tools/lexra-proof`,
with approximately 12 GiB free space required. The Linux host needs GCC/G++, make,
autoconf, automake, libtoolize, flex, bison, gperf, makeinfo, rsync, patch, curl,
tar and Python 3.11+. The builder installs help2man, GNU libtool and crosstool-NG
locally. Application builds never install a missing compiler automatically.

Set `CROSSTOOL_NG_ROOT` in the environment, Make arguments or gitignored
`workspace.local.env` to use another compatible installation. Environment and
Make command-line overrides win over private workspace settings. Explicit
`TARGET_CC`, `TARGET_CXX`, `TARGET_AR`, `TARGET_NM`, `TARGET_STRIP`,
`TARGET_READELF` and `TARGET_PREFIX` overrides remain supported. See
[development-host settings](../docs/workspace-settings.md).

## Artifacts and verification

MicroHAD target code, nanopb and Mbed TLS use `-Os`; host builds retain `-O2`.
Existing crosstool-NG paths are preserved:

| Artifact | Path |
| --- | --- |
| MicroHAD with Zigbee/Matter | `microhad/build/crosstool-ng/mips-ziggurat-matter/microhad` |
| Ziggurat multipan | `build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat.stripped` |
| Matter service | `build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips` |
| OpenThread CLI | `build/crosstool-ng/openthread-unix/ot-cli-mips` |
| Maintenance helper | `build/crosstool-ng/gateway-maintenance` |

Host fixture paths are unchanged. Target directories record compiler and runtime
archive identities in `toolchain-provenance.json`; changing the installation
invalidates those caches while retaining shared patched sources and host artifacts.
Verification checks static linkage, ELF32 big-endian MIPS-I/O32, soft-float
attributes and absence of `rdhwr`/`teq`. Other Lexra-sensitive instructions are
reported. Rust uses LLVM code generation and GCC linking; OpenThread uses GCC/G++.

```sh
python3 scripts/test-toolchain-config.py
```

## Experimental dynamic linkage using stock libraries

`LINK_MODE=static` remains the default. `LINK_MODE=dynamic` builds non-PIE
applications against verified stock uClibc 0.9.33 libraries. The compiler supplies
headers, startup objects and support archives; no replacement libc is installed.

Import libraries from an existing local extraction, without accessing a device:

```sh
python3 scripts/bootstrap-stock-runtime.py --from-root /path/to/stock-root
python3 scripts/bootstrap-stock-runtime.py --verify-only
```

The source must contain `lib/`. An explicitly authorized gateway read can instead
use `--host HOST`. See [runtime bootstrap](../docs/building.md#bootstrap-stock-libraries-for-dynamic-builds).
`STOCK_ROOTFS` defaults to `build/stock-runtime`. Hashes and SONAMEs must match
`config/stock-runtime.json`; generated links live in `link-crosstool-ng/` beside
`lib/`. These runtime files and links are ignored build artifacts.

```sh
make -C microhad LINK_MODE=dynamic mips
LINK_MODE=dynamic ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
LINK_MODE=dynamic bash micro-thread-matter-daemon/scripts/build.sh mips service
```

Dynamic outputs are under `microhad/build/crosstool-ng/dynamic/` and
`build/crosstool-ng/dynamic/`, with Rust target
`lexra-crosstool-ng-uclibc-dynamic`. Mbed TLS and OpenThread archives remain
statically incorporated. The stock image supplies the loader, libc, pthread,
math, realtime, C++ and GCC shared runtimes. Since it has no `libutil.so`, only
`forkpty`, `openpty` and `login_tty` are extracted from the compiler's libc archive
into a private `libutil.a`. The new libc is never linked wholesale.

Dynamic C++ uses `_GLIBCXX_USE_CXX11_ABI=0` and disables sized deallocation.
Stock `libstdc++.so.6` is followed by GCC 15 `libstdc++.a` for unresolved newer
helper symbols. OpenThread resolves `__gnu_local_gp` to `_gp`. The Rust shim
supplies ENOSYS fallbacks for missing pthread stack-inspection APIs. Firmware,
RNG seed and maintenance helpers retain static linkage.

Each executable receives an adjacent `.linkage.json`. Verification checks
non-PIE `ET_EXEC`, `/lib/ld-uClibc.so.0`, exact stock dependency closure, strong
imports and symbol versions, alongside the target architecture/instruction checks.
These checks do not establish all runtime behavior. No shared library upload is
required; device trials require explicit authorization.

## Runtime limits and recorded validation

LinuxThreads lacks `pthread_getattr_np` and `pthread_attr_getstack`; the shim
returns `ENOSYS`. Reliable stack-bound diagnostics and stack-overflow reporting
are not guaranteed. `pthread_setname_np` supports the current thread only.

Static crosstool-NG release artifacts completed a paired deployment on a
Silvercrest SGWZ 1 A1 / HG06339 gateway on 2026-10-21. MicroHAD, Ziggurat and
Matter started; dashboard, status and log APIs were reachable, and the formed
Zigbee network retained its 26 devices. Follow the existing maintenance flow
for future authorized deployments.

A stock-dynamic deployment trial on 2026-10-19 verified startup/integration of
all three services with the existing 26-device inventory and no new shared
libraries. It did not establish commissioning, device control or long-duration
behavior. Earlier compatibility investigation results remain in the
[stock-dynamic probe notes](../scripts/probes/stock-dynamic/README.md).

## Pinned LLVM host tools

Install the pinned host LLVM tools explicitly:

```sh
python3 scripts/install-llvm.py
```

The installer verifies its archive hash and installs under ignored
`tools/llvm-23.1.5/`. `LLVM_STRIP` and `LLVM_AR` remain available for Rust artifact
processing and compatibility tooling; `CLANG`/`CLANGXX` remain available for
standalone diagnostic probes. None selects an alternative gateway compiler.
Environment/private workspace overrides are supported. Relative paths resolve
against the repository root; bare executable names use PATH.

The historical RSDK/Clang OpenThread delay-slot investigation is preserved in
its standalone probe and `scripts/check-mips-load-delay.py --self-test`.
That workaround is no longer part of application builds.

## Historical RSDK/Clang investigation

The following records the retired backend investigation; it does not describe
the current GCC OpenThread build.

Clang 18.1.3 emitted a pointer load in the call delay slot before
`AddressResolver::UpdateCacheEntry` entered `LinkedList::PopAfter`. The first
callee instruction saw the old register value, bypassed a null check, and the
next instruction accessed address `0x12`. An isolated gateway probe reproduced
both the wrong branch and SIGSEGV; moving the load earlier corrected the branch.

LLVM 23.1.5 is built with normal delay-slot scheduling. The OpenThread build
scans all its archives for loads crossing call or indirect-jump boundaries;
a missing/unreadable archive fails validation. Direct local branches are not
rejected merely for containing a delay-slot load: the gateway probe confirms
that the conditional-branch fallthrough pattern behaves correctly. The original
call-boundary pattern remains a failing control in that same probe.

```sh
python3 scripts/check-mips-load-delay.py --self-test
```

The standalone reproduction source is
`micro-thread-matter-daemon/tests/helpers/mips-load-delay-probe.c`. It uses no
radio or application state; its child catches the deliberate SIGSEGV. This
check covers C/C++ scheduling, not Rust's bundled LLVM or the ARM radio
firmware toolchain. Host and crosstool-NG scheduling flags are unchanged.


## Historical stock-dynamic size measurements

These measurements describe the earlier stock-runtime trial, rather than
current compiler outputs.

Current stripped artifacts, linked and checked against the stock libraries:

| Service | Static bytes | Stock-dynamic bytes |
| --- | ---: | ---: |
| MicroHAD | 1,959,780 | 1,567,736 |
| Ziggurat multipan | 3,643,400 | 3,521,320 |
| Matter (static libutil) | 2,621,088 | 2,474,048 |
| Total | 8,224,268 | 7,563,104 |

The executable saving is 661,164 bytes (about 646 KiB), with zero extra shared
libraries. Stock runtime files are already present in the image and are not
part of the application upload.
