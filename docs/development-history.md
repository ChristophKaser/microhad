# Development history

The project began by establishing that a Rust Zigbee stack could run directly
on the Silvercrest gateway’s factory Linux. It grew into the standalone
MicroHAD, Ziggurat and `mthread-matter` stack. Current commands are in
[build setup](building.md) and [toolchain selection](../config/TOOLCHAINS.md).

## Gateway compatibility

The initial gateway tests on 2026-10-02 established that static Rust binaries
could run on the RTL8196E’s big-endian Lexra CPU with factory uClibc.
Arithmetic, allocation, threads, atomics, timers, randomness and loopback
networking passed after disabling native thread-local storage. Native TLS
had emitted an `rdhwr` instruction rejected by the stock kernel.

The maintained builds retain a custom MIPS-I/O32 soft-float target and
compatibility code for missing libc APIs. Rust uses LLVM for code generation;
selecting a GCC linker alone does not make a standard Rust MIPS target
compatible with Lexra. RSDK remains the default toolchain; source-built
crosstool-NG is an experimental alternative.

## Shared radio

Ziggurat supplies the host Zigbee stack without requiring Silicon Labs’
architecture-specific host Zigbee libraries. The shared-radio design uses
two-instance Spinel over HDLC: Ziggurat owns the UART and routes Zigbee and
Thread traffic to separate radio interface IDs. OpenThread runs inside
`mthread-matter`, providing the Matter controller’s IPv6/UDP transport.

The radio firmware is rebuildable from SDK sources and precompiled RAIL
libraries. See [radio firmware](../firmware/README.md) and
[HDLC multi-PAN usage](hdlc-multipan-usage.md).

## Early performance measurements

On 2026-10-02, Ziggurat running on the gateway against a host-simulated radio
used about 2.2 MiB RSS and averaged 11.3% CPU during a 25-second query and
channel-change test. A short load sweep kept up at 20 transmitted broadcasts
and 20 injected beacon requests per second; higher rates increased CPU use
and delayed beacon replies.

These were synthetic measurements of an earlier build, not current capacity
limits or tests of real radio delivery, routing or concurrent Thread traffic.
Factory uClibc uses LinuxThreads, so CPU accounting must include worker PIDs;
shared memory must not be summed across them. See
[Ziggurat’s synthetic tests](../ziggurat/README.md) for maintained tooling.
