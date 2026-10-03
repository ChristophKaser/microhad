# Optional Matter-over-Thread integration

## Current result: build selection implemented, target platform gate blocked

The opt-in build flag and reproducible compiler prerequisite checks are
implemented. The factory C++ runtime does not meet the pinned Matter SDK's
requirements. **No Matter controller, commissioning API, Thread service,
dashboard controls, or Garage BLE relay has been implemented or deployed.**
`MATTER=1` intentionally fails the build rather than producing an executable
that claims unsupported Matter functionality. Existing Zigbee builds remain
the usable configurations.

This follows the agreed implementation sequence: verify the MIPS controller
platform before implementing commissioning UI or enabling real-radio operation.
It is a compiler/library blocker for the pinned C++ SDK, not evidence that
Matter cannot fit on this hardware. The separate Rust feasibility probe below
now validates crypto/runtime on MIPS. Complete controller memory/CPU, discovery
and real-device operation remain unmeasured.

## Build contract

`MATTER` accepts exactly `0` or `1`, defaults to `0`, and defines
`MICROHAD_MATTER_ENABLED` for native host and target C++ compilation.
`MATTER=1` requires `ZIGGURAT=1`, because the planned Thread radio endpoint is
provided by Ziggurat. Empty, unknown, and multi-value flags are rejected.

| Selection | Host output directory | MIPS output directory | Availability |
| --- | --- | --- | --- |
| `ZIGGURAT=0 MATTER=0` | `build/host` | `build/mips` | Existing EZSP backend |
| `ZIGGURAT=1 MATTER=0` | `build/host-ziggurat` | `build/mips-ziggurat` | Existing Ziggurat backend |
| `ZIGGURAT=1 MATTER=1` | `build/host-ziggurat-matter` | `build/mips-ziggurat-matter` | Reserved; build gate rejects unavailable backend |

Matter-disabled builds do not fetch, compile, or link the Matter SDK. Switching
the flag cannot reuse enabled and disabled native artifacts. Native binaries
and tests also depend on the Makefile so changes to feature definitions trigger
recompilation. Existing deployment defaults continue to select the EZSP binary;
no launcher or deployment behavior has been changed.

```sh
make ZIGGURAT=0 MATTER=0 host test
make ZIGGURAT=0 MATTER=0 mips
make ZIGGURAT=1 MATTER=0 host test
make ZIGGURAT=1 MATTER=0 mips
make matter-build-flags-test
```

Run the two backend test suites sequentially: some integration tests share
temporary diagnostics paths. Concurrent suites caused a redaction assertion
failure during validation; isolated verification is recorded below.

## Reproducing the platform gate

The companion `ziggurat-compilation` repository holds the probes and SDK pin:
Matter SDK `v1.6.1.0`, commit
`3bcdd56ba54fb88b2afb4bfef575014671df7aa7`. Its sparse checkout lives in ignored
build storage. Probe execution never contacts the gateway.

```sh
cd ../ziggurat-compilation
bash scripts/fetch-matter-sdk.sh
python3 scripts/check-matter-platform.py host
python3 scripts/check-matter-platform.py mips
cd ../microhad-zigbee-native
make matter-platform-check
make ZIGGURAT=1 MATTER=1 mips
```

The host probe compiles, links, and executes an actual Matter `TypeTraits.h`
consumer and a small program using the controller's standard-library
requirements. The target probe tests both stock RSDK GCC and Clang 18 using
the factory headers and MIPS-I/o32/big-endian/soft-float flags.

Observed target failures:

- Stock GCC rejects `-std=c++17`.
- Clang compiles the newer language but the actual SDK header fails because
  the stock library lacks `std::underlying_type_t`.
- The controller requires `std::optional`; the stock `<optional>` header is
  absent. Stock `<atomic>` is also absent.

