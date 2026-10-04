# Plan: Matter device model and reports

## Current open work — 2026-10-04

Bounded production operational SRP/DNS discovery and persistent local node catalog
states are implemented and host-tested. Rust commit `e59c3dc` drives encrypted
CASE/CommissioningComplete and fresh CASE through the resolver in the synthetic
accessory fixture without injected rendezvous.

Bounded endpoint/cluster descriptor reads and service scheduling now have
encrypted host evidence (`05053ec`, `5c588df`). Revisioned service snapshot IPC
is implemented (`237d110`); native validation and queued snapshot HTTP exposure
are implemented. The dashboard snapshot view builds and its HTTP client is
verified, including rendered component interaction in Chromium. Capability-gated
OnOff execution with confirmed CASE
readback is verified (`0c66b87`), Rust service dispatch now exposes bounded
asynchronous OnOff jobs, and native HTTP routes/worker polling now expose
confirmed readback. The dashboard now renders OnOff read/control interactions
through confirmed readback. Rust service jobs implement Level Control read and
MoveToLevel with separate CurrentLevel readback (`6ec569a`). Native strict Level
IPC validators, HTTP admission, worker polling and typed U8 job-response
validation are implemented and host-tested; dashboard CurrentLevel reads and
MoveToLevel controls are rendered and verified in the shared device cards.
Native UI commit `7822c32` removes the separate Matter device list and adds the
Matter-to-common-device adapter. `make webui`,
`node webui/test/matter-devices-browser.cjs`, and
`python3 scripts/test_matter_webui.py` pass (20/20 API tests). The dashboard
now projects Matter endpoints into the common `Device` shape and
renders them with the same `DeviceGroups` cards used by Zigbee and HTTP devices
such as Shelly. Common Matter commands use the Matter controller adapter;
Shelly setup remains source-specific and its commands use its HTTP controller.
The frontend projection initially preceded native `Gateway::Device`
registration, which is covered by the later checkpoint below.
Commit `39a2469` maps Matter temperature, humidity, air quality, CO2 and PM2.5
into canonical `sensor_values` names and uses those names for shared card
classification and presentation; cluster IDs stay in Matter provenance and
the source adapter. The API and rendered browser tests pass. Existing
temperature/humidity fields already serve Zigbee and Shelly. Native commit
`a841aca` extends `Gateway::Device` and `/api/status` with shared known/value
fields for air quality, CO2 and PM2.5, with serialization coverage. Native
commit `1f5077d` now parses validated revisioned snapshot pages, assembles them
by cursor/revision and reconciles endpoint identity, online state and advertised
capabilities into the coordinator-owned `Gateway::Device` catalog. The
dashboard deduplicates those entries with the live Matter projection by stable
ID. Matter IDs remain in `id`/`external_id`; the Zigbee `ieee_address` field
stays empty.
Native commit `02f170f` feeds confirmed operation results into the shared
device state. Snapshot values continue to render in the shared cards, while
typed completed CASE reads also update matching registered native
`Gateway::Device` instances. The shared status view reports normalized
`sensor_values` with units and measured/unknown quality, and snapshot refreshes
preserve the last confirmed values. Confirmed values remain in memory; the new
manager report queue is not yet applied to `Gateway::Device`. Native commit `e81af44` routes Matter
temperature, humidity and brightness readings through the common value-alert
engine and preserves alert configuration/state across snapshot refresh. Matter
alert settings are restored across gateway restart and Matter snapshot
reconciliation by native commit `5cfb3b1`. Native commit `d5d94ba` adds unit-
matched CO2 and PM2.5 thresholds, persists their units with the alert settings,
and triggers alerts only when readings cross thresholds in the same unit. The
shared device settings dialog presents the reported concentration units.
Remote recovery of catalogued nodes and legacy
migration also remain open. Verification passed:
`make ZIGGURAT=1 MATTER=1 host test` and
`make ZIGGURAT=1 MATTER=1 mips` (the MIPS toolchain prints its existing
LOAD-USE warnings). Native commit `e35abb7` routes Shelly history into the
bounded unit-aware source-ID journal. Commit `a522062` adds Zigbee temperature,
humidity, contact, occupancy and battery samples and FRITZ!Box presence
transitions to the same journal. Matter confirmed measurements and Shelly
HTTP updates also enter it. All providers use the common device history route
and rendered dialog while keeping source-specific controllers. Stable IDs are
prefixed by source, and samples preserve units. Zigbee reads retain a legacy V2
fallback if the source-ID journal has no samples for that device. The journal
caps at 8,192 records, compacts to recent records and uses `/tmp`. History is
intentionally volatile across power loss because daily writes under `/tuya`
would violate the gateway's no-high-frequency-flash-write policy. Low-frequency
settings that require restart restoration remain in the runtime directory. The
source-unification checkpoint passed
`make ZIGGURAT=1 MATTER=1 host test`,
`make ZIGGURAT=1 MATTER=1 mips`, `npm run build`,
`node webui/test/matter-devices-browser.cjs`, and
`python3 scripts/test_matter_webui.py` (20/20 API tests).
Active Matter report delivery now decodes all scheduled sensor paths in Rust,
including signed/nullable temperature, humidity, air-quality enums, and CO2/
PM2.5 values normalized using each endpoint's reported measurement unit
(`1855f24`). The direct TLV decoder test passes, and the existing production
manager OnOff report fixture still passes. A notifier-backed synthetic fixture
now proves all five sensor report types through the production manager (`62d7fd4`).
Native commit `c22f38a` now verifies an unsolicited `matter_report` event over
the native worker's IPC connection updating the shared device model. The
process-level native proof uses a synthetic IPC peer and exercises a shared
switch state update. The isolated Rust-service/native-worker fixture also
passes, but it does not generate a report from a commissioned accessory, so a
live Rust-service-originated sensor event through native shared sensor state
remains open. Remote node recovery also remains open. Native endpoint
metadata is persisted in the low-frequency device store; confirmed values are
restored across gateway process restart from `/tmp`, and may be lost on power
loss.

