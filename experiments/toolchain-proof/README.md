# Stock gateway compiler proof

This isolated experiment evaluates a source-built replacement for RSDK while
keeping the stock Linux kernel. Application build selection now uses `TOOLCHAIN=rsdk|crosstool-ng`; see
[shared toolchain selection](../../config/TOOLCHAINS.md). The
production default remains RSDK until application compatibility is established.

The candidate uses crosstool-NG 1.28.0, GCC 15.2.0, binutils 2.45.1,
uClibc-ng 1.0.54 with LinuxThreads, and Linux 3.10.90 headers. Native TLS and
time64 are disabled to match the stock kernel and Rust uClibc ABI. Floating-point
exception handling is disabled because this MIPS soft-float configuration does
not provide its required exception constants. Ordinary soft-float arithmetic
and `sqrt` are exercised by the probes. GCC and
binutils use the Lexra patches from the pinned RTL8196E dependency. The
binutils source is patched before being passed to crosstool-NG as a custom
source because this crosstool-NG release lists binutils 2.45, not 2.45.1.

## Build and inspect

Run from the repository root on an x86_64 Linux build host. Allow at least
12 GiB free disk space. The host needs GCC/G++, make, autoconf, automake,
libtoolize, flex, bison, gperf, makeinfo, rsync, patch, curl, tar, and Python
3.11 or later. The script installs help2man, GNU libtool, and crosstool-NG
locally under `tools/crosstool-ng-proof`, without system installation or changes
to the user's home directory.

```sh
python3 experiments/toolchain-proof/build-toolchain.py
python3 experiments/toolchain-proof/build-probes.py host
python3 experiments/toolchain-proof/build-probes.py rsdk
python3 experiments/toolchain-proof/build-probes.py crosstool-ng
```

The source-built candidate installs under `tools/lexra-proof`. Source archives,
work directories, manifests, disassembly, and reports stay under
`build/toolchain-proof`. Sources have pinned SHA256 checksums in `sources.json`.
The toolchain script removes inherited compiler/include/library overrides.
It reuses a completed build only when its configuration and source fingerprints
match. It does not resume partial builds; rerunning starts crosstool-NG again.

Probe builds use the existing repository Rust installation, pinned nightly,
`llvm-objdump-18`, and `llvm-strip-18`. Cargo dependencies are locked and built offline from the
local cache. On a fresh checkout, populate that cache with:

```sh
CARGO_HOME="$PWD/tools/cargo" RUSTUP_HOME="$PWD/tools/rustup" \
  tools/cargo/bin/cargo fetch --locked \
  --manifest-path experiments/toolchain-proof/rust/Cargo.toml
```

RSDK is required only for the baseline. Its location defaults to the shared
configuration and can be overridden with `--rsdk-root PATH`. The candidate
uses its own headers and libraries. Rust still uses LLVM MIPS-I code generation;
GCC is its linker driver, not its code-generation backend.

ELF verification requires big-endian ELF32/MIPS-I/O32 and static linkage.
Disassembly is decoded with a standard MIPS32r2 instruction decoder to detect
`rdhwr` even when the old Realtek disassembler labels it differently. The
verifier rejects `rdhwr` and `teq`, and records other Lexra-sensitive instructions.
Reports include stripped and unstripped sizes. The device runner verifies binary
hashes against the build report before making any connection.
Instruction counts alone do not establish runtime compatibility.

## Bounded gateway test

```sh
python3 experiments/toolchain-proof/run-device.py \
  --backend rsdk --password-file ../ssh_password.txt
python3 experiments/toolchain-proof/run-device.py \
  --backend crosstool-ng --password-file ../ssh_password.txt
```

The runner defaults to `192.168.178.62`, requires a completed local build report,
and uses strict SSH host-key checking with the stock server's RSA algorithm.
It creates a private, unpredictable directory under `/tmp`, transfers only the
two probes, disables core dumps, and limits each program to 60 seconds. It
captures output and exit status locally and removes the directory afterward.
The C probe installs an alarm; the remote runner also supplies a watchdog.
No services are restarted and no radio, flash configuration, or network state
is accessed. Localhost sockets use ephemeral ports; native Unix listeners use
the abstract namespace.

The proof passes only when host and RSDK baselines pass, the candidate passes
local inspection, and every candidate checkpoint passes on the stock gateway.
This is a prerequisite for application migration, not validation of a complete
MicroHAD/Ziggurat/Matter release.

## Initial minimal-configuration result: 2026-10-09

**The first proof passed on the stock Linux 3.10.90 gateway.** Both native and
Rust/Tokio probes completed every checkpoint with exit status 0 for RSDK and
the source-built candidate. Host baselines also passed.

| Stripped probe | RSDK GCC 4.4.7 | crosstool-NG GCC 15.2.0 |
| --- | ---: | ---: |
| C/C++ | 235,660 bytes | 320,896 bytes |
| Rust/Tokio | 690,112 bytes | 742,980 bytes |

Both candidate binaries are static big-endian ELF32/MIPS-I/O32 with soft-float
ABI attributes. Neither contains `rdhwr` or `teq`. The native candidate contains
55 `sync` instructions and the Rust candidate 39; both contain `ll/sc` and
unaligned load/store instructions. The exercised paths run successfully on
the stock kernel, but this is not exhaustive instruction or application coverage.

The candidate's sysroot and libc, pthread, libstdc++, and libm archives all
resolve beneath `tools/lexra-proof`. No RSDK headers, libraries, or compiler
tools were used for its build. Repeating the toolchain builder reused the
completed compiler successfully. Its successful source build took 7 minutes
59 seconds on this host, after correcting the time64 and FENV configurations.

RSDK needs the existing `epoll_create1`, `eventfd`, `accept4`,
`clock_nanosleep`, and `pthread_setname_np` wrappers. The candidate supplies
the first four itself; its probe shim supplies `pthread_setname_np` and `bcmp`.
The candidate's LinuxThreads implementation also lacks `pthread_getattr_np`
and `pthread_attr_getstack`. The experiment returns `ENOSYS` for those two
interfaces, which Rust std explicitly handles by skipping stack-bound
diagnostics. This is an intentional limitation, not an implementation of those
APIs. Stack-overflow behavior was not tested. Resolve or explicitly retain this
limitation when evaluating the production build switch.

Detailed compiler/library provenance, source hashes, disassembly, and device
logs are under `build/toolchain-proof`. Temporary gateway files were removed.
The experiment's `ct-work` directory can be removed after a successful build;
the installed toolchain, source archives, reports, and probe caches suffice to
reuse the compiler and rerun the proof.

The application integration corrects the SUSv3/SUSv4 configuration option names
and enables `UCLIBC_SUPPORT_AI_ADDRCONFIG` for OpenThread interface enumeration.
The builder verifies these installed features. The size measurements and gateway
results above describe the original minimal configuration; the expanded libc
requires fresh application runtime validation before a release. The build switch
remains experimental and changes no production default.
