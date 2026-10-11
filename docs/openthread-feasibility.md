# OpenThread on factory Linux

OpenThread runs inside `mthread-matter`, providing IPv6/UDP over Ziggurat's
private HDLC Spinel socket. The factory kernel does not supply the IPv6/TUN
interfaces required for a conventional Linux border router. Matter uses the
internal stack; OTBR is outside this project's runtime.

## Build the standalone CLI

The CLI is a diagnostic/probe artifact, separate from the production service:

```sh
bash micro-thread-matter-daemon/scripts/build-openthread.sh host
bash micro-thread-matter-daemon/scripts/build-openthread.sh mips
```

Default output is `build/openthread-unix/ot-cli-mips`. RSDK builds use Clang 18
and the RSDK linker/sysroot; experimental crosstool-NG builds use native GCC/G++
and isolated outputs. See [toolchain selection](../config/TOOLCHAINS.md).
Build scripts apply the Unix adapter and compatibility patches in an ignored
worktree and check static, big-endian MIPS-I/o32, soft-float artifacts.

## Attach to the shared radio

The adapter accepts an absolute Unix HDLC URL ending in `?no-reset`. It uses
logical IID 0, which Ziggurat maps to radio IID 2. It does not open the UART or
reset the RCP. The service supplies private persistent OpenThread settings;
standalone fixtures use isolated temporary settings.

[Shared-radio policy](hdlc-multipan-usage.md) governs channel/reset ownership.
[UDP transport](rs-matter-openthread-transport.md) documents datagram limits,
queueing and lifecycle. Runtime Spinel compatibility checks remain enabled.

## Probe and measure

```sh
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh host
bash micro-thread-matter-daemon/scripts/build-openthread-relay.sh mips
python3 micro-thread-matter-daemon/scripts/test-openthread-device.py
```

Without `--host`, the runner validates host-side preparation. Supplying a host
explicitly stages/runs a bounded target probe against a synthetic RCP; it requires
authorized device testing. See [resource profiling](openthread-resource-profile.md)
for measurement commands and interpretation. CLI simulation does not establish
Matter performance, physical mesh compatibility or border routing.
