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
| [01 Runtime](01-rust-service-runtime.md) | Actual `thread-matter`/native worker HTTP and Thread-loss lifecycle passes; all 10 paired-supervisor retry/order/failure tests pass; target verification and commissioned-device report acceptance remain in plan 08 | Gateway startup/resource/deadline acceptance; host production-daemon report delivery into a registered endpoint |
| [02 IPC](02-commissioning-control-ipc.md) | Composed Rust Manager/IpcServer-to-native-worker reports and active commissioning cancellation across RCP/service restart verified in host fixtures (`53c49dd`, `34b4839` Rust; `3957d5d` native); service-daemon device reports and full failure/backpressure matrix remain open | Exercise reports from the installed service into a registered native endpoint; extend failure/backpressure coverage |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority/host CASE recovery, corrupt staged restore refusal, allocator high-water, and active commissioning reservation recovery after RCP loss verified (`34b4839` Rust); power-loss replacement and target filesystem checks remain open | Test crash during atomic replacement and target durability in plan 08 |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status, shared cards/history/alerts and bounded Matter job admission verified; HTTP queue accepts 16 jobs, returns `capacity` for excess and drains after recovery (current native worker test) | Complete absent/malformed/stalled service, event backpressure and cross-language failure gates |
| [05 Garage BLE relay](05-garage-ble-relay.md) | Foreground onboarding and parser/GATT/HTTP/WebSocket implemented; 60 tests and APK builds pass per flavor | Implementation checklist complete; physical acceptance stays in plan 08 |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Actual service/RCP socket-loss restart during active commissioning preserves Thread data, recovers reserved node state, rejects stale BLE callbacks, and recovers after repeated mismatched channel observations (`34b4839`, `77d70f0` Rust) | Verify native-worker continuity through mismatch recovery; target RF verification stays in plan 08 |
| [07 Devices/reports](07-device-model-and-reports.md) | All five notifier-backed CASE sensor reports now reach a registered native `Gateway::Device`, `/api/status`, and shared history in one host integration (`53c49dd` Rust, `3957d5d` native); production daemon-originated report delivery and remote recovery remain open | Run the real service binary through the same registered-device report path; add recovery, restart, and sleepy-device policy |
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
