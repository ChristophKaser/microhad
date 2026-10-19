# Gateway toolchains

Gateway builds accept `TOOLCHAIN=rsdk` (the default) or
`TOOLCHAIN=crosstool-ng` (experimental). Host builds use the host compilers and
do not require either SDK directory. Silicon Labs firmware keeps its ARM tools.

The source-built candidate targets Lexra LX4380, big-endian MIPS-I/O32 and
soft float. It uses GCC 15.2.0, binutils 2.45.1, uClibc-ng 1.0.54 LinuxThreads,
and stock Linux 3.10.90 headers. Native TLS, time64 and FENV remain disabled.
The application configuration enables interface enumeration and SUSv3/SUSv4
legacy APIs required by OpenThread and BusyBox.

## Build

Build the compiler explicitly before selecting it:

```sh
python3 scripts/build-toolchain.py
```

Sources, checksums and configurations are pinned under `config/crosstool-ng/`;
the builder uses the pinned upstream Lexra patches from `jnilo1/rtl8196e-gateway`.
See [configuration provenance and credits](crosstool-ng/README.md) for the source
configuration, exact revision, patch links and local adaptations. It installs
under `tools/lexra-proof`, uses local bootstrap tools, and requires approximately
12 GiB free space. The Linux build host needs GCC/G++, make, autoconf, automake, libtoolize,
flex, bison, gperf, makeinfo, rsync, patch, curl, tar, and Python 3.11 or later.
The builder installs help2man, GNU libtool and crosstool-NG locally. Application
builds never install a compiler automatically. A missing compiler fails the build.

Run from the repository root:

```sh
make -C microhad TOOLCHAIN=crosstool-ng mips
TOOLCHAIN=crosstool-ng ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
TOOLCHAIN=crosstool-ng bash micro-thread-matter-daemon/scripts/build.sh mips service
TOOLCHAIN=crosstool-ng bash firmware/scripts/build-maintenance.sh
```

Set `TOOLCHAIN`, `RSDK_ROOT` and `CROSSTOOL_NG_ROOT` in the gitignored root
`workspace.local.env` to share your selection across Make, shell and Python.
Environment and Make command-line overrides still win. See
[development-host settings](../docs/workspace-settings.md). Set `RSDK_ROOT` or
`CROSSTOOL_NG_ROOT` to select a compatible installation.
Explicit `TARGET_CC`, `TARGET_CXX`, `TARGET_AR`, `TARGET_NM`, `TARGET_STRIP`,
`TARGET_READELF` and `TARGET_PREFIX` overrides are supported by Make, shell and
Python consumers. Use a fresh process for each selection; exported explicit tool
overrides remain overrides when switching `TOOLCHAIN` in a sourced shell.

## Artifacts and verification

MicroHAD uses `-Os` for candidate C/C++ code, TinyCBOR and Mbed TLS; RSDK
and host builds retain `-O2`.

RSDK paths and host fixture paths remain unchanged. Candidate paths insert
`crosstool-ng/` immediately below `build/`:

| Artifact | Candidate path |
| --- | --- |
| MicroHAD with Zigbee/Matter | `microhad/build/crosstool-ng/mips-ziggurat-matter/microhad` |
| Ziggurat multipan | `build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat.stripped` |
| Matter service | `build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips` |
| OpenThread CLI | `build/crosstool-ng/openthread-unix/ot-cli-mips` |
| Maintenance helper | `build/crosstool-ng/gateway-maintenance` |

Native and Cargo target caches are separated. Compiler/tool identities and
runtime archive identities are recorded in each guarded target directory's
`toolchain-provenance.json`. Changing the selected installation invalidates that
directory's generated cache. Shared patched source and host artifacts are retained.

Build verification checks static linkage, ELF32 big-endian MIPS-I/O32, soft-float
attributes, and absence of `rdhwr`/`teq`. Other Lexra-sensitive instructions are
reported. Rust still uses LLVM for code generation and the selected GCC for linking.
RSDK OpenThread retains its Clang workaround; candidate OpenThread uses native GCC.

```sh
python3 scripts/test-toolchain-config.py
make -C microhad host test
make -C microhad mips
make -C microhad TOOLCHAIN=crosstool-ng mips
```

## Experimental dynamic linkage using stock libraries