This shared device contract applies equally to Zigbee, Matter, and external
HTTP sources such as Shelly: stable source identity, canonical semantic device
type, common capabilities, normalized state and telemetry with units and
quality, and common presentation. Equivalent functions map to the same
semantic type and capability vocabulary even when their transports differ.
Lights, switches, contacts, temperature/humidity sensors, and power readings,
for example, use shared fields and controls wherever each source exposes the
same behavior. Each source adapter owns discovery/setup and translates common
control intents to its transport. HTTP integrations are ordinary providers of
the shared model, not a parallel dashboard path. Matter cluster IDs and
commissioning details stay in Matter provenance/setup; Shelly HTTP details
stay in its source adapter. Add fields and controls to the common contract
when their meaning overlaps, and keep truly source-only settings at the
boundary. In particular, Shelly credentials, host/protocol details, polling or
callback options, and channel selection stay in Shelly setup. Shared relay,
light, contact, sensor and power capabilities appear in the same catalog and
cards as equivalent Zigbee or Matter capabilities and dispatch through the
Shelly HTTP adapter behind the common control intent.

Treat Shelly as an example of the external-HTTP source class, not a special
case. Any later HTTP integration must register devices in this contract and
implement the same source-adapter interface for discovery, updates, and
commands. Provider-specific UI is limited to connection and configuration
needs; shared controls, history, alerts, and device grouping remain capability
driven. The common controller accepts semantic intents and selects the source
adapter using stable device identity and advertised capability, so dashboard
components do not need provider-specific control branches.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Discover Thread Matter devices and expose their supported controls and sensor
values through the same gateway model, history, alerts, and shared dashboard
used by Zigbee and external HTTP devices such as Shelly. Do not create a
Matter-only device catalog or common-control UI. Keep join/commissioning and
Matter/Thread-only settings in the Matter area.

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
4. Map Matter endpoint identity and advertised capabilities into the same
   protocol-neutral device contract and controller dispatch used by Zigbee and
   external HTTP devices such as Shelly. Reuse shared cards and common controls;
   keep stable `matter:` IDs and source provenance, with source-specific setup
   and transport at the boundary.
5. Map supported sensor clusters and units into shared device values/history.
   For ALPSTUGA, include temperature, humidity, CO2, PM2.5, and air-quality
   values only when advertised by the device. Preserve unknown values as null
   with their units and quality state.
6. Add report subscriptions with bounded retries and refresh behavior suitable
   for sleepy devices. Avoid tight polling and unnecessary flash writes.
