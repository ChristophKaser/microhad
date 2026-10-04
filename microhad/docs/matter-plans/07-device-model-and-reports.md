# Plan: Matter device model and reports

## Current open work — 2026-10-04

Production operational discovery, node catalog, device operations and report integration
remain open.

Next deliverable: Implement bounded operational DNS-SD over userspace OpenThread and a
persistent node catalog. Verify production CASE completion before descriptors, commands,
subscriptions and native reports.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

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

The production-owned commissioner now reaches encrypted PASE, Thread provision
and CASE completion in the synthetic accessory fixture. The running service
contains that task, but lacks the production DNS-SD resolver and persistent node
catalog. It uses no fixture address injection and can time out waiting for CASE.
Descriptor/control/report integration and real operational discovery remain the
next requirements here; the owned fixture is not device-model acceptance.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [ ] Implement bounded operational DNS-SD over userspace OpenThread UDP,
   including answer validation, address changes, TTL/cache limits and timeout
   backoff. Remove the need for fixture-only rendezvous in integration tests.
2. [ ] Persist a bounded node catalog with stable fabric/node identity and
   explicit commissioning-completion/recovery states. Never reuse an allocated
   ID after interrupted completion; reconcile incomplete entries on restart.
3. [ ] Complete production CASE/CommissioningComplete through the resolver,
   then discover descriptors, endpoints and advertised clusters. Test restart
   against the same catalog and authority without the phone.
4. [ ] Implement validated OnOff/Level reads and commands plus typed IPC
   bodies in plan 02. Confirm resulting values through reads/reports and expose
   unsupported clusters and failed commands explicitly.
5. [ ] Decode advertised sensor attributes with units, signed values and null
   semantics. Cover temperature, humidity, CO2, PM2.5 and air quality using
   synthetic descriptors rather than assuming ALPSTUGA exposes every cluster.
6. [ ] Add bounded subscriptions, duplicate filtering, sleepy-device refresh
   and reconnect backoff; persist meaningful changes rather than each report.
7. [ ] Feed native snapshots/history/alerts and dashboard through plan 04.
   Test unsupported clusters, missing/duplicate reports, address changes,
   restart and concurrent synthetic Zigbee activity; document remaining limits.

Validation: encrypted synthetic accessory tests using production discovery,
private catalog restore and phone-detached CASE, followed by report/API tests.
