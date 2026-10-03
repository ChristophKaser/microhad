# Plan: Garage phone BLE relay

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
