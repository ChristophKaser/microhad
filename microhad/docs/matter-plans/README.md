# Matter-over-Thread implementation plans

These files break the remaining integration work into reviewable stages. The
native observation/status worker, Rust owned commissioner and guarded Thread
lifecycle and Garage onboarding have host evidence;
operational SRP/DNS and descriptor reads have host evidence; service device
recovery, control and report integration remain open.

Recommended order:

1. [Rust service runtime](01-rust-service-runtime.md)
2. [Commissioning and control IPC](02-commissioning-control-ipc.md)
3. [Persistent fabric identity](03-persistent-fabric-identity.md)
4. [Native API and dashboard](04-native-api-dashboard.md)
5. [Garage phone BLE relay](05-garage-ble-relay.md)
6. [Thread network lifecycle](06-thread-network-lifecycle.md)
7. [Matter device model and reports](07-device-model-and-reports.md)
8. [Real-device acceptance](08-real-device-acceptance.md)

The runtime, IPC, storage, and network lifecycle form the gateway foundation.
The API and phone relay then enable onboarding. Device modeling enables useful
control and telemetry. Real-device acceptance closes the sequence. Some work
can proceed in parallel after the IPC contract is agreed, but acceptance must
wait for all earlier pieces.

## Open-point tracking — 2026-10-04

Each numbered file contains its own multistep implementation plan, dependencies,
acceptance criteria and remaining checkpoints. “Complete” applies only to the
specific evidence recorded in a file, not to end-to-end Matter support.

| Open point | Current status | Next checkpoint |
| --- | --- | --- |
| [01 Runtime](01-rust-service-runtime.md) | Host service/supervision implemented; Matter-aware paired launcher activation and opt-in service selection now pass mocked deployment tests; target verification remains in plan 08 | Extend integrated failure/restart coverage and record final artifacts |
| [02 IPC](02-commissioning-control-ipc.md) | Commissioning/relay/control and synthetic-peer OnOff/temperature report events into shared state host-tested (`3aa4600`, `5ec72cb`, `c22f38a`, `6fe31b3`); Rust-service/native worker lifecycle passes; broad recovery and cross-language vectors incomplete | Generate a report from the Rust service through native shared state; extend failure/backpressure coverage |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority/host CASE recovery, private archive staging and offline read-only staged-state validation implemented (`7247c94`, `7f5ac3d`, Rust `1ec79fd`, `0e133b4`); synthetic archive-to-validator and corrupt-envelope refusal pass without writes | Add interrupted-replacement and failed-restore/allocator-rollback coverage; target checks stay in plan 08 |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status, shared cards, restart-restored common alerts, unit-aware CO2/PM2.5 thresholds and bounded shared-source history (`d5d94ba`); history intentionally remains volatile to avoid flash wear | Extend API failure gates |
| [05 Garage BLE relay](05-garage-ble-relay.md) | Foreground onboarding and parser/GATT/HTTP/WebSocket implemented; 60 tests and APK builds pass per flavor | Implementation checklist complete; physical acceptance stays in plan 08 |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Guarded dataset lifecycle and native observation worker verified on host | Extend channel-change/restart recovery and target verification |
| [07 Devices/reports](07-device-model-and-reports.md) | Shared model and notifier-backed Rust reports; synthetic IPC OnOff/temperature reports update native shared state (`1855f24`, `62d7fd4`, `c22f38a`, `6fe31b3`); live Rust-service-originated native sensor report and recovery remain open | Generate a sensor event from the Rust service through native shared state; add recovery, restart, and sleepy-device policy |
| [08 Real-device acceptance](08-real-device-acceptance.md) | Pending final acceptance gate | Review plans 01–07, then arrange backed-up device testing |

Update the relevant file after each verified checkpoint with the changed
behavior, commands/results, remaining gaps and source commit reference when
available. Commit documentation and implementation in their owning repositories.
Do not promote uncommitted experiments or synthetic fixtures to production
acceptance evidence. Planning alone does not authorize deployment, pairing or
permanent launcher activation.

## How to execute these plans

Each open point has a dedicated file with a current open-work summary, ordered
steps, dependencies, acceptance criteria, evidence and an execution checklist.
The native persistent IPC channel, queued mutation APIs and owned relay are
already implemented. The dashboard uses shared device cards for Matter,
Zigbee and external HTTP devices such as Shelly. The roadmap requires a shared
source-neutral identity, semantic device type, capability, state, and telemetry
contract and common presentation; each source adapter retains its own setup
and transport. HTTP devices are first-class model providers, not a
Shelly-specific dashboard exception. Matching functions such as lights,
switches, contacts, temperature/humidity sensors, and power readings must map
to the same shared types and controls wherever capabilities overlap,
independent of protocol. Plan
05's Android onboarding
implementation is verified on host. Continue with plan 04's shared data,
history and failure gates using plan 07's production pipeline. Finish recovery
work in plans 01/02/03/06 alongside those integrations. Plan 08 remains the
physical acceptance gate and is not initiated by writing these plans.

For external HTTP providers, provider-specific dashboard UI is limited to
connection, discovery, credentials, and settings with no common equivalent.
Devices themselves use the shared catalog, capability cards, and semantic
controller intents. The controller routes those intents through the provider's
adapter; HTTP endpoints and payloads do not leak into common device UI.

After each verified step, check its item, update status and commands/results,
and commit the implementation and documentation in their owning repositories.
Leave unverified target behavior open. A completed fixture or cross-build does
not imply production commissioning, Android onboarding or RF acceptance.
