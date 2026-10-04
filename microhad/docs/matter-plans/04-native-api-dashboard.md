# Plan: Native Matter API and dashboard

## Current open work — 2026-10-04

Queued commissioning/Thread APIs, the owned WebSocket relay and a feature-aware
dashboard onboarding/progress panel exist. Queued device snapshot pages now have
native validation, HTTP exposure and a dashboard view. The dashboard exposes
confirmed OnOff reads and commands for online cluster-6 endpoints. Native HTTP
also admits validated CurrentLevel reads and MoveToLevel jobs for cluster-8
endpoints. The dashboard now renders Level controls and confirmed readback.
Matter endpoint projections now feed the same dashboard `DeviceGroups` cards
used by Zigbee and external HTTP devices such as Shelly. Shelly already uses
those shared cards; its setup remains in the external-device dialog and its
commands use its HTTP controller. The dashboard adapter for Matter preserves
the common device shape while sending commands through the Matter controller.
The native persistent device model and broader failure gates remain open. A
new stalled-peer regression test proves the native Matter IPC exchange returns
at its deadline without waiting for a peer that accepted but never answered
(`750c4c9`); absent/stalled service matrix coverage remains incomplete. A
bounded unit-aware history journal now stores confirmed
Matter measurements under stable source IDs and is exposed by the common
history API/dashboard. Keep only joining/commissioning and Matter/Thread-
specific settings in the Matter area.

The shared-device requirement includes external HTTP integrations such as
Shelly as first-class sources, not just a visual reuse of cards. The common
device contract owns stable identity, semantic device type, displayed state,
telemetry names/units/quality, and capability presentation. Equivalent
capabilities must produce the same common type and controls across Zigbee,
Matter, and HTTP devices: a controllable light, for example, uses the common
light presentation and intent regardless of adapter. Each source adapter owns
discovery/setup and translates common control intents to its transport
(Zigbee coordinator, Matter service, or Shelly HTTP API). Common dashboard
components consume the contract and capabilities rather than branch on
protocol names or cluster IDs. Source-specific controls remain at the boundary
only when their meaning or transport behavior has no shared equivalent.
For Shelly, connection details, credentials, polling/callback configuration,
and device-specific channel selection belong in the Shelly setup flow; relay,
light, contact, temperature and power behavior that overlaps the common model
must use the shared catalog, cards, history and controller intents. Do not add
a parallel Shelly device dashboard or require users to learn a different
control for the same capability.

Rendered snapshot and OnOff component interaction is verified in Chromium.
Next deliverable: route Zigbee and external HTTP measurements through the same
journal, restore Matter alert settings, and extend broader API failure gates.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Expose commissioning and Thread status through Matter-specific setup UI, and
expose Matter devices through the same gateway device model, catalog, dashboard
and controller paths used by Zigbee and external HTTP devices such as Shelly.
Matter-specific UI is limited to join/commissioning and settings that have no
shared equivalent.

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
5. Add feature-aware Matter setup UI, then project commissioned endpoints into
   the existing protocol-neutral device model. Reuse the common device catalog,
   capability controls, telemetry presentation and controller dispatch for
   overlapping behavior across Zigbee, Matter and external HTTP sources such as
   Shelly. Treat HTTP integrations as normal contract providers, not special
   dashboard-only exceptions. Keep transport and source-specific setup at the
   source boundary; retain Matter-specific join and Thread settings.
   Hide Matter setup when `MATTER=0`; show unavailable/reconnecting state when
   the feature is enabled but the service is absent.
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
- Device discovery, identity, common controls and telemetry appear in the same
  catalog and dashboard flow for Zigbee, Matter and external HTTP devices such
  as Shelly; protocol differences do not create duplicate device cards,
  protocol-specific copies of common telemetry, or common-control flows.
  Equivalent functions use the same semantic type, capability names, state
  fields, and common control intents across all three source families.
  Source adapters may provide extra settings only where their transport or
  device type requires them.
- The Matter-specific area contains only joining/commissioning and Matter or
  Thread settings; disabled builds hide it and enabled builds report service
  unavailability clearly.
- The dashboard works over the selected trusted-LAN HTTP/WebSocket transport.

## Status quo — 2026-10-04

