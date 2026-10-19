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