7. Route Matter observations into snapshots, bounded history, alert thresholds,
   and notification rules while keeping Matter IDs distinct from Zigbee IEEE
   addresses. Keep the common presentation/source capabilities aligned with
   Zigbee and HTTP devices such as Shelly wherever they overlap.
8. Test with synthetic endpoints for supported/unsupported clusters, signed
   and unknown values, missing reports, reconnect, duplicate reports, and
   restart recovery. Verify Matter uses the shared controller/dashboard path.

## Acceptance

- Supported lights and plugs can be read and controlled over CASE.
- Zigbee, Matter and external HTTP devices such as Shelly share the device
  catalog and common dashboard/controller behavior wherever capabilities
  overlap.
- Equivalent functions use the same semantic type, capability names, state
  fields, and common control intents across Zigbee, Matter, and HTTP sources;
  adapters provide protocol translation rather than duplicate UI models.
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
3. Descriptor discovery and capability-gated OnOff/Level reads and commands
   have encrypted host evidence (`05053ec`, `6ec569a`) and native rendered
   controls (`1ab6e91`, `7ea5a52`). Reconcile catalogued nodes after remote
   CASE failure and integrate future reports into confirmed state.
4. A frontend adapter now projects Matter descriptors and cached values into
   the shared dashboard `Device` shape and `DeviceGroups` cards. Generic light
   controls dispatch through the Matter adapter; Zigbee and Shelly retain their
   existing source controllers while using the same capability/state contract.
   HTTP integrations such as Shelly are peers in this shared model, not a
   separate dashboard path. Commit `39a2469` adds source-neutral sensor names
   for shared card grouping/rendering. Commit `1f5077d` registers
   validated Matter snapshot endpoints in native `Gateway::Device` and
   deduplicates them in the common dashboard by stable ID. Keep Matter UI
   limited to joining and Matter/Thread settings. Native endpoint identity,
   descriptors and capabilities persist in `devices.json`; confirmed readings
   persist for process restart in `/tmp/microhad-zigbee-native-values.json` and are
   intentionally not power-loss durable.
5. Implemented CASE reads and typed native validation cover advertised
   temperature, humidity, CO2, PM2.5 and air-quality clusters; the frontend
   renders those values from its snapshot cache. Successful typed CASE results
   now update matching common native values, preserving units and null/unknown
   quality through snapshot reconciliation. Endpoint metadata is restored
   from the low-frequency store and latest confirmed values from `/tmp` after
   process restart. Shared history and alerts serve Matter, Zigbee and HTTP
   sources such as Shelly. Test signed values and ALPSTUGA-related clusters
   without assuming every target advertises all measurements.
6. A bounded one-attribute subscription request with 1-second minimum and
   3600-second maximum interval drains priming chunks and returns the
   negotiated subscription ID (`e4b5f8d`). The production manager schedules
   supported endpoint attributes and retries/renews bounded subscriptions;
   active OnOff reports reach its queue (`713948c`). Rust now decodes subscribed
   sensor paths, reads CO2/PM2.5 unit metadata before subscribing, and
   normalizes those values (`1855f24`). A TLV-level test covers measured and
   unknown values. The notifier-backed `--sensor-reports` fixture verifies
   temperature, humidity, air quality, CO2, and PM2.5 through multi-cluster
   subscriptions and manager correlation (`62d7fd4`). Still prove those sensor
   events through live service IPC, native state, and shared history/alerts.
   Confirm bounded retry/refresh with multiple endpoints and restart recovery.
7. Integrate snapshots, history, alerts and the shared dashboard through the
   native worker using
   distinct Matter IDs. Test restart, unsupported clusters, missing reports
   and concurrent Zigbee activity with synthetic peers.

## Native endpoint and reading restart restoration — 2026-10-04

Native commit `be2e98a` persists Matter endpoint identity, endpoint number,
advertised server clusters, and capability metadata to the low-frequency
`devices.json` store when the reconciled catalog changes. Confirmed values are
written to `/tmp/microhad-zigbee-native-values.json` only when they change, then
merged with the persisted endpoint by stable Matter ID during gateway startup.
This restores last confirmed values across a gateway process restart, while
keeping high-frequency updates off persistent flash; readings can be lost on
power loss. Snapshot reconciliation retains restored values and alert settings.
The persistence test round-trips Matter descriptors/capabilities and the
restart test restores CO2/PM2.5 units and readings before and after snapshot
reconciliation. Verified with `make ZIGGURAT=1 MATTER=1 host test` and
`make ZIGGURAT=1 MATTER=1 mips`; the latter passed with the toolchain's existing
LOAD-USE warnings. Report subscriptions and remote node recovery remain open.

