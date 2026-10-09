# Optional Matter-over-Thread integration

The current source component is `micro-thread-matter-daemon/`, its executable
is `mthread-matter`, and private IPC uses [CBOR v2](docs/matter-ipc-v2.md).
Only a two-byte little-endian length precedes the CBOR envelope. There is no
reserved field; unknown fields are skipped with safe warnings. Structural bounds
are 16 nested containers, 256 map pairs and 512 array entries, within the
2048-byte envelope limit. The rename and v2 migration have been verified locally
and have not been deployed. Gateway observations and older framing descriptions
below record earlier releases, including JSON IPC v1; they do not describe the
currently built wire format. A release must update both daemons together using
the documented maintenance deployment.

## Current result: gateway Matter service and Thread are running

The opt-in native build, Rust Matter/OpenThread service, private IPC, Garage
BLE relay, Thread lifecycle, persistent controller identity, operational
discovery, bounded reads/commands, shared device model and dashboard controls
are implemented with synthetic host evidence. The Matter-aware paired launcher
is active on the development gateway, and the production Rust service is
available through the native gateway. The service created and attached a
private Thread dataset on channel 11 while the existing Zigbee network remained
online and formed. No accessory has been commissioned yet; live device
compatibility, service-process CASE report delivery, and physical acceptance
remain open. Plan 08 remains excluded.

Matter deployment is explicitly opt-in. `deploy.sh --matter-service PATH`
stages the service and paired supervisor without changing the active launcher.
`--activate-paired` installs the paired launcher with the package;
`--activate-paired-only` checks local hashes against an already staged package
and switches the launcher without copying the large artifacts again. The paired
launcher enables Matter only when `/tuya/native/mthread-matter` is executable.
Neither activation mode restarts live processes. The deployer reads `/tuya`
free space and existing destination sizes, simulates the atomic replacement
order, and requires a 512 KiB free-space reserve by default. Set
`DEPLOY_FLASH_RESERVE_KIB` to choose another non-negative reserve. An
insufficient-space result stops before staging or changing files. This is a
snapshot preflight, not a reservation; recheck immediately before deployment.
For Matter packages, preflight also inspects `/proc/*/exe`: when native or
`mthread-matter` is still running from the old path, it counts that executable
inode as retained after atomic replacement. An executable already running from
a `(deleted)` path makes the estimate ambiguous, so preflight refuses until
that service is restarted. Keep the existing 512 KiB reserve during routine
updates.
After launcher activation, gracefully stop the paired supervisor by its PID in
`/tmp/ziggurat-supervisor/supervisor.pid`, wait for cleanup, then launch
`/tuya/tuya_start.sh`. The native-only `--restart` switch does not restart
Ziggurat or reload the paired launcher.

The remaining work is broken into eight sequenced implementation plans in
[`docs/matter-plans/README.md`](docs/matter-plans/README.md).

The pinned C++ Matter SDK's factory standard-library blocker is historical
motivation for the Rust path. The Rust fixture now establishes a functioning
controller/crypto/transport platform, including bounded measurements; it does
not establish real-device compatibility or a deployed production controller.

## Build contract

`MATTER` accepts exactly `0` or `1` and defines `MICROHAD_MATTER_ENABLED` for
native host and target C++ compilation. By default both `ZIGGURAT` and
`MATTER` are enabled to match the current gateway deployment. Setting
`ZIGGURAT=0` automatically defaults `MATTER` to `0`; either flag can be
overridden explicitly.
`MATTER=1` requires `ZIGGURAT=1`, because the planned Thread radio endpoint is
provided by Ziggurat. Empty, unknown, and multi-value flags are rejected.

| Selection | Host output directory | MIPS output directory | Availability |
| --- | --- | --- | --- |
| `ZIGGURAT=0 MATTER=0` | `build/host` | `build/mips` | Existing EZSP backend |
| `ZIGGURAT=1 MATTER=0` | `build/host-ziggurat` | `build/mips-ziggurat` | Existing Ziggurat backend |
| `ZIGGURAT=1 MATTER=1` | `build/host-ziggurat-matter` | `build/mips-ziggurat-matter` | Native IPC, commissioning/Thread and device-control APIs; Rust service is built separately |

Matter-disabled builds do not fetch, compile, or link the Matter SDK. Switching
the flag cannot reuse enabled and disabled native artifacts. Native binaries
and tests also depend on the Makefile so changes to feature definitions trigger
recompilation. The deployer defaults to the Matter-capable output; use `--binary`
to choose another build.

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

## Historical C++ platform gate

