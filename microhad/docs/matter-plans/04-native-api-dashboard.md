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

## Depends on

[Commissioning and control IPC](02-commissioning-control-ipc.md) and
[persistent identity](03-persistent-fabric-identity.md). Define the relay
endpoint contract with the Garage workstream so the app can connect when its
plan is implemented.