## Matter report peer-correlation prerequisite — 2026-10-04

Rust repository commit `f58dcb7` adds a public `Exchange::peer_identity()` API
to the pinned `rs-matter` source through the repository's reproducible patch
mechanism. It returns the authenticated local fabric index and peer node ID for
an exchange, while group or unauthenticated sessions have no peer identity.
This provides the key needed to correlate later server-initiated reports
without treating a device-chosen subscription ID as globally unique. The patch
applies to the clean pinned source and is applied idempotently by
`scripts/fetch-rs-matter.sh`; Rust controller-identity tests pass 25/25.
Active report intake, managed subscription lifecycle, native IPC event delivery
and report lifecycle tests remain open.

## Bounded Matter subscription establishment — 2026-10-04

Rust commit `e4b5f8d` adds `subscriptions::establish_attribute`, which requests
one endpoint/cluster/attribute with a one-second minimum and one-hour maximum
report interval, acknowledges every priming chunk, and returns the negotiated
subscription ID and maximum interval. The production-owned synthetic accessory
fixture establishes an OnOff subscription after commissioned CASE discovery.
Verified with the release build and
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery`; the fixture passed commissioning,
catalog restore, descriptor discovery, read/control, and subscription
establishment over its synthetic RCP.

A first diagnostic follow-up used a sensor value change that did not match its
active subscription and timed out. The later OnOff fixture below corrects the
path and proves one supported active report exchange.

## Synthetic active Matter report exchange — 2026-10-04

Rust commit `4cc53c4` extends the production-owned commissioning fixture: it
establishes a bounded OnOff attribute subscription after operational CASE,
changes OnOff, accepts the immediate server-initiated `ReportData`, and checks
the authenticated fabric/node, negotiated subscription ID, endpoint, cluster,
and value. It returns `StatusResponse(Success)`, changes OnOff back, and
receives/acknowledges the second report so the remaining fixture starts from
the original device state. Verified with the release commissioning-probe build
and `python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery` against the synthetic RCP; all commissioning,
catalog, CASE control, two report transitions, and restart checks passed.
The listener must use immediate `Exchange::accept()`; delaying acceptance for
one second lets the transport discard an unclaimed responder exchange.

## Typed report decoder checkpoint — 2026-10-04

Rust commit `1b23a36` adds supported-path decoding for OnOff and CurrentLevel
`ReportData` values into the same `ResultValue` shape used by confirmed reads.
The synthetic OnOff report fixture asserts the decoded node, endpoint, cluster,
attribute, and boolean value after checking the authenticated peer and
subscription ID. At this checkpoint, reports are still fixture-only and do not
reach native IPC or `Gateway::Device`. Sensor report decoding, production
subscription scheduling, endpoint/subscription correlation, duplicate
filtering, reconnect behavior, and gateway state/history/alert updates remain
open.

Verification passed: focused Rust operations unit test, release commissioning
probe build, and
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery` against the synthetic RCP. The broader
Rust library suite passed all 62 tests when run with Unix-domain socket access.

## Bounded active-report receiver checkpoint — 2026-10-04

Rust commit `5bcd632` adds a reusable receiver for server-initiated `ReportData`
exchanges. It accepts immediately, requires authenticated fabric/node identity
and a subscription ID, validates the report opcode, decodes supported OnOff and
CurrentLevel paths, combines chunked reports under a 128-value bound, and sends
`StatusResponse(Success)` for chunks that request an acknowledgement. The
synthetic fixture now uses this helper for both OnOff transitions and verifies
the peer, subscription, typed value, and continued accessory operation.