Native status, queued commissioning/Thread mutation routes and the owned relay
are host-tested. The Controls tab now hides Matter setup on disabled builds,
gates mutations on validated readiness, creates/imports an absent Thread network,
hands accessory onboarding to Garage and watches/cancels a known commissioning
session. The dashboard client has nine actual localhost HTTP tests and passes
the required production web build. The dashboard projects Matter endpoints
into the shared device cards and routes common controls through the Matter
adapter. Shelly continues to use those cards with its HTTP controller. Matter
endpoints also enter the native in-memory catalog from validated snapshot
pages; native persistence, history/alerts, reports and broad failure gates
remain open.

Native UI commit `7822c32` removes the separate Matter device list and adds the
Matter-to-common-device adapter and controller dispatch. `make webui`,
`node webui/test/matter-devices-browser.cjs`, and
`python3 scripts/test_matter_webui.py` pass. The browser test confirms shared
device cards plus the Matter join/settings area; the API suite passes 20/20.
Commit `39a2469` moves air-quality classification and CO2/PM2.5 presentation to
canonical `sensor_values` keys, so shared cards no longer inspect Matter cluster
IDs. Temperature and humidity continue through the gateway's existing common
fields. The dashboard build, browser interaction and 20 API tests pass again.
Native commit `a841aca` adds shared `/api/status` fields for air quality, CO2
and PM2.5, including unknown-state flags and serialization tests. Commit
`1f5077d` parses completed snapshot pages and registers Matter endpoint
identity, online state and advertised capabilities in native `Gateway::Device`
through the coordinator runner. The UI deduplicates matching IDs with its live
projection. Native commit `02f170f` decodes successful typed CASE operation
results and updates the matching registered device before the job is published
as complete. Shared `/api/status` exposes confirmed state and canonical
`sensor_values` with measured/unknown quality and service-reported units;
legacy `co2_ppm` and `pm25_micrograms_m3` fields remain meaningful only for
their named units. Snapshot reconciliation preserves values and quality.
Values remain in memory only outside the bounded history journal; report
integration remains open. Native commit `e81af44` now feeds Matter
temperature, humidity and brightness values into the common alert-threshold
processor and preserves alert configuration/state when snapshot capabilities
refresh. Native commit `5cfb3b1` restores common alert settings for stable
Matter endpoint IDs from `devices.json` across gateway restart and snapshot
reconciliation. Native commit `d5d94ba` adds CO2 and PM2.5 threshold alerts
whose configured unit must match the current reported sensor unit; that unit
survives restart with the limits. The shared Matter card now exposes the same
settings and history actions as other providers, and Chromium verifies the
unit-labeled CO2/PM2.5 alert fields. Host, API, browser and MIPS checks pass;
the MIPS toolchain emits its existing LOAD-USE warnings.

## Source-neutral history checkpoint — 2026-10-04

The current implementation adds a bounded `.values` sidecar journal keyed by
the complete stable source ID, preserving units without placing Matter IDs in
Zigbee IEEE-address fields. It stores confirmed Matter temperature, humidity,
CO2, PM2.5, air-quality, light and brightness samples, plus Shelly contact,
relay, temperature and power updates under `shelly:<external-id>:<component>`.
The journal caps at 8,192 records and compacts to recent records; duplicate and same-minute
numeric samples are suppressed. The common history route serves these records,
and the dashboard renders the added measurements with unit-aware plots.
Native commit `e35abb7` routes Shelly history through the same stable-ID
journal and dashboard history dialog. Shelly contact, relay, temperature and
power history use the shared device history route. Native commit `a522062`
moves Zigbee temperature, humidity, contact, occupancy and battery, plus
FRITZ!Box presence transitions, into this same journal. IDs are source-prefixed
(`zigbee:<IEEE>`, `shelly:<external-id>:<component>`, and
`fritz:<external-id>:presence`); Matter retains its full endpoint ID. Values
keep their units. Zigbee still falls back to the legacy history file when its
source-ID journal has no samples. The bounded journal uses `/tmp` and does not
survive power loss. Alert restoration and CO2/PM2.5 threshold support remain
open. Verification passed:
`make ZIGGURAT=1 MATTER=1 host test`, `make ZIGGURAT=1 MATTER=1 mips`,
`npm run build`, `node webui/test/matter-devices-browser.cjs`, and
`python3 scripts/test_matter_webui.py` (20/20 API tests).