These archived checks used Matter SDK `v1.6.1.0`, commit
`3bcdd56ba54fb88b2afb4bfef575014671df7aa7`. The superseded C++ SDK
submodule, probe programs and Make target have been removed.

The host probe compiled, links, and executes an actual Matter `TypeTraits.h`
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
as deployable MIPS libraries. For the C++ approach, the next prerequisite would be a compatible modern C++
library/ABI runtime for factory uClibc, followed by full SDK linking and target
execution. The selected Rust controller avoids that library requirement. It must retain the gateway's instruction and soft-float
constraints rather than selecting a modern MIPS ISA by accident.

`MATTER=1` builds
the small C++03 IPC/status bridge directly; the separate Rust controller has
its own build and platform checks.

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
Garage BLE relay, persistence, CASE or native IPC. The optional native IPC
build uses only the existing C++03 gateway toolchain; the separate Rust runtime
has its own verified build/package flow. A successful probe alone does not
provide the controller daemon.

## Controller simulation follow-up

The companion host harness now checks UDP/TCP PASE commissioning, fabric
installation, CASE, light read/invoke, signed/null temperature reads and stale
sessions. It uses a development fixture and stub discovery, not a Thread mesh.
Source inspection confirms the pinned commissioner skips device attestation
when its test bypass is enabled and does not provision a Thread dataset.
Secure defaults reject commissioning; no native production path enables the
bypass. The owner later selected skipping manufacturer attestation, as recorded
below; its verifier is optional for this setup. NetworkCommissioning and the
OpenThread/BLE transports have since passed the synthetic fixtures described
below. The trusted-LAN HTTP/WebSocket requirement
remains independent of Matter device attestation trust.