Verification passed: release commissioning probe build, all 62 Rust library
tests with Unix-domain socket access, and
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery` against the synthetic RCP. The production
Manager at this earlier checkpoint did not schedule subscriptions or poll this
receiver, and the
service/native IPC path does not yet deliver reports to shared device state.

## Production device-manager subscription checkpoint — 2026-10-04

Rust commit `713948c` makes the production `devices::Manager` derive
subscriptions from each online endpoint's advertised OnOff, Level, air-quality,
temperature, humidity, CO2, and PM2.5 clusters. Establishment is serialized
behind queued control jobs and discovery, bounded to 224 paths with 30-second
deadlines, exponential retry, and renewal at the negotiated maximum interval.
Radio detach clears established IDs for fresh establishment after recovery;
commissioning pauses new setup while allowing an already accepted report to
finish its acknowledgement. The manager polls the active-report receiver,
correlates fabric/node/subscription/path, suppresses identical consecutive
values, and exposes a 512-value bounded queue for its consumer.

The production-owner synthetic fixture limits itself to one subscription,
proves the manager establishes it from descriptors, then toggles OnOff twice
and reads both peer-correlated values from the manager queue. The harness now
requires that exact success marker. Verification passed: all 63 service-feature
library tests, release builds of the controller service and commissioning
probes, and
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery` against the synthetic RCP.

At that checkpoint, the queue was not yet published through Matter IPC or
applied to native `Gateway::Device`; only OnOff and CurrentLevel report values
were decoded.
Sensor report parsing, delivery/backpressure, shared history/alerts, adaptive
sleepy-device timing, broad multi-subscription/reconnect coverage, and remote
catalog recovery remain open. Plan checklist items 6 and 7 therefore remain
unchecked.

## Shared-device report delivery checkpoint — 2026-10-04

Rust commit `3aa4600` serializes validated readings as a typed `matter_report`
event, broadcasts to connected native IPC clients, and retains queue entries
until at least one client accepts them. Native commit `5ec72cb` validates the
event and applies it in `MatterWorker::collectEvents` through
`Gateway::applyMatterReading`. OnOff and CurrentLevel reports therefore update
the shared device state, source-neutral history, and common alert engine, and
remain separate from commissioning/phone BLE events.

Verification passed: Rust's 65 service library tests (including strict event
serialization and broadcast to two IPC clients), controller-service release
build, native `make ZIGGURAT=1 MATTER=1 host test`, and MIPS build. The C++ tests
cover valid OnOff/Level report payloads and reject CurrentLevel 255. The manager
synthetic fixture separately verifies production subscription scheduling and
report correlation; the direct live IPC-to-native-worker process round trip is
not yet covered. Only OnOff and Level report values are currently decoded.
Sensor reports, broad multiple-subscription/reconnect/backpressure behavior,
remote-node reconciliation, and sleepy-device policy remain open.

## Dependencies

[persistent identity](03-persistent-fabric-identity.md),
[Thread lifecycle](06-thread-network-lifecycle.md), and
[commissioning/control IPC](02-commissioning-control-ipc.md).

The production-owned commissioner now reaches encrypted PASE, Thread provision
and CASE completion in the synthetic accessory fixture. The running service
contains that task, persistent catalog and production SRP/DNS resolver. Remote
node recovery remains open. Production lookup uses no fixture address injection and remains bounded when
registration/attachment is absent.
Descriptor/control/report integration and remote catalog reconciliation remain the
next requirements here; the owned fixture is not device-model acceptance.

## Descriptor helper checkpoint — 2026-10-04

Rust commit `05053ec` adds a reusable operational CASE discovery helper: root
PartsList followed by ServerList for root and advertised endpoints. It bounds
snapshots to 32 endpoints and 64 server clusters per endpoint, rejects malformed,
duplicate and wildcard endpoint IDs, and returns no partial snapshot on failure.
The encrypted synthetic accessory fixture finds endpoints 0/1 and the OnOff
cluster through this helper; the harness requires its success marker.

All 55 service-feature library tests and both host/MIPS commissioning-probe
builds pass. Production SRP/DNS, owned commissioning, temperature/OnOff and
persisted-authority fresh CASE fixture checks continue to pass. Details and
commands: `ziggurat-compilation/docs/matter-device-discovery.md`.

Checklist item 3 remains open: service scheduling, catalog reconciliation,
device type/feature metadata, bounded job deadlines and native device snapshots
are not implemented by this helper. No device deployment or plan 08 acceptance
was performed.

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
   Bounded PartsList/ServerList helper and service scheduler are verified over
   encrypted CASE (`05053ec`, `5c588df`). Completed nodes refresh after catalog
   restore and radio reattachment; revisioned snapshot IPC is implemented (`237d110`).
   Incomplete-node recovery, native snapshot validation/API/UI remain open.