## Remaining checkpoints

1. [x] Apply typed confirmed Matter operation results to registered native
   `Gateway::Device` instances while preserving stable `matter:` IDs and
   source provenance. Unknown readings retain their units and serialize as
   null/unknown; refresh reconciliation preserves confirmed values.
2. [x] Route Matter, Zigbee, Shelly and FRITZ!Box telemetry through the
   source-neutral unit-aware journal and restore common Matter alert settings
   across gateway restart, while keeping setup and transport in source
   adapters. Unit-aware CO2/PM2.5 alerts and gateway-restart restoration are
   implemented. History is intentionally volatile across power loss: the
   bounded journal remains in `/tmp` to honor the no-high-frequency-flash-write
   policy; low-frequency settings that need restart restoration remain in the
   runtime directory. Keep join and Thread-specific settings in the Matter
   area.
3. Extend broad enabled/disabled, absent/stalled service, malformed reply, queue
   pressure, cancellation, socket-loss and secret-redaction gates. Native IPC
   deadline behavior now has a stalled-peer test (`750c4c9`); existing native
   and frontend tests cover other subsets, so checklist item 6 remains open.
4. Inspect rendered browser interactions in addition to the host client and
   TypeScript/build evidence. Physical pairing remains exclusively in plan 08.

## Native Level API checkpoint — 2026-10-04

Native commit `1ab6e91` adds strict HTTP/IPC admission for CurrentLevel reads
and MoveToLevel commands. Levels are bounded to 0–254 and transition duration
to 0–65535 deciseconds. The native worker polls the service operation job and
returns the typed U8 value only after service confirmation. The fake-service
HTTP round trip reads level 0, moves to 124 over 20 deciseconds, and confirms
124. Native host tests and both Matter-enabled and default MIPS builds pass.

The device view still has no Level slider/read control. Rendered Level
interaction, sensor measurements, subscriptions/reports and the broader
failure matrix remain open. No device acceptance or plan 08 action occurred.

## Rendered Level controls checkpoint — 2026-10-04

Native UI commit `7ea5a52` adds a CurrentLevel read, a 0–254 brightness slider,
transition input in deciseconds, and a Set brightness action for online
endpoints advertising cluster 8. Values update only after the service returns
its typed CurrentLevel readback; null reads display as unknown and cannot
confirm a mutation. Controls are disabled for offline endpoints or unavailable
service.

Verification passed: `make webui`, 18 actual-localhost tests in
`python3 scripts/test_matter_webui.py`, and
`node webui/test/matter-devices-browser.cjs` under Chromium. The rendered test
changes the selected level and transition, submits MoveToLevel, and confirms the
returned value. It also retains the existing pagination/revision and availability
checks. Sensors, other cluster controls, broad failure gates and physical
accessory behavior remain open; plan 08 was not performed.

## Dependencies

[Commissioning and control IPC](02-commissioning-control-ipc.md) and
[persistent identity](03-persistent-fabric-identity.md). Define the relay
endpoint contract with the Garage workstream so the app can connect when its
plan is implemented.

Native snapshot checkpoint — 2026-10-04: `observed_network_channel` is populated
only by Ziggurat Ready, cleared on Down, and suppressed when offline/unformed.
Legacy EZSP and persisted configuration report zero until a radio observation
exists. `ViewLease::coordinatorOnline()` and `coordinatorChannel()` expose the
immutable data without accessing Gateway from a worker. Tests cover persisted
channel versus observed channel, immutable copies and offline/unformed state.
The periodic Matter worker, queues and IPC forwarding remain pending.

Verification passed: `make host test`, `make mips`,
`make ZIGGURAT=1 MATTER=1 host test` and
`make ZIGGURAT=1 MATTER=1 mips`. The enabled suite includes 13 native peer
checks. Suites must run sequentially because their legacy tests share `/tmp`
log paths; the initial concurrent enabled run failed a redaction assertion,
then the isolated full rerun passed. No gateway deployment occurred.

Native worker checkpoint — 2026-10-04

