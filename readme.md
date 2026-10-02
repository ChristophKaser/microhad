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
flash firmware. Device upload/execution and firmware changes are separate
steps requiring a planned backup and explicit authorization.

Toolchains, caches, generated target specifications and build logs live in
ignored directories. Credentials, device configuration and network keys must
never be copied into this repository.

## References

- [Gateway build environment](https://github.com/jnilo1/rtl8196e-gateway/tree/main/1-Build-Environment)
- [OpenThread RCP firmware](https://github.com/jnilo1/rtl8196e-gateway/tree/main/2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP)
- [Rust MIPS target definition](https://github.com/rust-lang/rust/blob/master/compiler/rustc_target/src/spec/targets/mips_unknown_linux_musl.rs)
- [GCC backend for rustc](https://github.com/rust-lang/rustc_codegen_gcc)
