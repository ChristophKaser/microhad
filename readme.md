# Ziggurat on the Tuya RTL8196E gateway

This repository investigates compiling and running the open-source Rust
[Ziggurat](https://github.com/zigpy/ziggurat) Zigbee stack on the gateway's
big-endian Lexra CPU. The aim is a standalone host stack using OpenThread RCP
firmware on its EFR32MG1 radio, without Silicon Labs' precompiled host Zigbee
stack libraries.

## Source projects

Sources are pinned Git submodules:

- `ziggurat/`: upstream Rust Zigbee stack and Spinel radio transport.
- `rtl8196e-gateway/`: modern Lexra GCC/binutils/musl toolchain, kernel patches,
  and board-specific OpenThread RCP firmware.

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
  Lexra. Support on the stock kernel and its performance remain unverified.
- The inspected Ziggurat workspace requires Rust 1.98 and its host server
  uses Tokio. Its protocol core supports `no_std`. Its client protocol is
  different from EZSP; native gateway or Zigbee2MQTT integration is separate
  work from compiling the stack.

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
prototype-only C shim supplies missing `accept4` and `clock_nanosleep` wrappers
using the RSDK headers' syscall numbers, and `pthread_setname_np` for the current
thread using `prctl`. Errors are preserved; socket flags are passed atomically
to the kernel. The shim supports neither naming another thread nor a name
longer than 15 bytes. A diagnostic constructor reports the PC of SIGILL using
an async-signal-safe write and exits without a core dump.

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

## Next gates

- Build a small Tokio/serial/entropy-dependency probe with this target. Passing
  `std` does not prove Ziggurat's third-party dependencies work on Linux 3.10.
- Try compiling Ziggurat, then measure resident memory and CPU use; the simple
  prototype's success is not a production-readiness claim.
- Validate RCP firmware, UART flow control and client integration separately,
  with explicit authorization and a backup before any firmware change.
- Build the modern Lexra/musl toolchain if old-libc compatibility becomes too
  restrictive. Keep the successful stock-RSDK path as a reproducible baseline.

## References

- [Gateway build environment](https://github.com/jnilo1/rtl8196e-gateway/tree/main/1-Build-Environment)
- [OpenThread RCP firmware](https://github.com/jnilo1/rtl8196e-gateway/tree/main/2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP)
- [Rust MIPS target definition](https://github.com/rust-lang/rust/blob/master/compiler/rustc_target/src/spec/targets/mips_unknown_linux_musl.rs)
- [GCC backend for rustc](https://github.com/rust-lang/rustc_codegen_gcc)