Matter-enabled startup creates one `matter-ipc` worker with a 64 KiB stack;
Matter-disabled startup creates no Matter thread. Both HTTP listeners share its
cached, validated status and never connect to the service for those reads.
The worker sends private channel/online observations about once per second,
then fetches Matter/Thread status. Failures clear the cache; snapshots expire
after five monotonic seconds. Shutdown joins the worker before deleting the
coordinator; each IPC operation has the existing one-second total deadline.

Coordinator poll progress is recorded separately from dirty dashboard views.
Progress older than 2.5 seconds, unknown channel, offline/unformed state or
shutdown produces an offline channel-zero observation. Matter-enabled idle
coordinator select sleeps are capped at one second to refresh liveness. A frozen
coordinator therefore cannot keep renewing an old online snapshot. The worker
does not read Gateway or radio state directly, write flash, or log payloads.

Worker tests cover the offline observation body, strict snapshot exchange,
cached reads, duplicate start, stop/restart and absent service. Synthetic native
round-trip coverage uses the actual Matter-enabled binary and scripted Ziggurat
and IPC peers for confirmed channel, radio loss, twenty quick HTTP reads and
absent-service 503. Commissioning/relay mutation routes and queues remain open.

Verification: default and Matter-enabled `make host test`/`make mips` pass,
including the final default rerun after startup cleanup/test edits. Both
`python3 scripts/test_matter_native_worker.py` and the same command with
`--real-service` pass. The latter runs the real Rust service against a synthetic
RCP, authorizes create through the native channel observation, confirms cached
HTTP status/configuration, records only matching channel writes, then observes
Thread return to `awaiting_coordinator` on Zigbee peer loss. It shuts down both
temporary services and removes private synthetic state; no physical gateway
or real accessory is used. The real-service check requires the already built
companion host service and synthetic relay binaries.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Consume plan 02's persistent channel and bounded queue in MatterWorker.
   Publish protected session/job snapshots and events; retain coordinator-only
   ownership of Gateway and keep HTTP handlers independent of IPC waits.
2. [x] Define and implement asynchronous commissioning start/status/cancel
   and Thread create/import routes with bounded input, stable IDs, fixed
   errors and immediate queue acceptance/rejection.
3. [x] Define and implement one bounded WebSocket BLE relay contract with
   attach, indication, write, write-complete and connection-loss behavior.
   Test generations, ordering, cancellation and relay ownership with plan 05.
4. [x] Add feature-aware onboarding/progress/retry UI. Preserve trusted-LAN
   HTTP/WS operation without requiring a trusted gateway certificate; keep
   setup input out of logs and persistent dashboard state.
5. [ ] Expose device snapshots/read/control and render advertised controls and
   measurements, including unavailable/null values. Snapshot API/view, bounded
   HTTP client, OnOff read/control and rendered OnOff/Level interaction are
   verified. Sensor measurements and other cluster controls remain open.
6. [ ] Verify enabled/disabled builds, unavailable/stalled service, invalid
   input, queue saturation, WebSocket loss and secret redaction. Run default
   and enabled host/MIPS suites sequentially and make webui for UI edits.

Validation: fake and actual-service API tests must show responsive cached reads
and immediate job submission while commissioning waits on BLE or Thread.

## Confirmed OnOff dashboard checkpoint — 2026-10-04

The device view now offers power read, On, Off and Toggle only for an online
endpoint advertising OnOff cluster 6. Each action uses the bounded native job
API, waits for operation completion and displays only a schema-checked CASE
readback boolean. Requests are not automatically retried; operation polling
allows 35 seconds for the service's bounded 30-second queue/operation deadline.
Offline endpoints and unavailable service disable controls, and operation
failures ask the user to read again before trusting the displayed state.

`make webui` passes TypeScript checking and the production build.
`python3 scripts/test_matter_webui.py` passes 16 client tests, including exact
typed values and rejection of malformed/unsupported requests. The headless
Chromium harness passes rendered refresh/pagination plus confirmed read, On and
Toggle interactions, stale revisions, recovery state, duplicates, empty state,
availability and unmount. No device deployment or plan 08 acceptance occurred.

Plan item 5 remains open for sensor measurement rendering and controls for
additional clusters; item 6's broader API and failure matrix also remains open.

## Persistent worker and queued-call checkpoint — 2026-10-04

