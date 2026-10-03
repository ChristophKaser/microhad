# Rust Matter controller feasibility experiment

The Rust alternative bypasses the factory C++ standard-library blocker: the
actual rs-matter library builds with RustCrypto on both the development host
and the custom factory-uClibc MIPS target. The expanded probe passes its
cryptography, SPAKE2+, TLV and initialization checks on the host and gateway. This is a prerequisite
experiment, not a complete controller integration or a real-device
commissioning result. Native `MATTER=1` remains deliberately unavailable.

## Source and build contract

Upstream: [project-chip/rs-matter](https://github.com/project-chip/rs-matter),
version 0.4.1, pinned commit `1570356ecb938a9d19be032b93ca3bb487069e75`.
The probe's tracked `Cargo.lock` fixes its dependency graph. The upstream
checkout remains clean in ignored `build/rs-matter/source`; a separate worktree
in `build/rs-matter/probe-source` applies
`patches/rs-matter-probe-spake2p.patch`. That patch only exposes the otherwise
private SPAKE2+ module to the probe. It changes no cryptographic implementation
and is not a proposed production API.

The actual upstream commissioner type is compiled and its size is printed.
The probe does not instantiate or run a commissioner, exchanges, or discovery.
Matter state initialization uses upstream public development fixtures, never
the gateway's production credentials. New P-256 keys use `/dev/urandom`;
entropy read failures abort. Keys and derived secrets are never printed or
persisted.

rs-matter default features are disabled; only `rustcrypto` is enabled. The
standalone probe uses host/std implementations of `critical-section` and
`embassy-time` for platform hooks. It therefore is **not** a no-std gateway
controller measurement, despite upstream supporting embedded configurations.
No factory C++ library, Mbed TLS configuration change, or dependency source
compatibility patch was needed for this Rust build.

## Reproduce

Use the existing local Rust installation from `scripts/rust-env.sh`, pinned
`nightly-2026-10-02`, and the sibling stock RSDK described in
[building.md](building.md). Commands below run on the development computer:

```sh
bash scripts/build-rs-matter-probe.sh host
build/rs-matter/target/release/lexra-rs-matter-probe
bash scripts/build-rs-matter-probe.sh mips
llvm-strip-18 --strip-all --keep-section=.gnu.attributes \
  -o build/rs-matter/probe-mips \
  build/rs-matter/target/lexra-stock-uclibc/release/lexra-rs-matter-probe
file build/rs-matter/probe-mips
llvm-readelf-18 -h build/rs-matter/probe-mips
llvm-size-18 build/rs-matter/probe-mips
```

The build script regenerates the custom target: big-endian MIPS-I, o32,
soft-float, 32-bit atomics, factory uClibc, static linking. Native ELF TLS is
disabled to avoid `rdhwr`; LLVM division traps are disabled with
`-mno-check-zero-division` to avoid Lexra's unsupported `teq`. The latter
setting is now in the tracked target generator rather than only the generated
JSON. Rust flags also select compact software SHA2/AES implementations.
LLVM warns that MIPS-I support is experimental. The old GCC driver reports
`-no-pie` as unrecognized, but linking completes and the result is a static
ELF EXEC, not a position-independent shared object.

## Checks and measured artifacts

The final host run completed in 7 ms and passed:

- SHA-256 `abc`, RFC 5869 HKDF and PBKDF2-SHA256 known answers.
- AES-CCM encrypt/decrypt roundtrip and modified-ciphertext rejection.
- P-256 ECDSA signature verification and modified-message rejection; ECDH
  shared-secret agreement with generated keys.
- Both sides of SPAKE2+, including matching derived keys and rejection of an
  invalid confirmation. The PBKDF request/response transcript is synthetic;
  this does not test a PASE packet exchange or BLE framing.
- Matter TLV roundtrip for a signed temperature value and an unsigned CO2
  value, plus Matter runtime initialization.

ECDSA verification returns `Result<bool, Error>`: successful verification must
check `true`, and a modified message must check `false`. An initial probe
incorrectly expected an error for a bad signature; the probe was corrected
against the upstream API before recording the successful run.

Host type sizes: `Matter` 26,008 bytes, `Commissioner` 48 bytes. These are
individual type sizes, not total heap, stack, RSS or controller capacity.

MIPS executable: 767,064 bytes before stripping; 587,596 bytes after stripping.
Stripped SHA256:
`37973773568a9e8d448342e8d02ec711a4aea35e0d18e9a7cf3a284b71e7c18e`.
ELF section sizes before stripping: text 550,780, data 33,532, BSS 35,768 bytes.
Release profile uses size optimization, fat LTO, one codegen unit and aborting
panics. Unused upstream controller code may be removed by the linker; this
artifact's size is not the size of a complete commissioned-device controller.

## Gateway test procedure and limits

The test runner stages only a new uniquely named binary in
`/tuya/native-test`, requires the existing bridge to be online and formed,
checks free RAM and flash before upload, and removes the binary afterward.
It bounds virtual memory to 4 MiB and runtime to 40 seconds, samples RSS and
MemFree once per second, and stops if MemFree drops below 1 MiB. It neither
opens the UART nor creates network sockets, modifies services, or accesses
production fabric/Thread/Zigbee state. The SSH password file remains untracked
and is passed directly to `sshpass` without reading or printing it.

With explicit authorization for an in-device test:

```sh
python3 scripts/test-rs-matter-device.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt
```

The test's generated report is under ignored `build/rs-matter/`; the shareable
record is [rs-matter-validation.json](rs-matter-validation.json). One-second
RSS sampling can miss short peaks. The runner measures a short crypto-only
process, not sustained controller CPU utilization.

## Gateway result

The final test on `192.168.178.62` passed every check, exited with status 0,
and completed in 1,680 ms. Sampled RSS was 292–300 KiB; sampled MemFree was
2,228–2,236 KiB, from 3,076 KiB before the test. Gateway type sizes were
`Matter` 25,328 bytes and `Commissioner` 24 bytes. The short duration and
one-second sampling do not establish peak RSS or sustained CPU load.

The first preflight command used `tail -1`, which this old BusyBox rejects;
the runner now uses `tail -n 1`. Another preflight declined to stage the binary
because available RAM was below the conservative 3 MiB threshold. The final
run passed that threshold. No production service was stopped to free memory.
The original production status was preserved (`online=true`,
`network_formed=true`, `coordinator_backend=ziggurat`), and the temporary binary
was removed. No real Thread device, radio, PASE transport, certificate chain
or commissioned fabric was exercised.

## Remaining gates

Implement and test the OpenThread internal IPv6/UDP transport adapter, timers,
discovery, BLE transport framing/flow control, credential persistence, full
PASE commissioning, attestation, certificates/CASE, and Interaction Model
reads, invokes and subscriptions. Then connect the native bridge through
private Unix IPC and the Garage BLE relay through trusted-LAN HTTP/WebSocket,
without requiring a trusted gateway certificate. Matter attestation and session
security remain required.

Keep Ziggurat as sole UART/reset owner, and Thread on the existing Zigbee
channel. Validate a factory-new ALPSTUGA, actual reports, operation after phone
disconnect, restart recovery and combined Zigbee/OpenThread/Matter CPU/RAM.
No such acceptance or native/Garage integration is established by this probe.
