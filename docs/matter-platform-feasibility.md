# Matter platform

The factory gateway uses an old C++03 runtime. MicroHAD supplies the native
HTTP/WebSocket bridge and common device model; the Matter controller runs in
the separate Rust `mthread-matter` process with rs-matter and RustCrypto.
OpenThread provides userspace IPv6/UDP over Ziggurat's private Spinel socket.

The service builds for the factory MIPS-I, big-endian, o32, soft-float/uClibc
target using the shared toolchain settings. MicroHAD uses the selected C/C++
compiler: RSDK by default, or experimental crosstool-NG. See
[toolchain selection](../config/TOOLCHAINS.md). Do not substitute a host executable for a target artifact.

See [building](building.md), the [component README](../micro-thread-matter-daemon/README.md),
[runtime](matter-service-runtime.md) and [integration](../microhad/MATTER_INTEGRATION.md).
Both daemons use [CBOR IPC v2](../microhad/docs/matter-ipc-v2.md); target builds
and synthetic tests do not establish physical accessory acceptance.