The native Matter worker now owns one persistent IPC channel. Its private
coordinator observations, Matter/Thread snapshots and commissioning/relay calls
share that channel; the worker polls typed events between calls. Heartbeats
remain approximately one second apart and take scheduling priority over queued
jobs. HTTP status still reads only protected cached snapshots.

The worker accepts strict schema-1 commissioning/relay requests (opcodes 3–8)
into a RAM queue of at most eight pending calls. Job IDs are nonzero u32 and do
not wrap; at most sixteen job snapshots are retained, with only terminal jobs
eligible for eviction. Submission returns immediately; a completed IPC call's
validated reply is available separately from the commissioning session lifetime.
A service-level error is a completed call with an `ok:false` reply, while a
transport failure marks the call failed. Pending request bodies are cleared on
execution/failure/shutdown and never returned in snapshots or logs.

Eight validated events may await the relay consumer. Event overflow or service
loss closes the IPC channel, clears stale cached status/events and fails pending
calls. The worker reconnects for the next heartbeat but never automatically
replays a mutation or BLE callback. A later relay implementation must explicitly
recover session state and reattach rather than assume a lost C1 write was sent.
No new flash writes are introduced.

Public commissioning/job/Thread mutation APIs and the WebSocket event consumer
remain open in plan 04; Thread create/import queue support is also still open.

Focused worker tests use a single persistent fake peer to verify offline
heartbeat/status correlations, asynchronous cancellation submission, cached
reads during a delayed reply and an event arriving between calls. A failed
peer test blocks one call, fills all eight pending slots, rejects overflow and
checks all nine jobs fail on disconnect without returning request bodies.
Restart preserves monotonic IPC correlations; worker shutdown clears job/event
caches. Invalid setup PINs and unavailable-service submissions fail early.

Verification passed: default `make host test`/`make mips` and
`make ZIGGURAT=1 MATTER=1 host test`/`make ZIGGURAT=1 MATTER=1 mips`.
Host suites ran sequentially; enabled peer checks pass (13 tests). Both
factory binaries remain stripped static big-endian MIPS-I ELF files.
`python3 scripts/test_matter_native_worker.py` passes with its updated persistent
fake peer, and `python3 scripts/test_matter_native_worker.py --real-service`
passes against companion commit `45782d3` with synthetic RCP/Ziggurat peers.
The actual native binary renews confirmed channel observations, reads cached
HTTP snapshots, observes radio loss and returns 503 after service exit.
Queue/cancellation/event delivery evidence is from the focused fake-peer tests;
public API-to-commissioner integration remains open. No physical gateway test,
deployment, accessory pairing or plan 08 action was performed.

## Queued commissioning HTTP checkpoint — 2026-10-04

Start/status/cancel now have native POST routes that validate strict schema-1
bodies and return 202 with an ephemeral job ID. GET job reads use protected RAM
snapshots only. `complete` means receipt of a validated service reply, not
successful accessory commissioning; service errors remain in that reply.
Invalid input returns 400; unavailable/full queues return fixed 503 errors;
unknown jobs and disabled control routes return 404. The 202 reason phrase is
`Accepted`. Input/setup codes are not reflected in replies or logs.

The route contract is [Native Matter HTTP API v1](../matter-native-api-v1.md).
Native integration tests cover disabled routes and malformed/overflowing input.
The actual-binary harness adds queued start, busy/status, cancel/idempotence,
stale-session and job-ID validation, plus fast HTTP acceptance and cached reads
while the synthetic service delays a reply. Actual Rust service verification
uses synthetic coordinator/RCP peers and the existing private dataset-create
setup; this checkpoint does not add public Thread create/import or BLE transport.
WebSocket relay, dashboard onboarding, Garage and production discovery/device
operations remain open. Plan 08 remains excluded.

Verification passed: default and Matter-enabled `make host test`/`make mips`,
with host suites run sequentially; enabled peer checks pass (13 tests). Both
`python3 scripts/test_matter_native_worker.py` and its `--real-service` variant
pass, including fixed 503 `not_ready` rejection after service exit and setup-PIN
log redaction. The latter reaches companion commit `45782d3` through the actual
native HTTP/worker/IPC path. Both factory binaries remain static stripped
big-endian MIPS-I ELF files; target execution is not inferred from these builds.

