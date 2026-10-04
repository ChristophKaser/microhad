# Plan: Garage phone BLE relay

## Current open work — 2026-10-04

The native relay contract and tested Android GATT building blocks exist; onboarding UI,
WebSocket wiring and end-to-end app validation remain open.

Next deliverable: Build the setup-code/session model with fake gateway/GATT tests, then
wire permissions, foreground UI, scanning and serialized relay transport. Unverified app
experiments do not close these steps.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Use the Garage Android app as a temporary BLE/GATT bridge during Matter
commissioning while the gateway remains the Matter commissioner.

## Steps

1. Add a setup-code/QR entry flow and a commissioning screen with clear
   progress, timeout, retry, and cancel states. Keep setup codes in memory only
   for the active attempt and redact them from diagnostics.
2. Implement BLE scanning, peripheral selection, connection, service/characteristic
   discovery, MTU negotiation, C1 writes, C2 indications, and disconnect
   handling for the target commissioning transport.
3. Implement one WebSocket relay session to the gateway for attach, indication,
   gateway write, write completion, and disconnect messages using the agreed
   IPC/API schema.
4. Preserve ordering and confirmations for each GATT write. Apply message
   bounds, session generations, reconnect rules, and cancellation so late BLE
   callbacks cannot enter a newer commissioning attempt.
5. Keep PASE, attestation policy, Thread provisioning, fabric credentials,
   CASE, and device control in the gateway service. The phone forwards BLE data
   only and can be disconnected after commissioning.
6. Add Android tests with a fake GATT peripheral and a fake WebSocket peer for
   fragmentation, MTU limits, stale callbacks, timeout, cancellation, and
   disconnect during a write.
7. Document supported Android versions, permissions, pairing flow, and
   recovery steps. Commit app changes in the separate Garage app repository.

## Acceptance

- Setup can complete without storing the code after the active session ends.
- Indications and writes remain ordered and correlated across reconnects.
- A phone disconnect fails or cancels the active attempt safely and does not
  affect an already commissioned device.
- No Matter keys, Thread credentials, or BLE payloads appear in app logs.

## Status quo — 2026-10-04

Gateway BTP and session-lifetime building blocks have synthetic coverage.
Garage GATT/WebSocket onboarding has not been integrated; a simulated phone
in a Rust fixture is not an Android implementation.

## Remaining checkpoints

1. Freeze the native relay contract and supported Android permission/foreground
   lifecycle requirements; identify the app's existing networking and UI paths.
2. Add setup input and session progress/cancel UI with in-memory setup-code
   handling and a fake gateway peer before attaching real BLE hardware.
3. Implement scan/connect/discover/MTU/indication subscription and serialized
   C1 writes. Forward confirmation only after the matching GATT callback.
4. Wire one WebSocket relay session; enforce bounds and reject callbacks from
   obsolete connections. Handle app backgrounding, socket loss and cancellation.
5. Run fake-GATT/fake-WebSocket ordering, fragmentation and failure tests;
   build the Android app and document permissions and retry instructions.
6. Commit in the separate Garage repository. Reserve real accessory pairing
   and phone-detached operation for plan 08.

## Dependencies

[Commissioning and control IPC](02-commissioning-control-ipc.md) and the native
API routes in [the API plan](04-native-api-dashboard.md).

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Inspect the separate Garage repository, identify networking/UI paths,
   and document Android API/permission requirements and foreground lifecycle.
   Agree plan 04's WebSocket schema before implementing app transport.
2. [ ] Add setup-code/QR input, start/progress/cancel/retry UI and an in-memory
   session model tested against a fake gateway. Clear setup data at termination.
3. [ ] Implement discriminator-aware scan/selection, connect, service discovery,
   MTU negotiation and C2 indications, then serialize C1 writes and acknowledge
   only matching GATT completion callbacks.
4. [ ] Connect GATT to the WebSocket relay with bounded payloads and per-attempt
   generations. Define socket loss, BLE loss, backgrounding, reattach and cancel
   handling; reject callbacks from prior connections.
5. [ ] Test fake-GATT/fake-WebSocket ordering, fragmentation, MTU limits, stale
   callbacks, failed writes, deadlines and lifecycle loss. Build the Android app.
6. [ ] Document permissions and recovery, commit app changes in its own
   repository, and hand physical pairing verification to plan 08. Keep PASE,
   fabric keys and Thread provisioning in the gateway service.

Validation: Android build and fake transport/lifecycle tests before real BLE;
the Rust synthetic phone fixture does not satisfy this app checkpoint.

## WebSocket transport checkpoint — 2026-10-04

