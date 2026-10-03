# Plan: Matter device model and reports

## Goal

Discover Thread Matter devices and expose their supported controls and sensor
values through the existing gateway model, history, alerts, and dashboard.

## Steps

1. Implement commissioning completion and persistent CASE session restore in
   the Rust service. Add bounded DNS-SD discovery and subscriptions using the
   existing userspace OpenThread networking path.
2. Discover endpoint descriptors and supported clusters; retain stable string
   identifiers based on fabric, node, and endpoint. Do not assume every device
   implements the same clusters.
3. Add attribute reads and commands for standard OnOff and Level Control
   clusters, with explicit command/value validation and optimistic-state
   reconciliation after reports.
4. Map supported sensor clusters and units into the native device model. For
   ALPSTUGA, include temperature, humidity, CO2, PM2.5, and air-quality values
   only when advertised by the device. Preserve unknown values as null with
   their units and quality state.
5. Add report subscriptions with bounded retries and refresh behavior suitable
   for sleepy devices. Avoid tight polling and unnecessary flash writes.
6. Route Matter observations into snapshots, bounded history, alert thresholds,
   and notification rules while keeping Matter IDs distinct from Zigbee IEEE
   addresses.
7. Test with synthetic endpoints for supported/unsupported clusters, signed
   and unknown values, missing reports, reconnect, duplicate reports, and
   restart recovery.

## Acceptance

- Supported lights and plugs can be read and controlled over CASE.
- Sensor values retain correct units and unknown/null semantics.
- Device identity remains stable across gateway and service restart.
- Reports flow without blocking Zigbee coordination or causing frequent flash
   writes.

## Status quo — 2026-10-04

Encrypted synthetic fixtures prove commissioning and fresh CASE with persisted
authority. Production node catalog, operational DNS-SD, endpoint discovery,
commands, subscriptions and native report integration remain pending. Fixture
address injection does not establish operational discovery support.

## Remaining checkpoints

1. Persist a bounded node catalog after successful commissioning, including
   stable identity and recovery state; define failed/interrupted completion
   behavior without reusing allocated node IDs.
2. Resolve operational nodes using bounded DNS-SD over userspace OpenThread;
   handle address changes and resolve/CASE timeout with bounded backoff.
3. Discover descriptors/clusters and implement validated OnOff/Level reads and
   commands. Reconcile confirmed values instead of keeping optimistic state
   after an operation fails.
4. Implement advertised sensor decoding with signed values, correct units and
   null/unknown handling; test ALPSTUGA-related clusters synthetically without
   assuming every target advertises all measurements.
5. Add bounded subscriptions, duplicate filtering and reconnect/refresh policy;
   retain only state changes that warrant persistence.
6. Integrate snapshots, history, alerts and UI through the native worker using
   distinct Matter IDs. Test restart, unsupported clusters, missing reports
   and concurrent Zigbee activity with synthetic peers.

## Dependencies

[persistent identity](03-persistent-fabric-identity.md),
[Thread lifecycle](06-thread-network-lifecycle.md), and
[commissioning/control IPC](02-commissioning-control-ipc.md).
