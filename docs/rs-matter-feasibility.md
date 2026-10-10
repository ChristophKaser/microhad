# Rust Matter controller

The gateway uses the pinned rs-matter controller with RustCrypto in
`micro-thread-matter-daemon/`. It builds for the development host and the
factory big-endian MIPS/uClibc target. The executable is `mthread-matter`.

The [component README](../micro-thread-matter-daemon/README.md) contains source
pins, build commands and test entrypoints. Build scripts apply the tracked
controller patches to an ignored build worktree, preserving the upstream
submodule. Cargo lockfiles pin the dependency graphs.

Use the [runtime contract](matter-service-runtime.md),
[commissioning fixture](rs-matter-openthread-commissioning.md) and
[CBOR IPC contract](../micro-thread-matter-daemon/docs/matter-ipc-v2.md). Synthetic protocol tests
cover PASE, provisioning, CASE, reads, commands and report delivery. Physical
accessory interoperability and sustained target resource/RF acceptance remain
open; see the [acceptance plan](../microhad/docs/matter-plans/08-real-device-acceptance.md).
