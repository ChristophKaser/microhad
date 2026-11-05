# RSDK-free stock-runtime probes

This is an isolated compatibility investigation. It does not enable the
`crosstool-ng + dynamic` combination in application build selectors, install
libraries, change workspace settings, or restart services.

From the physical repository root:

```sh
python3 scripts/probe-stock-dynamic.py
# Explicitly run only validated binaries on a selected gateway:
python3 scripts/probe-stock-dynamic.py --device GATEWAY_IP
# Repeat only the Rust investigation:
python3 scripts/probe-stock-dynamic.py --only rust
# Link the existing production shim, compiled with GCC 15:
python3 scripts/probe-stock-dynamic.py --only rust --rust-compat --device GATEWAY_IP
```

The builder uses GCC/G++ 15.2 from `CROSSTOOL_NG_ROOT`, its headers and startup
objects, and the stock shared libraries under `STOCK_ROOTFS`. Both settings
come from the shared workspace loader. It creates a private directory under
ignored `build/`, with its own link directory: it never borrows the old RSDK
`libutil.a` link. Compiler support archives may be statically incorporated;
all **shared** dependencies must belong to the recorded stock runtime.

Each output directory contains `report.json`, compiler/link traces, link maps,
C/C++ header traces, and any runtime output. The report records compiler
identities, commands, target configuration, stock library hashes, validation
results, build failures and missing symbols. Exit zero means the investigation
finished, not that every probe is compatible: inspect each probe's `build_exit`,
`verified`, and, when requested, `run_exit`.

Before a device run, the harness checks ELF32 big-endian MIPS-I/O32, soft float,
non-PIE linkage, the stock loader, dynamic imports and symbol versions, and the
absence of unsupported `rdhwr`/`teq` instructions and RSDK paths. It compares the
required installed library hashes with the pinned copies. Validated executables
run under unique `/tmp` filenames, with a 20-second watchdog, then are removed.
Probes use local socket pairs, not the radio or external network traffic.

## Results on the stock gateway, 2026-11-05

| Probe | Build | Gateway execution |
|---|---|---|
| C: allocation, pthread/mutex, clocks, sockets, interface enumeration | Pass | Pass |
| C++: old string ABI, strings/vectors, allocation, exception unwinding | Pass | Pass |
| C++: `vector::at()` bounds check | Link fails | Not run |
| Rust standard library without compatibility shim | Link fails | Not run |
| Rust with production compatibility shim compiled by GCC 15 | Pass | Pass |

The bounds-checking probe needs `std::__throw_out_of_range_fmt`, absent from the
stock libstdc++. Selecting `_GLIBCXX_USE_CXX11_ABI=0` and disabling sized
allocation do not make every GCC 15 header compatible with that old runtime.
The Rust probe needs `pthread_getattr_np`, `pthread_attr_getstack` and
`pthread_setname_np`, absent from the stock pthread library. It intentionally
uses no compatibility shim by default. `--rust-compat` explicitly compiles the
existing `ziggurat/stock-uclibc-compat/native/stock_compat.c` with GCC 15, enabling
only wrappers absent from the stock libc/pthread/realtime libraries. Its source
hash, compiler command, enabled wrappers and header trace are saved in the report
and output directory. The resulting object is linked directly into the Rust
executable; no replacement shared library is installed.

The shim-enabled Rust probe passed on the gateway, including creation of a
named thread. The stack-inspection wrappers still return `ENOSYS`, as in the
production build; they do not add native stack-bound inspection. The naming
wrapper uses the kernel syscall for the current thread. This removes the three
missing pthread symbols as blockers for the tested Rust operations, while the
unshimmed baseline continues to expose the stock runtime limitations.

The successful probes demonstrate that GCC 15 can generate working dynamic
code for the stock runtime without RSDK. They do not establish compatibility
for entire services, all C++ library operations, threading edge cases, or all
uClibc-ng headers versus the older stock ABI. Follow-up application enablement
must resolve these blockers, preserve the same runtime provenance checks, and
validate real workloads before deployment.