4. [x] Implement validated OnOff/Level reads and commands plus typed IPC
   bodies in plan 02. Confirm resulting values through reads/reports and expose
   unsupported clusters and failed commands explicitly.
   OnOff executor/readback and native API/dashboard are verified (`0c66b87`,
   `ee9a4bb`). Rust Level executor/readback is verified (`6ec569a`); native
   Level IPC/API validation, HTTP admission and rendered controls are verified
   (`1ab6e91`, `7ea5a52`). Commands return confirmed values,
   with explicit operation errors and no optimistic state.
5. [ ] Decode advertised sensor attributes with units, signed values and null
   semantics. Cover temperature, humidity, CO2, PM2.5 and air quality using
   synthetic descriptors rather than assuming ALPSTUGA exposes every cluster.
6. [ ] Add bounded subscriptions, duplicate filtering, sleepy-device refresh
   and reconnect backoff; persist meaningful changes rather than each report.
7. [ ] CASE operation values feed the common in-memory state and the shared
   temperature/humidity/brightness alert engine. Matter writes confirmed
   values to the source-neutral unit-aware journal; route Zigbee and remaining
   external HTTP history through it too. Restart restoration of endpoint
   metadata and readings, CO2/PM2.5 alert support and alert-setting restoration
   are implemented. Bounded reports remain open.
   Test unsupported clusters, missing/duplicate reports, address changes,
   restart and concurrent synthetic Zigbee activity; document remaining limits.

Validation: encrypted synthetic accessory tests using production discovery,
private catalog restore and phone-detached CASE, followed by report/API tests.

## Rust OnOff job-dispatch checkpoint — 2026-10-04

Rust now exposes OnOff attribute reads and on/off/toggle commands through a
bounded asynchronous job queue. Admission and start-time authorization require
a completed catalog node and an online discovered endpoint advertising cluster
6. A job returns its confirmed post-command OnOff readback; unsupported paths
are rejected without an exchange. The queue holds at most eight live jobs, runs
one CASE operation at a time, includes queue wait in its 30-second deadline and
fails on Thread loss without replaying mutations. Commissioning and operations
are serialized. The encrypted synthetic accessory fixture exercises all three
commands via this service queue.

Verification: 59 service-feature Rust tests, encrypted service-owner fixture,
and host/MIPS service and commissioning builds pass. Native read/control HTTP
routes and worker polling now deliver Rust's confirmed boolean readback through
the native job API. Rendered controls, Level Control, sensors,
subscriptions/reporting, remote recovery and broader restart handling remain
open. No physical-device test or plan 08 acceptance was performed.

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

## Service descriptor scheduler checkpoint — 2026-10-04

Rust commit `5c588df` schedules operational discovery for Complete catalog nodes
in the production service event loop. New completions are observed without a
restart. One job runs at a time with a 30-second total deadline; successful
snapshots refresh after 300 seconds and failed nodes back off from 5 to 320
seconds. Oldest due jobs run first. Commissioning preempts discovery; Thread
detach drops work, marks cached descriptors offline and triggers refresh after
reattachment. No scheduler operation writes credentials or promotes an incomplete
catalog entry. In-memory snapshots have at most 64 nodes, each with the existing
32 endpoint/64 server-cluster bounds.

Verification: 57 library tests pass, including deadline expiry, fairness,
preemption, radio invalidation, retry cap, incomplete-node exclusion and no flash
writes. The encrypted fixture exercises the actual manager after catalog reopen,
then detach and refresh. Both service/probe host and MIPS builds pass; actual
service IPC and native real-service worker regressions pass. Stripped MIPS
service/probe sizes are 1,961,656/2,301,648 bytes. Findings and commands are in
`ziggurat-compilation/docs/matter-device-discovery.md`.

Checklist items 2–7 remain open. Completed-node runtime recovery is implemented;
remote recovery of incomplete nodes, legacy migration, feature/type metadata,
snapshot IPC/API, reads/commands, subscriptions and native reports remain.
No physical gateway execution or plan 08 acceptance was performed.

## Revisioned snapshot IPC checkpoint — 2026-10-04

