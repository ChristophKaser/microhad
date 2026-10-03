# Plan: Native Matter API and dashboard

## Goal

Expose commissioning, Thread network status, discovered Matter devices, and
supported controls through the native HTTP service and dashboard.

## Steps

1. Keep the existing `/api/matter/status` and `/api/thread/status` routes, then
   define asynchronous start/status/cancel endpoints and Thread status/create/
   import endpoints against the agreed IPC schema.
2. Validate setup-code or QR input, dataset size and channel compatibility,
   device IDs, endpoint/cluster/attribute IDs, command values, and request
   lengths before forwarding them to the service.
3. Add coordinator-owned commands and protected snapshots. HTTP handlers must
   not call `Gateway` directly, mutate Matter state, or wait on phone BLE
   indications.
4. Return stable public errors and session states. Do not return private keys,
   datasets, setup codes, or other secrets in status JSON or logs.
5. Add feature-aware dashboard setup and device views. Hide onboarding and
   controls when `MATTER=0`; show unavailable/reconnecting state when the
   feature is enabled but the service is absent.
6. Keep the user's trusted-LAN `http://` and `ws://` policy. Do not require an
   installed gateway certificate or disable TLS validation globally. Document
   that session IDs correlate relay traffic and are not administrator
   authentication.
7. Add API integration tests for enabled/disabled builds, unavailable service,
   invalid input, cancellation, queue pressure, and secret redaction. Run the
   native host and MIPS checks after C++ changes and `make webui` after UI
   changes.

## Acceptance

- Onboarding and status work without blocking either HTTP worker for the
  duration of commissioning.
- All state changes reach the Matter service through IPC and Zigbee changes
  remain coordinator-queued.
- Disabled builds do not expose Matter controls; enabled builds clearly report
  service unavailability.
- The dashboard works over the selected trusted-LAN HTTP/WebSocket transport.

## Status quo — 2026-10-04

Read-only Matter/Thread status routes and the `MATTER` compiler flag exist.
Commissioning, relay, Thread mutation, device-control routes and dashboard
onboarding are pending. The current status client is insufficient for the
asynchronous integration: connection deadlines and reply validation need work.

## Remaining checkpoints

1. Expose the actual active coordinator channel and online state in protected
   `GatewayRunner` snapshots. Never substitute the configured channel for a
   confirmed radio observation.
2. Add a single bounded Matter IPC worker with queues and cached snapshots.
   Send fresh coordinator observations, stop authorizing Thread when stale,
   and keep IPC waits outside the coordinator and HTTP workers.
3. Implement commissioning start/status/cancel and Thread create/import using
   validated bodies, fixed public errors and asynchronous job/session IDs.
4. Add bounded WebSocket relay routing with generation and sequence checks,
   disconnect cleanup and write-confirmation forwarding. Agree the contract
   with plan 05 before building the app against it.
5. Expose device snapshots/read/control once plan 07 provides them, then build
   feature-aware onboarding and device views with redacted progress/errors.
6. Verify disabled/enabled builds, absent/stalled service, malformed replies,
   queue pressure, cancellation and HTTP/WebSocket behavior. Run required
   native host/MIPS and web UI builds for their respective changes.

## Dependencies

[Commissioning and control IPC](02-commissioning-control-ipc.md) and
[persistent identity](03-persistent-fabric-identity.md). Define the relay
endpoint contract with the Garage workstream so the app can connect when its
plan is implemented.