Clang was enough for the earlier C++11 OpenThread build, but it does not supply
a replacement C++ standard library. Host C++ headers/libraries cannot be used
as deployable MIPS libraries. The next prerequisite is a compatible modern C++
library/ABI runtime for the factory uClibc system, followed by full SDK linking
and target execution. It must retain the gateway's instruction and soft-float
constraints rather than selecting a modern MIPS ISA by accident.

The native Makefile exposes `MATTER_PLATFORM_CHECK`, `MATTER_SDK_ROOT`, and
`RSDK_ROOT` overrides for checkout/toolchain paths. These overrides do not
enable a backend. Even after host compiler prerequisites pass, enabled native
builds explicitly stop because the actual controller integration is pending.
The eventual implementation must replace that stop with verified daemon
build/package prerequisites, not simply delete it.

Compiler evidence and full probe commands are recorded in the companion
[Matter platform report](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/matter-platform-validation.json).

## Rust alternative: probe runs on the gateway

The companion repository now pins rs-matter 0.4.1 at
`1570356ecb938a9d19be032b93ca3bb487069e75`, builds with RustCrypto against
factory uClibc, and runs an isolated crypto/TLV/runtime probe on the gateway.
All checks pass, including SPAKE2+ agreement and tamper rejection, in 1.68 s.
The stripped static executable is 587,596 bytes; sampled RSS peaks at 300 KiB
and sampled MemFree stays above 2.2 MiB. Production Zigbee remains online and
formed; the staged binary is removed. One-second samples can miss peaks, and
this does not measure a complete controller or sustained CPU utilization.

Build commands, dependency lock, test-only visibility patch, source pin and
shareable measurements are recorded in the companion
[Rust feasibility report](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/rs-matter-feasibility.md).
The probe does not commission devices or implement OpenThread UDP, discovery,
Garage BLE relay, persistence, CASE or native IPC. The native Makefile still
uses the C++ prerequisite checker and intentionally rejects `MATTER=1` until
the chosen controller and its actual integration are verified. A Rust runtime
must replace that prerequisite contract with its own verified build/package
checks; a successful probe alone does not justify deleting the stop.

## Controller simulation follow-up

The companion host harness now checks UDP/TCP PASE commissioning, fabric
installation, CASE, light read/invoke, signed/null temperature reads and stale
sessions. It uses a development fixture and stub discovery, not a Thread mesh.
Source inspection confirms the pinned commissioner skips device attestation
when its test bypass is enabled and does not provision a Thread dataset.
Secure defaults reject commissioning; no native production path enables the
bypass. Existing declaration/trust-store components must be connected to a
complete verifier before real-device pairing, along with NetworkCommissioning
and the OpenThread/BLE transports. The trusted-LAN HTTP/WebSocket requirement
remains independent of Matter device attestation trust.

Reproduction, dependency lock and measured test counts are in the companion
[controller test report](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/rs-matter-controller-tests.md).

## Remaining implementation contract

After a controller implementation passes its complete platform gate:

1. Build a local `thread-matter` daemon containing OpenThread and the Matter
   controller. Keep the native bridge on its RSDK C++03 build and use private
   Unix-socket IPC. Implement the userspace networking/discovery, timers,
   entropy, crypto, and atomic credential storage platform. Prove controller
   startup and crypto on MIPS; preserve at least 1 MiB MemFree during bounded
   combined-service testing.
2. Enable Ziggurat's private Thread HDLC socket, logical IID 0 to physical IID 2.
   Ziggurat remains the UART/reset owner. Reconcile OpenThread initialization
   with shared reception/filter/source-match policy. Create or import Thread
   datasets only on the current Zigbee channel; reject incompatible imports
   before changing state. No kernel IPv6/TUN is required by the chosen internal
   OpenThread transport.
3. Extend the separate Garage Android app with setup code/QR input and BLE
   discovery/GATT. Relay operations, MTU, confirmations, subscriptions,
   indications, and disconnects through one HTTP/WebSocket session. The gateway
   owns PASE, device attestation, Thread provisioning, and its Matter fabric.
   One commissioning session runs at a time with cancellation and timeout.
