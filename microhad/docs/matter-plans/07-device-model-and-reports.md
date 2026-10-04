# Plan: Matter device model and reports

## Current open work — 2026-10-04

Bounded production operational SRP/DNS discovery and persistent local node catalog
states are implemented and host-tested. Rust commit `e59c3dc` drives encrypted
CASE/CommissioningComplete and fresh CASE through the resolver in the synthetic
accessory fixture without injected rendezvous.

Remote recovery of catalogued nodes, legacy migration, descriptors, device
operations and report integration remain open. Next deliverable: reconcile
recovery-required nodes over operational CASE, discover endpoint descriptors and
advertised clusters, then add commands/subscriptions/native reports.

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
authority. The production owner now persists node identity and completion/recovery
states. Operational SRP/DNS-SD is implemented; remote catalog reconciliation, endpoint discovery,
commands, subscriptions and native report integration remain pending. The new SRP/DNS encrypted fixture proves the production resolver path; the older
explicit-rendezvous fixture alone does not establish operational discovery.

## Remaining checkpoints

1. Local catalog persistence, stable identity and failed/interrupted state are
   implemented. Reconcile recovery-required remote nodes and legacy stores without
   catalog metadata without reusing allocated node IDs.
2. Bounded operational SRP/DNS-SD, TTL/cache limits and query backoff are
   implemented. Add proactive address invalidation/re-resolution on CASE failure
   with the production device/recovery pipeline.
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
contains that task, persistent catalog and production SRP/DNS resolver. Remote
node recovery remains open. Production lookup uses no fixture address injection and remains bounded when
registration/attachment is absent.
Descriptor/control/report integration and real operational discovery remain the
next requirements here; the owned fixture is not device-model acceptance.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Implement bounded operational DNS-SD over userspace OpenThread UDP,
   including answer validation, address changes, TTL/cache limits and timeout
   backoff. Remove the need for fixture-only rendezvous in integration tests.
2. [ ] Persist a bounded node catalog with stable fabric/node identity and
   explicit commissioning-completion/recovery states. Never reuse an allocated
   ID after interrupted completion; reconcile incomplete entries on restart.
   Local catalog/state persistence is verified (`5002f88`); remote reconciliation
   and legacy no-catalog migration keep this item open.
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

## Persistent catalog checkpoint — 2026-10-04

Rust commit `5002f88` adds TMKV key 0x1002 with a versioned bounded catalog of up
to 64 nodes. The owned commissioner checks capacity before allocation, commits
a reservation before accessory work, records fabric and Thread milestones and
marks complete only after successful operational CASE/CommissioningComplete.
Endpoint identity strings combine fabric, node and endpoint. Catalog/allocator
gaps never permit ID reuse. Reads do not write flash.

Cancelled, failed, replaced or timed-out futures mark recovery-required. Startup
converts interrupted local stages once, preserving completed/recovery records
on later reads. Cleanup storage failure makes the owner fatal and exits the
service. Wrong fabric, allocator inconsistency, malformed catalog and orphaned
catalog-without-authority fail closed; no replacement authority is generated.

Validation: 48 service-feature library tests, 23 identity-only library tests,
the running-service IPC fixture with independent catalog decoding and the
encrypted production-owner fixture passed. The latter confirms a catalogued
complete node and then persisted authority/fresh CASE. Both host and MIPS
service/probe builds passed; static big-endian MIPS-I/o32 soft-float artifacts
are 1,748,648 and 2,084,544 stripped bytes. Each increased 8,192 bytes. Hashes and
scope are in `ziggurat-compilation/docs/matter-node-catalog-validation.json`.

The encrypted fixture still supplies rendezvous explicitly. SRP/DNS-SD over
userspace OpenThread, remote recovery, legacy catalog migration, descriptors,
commands, reports and native device integration remain open. Recovery records
consume capacity; they are never silently discarded. Checklist items 1–3 remain
open until those integration requirements are proven. No physical deployment
or plan 08 action was performed.

## Operational SRP/DNS checkpoint — 2026-10-04

Rust commit `e59c3dc` enables SRP server/client, DNS-SD server, DNS client, ECDSA
and Thread Network Data services in OpenThread. The service advertises an SRP
server through normal unicast publication and resolves operational labels through
userspace UDP SRV/TXT/AAAA queries to the selected advertised server. No Linux
IPv6/TUN, arbitrary DNS domain override, new UART owner or trusted certificate
requirement is introduced.

The adapter permits one bounded query and copies at most four addresses/256 TXT
bytes. It validates label/domain, port/TTL and complete answers. Static generation
cookies keep callbacks independent of Rust future memory; cancelled queries
discard their results. SRP host/service/TXT bounds cap retained registry state.
The Rust layer validates addresses/known TXT, removes duplicates, caps its cache
at 16 entries/120 seconds and never renews TTL on reads. It handles lookup-time
Thread loss, attachment wait and bounded retry delays.

A separate probe-only archive advertises the synthetic accessory through signed
SRP registration. `--service-discovery` then uses the production resolver for
encrypted commissioning, catalog completion, commands and fresh CASE after
fabric reopen with the phone detached. No address is deposited by the fixture.
Missing-service and dropped-query callbacks are verified before a subsequent
valid SRV/TXT/AAAA lookup. Fixture symbols are absent from production artifacts.

Verification: 54 service-feature tests, SRP/DNS encrypted fixture, running-service
IPC/catalog regression and native worker `--real-service` HTTP/WebSocket checks
passed. Both host and MIPS service/probe builds passed. Static big-endian
MIPS-I/o32 soft-float artifact sizes are 1,924,788 and 2,264,780 bytes. Hashes/scope
are in `ziggurat-compilation/docs/matter-operational-discovery-validation.json`;
flow/bounds are in `ziggurat-compilation/docs/matter-operational-discovery.md`.

Checklist item 1 is complete at host implementation scope. Items 2–7 remain
open: remote catalog reconciliation/legacy migration, descriptor/endpoint/cluster
discovery, production operations/subscriptions and native reports. Proactive
cache invalidation after CASE failure belongs to that recovery pipeline. Older
comparison fixtures retain explicit rendezvous; they are not the discovery
evidence. Host timing includes Thread/SRP bootstrap and is not target CPU/RSS.
No deployment or plan 08 acceptance occurred.
