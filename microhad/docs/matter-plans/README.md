# Matter-over-Thread implementation plans

These files break the remaining integration work into reviewable stages. The
native observation/status worker, Rust owned commissioner and guarded Thread
lifecycle and Garage onboarding have host evidence;
operational SRP/DNS and descriptor reads have host evidence; service device
recovery and control remain open. Native report delivery now tolerates reports
arriving before endpoint registration. The production Manager and daemon loop
contain the active CASE report receive path; a separate-daemon wire-decoded
CASE integration fixture and wider failure/backpressure coverage remain open.

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

## Open-point tracking — 2026-10-05

Each numbered file contains its own multistep implementation plan, dependencies,
acceptance criteria and remaining checkpoints. “Complete” applies only to the
specific evidence recorded in a file, not to end-to-end Matter support.

| Open point | Current status | Next checkpoint |
| --- | --- | --- |
| [01 Runtime](01-rust-service-runtime.md) | Native and Rust Matter artifacts are staged and MD5-verified at `/tuya/native`; the paired launcher is active with the old launcher preserved at `/tuya/tuya_start.sh.lowjs-backup`. The restarted gateway runs `thread-matter`; Matter status is available and `POST /api/thread/create` produced an attached Thread dataset on channel 11. Zigbee remains online/formed on channel 11. Idle daemon RSS/high-water is 1,548 KiB and CPU advanced one tick over 29.27 seconds; 2,260 KiB flash remains. The cause of earlier unrelated resets remains unknown | Verify production-daemon CASE reports into native state/history, then commission a first accessory when its setup code/device are available |
| [02 IPC](02-commissioning-control-ipc.md) | Native HTTP/worker OnOff and Level jobs reach the Rust dispatcher. The host integration publishes subscribed, wire-decoded CASE reports before endpoint snapshot registration; the native worker retains up to 128 unmatched readings and applies them after reconciliation. On IPC loss, the native worker cancels known sessions after reconnect, and the Rust commissioner cancels sessions when the owning peer disconnects, including lost start responses (`fc204f2`). Native worker and real Rust service-process fixtures pass; Matter-enabled MIPS artifacts are 1,741,544-byte native and 2,158,276-byte service binaries. All five values pass `/api/status` and history. The report producer is still the commissioning probe, not installed `thread-matter`; broader failure/backpressure gates remain open | Exercise the installed daemon's active CASE reports into native state; extend failure and backpressure coverage |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority/host CASE recovery, allocator high-water, both simulated atomic-rename failure boundaries, and DNS-SD-based sparse legacy migration verified (`8bf8dc2` Rust); catalog remains bounded to 64 live devices; actual power-loss and target filesystem checks remain open | Verify real power-loss durability and target filesystem in plan 08 |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status, shared cards/history/alerts, bounded Matter job admission, common OnOff/Level controls, malformed/truncated IPC recovery, and commissioning PIN absence from job responses, `/api/status`, `/api/logs`, and process output verified; failures return 503 and recover (`1f8ebac`, `7618e0b`, `df7ae8e`, `9f8e562`, checkpoint 2026-10-05) | Cancellation/socket-loss and remaining cross-language/WebSocket redaction/failure gates |
| [05 Garage BLE relay](05-garage-ble-relay.md) | Foreground onboarding and parser/GATT/HTTP/WebSocket implemented; 60 tests and APK builds pass per flavor | Implementation checklist complete; physical acceptance stays in plan 08 |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Host lifecycle/recovery fixtures pass; wire-level synthetic-RCP Spinel error proves service exit, dataset persistence and restore (`68d8e21`). Paired integration verifies native-worker recovery (`a90ada3`). The live gateway's Rust Thread instance now creates a private channel-11 dataset and reaches `attached:true` through Ziggurat while Zigbee remains online/formed. Physical accessory attachment, RF coexistence and failure recovery remain open; earlier reset cause is unknown | Preserve current network identities and verify first accessory joins the attached Thread network; broader physical RF acceptance remains excluded in plan 08 |
| [07 Devices/reports](07-device-model-and-reports.md) | Rust `d3092ac` fixes ICD responder exchange stealing with protocol/opcode-filtered acceptance. Host integration proves live CASE reports update all five values in native state/history; a separate daemon process publishes all five seeded values through IPC into the same state/history. The installed daemon code polls its active CASE report exchange, but a separate-process wire-decoded CASE test remains open | Prove the daemon's live CASE receive path reaches native state/history, then verify restart recovery |
| [08 Real-device acceptance](08-real-device-acceptance.md) | Excluded from the current implementation scope | None |

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