`LINK_MODE=dynamic` builds non-PIE application executables using only shared
libraries supplied by the stock image. `LINK_MODE=static` remains the default;
dynamic mode requires `TOOLCHAIN=rsdk`. The compiler and headers remain RSDK,
but dynamic linking and ELF verification use the stock uClibc 0.9.33 libraries,
not the SDK's uClibc 0.9.30.3 runtime.

Extract only `/lib` from a local stock flash dump before building. This gateway's
SquashFS starts at offset `0x200000`; do not extract or commit userdata:

```sh
unsquashfs -d build/stock-runtime -o 2097152 /path/to/stock-flash.bin lib
```

`STOCK_ROOTFS` can select another extraction directory through the environment,
Make arguments, or `workspace.local.env`. The default is `build/stock-runtime`.
`config/stock-runtime.json` records stock SONAMEs and file hashes; a missing or
modified library fails validation. Build preparation creates a generated `link/`
directory beside the extracted `lib/`, bypassing the stock `libc.so` script's
reference to an archive absent from the image. Extracted binaries and generated
links are ignored build artifacts and must not be committed.

```sh
make -C microhad TOOLCHAIN=rsdk LINK_MODE=dynamic mips
TOOLCHAIN=rsdk LINK_MODE=dynamic ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
TOOLCHAIN=rsdk LINK_MODE=dynamic bash micro-thread-matter-daemon/scripts/build.sh mips service
```

Outputs and caches remain isolated under `microhad/build/dynamic/` and
`build/dynamic/`; the Rust target is `lexra-stock-uclibc-dynamic`. MicroHAD's
Mbed TLS and Matter's OpenThread archives remain statically incorporated.
The stock image supplies the loader, libc, pthread, math, realtime, C++ and GCC
runtime libraries. It has no `libutil.so`: the selected SDK's `libutil.a` supplies
`forkpty`/`openpty` locally. The Rust compatibility shim inspects stock exports
and supplies its existing ENOSYS fallbacks for missing pthread stack-inspection
APIs. Firmware, RNG seed, maintenance helpers and deployment defaults retain
static linkage.

Each executable receives an adjacent `.linkage.json`. Verification requires
non-PIE `ET_EXEC`, `/lib/ld-uClibc.so.0`, and a dependency closure contained in
the recorded stock library set. It checks strong imports and symbol versions
against those exact libraries, plus executable MIPS-I/O32, soft-float and
forbidden-instruction checks. Loader-required symbols and copy relocations
remain visible. These checks do not establish runtime compatibility of RSDK
headers/CRT with the stock libraries or stock kernel behavior; a device trial
still requires explicit authorization.

Compare the stripped executables with static counterparts:

```sh
python3 scripts/report-dynamic-linkage.py --output build/dynamic/linkage-summary.json \
  microhad/build/dynamic/mips-ziggurat-matter/microhad.linkage.json \
  build/dynamic/ziggurat-hdlc/lexra-stock-uclibc-dynamic/release/ziggurat.stripped.linkage.json \
  build/dynamic/micro-thread-matter-daemon/mthread-matter-mips.linkage.json
```

All shared dependencies are part of the stock image, so the additional
shared-library upload is **zero bytes**. The summary also lists their existing
uncompressed size for reference; it is not additional application storage.
JFFS2 compression and process memory use are not measured.

Current stripped artifacts, linked and checked against the stock libraries:

| Service | Static bytes | Stock-dynamic bytes |
| --- | ---: | ---: |
| MicroHAD | 1,959,780 | 1,567,736 |
| Ziggurat multipan | 3,643,400 | 3,521,320 |
| Matter (static libutil) | 2,621,088 | 2,474,048 |
| Total | 8,224,268 | 7,563,104 |

The executable saving is 661,164 bytes (about 646 KiB), with zero extra shared
libraries. Stock runtime files are already present in the image and are not
part of the application upload. No deployment or gateway runtime test was run.

## Experimental limits

The shared Rust shim supplies only APIs missing from the selected libc.
For LinuxThreads, `pthread_getattr_np` and `pthread_attr_getstack` return `ENOSYS`.
Stack-bound diagnostics and reliable stack-overflow reporting are not guaranteed.
`pthread_setname_np` supports the current thread only. Application builds require
the expanded libc configuration, rather than the initial minimal proof compiler.

The initial compiler proof passed bounded native and Rust/Tokio tests on the
stock gateway. Application compilation and ELF checks do not validate a complete
release on the device. RSDK remains the production default. Deployment defaults
remain on RSDK artifacts; a future explicitly requested candidate deployment must
pass all selected artifact paths and follow the existing maintenance flow.
