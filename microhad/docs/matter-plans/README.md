# Matter-over-Thread implementation plans

These files break the remaining integration work into reviewable stages. The
native status client and Rust commissioning fixture are already implemented;
the plans below describe production service behavior and acceptance.

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
| [01 Runtime](01-rust-service-runtime.md) | Host service and optional supervision implemented; recovery and target verification incomplete | Verify channel-gated startup and bounded failure/restart behavior |
| [02 IPC](02-commissioning-control-ipc.md) | Framing, typed requests and session building blocks implemented | Wire commissioning, relay and control dispatch with typed replies/events |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority and host CASE recovery implemented | Finish backup/recovery procedure and target filesystem checks |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status routes implemented; write APIs and UI pending | Add bounded native service worker and coordinator channel observation |
| [05 Garage BLE relay](05-garage-ble-relay.md) | App integration pending | Agree relay contract, then implement fake-peer-tested GATT/WebSocket flow |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Private transport implemented; lifecycle changes in progress | Finish synthetic lifecycle checks and native channel heartbeat wiring |
| [07 Devices/reports](07-device-model-and-reports.md) | Encrypted fixture proves transport; production device pipeline pending | Persist commissioned nodes and resolve them through real DNS-SD |
| [08 Real-device acceptance](08-real-device-acceptance.md) | Pending final acceptance gate | Review plans 01–07, then arrange backed-up device testing |

Update the relevant file after each verified checkpoint with the changed
behavior, commands/results, remaining gaps and source commit reference when
available. Commit documentation and implementation in their owning repositories.
Do not promote uncommitted experiments or synthetic fixtures to production
acceptance evidence. Planning alone does not authorize deployment, pairing or
permanent launcher activation.