4. Treat the LAN as trusted, as explicitly requested: use `http://`/`ws://`
   without assuming a publicly trusted or installed gateway certificate. Do
   not require certificate import/pinning and do not bypass TLS validation
   globally. Preserve Matter attestation and secure sessions independently.
   Session identifiers correlate relay traffic; they are not an administrator
   authentication scheme.
5. Add asynchronous `/api/matter` commissioning start/status/cancel, the BLE
   WebSocket, and `/api/thread` status/create/import. Expose compiled capability
   in protected snapshots and hide setup in disabled builds. Use coordinator
   queues for native mutations; no HTTP worker directly calls `Gateway`.
6. Discover Matter endpoint capabilities and use `source="matter"` with stable
   fabric/node/endpoint string IDs. Support lights/plugs and sensor reports,
   including ALPSTUGA temperature, humidity, CO2, PM2.5, and air quality where
   advertised. Integrate supported values with dashboard, bounded history,
   alerts, and notifications, preserving null/unknown values and units.
7. Persist Thread/Matter security state atomically with private permissions;
   do not log setup codes or credentials. Restore sessions/subscriptions after
   daemon/gateway restart. The phone is needed only during BLE commissioning.
   Gate deployment on factory-new ALPSTUGA pairing, actual reports, operation
   with the phone disconnected, restart recovery, combined Zigbee traffic,
   CPU/RSS, free RAM, and flash-write measurements.

Stage disabled and retain backups before any real-radio acceptance test.
Commit verified native, Garage, and compilation changes in their respective
repositories. No certificate, key, configuration, dataset, or production state
belongs in those commits.

## Verification record

The build-flag test covers disabled defaults, valid selections, separated
artifact paths, compiler definitions, rejected inputs, and the enabled MIPS
binary's mandatory platform-gate dependency. Host compiler prerequisite probes
passed; both factory target compiler variants failed as described above.
Enabled host and target native builds were checked to reject unavailable
support; neither produced a native executable in the reserved Matter output
directories. Verification passed:

- `make matter-build-flags-test`, including rejection of wildcard flag inputs.
- `make ZIGGURAT=0 MATTER=0 host test` in isolation.
- `make ZIGGURAT=1 MATTER=0 host test` in isolation, including all 13 native-peer
  tests and rejection of a host executable by the target verifier.
- `make ZIGGURAT=0 MATTER=0 mips` and
  `make ZIGGURAT=1 MATTER=0 mips`, with the factory RSDK and target ELF checks.
- Python syntax checks, fetch-script shell syntax, clean SDK-pin fetch,
  and diff whitespace checks.

The initial parallel backend test run failed at the EZSP diagnostics-redaction
assertion while the Ziggurat suite was running. Sequential reruns passed; no
runtime source was changed to suppress the assertion.

## Owner-selected policy and Thread provisioning implementation

The owner explicitly selected skipping manufacturer attestation. The shared
Rust controller policy now defaults `verify_manufacturer_attestation=false`
and maps it to upstream's bypass; PASE/CASE authentication and encryption stay
enabled. Selecting verification fails closed until its optional verifier is
implemented. The verifier is no longer a prerequisite for this setup. This
choice is separate from the trusted-LAN HTTP/WebSocket transport policy.

A shared Rust helper now sends an active Thread dataset through
`AddOrUpdateThreadNetwork` and then `ConnectNetwork` over PASE. It validates
dataset structure and the shared Zigbee channel before sending credentials,
rejects failure responses, and has no credential logging/persistence. Host
UDP/TCP fixtures exercise success and failure paths; the module also builds
for factory MIPS. The native daemon has no callable Matter endpoint yet: the
OpenThread adapter, native IPC, Garage BLE and real-device acceptance remain
in progress. The native build gate is retained until those paths are usable.