## Queued Thread HTTP mutation checkpoint — 2026-10-04

Public `POST /api/thread/create` and `/api/thread/import` now use the bounded
Matter job queue. Strict bodies contain schema and, for import, 1–254 decoded
hex bytes. HTTP input cannot supply an action/channel override or private online
observation. Native parsing supports the full 508-character dataset bound while
BLE bodies retain their smaller limits.

The worker derives the confirmed coordinator channel at execution time; a
queued request never retains an earlier channel. Missing/offline/stale observation
returns a fixed `not_ready` result without sending the mutation. Rust remains the
owner of dataset TLV validation, persisted network policy and radio apply. Status
and job replies expose redacted snapshots only. Public API semantics, including
busy create and idempotent identical import, are documented in
[Native Matter HTTP API v1](../matter-native-api-v1.md).

Focused tests cover native public/private body separation, channel override
rejection, invalid hex, full dataset limits, duplicate schema and offline channel
preparation. The actual-binary harness now creates through HTTP rather than a
private test IPC call, tests malformed import and busy create, and exercises
valid identical and wrong-channel imports against the actual Rust lifecycle.
The valid dataset is read only from this harness's temporary synthetic private
store and is never exported/logged. Offline create is rejected before forwarding.
WebSocket/Android BLE, production discovery/devices and broader recovery work
remain open; no plan 08 action is involved.

Verification passed: default and Matter-enabled `make host test`/`make mips`,
with sequential host suites and 13 enabled native peer checks. Both actual-native
harness variants pass: `python3 scripts/test_matter_native_worker.py` and
`python3 scripts/test_matter_native_worker.py --real-service` (companion commit
`45782d3`). The real-service run verifies identical import without credential
changes, refusal of a different valid PAN dataset with `busy`, wrong-channel
import with `malformed`, all recorded channel writes matching the coordinator,
dataset log redaction and offline local rejection. Both factory binaries are
static stripped big-endian MIPS-I ELF files. No physical deployment or accessory
acceptance was performed; WebSocket/Android and production discovery remain open.

## WebSocket transport checkpoint — 2026-10-04

`matter_websocket.cpp` supplies the server handshake and incremental frame codec
needed before socket ownership can move from an HTTP request worker to the
Matter worker. It follows the relevant framing/handshake rules from
[RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html). The handshake checks the
exact HTTP/1.1 GET path, required headers/version, a canonical 16-byte base64
nonce and duplicate fields, and computes the accept value using existing Mbed
TLS SHA-1/base64 support. It does not negotiate extensions or a subprotocol.

Inbound frames must be masked with clear reserved bits. Text continuations can
span input chunks and UTF-8 sequences; ping/pong/close frames may occur between
fragments. The application profile rejects binary messages. Frame/message limits
are 2048 bytes, input chunks 4096 bytes and eight queued messages. Invalid framing,
UTF-8, oversized data and queue overflow fail closed with fixed close codes and
clear retained payloads. Outbound frames are unmasked and validate their type,
control limits and text/close payloads. No packet logging is added.

Focused tests cover the standard handshake accept vector, bad/duplicate keys,
wrong version/path/body, byte-fragmented masked input, interleaved ping, split
UTF-8, maximum-size messages, server encoding, missing masks, invalid fragment
ordering, binary input, invalid UTF-8, oversized/fragmented messages, noncanonical
lengths, bad close payloads, queue saturation and post-close input.

This checkpoint is a transport building block. No WebSocket endpoint is active,
no HTTP socket is handed off yet, and no phone relay/session ownership is claimed.
The next checkpoint must integrate the single connection owner, nonblocking
read/write deadlines, typed attach/indication/write-complete dispatch, progress/C1
events, cancellation/reconnect and shutdown with the existing worker. Garage
integration and real-accessory acceptance remain separate work.

Verification passed: default and Matter-enabled `make host test`/`make mips`.
Host suites ran sequentially; enabled peer regression checks pass (13 tests).
Both factory builds produce static stripped big-endian MIPS-I ELF files.
No HTTP handoff, actual phone relay, deployment or plan 08 action occurred.

## Worker-owned relay checkpoint — 2026-10-04