Reproduction, dependency lock and measured test counts are in the companion
[controller test report](https://github.com/ChristophKaser/ziggurat-compilation/blob/main/docs/rs-matter-controller-tests.md).

## Remaining implementation contract

After a controller implementation passes its complete platform gate:

1. Build a local `mthread-matter` daemon containing OpenThread and the Matter
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
   owns PASE, the selected attestation policy, Thread provisioning, and its Matter fabric.
   One commissioning session runs at a time with cancellation and timeout.
4. Treat the LAN as trusted, as explicitly requested: use `http://`/`ws://`
   without assuming a publicly trusted or installed gateway certificate. Do
   not require certificate import/pinning and do not bypass TLS validation
   globally. Apply the owner-selected attestation bypass while retaining PASE/CASE secure sessions.
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
artifact paths, compiler definitions, rejected inputs, and the enabled native
bridge build prerequisite. The earlier C++ controller prerequisite probes
remain historical; the Rust controller follows the separate build and
validation flow recorded in the companion repository.
Verification passed:

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
for factory MIPS. The native service still has no commissioning or control
endpoint; the Rust/OpenThread daemon, Garage BLE and real-device acceptance
remain in progress.

## OpenThread userspace transport checkpoint

The Rust/OpenThread UDP adapter now passes host and factory-gateway tests
using a synthetic RCP. It implements the rs-matter network traits over two
internal sockets, with no Linux IPv6/TUN requirement and only guarded Unix
radio URLs. Payloads up to 1232 bytes pass; larger payloads are rejected before
enqueueing. The gateway's timed transport section took 60 ms; its stripped
probe is 1,146,488 bytes. One startup sample showed 520 KiB probe RSS and
1512 KiB free system RAM, which cannot establish peak controller memory.
Production remained online/formed and the physical radio was untouched.

The implementation, reproducible build/test commands, libc timer shims and
limits are documented in
[`../docs/rs-matter-openthread-transport.md`](../docs/rs-matter-openthread-transport.md).
The native gateway now has a bounded Unix-stream client and read-only Matter
and Thread status routes. They use `/tmp/microhad-matter.sock`, require a same-owner
socket with private permissions, cap frames at the shared 2048-byte protocol
bound, and wait at most one second for a response. The Rust daemon now creates
this socket when started; absent services still return 503. Garage BLE and
control operations remain pending.


## Encrypted commissioning and BLE relay checkpoint

The companion implementation now passes the actual rs-matter commissioning
flow on the gateway in two synthetic modes: PASE/Thread provisioning over
internal UDP, or over BTP with simulated phone C1 writes/C2 indications, then
CASE over OpenThread. OnOff toggle/readback and signed/null temperature reads
pass. The latter artifact took 4.39 seconds over UDP and 4.30 seconds over
synthetic GATT/BTP; sampled RSS reached 1628 KiB for both peers in one process.
This is not a real ALPSTUGA, phone GATT or external Thread mesh result.

The original 1 MiB MemFree test guard stopped the larger fixture because it
counted inactive file cache as unavailable RAM. Its isolated runner now keeps
a conservative 4 MiB free/clean-inactive-file budget plus a 512 KiB hard
MemFree floor, with a 3 MiB RSS ceiling and 100-second timeout. This revised
simulation guard does not replace the combined-service deployment acceptance
checks. Production remained online/formed and test artifacts were removed.

The gateway BTP relay rejects stale connection/write callbacks and resets
failed or stalled writes. A private atomic controller store passes host tests;
committed-flash fabric reopen and fresh CASE now pass on host and MIPS. CA/node persistence integration, Android GATT/WebSocket
wiring, discovery/subscriptions and real-device acceptance remain pending.

Detailed commands, measurements, failed guards and test scope are in
[`../docs/rs-matter-openthread-commissioning.md`](../docs/rs-matter-openthread-commissioning.md).


## Committed-flash recovery and native IPC framing

The strengthened fixture closes the original controller store, reopens its
committed file on the gateway flash filesystem, creates a fresh Matter instance
and establishes a new CASE session with the simulated phone detached. The final
BTP/UDP runs took 5.03/5.16 seconds and reached 1692/1700 KiB sampled RSS. This
replaces the earlier fixture's in-memory clone reload with an actual file reopen.
It is not yet daemon/radio restart or real-device recovery acceptance.

Native C++03 `MatterWire` and Rust `ipc` implement the same bounded stream
framing. The native client now serves read-only status routes; the Rust daemon
listener is implemented as described below. Fields are explicit bytes, independent of host/MIPS
ABI:

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 2 | Payload length, little-endian, at most 2048 |
| 2 | 1 | Version, exactly 1 |
| 3 | 1 | Kind: request 0, response 1, event 2 |
| 4 | 1 | Operation: 1 Matter status, 2 Thread status, 3 through 12 reserved |
| 5 | 1 | Reserved, exactly 0 |
| 6 | 4 | Correlation ID, little-endian |
| 10 | variable | Status response is a UTF-8 JSON object; requests have an empty body |

Requests/responses require a nonzero correlation ID; events require zero.
Input chunks are capped at 4096 bytes and at most 32 frames can await dispatch.
Malformed or overflowing streams fail closed and discard pending data. Payload
contents have no logging/formatting path in the codec. Native socket client
lifecycle and status calls are implemented. Event
dispatch, commissioning/control body schemas, and asynchronous phone BLE transport
remain pending.

Matching golden-vector and malformed-stream tests pass in Rust and native C++.
Native verification passed for the disabled defaults and the enabled Matter
bridge: `make host test`, `make mips`, `make ZIGGURAT=1 MATTER=1 host test`,
`make ZIGGURAT=1 MATTER=1 mips`, and `make matter-build-flags-test`. The enabled
host run includes 13 native peer tests; the MIPS build uses the factory RSDK.
The focused native codec probe also ran on actual factory MIPS:

```sh
python3 scripts/test-matter-ipc-codec.py \
  --host 192.168.178.62 --password-file ../ssh_password.txt
```

It uploads only a unique small test executable, applies virtual-memory/time
limits, exercises framing vectors, and removes the artifact. It never accesses
radio, device configuration or persistent network state. The framing source is
compiled for both native backends; Matter-disabled runtime behavior is unchanged.

## Service runtime and optional supervision — 2026-10-04

The companion `mthread-matter` service now runs Matter/OpenThread/storage and
the Unix listener on one event-loop thread. Both status opcodes pass a real
host process test against the synthetic RCP. It reports `awaiting_fabric` and
`detached` until the pending fabric and network lifecycle work is implemented.
The stripped static factory MIPS-I/o32 binary is 1,228,412 bytes. This checkpoint
has not been executed on the physical gateway or activated in production.

`paired_start.sh` optionally supervises it when `MATTER_BIN` is set to an
absolute executable path. Configuration:

| Variable | Default / purpose |
| --- | --- |
| `MATTER_BIN` | Unset: Matter disabled; no Thread endpoint requested |
| `MATTER_STATE_DIR` | `/tuya/native/matter`, private persistent state |
| `MATTER_SOCKET` | `/tmp/microhad-matter.sock`, must match the native status client |
| `THREAD_SOCKET` | `/tmp/ziggurat/thread.sock`, private coordinator HDLC endpoint |
| `MATTER_RESTART_LIMIT` | 5 start attempts during this supervisor invocation |
| `RESTART_BACKOFF` | 3 seconds between failed Matter attempts |

Existing paired-supervisor variables for coordinator/native executable,
configuration, maintenance tool and control paths remain required. With Matter
enabled, Ziggurat receives `--thread-socket` in addition to IID 1 and its COBS
endpoint. Only Ziggurat owns the UART/reset. The native bridge starts after COBS
readiness; a missing Thread socket leaves Matter absent while Zigbee runs.
Matter starts when its socket exists. It receives only the Unix endpoint with
`?no-reset`, the private state path and IPC path.

Matter failure never consumes the coordinator/native retry budget. On Matter
budget exhaustion, the supervisor writes the empty RAM marker
`$RUNTIME_CONTROL/matter.failed` and continues Zigbee/native service. The
attempt budget includes starts following coordinator restarts; it resets only
on a fresh supervisor invocation. The exact current PID, when alive, is in
`matter.pid`. State corruption causes service startup to fail; inspect and
recover private state from a known backup rather than deleting it to retry.

Quiesce, supervisor shutdown and coordinator death stop Matter before native
and radio processes. Coordinator restart recreates clients within their
budgets. Native-only restart leaves Matter running. Child output is discarded,
so retries create no growing log or flash log. The standalone runtime emits
only static start/stop/failure messages. Shutdown has the existing bounded
TERM/KILL deadline; restart socket ownership is checked by the Rust listener.

For a Matter-enabled native bridge, build and pass the `ZIGGURAT=1 MATTER=1`
binary explicitly. The MIPS build emits the adjacent
`microhad.matter-manifest`, which records both feature selections
and the binary SHA-256. Matter-service deployment refuses missing, disabled,
or stale manifests before opening SSH and stages the manifest beside the
binary:

```sh
make ZIGGURAT=1 MATTER=1 mips
cd ..
bash micro-thread-matter-daemon/scripts/build.sh mips service
cd microhad
../deploy.sh --binary-only --binary build/mips-ziggurat-matter/microhad \
  --matter-service ../build/micro-thread-matter-daemon/mthread-matter-mips \
  --target /tuya/native-test --password-file ../ssh_password.txt
```

Staging uses mode 0700 and leaves the active launcher alone. After the paired
Ziggurat installation has passed its own acceptance, the explicit
`--activate-paired` option can install the Matter-aware launcher for the next
boot; it requires `--target /tuya/native`, `--matter-service PATH`, and an
explicit `--binary`, preserves `/tuya/tuya_start.sh.lowjs-backup`, and does not
restart existing processes. Use it only when the installed Ziggurat binary,
configuration, maintenance tool, paired supervisor, backup and recovery path
have already been verified. Existing `--activate` remains native-only. No
target execution, launcher activation, credentials transfer or process restart
was performed here; physical acceptance remains in plan 08.

Validation for the deployment path: eight mocked service/launcher deployment
tests, including insufficient-flash refusal and in-place replacement sizing;
the Matter build-flag/manifest test, shell syntax checks, ten paired supervisor
tests and the Rust service-feature library suite pass. No physical radio, target
CPU/RSS sample or device configuration was touched; startup latency and
sustained resource feasibility remain open. See
[`docs/matter-plans/01-rust-service-runtime.md`](docs/matter-plans/01-rust-service-runtime.md)
for remaining acceptance gaps.

The subsequent strict request-body checkpoint validates every reserved schema-1
action in Rust and returns fixed `malformed`/`unsupported` replies without echoing
input. It does not yet accept commissioning or control jobs. All 22 Rust library
checks and the extended synthetic service test pass. The current factory stripped
binary is 1,343,120 bytes (114,708 more than the status-only runtime above).
The full assigned contract is in [`docs/matter-ipc-v2.md`](docs/matter-ipc-v2.md).

The current service also creates/restores its gateway-owned authority and fabric.
It now reports `running`/`fabric_ready:true` with Thread still `detached`. Complete
authority creation is atomic; restoration verifies the chain, key correspondence,
metadata and loaded fabric. Node allocation persists its successor before use
and rejects exhaustion. Administrator vendor metadata defaults to development
ID `0xfff1` (CLI override before initialization); it is not a manufacturer
certificate or HTTPS trust assumption. All 31 combined Rust checks and actual
host service process restart pass, including no credential rewrite on reads and
corrupt/mismatched state refusal. Factory stripped binary is 1,527,452 bytes.
Production-authority CASE recovery now passes in the encrypted host fixture,
which uses the same authority/allocator as the service, reopens credentials,
reloads authority and creates a fresh CASE session with the simulated phone
detached. Both BTP and internal UDP pass (217/264 ms on the development host).
Current target filesystem/runtime validation remains pending in
[plan 03](docs/matter-plans/03-persistent-fabric-identity.md).