Rust commit `237d110` dispatches opcode-10 snapshot requests from the private
catalog and cached descriptors, including before Thread startup. One endpoint
per page bounds payloads; incomplete/undiscovered nodes remain visible with
explicit state and null endpoint/ID. Stable endpoint IDs include fabric/node.
Each reply includes an opaque revision initialized from OS randomness; nonzero
cursors require it, and visible changes invalidate prior pages. Revisions do
not change on identical refreshes or wrap on exhaustion.

All 57 service-feature tests pass, including pagination, stable IDs, stale or
missing revision, invalid cursor, maximum cluster-ID page size and exhaustion.
Actual service IPC verifies empty snapshots and error handling before Thread
startup. The encrypted synthetic accessory fixture verifies both descriptor
pages through the actual manager. Host/MIPS service/probe builds and existing
native real-service worker regressions pass. Details and artifact hashes are in
`ziggurat-compilation/docs/matter-device-snapshot-ipc.md`.

Plan 02 device dispatch and plan 04 snapshot API/views remain open: native
opcode-10 request/reply validation must precede HTTP exposure. Reads, commands,
subscriptions, incomplete-node reconciliation and native reports remain pending.
No physical gateway/accessory acceptance or plan 08 action was performed.

## Capability-gated OnOff executor checkpoint — 2026-10-04

Rust commit `0c66b87` adds strict OnOff read/on/off/toggle transaction decoding
and admission through the manager's Complete catalog/online endpoint/advertised
cluster checks. The executor performs encrypted invocation then a separate
confirmed readback, with a 30-second execution deadline and typed boolean result.
It does not publish optimistic values, retry mutations or write flash. Offline,
unknown and unsupported endpoints fail before an exchange starts.

All 58 library tests and host/MIPS service/probe builds pass. The encrypted
synthetic accessory fixture exercises on/toggle/off confirmed values, rejects
root OnOff support that is not advertised and rejects offline operation. The
harness requires its executor success marker; existing discovery/catalog,
snapshots, temperature and fresh CASE checks still pass. Commands, result shape
and cancellation limits are documented in
`ziggurat-compilation/docs/matter-operational-transactions.md`.

At this earlier checkpoint, item 4 remained open because operations were not
connected to service IPC. The subsequent Rust OnOff job-dispatch and Level
executor checkpoints supersede that state. Native Level routes/UI and sensor,
subscription/report integration remain pending. This is host implementation
evidence, not plan 08.

## Rust Level Control executor checkpoint — 2026-10-04

Rust commit `6ec569a` adds capability-gated cluster-8 CurrentLevel reads and
MoveToLevel commands to service jobs. The operation rechecks endpoint support
when it leaves the queue and reports only a separate CurrentLevel readback.
The encrypted accessory fixture advertises cluster 8 and confirms read level 1,
move to level 124 and read back 124. It rejects Level reads against endpoints
without cluster 8. Rust host tests, host/MIPS builds and the complete encrypted
service-owner fixture pass; MIPS artifact hashes and limits are in plan 02.

This closes the Rust service-executor portion of checklist item 4. Native typed
IPC validation, HTTP read/command admission, worker result handling and dashboard
controls for Level remain open; sensor measurements, subscriptions and reports
also remain open. No target execution or plan 08 acceptance occurred.

## Native Level operation pipeline checkpoint — 2026-10-04

Native commit `1ab6e91` completes the Level request/reply half of item 4: strict
CurrentLevel read and MoveToLevel IPC validation, HTTP admission, generic worker
polling, and cluster-typed U8 response validation. Unknown CurrentLevel reads
retain `null`; mutations require a concrete value from the service's separate
CASE readback. Range validation rejects levels above 254 and transition values
above 65535.

Verification passed: default and Matter-enabled native host suites, default and
Matter-enabled MIPS cross-builds, and the synthetic native worker HTTP round
trip. It read level 0, commanded 124 with a 20-decisecond transition and
confirmed level 124. Host/MIPS builds do not establish target radio behavior.
Rendered controls, sensors, subscriptions/reports, remote node recovery and
legacy migration remain open; plan 08 was not performed.

## Rendered Level control checkpoint — 2026-10-04

Native UI commit `7ea5a52` exposes a cluster-8 CurrentLevel read and MoveToLevel
control in the device catalog. The slider sends a selected 0–254 level and an
explicit transition duration; the displayed value changes only after confirmed
CurrentLevel readback. Null read values display as unknown, and a null command
readback remains an error. Offline or unavailable endpoints cannot be operated.