The plain HTTP WebSocket upgrade now hands a duplicated nonblocking socket to
the existing Matter worker; HTTP threads return promptly and no relay thread is
added. One phone owner is allowed. The typed schema/id/opcode/body envelope binds
status first, then permits only session-matching status/cancel/attach/indication/
write-complete calls. Bind requests select the private event subscription; reply
and event validation remains strict. IDs increase on the connection; outgoing
progress is session-filtered and C1 writes also require the active generation.

Read/write work is bounded, with eight-frame/message queues, 30-second input
idle expiry and two-second incomplete-write expiry. Disconnect/protocol failure
cancels only the bound active session; terminal results remain intact. Service
loss drops the phone without replay. Pending handoff and active descriptors are
closed on shutdown. Phone I/O never accesses Gateway or opens/reset the UART.
The public contract and reconnect policy are in
[Native Matter HTTP API v1](../matter-native-api-v1.md#owned-ble-relay-websocket).

Focused tests validate the relay envelope and session/generation event filters.
The actual-native harness adds upgrade, single-owner rejection, fast concurrent
HTTP reads, ping/pong, status bind, BLE attach/C1, stale-generation rejection,
failed write, terminal state preservation, disconnect cancellation and malformed
client-mask cleanup. Actual Rust service evidence remains synthetic BTP/GATT;
Android integration and production operational discovery remain separate work.
No plan 08 physical gateway/accessory acceptance is performed.

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

## Dashboard onboarding/progress checkpoint — 2026-10-04

The Controls tab contains a feature-aware Matter/Thread panel. Disabled builds
hide its controls; unavailable/reconnecting status gates mutations. Users can
create/import only an absent Thread network, hand setup to Garage without
entering PINs into the browser, and watch/cancel a copied session ID. Garage
commit `7540b17` adds selectable session IDs with retained terminal results.
Device controls remain pending plan 07.

The separate bounded frontend client validates Matter/Thread readiness, canonical
session/dataset IDs, correlation and fixed errors. It caps retained replies at
2048 bytes, each request at three seconds and an operation at ten seconds.
Job completion is distinguished from accessory session completion; mutations
are never automatically replayed. Dataset input is masked and cleared on
submission/background/configuration, without dashboard persistence or logs.

Verification: `make webui` passed TypeScript and Vite production build;
`python3 scripts/test_matter_webui.py` passed nine tests against actual localhost
HTTP fixtures. Both Garage flavor suites pass 60 tests and both APKs build after
session display. Browser rendering/interaction and physical accessory behavior
were not tested. No C++ source/header changes or deployment occurred.

Usage, cleanup limits, trusted-LAN policy and evidence:
[dashboard guide](../matter-dashboard.md). This completes checklist item 4's
implementation checkpoint; items 5 and 6 remain open.

## Native snapshot API checkpoint — 2026-10-04

`POST /api/matter/devices/snapshot` now queues opcode 10 through the existing
single-owner worker; HTTP returns 202 and job polling supplies the validated
page. The native parser bounds nesting/arrays and validators enforce exact
fields, canonical node/revision/endpoint IDs, ID/path agreement, cluster bounds,
sorted uniqueness, Descriptor presence and catalog/online consistency. Nonzero
cursors require a revision. Stale revision errors are delivered explicitly;
malformed responses fail the call rather than reaching HTTP unchecked. There
is no synchronous radio wait, flash write or implicit pagination retry.

Verification passed: `make host test`, `make mips`, then `make host test
MATTER=1 ZIGGURAT=1` and `make mips MATTER=1 ZIGGURAT=1`. Both host suites include
unit/integration checks; enabled tests also pass 13 Ziggurat peer tests. Native
socket tests cover valid empty/endpoint/error pages and reject contradictory
IDs/states, duplicate clusters, absent Descriptor and multiple entries. Existing
MIPS toolchain LOAD-USE warnings remain.

`python3 scripts/test_matter_native_worker.py` verifies two synthetic endpoint
pages over actual HTTP/jobs, malformed request rejection and stale revisions.
The `--real-service` variant verifies the real Rust service's empty catalog and
stale revision through HTTP. Both retain commissioning/Thread/relay/radio-loss
and unavailable-service regression coverage. These checks do not claim a
nonempty physical catalog or device control. No plan 08 test was performed.

Device API implementation remains partial: OnOff read/control is wired through
confirmed readback; Level, sensor measurements, subscriptions, history and alerts
still require plan 07 work.

## Dashboard device snapshot checkpoint — 2026-10-04

`MatterDevices` now consumes queued snapshot pages through the production
TypeScript client. Refresh begins a new revision; Load next endpoint advances
one page explicitly. It shows recorded node/endpoint IDs, discovery/setup/recovery
state, availability and advertised features using pinned SDK cluster IDs. Empty
and undiscovered catalogs are distinct. Changed revisions stop pagination and
require explicit refresh. Previous information is labelled cached on error;
service loss/unmount aborts outstanding work. Calls retain the existing 10-second
job deadline, 3-second HTTP deadline and 2048-byte response limit.

The client rejects changed revisions, cursor loops/skips, contradictory IDs,
invalid availability, duplicate/unsorted clusters, excess entries and unknown
fields. The view additionally bounds retained entries to 2048, rejects duplicate
identities and checks ordering between pages. UI state stays in memory; no
background full-catalog traversal or automatic retry is introduced.

Verification: `make webui` passes TypeScript and Vite compilation; production
client tests via `python3 scripts/test_matter_webui.py` pass 14 actual-localhost
HTTP tests. New checks cover two-page revision handoff, stale revision without
replay, contradictory metadata, empty/recovery entries and invalid cursor/token
rejection before HTTP submission. No native C++ source changed in this checkpoint.
Rendered browser interaction remains unverified: local Chromium needs runtime
paths outside the workspace sandbox. This does not close checklist items 5/6.
Controls, measurements, history and alerts remain open; plan 08 is excluded.

## Rendered device component checkpoint — 2026-10-04

`webui/test/matter-devices-browser.cjs` bundles the production React
`MatterDevices` component and serves it with actual localhost HTTP job/page
fixtures. It runs headless Chromium and requires the browser's DOM success
marker; a failed condition or browser exit fails the command. The harness has
bounded virtual time and a 55-second process timeout, uses a temporary profile,
and closes the browser/server on completion. `CHROMIUM` can select another
Chromium executable. Local browser execution requires access to its runtime
paths outside the workspace sandbox; this was verified with approved execution.

Verified with Chromium 154.0.8037.57: initial mount submits no page calls;
explicit refresh/next renders two endpoints and feature labels; final pages
remove the next button; stale revision stops further loading without replay and
retains labelled cached rows; refresh replaces old endpoints; recovery and
offline states retain identity; duplicates are rejected; empty results clear
rows; unavailability disables requests and aborts pending work; unmount prevents
late UI updates. The fixture waits for refreshed data and enabled controls
before interacting, so a click on a disabled button cannot falsely satisfy it.

Commands passed at that checkpoint: `make webui`,
`node webui/test/matter-devices-browser.cjs`,
`python3 scripts/test_matter_webui.py` (14 actual-HTTP client tests).
This proves the isolated production device component's interactions, not the
entire dashboard layout or onboarding flow, nor physical gateway/accessory
behavior. Checklist 5/6 remain open for read/control/measurements and broader
failure gates. No native C++ code changed or plan 08 action was performed.

## Confirmed OnOff dashboard checkpoint — 2026-10-04

The device view now offers power read, On, Off and Toggle only for an online
endpoint advertising OnOff cluster 6. Each action uses the bounded native job
API, waits for operation completion and displays only a schema-checked CASE
readback boolean. Requests are not automatically retried; operation polling
allows 35 seconds for the service's bounded 30-second queue/operation deadline.
Offline endpoints and unavailable service disable controls, and operation
failures ask the user to read again before trusting the displayed state.

`make webui` passes TypeScript checking and the production build.
`python3 scripts/test_matter_webui.py` passes 16 client tests, including exact
typed values and rejection of malformed/unsupported requests. The headless
Chromium harness passes rendered refresh/pagination plus confirmed read, On and
Toggle interactions, stale revisions, recovery state, duplicates, empty state,
availability and unmount. No device deployment or plan 08 acceptance occurred.

Plan item 5 remains open for sensor measurement rendering and controls for
additional clusters; item 6's broader API and failure matrix also remains open.