The bounded handshake/frame codec and focused host tests are implemented as a
building block for the native relay. Details, message limits and current scope
are recorded in [plan 04](04-native-api-dashboard.md#websocket-transport-checkpoint--2026-10-04).
There is no public upgrade route yet. Socket handoff, typed session/BLE dispatch,
connection ownership, deadlines and recovery remain open before Garage can
consume a stable native relay API. This is neither Android implementation nor
real-accessory acceptance evidence.

Verification passed: default and Matter-enabled `make host test`/`make mips`.
Host suites ran sequentially; enabled peer regression checks pass (13 tests).
Both factory builds produce static stripped big-endian MIPS-I ELF files.
No HTTP handoff, actual phone relay, deployment or plan 08 action occurred.

## Native relay ownership checkpoint — 2026-10-04

The native plain-HTTP WebSocket endpoint and typed session/BLE dispatch are now
wired to the persistent Matter worker. Contract, queue/deadline bounds, single
phone ownership, session/generation filtering and disconnect/reconnect policy
are documented in [the native API](../matter-native-api-v1.md#owned-ble-relay-websocket)
and [plan 04](04-native-api-dashboard.md#worker-owned-relay-checkpoint--2026-10-04).
The app can now target a concrete relay contract; Garage code and fake-GATT/app
lifecycle tests remain open. Native synthetic peer evidence is not Android or
real-device acceptance evidence.

Verification passed: default and Matter-enabled `make host test`/`make mips`,
with sequential host suites and 13 enabled native peer regression checks. Both
`python3 scripts/test_matter_native_worker.py` and its `--real-service` variant
pass. The real-service run reaches companion commit `45782d3` and verifies actual
BTP C1 events, successful write confirmation, reattach, stale indication/write
generations, matching failed-write `transport`, terminal preservation, unbound
phone isolation, bound disconnect cancellation, malformed-mask/repeated-ID
cleanup and setup-code log redaction. The initial failed-write test assumed a
success ack; it was corrected to the existing Rust error contract and the fake
peer was aligned. No production code change was needed for that test mismatch.
Both factory builds remain static stripped big-endian MIPS-I ELF files. Android
GATT/permissions, dashboard onboarding, production DNS-SD/catalog/device reports
and broader timeout/backpressure/target recovery evidence remain open. No
physical gateway, real accessory or plan 08 acceptance action was performed.

## Garage setup and relay core checkpoint — 2026-10-04

Garage commit `6b54fec` implements this checkpoint. The app now has a bounded manual/QR setup parser
and serialized transport-independent relay model. Manual short discriminators
remain distinct from full advertised discriminators; PINs are consumed once,
cleared on close and excluded from debug/error text. Epoch/generation/write
correlation, matching GATT completion, gateway acknowledgement, payload bounds,
fixed attempt/write deadlines and transport cleanup have fake-port coverage.

Both `testPommernDebugUnitTest`/`assemblePommernDebug` and
`testNeptunDebugUnitTest`/`assembleNeptunDebug` pass using a complete temporary
JDK 17: seven parser and nine relay tests in each flavor, plus the existing
sample test. Earlier JRE-only builds failed because AGP required `jlink`;
the complete JDK resolves this without changing Android dependencies.
The existing pump string formatting warning remains unrelated.

App requirements and findings are in Garage `docs/matter-onboarding.md`:
API 28+, legacy BLE/location permissions through API 30, runtime scan/connect
permissions from API 31, foreground cleanup and trusted-LAN HTTP/WS. The current
classes are not yet connected to Android permissions, GATT, WebSocket or UI.
These checklist items remain open; core tests are not app or physical acceptance.
No gateway/phone deployment, pairing or plan 08 action occurred.

## Android BLE adapter checkpoint — 2026-10-04

Garage commit `9442f02` implements foreground Matter scanning and a serialized
GATT adapter, alongside version-zero commissionable advertisement decoding.
The manifest declares legacy Bluetooth/location permissions through API 30 and
runtime scan/connect permissions from API 31. Fixed availability errors,
20-second scan/32-candidate limits, discriminator and optional vendor/product
matching, discovery, MTU fallback, C2 CCCD completion and acknowledged C1 writes
are implemented. Android 33 explicit-value and legacy GATT APIs are supported.

The main looper owns operations/callbacks; connection tokens and GATT identity
reject obsolete callbacks. Callback tasks are capped at eight; preparation
stages and pending writes have ten-second deadlines. Cleanup releases scan,
timers and GATT without logging payloads. Permissions are declared and checked;
runtime permission requests and selection/progress UI still require wiring.

Both flavor unit suites and debug APK builds pass: 29 tests per flavor, including
28 Matter tests and the existing sample. Ten new fake-GATT-port tests verify
preparation order, MTU fallback, writes/callbacks, bounds, deadlines and teardown;
two advertisement tests verify matching and malformed/unsupported rejection.
The Android SDK adapter is compile-checked, not tested on a phone. The legacy
callback deprecation warning is expected for supporting API 28–32.

App documentation is in Garage `docs/matter-onboarding.md`. Next implement the
HTTP/WebSocket client and foreground onboarding UI, wire these adapters and run
an end-to-end fake gateway/peripheral flow. Steps 2–6 remain incomplete; current
GATT building blocks do not establish working Android commissioning. No APK or
gateway deployment, physical pairing or plan 08 action occurred.
