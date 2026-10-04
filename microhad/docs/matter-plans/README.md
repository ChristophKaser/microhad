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
| [01 Runtime](01-rust-service-runtime.md) | Host service and optional supervision implemented; recovery and target verification incomplete | Extend integrated failure/restart coverage and record final artifacts |
| [02 IPC](02-commissioning-control-ipc.md) | Service/native commissioning, relay and OnOff/Level job dispatch host-tested; device events and broad recovery incomplete | Extend failure/backpressure and event coverage |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority and host CASE recovery implemented | Exercise documented backup/restore on synthetic state; target checks stay in plan 08 |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status, shared cards, native Matter endpoint registration, restart-restored common alerts, and bounded shared-source history (`5cfb3b1`) | Add unit-aware CO2/PM2.5 thresholds and decide power-loss history policy, then extend failure gates |
| [05 Garage BLE relay](05-garage-ble-relay.md) | Foreground onboarding and parser/GATT/HTTP/WebSocket implemented; 60 tests and APK builds pass per flavor | Implementation checklist complete; physical acceptance stays in plan 08 |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Guarded dataset lifecycle and native observation worker verified on host | Extend channel-change/restart recovery and target verification |
| [07 Devices/reports](07-device-model-and-reports.md) | Owned commissioner, CASE reads, shared cards, native endpoint registration, restart-restored common alerts and bounded shared-source history (`5cfb3b1`) | Add unit-aware CO2/PM2.5 thresholds and reports; reconcile incomplete nodes |
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
source-neutral identity/capability/telemetry contract and common presentation;
each source adapter retains its own setup and transport. HTTP devices are
first-class model providers, not a Shelly-specific dashboard exception. Plan
05's Android onboarding
implementation is verified on host. Continue with plan 04's shared data,
history and failure gates using plan 07's production pipeline. Finish recovery
work in plans 01/02/03/06 alongside those integrations. Plan 08 remains the
physical acceptance gate and is not initiated by writing these plans.

After each verified step, check its item, update status and commands/results,
and commit the implementation and documentation in their owning repositories.
Leave unverified target behavior open. A completed fixture or cross-build does
not imply production commissioning, Android onboarding or RF acceptance.