Verified with `make webui`, all 18 actual-HTTP tests in
`python3 scripts/test_matter_webui.py`, and the production `MatterDevices`
component in headless Chromium via `node webui/test/matter-devices-browser.cjs`.
The fixture verified level 0 read, MoveToLevel 124 with a 20-decisecond
transition, and confirmed 124. Sensors, subscriptions/reports, remote node
recovery and legacy migration remain open; plan 08 was not performed.

## CASE sensor transport checkpoint — 2026-10-04

Rust commit `ca0019e` adds capability-gated CASE reads for Air Quality (91),
Temperature Measurement (1026), Relative Humidity (1029), CO2 Concentration
(1037), and PM2.5 Concentration (1066). It converts signed hundredths-Celsius
and hundredths-percent values, preserves nullable values and quality, and
returns concentration measurements with the endpoint's MeasurementUnit.
Negative/non-finite concentrations and humidity above 100 percent are rejected.
The encrypted production-owner fixture reads all five from an advertised
synthetic endpoint.

Native commit for this checkpoint adds strict decimal JSON parsing,
cluster-specific typed response validation for units/ranges/null-quality, and
read admission for those five clusters. Verification passed: 60 Rust service
library tests; host commissioning-probe build; the full encrypted production
service-owner fixture with synthetic BLE and SRP/DNS discovery; and
`make ZIGGURAT=1 MATTER=1 host test` including native unit/integration tests and
the 13-case Ziggurat peer verifier. Native validation rejects malformed units
and inconsistent null/quality pairs. These are service/native transport
capabilities only. The values are not yet in the shared Zigbee/Matter/Shelly
gateway model, controller or dashboard. That common projection remains the next
device-model checkpoint. Synthetic host evidence does not establish
physical-device behavior or plan 08 acceptance.

## Sensor report decoding checkpoint — 2026-10-04

Rust commit `1855f24` extends subscription decoding to standard air-quality,
temperature, relative-humidity, CO2, and PM2.5 measured-value attributes.
Temperature is converted from hundredths of a degree Celsius; humidity is
converted from hundredths of a percent and rejects values above 100%; air
quality enums map to canonical shared strings. CO2 and PM2.5 values remain raw
until the manager reads each endpoint's Matter `measurementUnit` metadata, then
become typed measurements with units and measured/unknown quality. Invalid,
negative, or non-finite concentration values are rejected.

The new TLV decoder test verifies signed and unknown temperature, humidity,
air-quality enum mapping, and raw CO2/PM2.5 decoding. Verification passed:
`cargo +nightly-2026-10-02 test --manifest-path rs-matter-probe/Cargo.toml
--features controller-probe --lib` (66/66), the release
`thread-matter` service build, and
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery` (existing production-manager OnOff report
path). A sensor report was not claimed from that fixture: changing its external
test sensor cells does not notify the accessory's Matter subscription
scheduler. A notifier-backed live sensor report and native IPC integration test
were open at this checkpoint.

## Active Matter sensor report fixture — 2026-10-04

Rust commit `62d7fd4` adds a reproducible probe-only patch to the pinned
`rs-matter` worktree exposing the interaction model's attribute-change
notifier. The synthetic accessory now uses it when its sensor values change,
so the fixture exercises actual server-initiated `ReportData` rather than
priming reads. `scripts/fetch-rs-matter.sh` applies the patch idempotently.
The production `Manager` establishes subscriptions for all seven advertised
paths, receives OnOff changes and five sensor clusters, correlates fabric/node/
subscription/path, converts measurement units, and returns normalized values.
The isolated `--sensor-reports` mode exits before the separate controller-stack
restart scenario; multi-subscription behavior across restart is still open.

Verified with
`python3 scripts/test-openthread-matter-transport.py --commissioning --ble
--service-owner --service-discovery --sensor-reports`, which passes the
notifier-backed temperature, humidity, air-quality, CO2, PM2.5, and OnOff
reports over the synthetic RCP. The normal production-owner fixture without
`--sensor-reports` also passes, including fresh CASE after controller restart.
The Rust library suite passes 66/66 and the release `thread-matter` service
build passes against the patched pinned dependency. This remains host-only
evidence; live Rust-service-to-native report-event integration, remote recovery,
and plan 08 acceptance remain open.
