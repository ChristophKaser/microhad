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

## Experimental dynamic uClibc linkage

`LINK_MODE=dynamic` opts the three application services into shared RSDK runtime
linkage. `LINK_MODE=static` remains the default. Dynamic mode currently requires
`TOOLCHAIN=rsdk`; it uses the stock uClibc 0.9.30.3 ABI rather than uClibc-ng.

```sh
make -C microhad TOOLCHAIN=rsdk LINK_MODE=dynamic mips
TOOLCHAIN=rsdk LINK_MODE=dynamic ZIGGURAT_MULTIPAN=1 bash ziggurat/scripts/build.sh
TOOLCHAIN=rsdk LINK_MODE=dynamic bash micro-thread-matter-daemon/scripts/build.sh mips service
```

Outputs and caches are isolated under `microhad/build/dynamic/` and
`build/dynamic/`. The Rust target is `lexra-stock-uclibc-dynamic`. MicroHAD's
Mbed TLS and Matter's OpenThread archives remain statically incorporated;
system and C++ runtime libraries are shared. Dynamic executables use fixed-address
(non-PIE) code, with Rust static relocation and `crt-static` disabled; OpenThread
and compatibility objects also avoid PIC. The compatibility shim inspects shared-
library exports when choosing missing-API implementations. Firmware, RNG seed and maintenance
helpers retain static linkage. Deployment defaults continue selecting static
artifacts and their existing verifier requirements are unchanged.

Each dynamic executable receives an adjacent `.linkage.json` report. Verification
requires a non-PIE `ET_EXEC` executable and `/lib/ld-uClibc.so.0`, resolves
transitive dependencies within the selected RSDK, checks strong imports against
dependency exports, and retains executable
MIPS-I/O32, soft-float and forbidden-instruction checks. It does not prove symbol
version compatibility, loader execution or device kernel behavior. The selected
RSDK libraries must match the installed gateway runtime before any device trial.
No gateway is contacted during these builds.

After building both modes, compare stripped service artifacts and count shared
runtime files once:

```sh
python3 scripts/report-dynamic-linkage.py --output build/dynamic/linkage-summary.json \
  microhad/build/dynamic/mips-ziggurat-matter/microhad.linkage.json \
  build/dynamic/ziggurat-hdlc/lexra-stock-uclibc-dynamic/release/ziggurat.stripped.linkage.json \
  build/dynamic/micro-thread-matter-daemon/mthread-matter-mips.linkage.json
```

The report compares current static counterparts and counts uncompressed RSDK
runtime files. Installed library reuse and JFFS2 compression are not measured.
Dynamic linking does not guarantee a smaller release or lower memory use.
Loader-required dynamic symbols, including copy-relocated runtime data, remain
visible; removing the entire dynamic symbol table would break loading. A device
startup, threading, networking and TLS trial requires separate explicit authorization.

The initial experiment (Rust services built as PIE, stripped artifacts) was:

| Service | Static bytes | Dynamic bytes |
| --- | ---: | ---: |
| MicroHAD | 1,959,780 | 1,567,736 |
| Ziggurat multipan | 3,643,400 | 3,960,672 |
| Matter | 2,621,088 | 3,200,644 |

The dynamic executables total 8,729,052 bytes, an increase of 504,784 bytes.
Their combined RSDK runtime dependency set occupies 2,008,707 additional bytes
when counted once. These PIE results motivated disabling position independence.
Local ELF checks establish linking feasibility, not verified gateway runtime
compatibility.

After disabling PIE and PIC, the same dynamic artifacts measure:

| Service | Non-PIE dynamic bytes | Saved versus static bytes |
| --- | ---: | ---: |
| MicroHAD (already non-PIE) | 1,567,736 | 392,044 |
| Ziggurat multipan | 3,521,320 | 122,080 |
| Matter | 2,469,952 | 151,136 |

The executables total 7,559,008 bytes, saving 665,260 bytes versus static and
1,170,044 bytes versus the initial experiment. The shared runtime set is still
2,008,707 bytes; packaging all those SDK files gives 9,567,715 bytes total.
Ziggurat now defines only five linker/runtime dynamic symbols. Matter defines
those five plus two C++ runtime vtables with copy relocations; OpenThread
application symbols are no longer exported. Imports and loader-required dynamic
symbols are retained. No gateway runtime test or deployment has been performed.

## Experimental limits

The shared Rust shim supplies only APIs missing from the selected libc archives.
For LinuxThreads, `pthread_getattr_np` and `pthread_attr_getstack` return `ENOSYS`.
Stack-bound diagnostics and reliable stack-overflow reporting are not guaranteed.
`pthread_setname_np` supports the current thread only. Application builds require
the expanded libc configuration, rather than the initial minimal proof compiler.

The initial compiler proof passed bounded native and Rust/Tokio tests on the
stock gateway. Application compilation and ELF checks do not validate a complete
release on the device. RSDK remains the production default. Deployment defaults
remain on RSDK artifacts; a future explicitly requested candidate deployment must
pass all selected artifact paths and follow the existing maintenance flow.
