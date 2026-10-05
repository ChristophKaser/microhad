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
| [01 Runtime](01-rust-service-runtime.md) | Current Matter-enabled static MIPS-I native/service artifacts cross-build at 1,736,548/2,109,120 bytes; Rust/OpenThread ABI alignment fixed (`4c02d8a`); host supervisor and HTTP failure gates pass | Verify target startup/resources/deadlines in plan 08; host production-daemon report delivery remains open |
| [02 IPC](02-commissioning-control-ipc.md) | Native HTTP/worker routes for OnOff and Level jobs reach the Rust production dispatcher; Level service branch fixed (`1b7ae98`, composed check `9f8e562`). Manager/IpcServer reports and active commissioning recovery have host fixtures; service-daemon live-report and broader failure/backpressure gates remain open | Exercise real accessory reports from the installed service into a registered native endpoint; extend failure/backpressure coverage |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority/host CASE recovery, allocator high-water and both simulated atomic-rename failure boundaries verified (`8db4897`, `980db77` Rust); actual power-loss and target filesystem checks remain open | Verify real power-loss durability and target filesystem in plan 08 |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status, shared cards/history/alerts, bounded Matter job admission, common OnOff/Level controls, and unsupported-version/reserved-byte/oversized-length header recovery verified; failures return 503 and recover (`1f8ebac`, `7618e0b`, `df7ae8e`, `9f8e562`) | Other malformed header fields, cancellation/socket-loss, secret-redaction and remaining cross-language failure gates |
| [05 Garage BLE relay](05-garage-ble-relay.md) | Foreground onboarding and parser/GATT/HTTP/WebSocket implemented; 60 tests and APK builds pass per flavor | Implementation checklist complete; physical acceptance stays in plan 08 |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Host lifecycle/recovery fixtures pass; the gateway completed a constrained 100-second OpenThread CLI/synthetic-RCP profile (11.8% mean loaded CPU, 808 KiB sampled RSS, 1,544 KiB minimum raw MemFree) while its service stayed online (`1a75661`); the harness now also counts clean inactive file cache (`eb04f96`) | Matter-service and combined Zigbee/Thread CPU, memory, deadline and RF verification remain open |
| [07 Devices/reports](07-device-model-and-reports.md) | CASE reports reach native state/history; attached `thread-matter` consumes five fixture `ReceivedReport` values through production `Manager::poll()`, correlation/filter/dedupe, and IPC into that registered device (`a0dbf1d`, `99b3635` Rust plus native integration) | Feed wire-decoded CASE reports through the daemon’s live subscription task into native state/history/alerts; add remote recovery, restart, and sleepy-device policy |
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
