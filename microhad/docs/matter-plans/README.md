# Matter-over-Thread implementation plans

Current private IPC uses [CBOR v2](../matter-ipc-v2.md); the component is
`micro-thread-matter-daemon/` and the executable is `mthread-matter`. The v2
rename/migration has host and build verification only and has not been deployed.
Dated implementation and device checkpoints below retain historical names,
measurements and v1 protocol details. JSON examples describe the public or
in-process models; private-wire IDs and binary data use CBOR integers and byte
strings. Current wire rules, unknown-field warnings and limits are in the v2
contract.

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
| [01 Runtime](01-rust-service-runtime.md) | Native and Rust Matter artifacts remain staged and the paired launcher remains active with its backup preserved. The private Thread socket and Matter service were present at the last read-only check. The latest guarded redeploy preflight measured 2,256 KiB free and refused before upload; the current complete update needs 4,340 KiB including the 512 KiB reserve while executable inodes are retained. Last valid RAM sample: 2,952 KiB `MemFree`, native/Rust RSS 1,696/1,552 KiB. No device file or process changed. Earlier reset cause remains unknown | Preserve the live stack until a planned maintenance window or safe flash headroom is available; verify installed-daemon reports and commission the first accessory when available |
| [02 IPC](02-commissioning-control-ipc.md) | Native HTTP/worker OnOff and Level jobs reach the Rust dispatcher. The host integration publishes subscribed, wire-decoded CASE reports before endpoint snapshot registration; the native worker retains up to 128 unmatched readings and applies them after reconciliation. The production service loop publishes queued Manager reports to IPC and the native worker applies them. On IPC loss, the native worker cancels known sessions after reconnect, and the Rust commissioner cancels sessions when the owning peer disconnects, including lost start responses (`fc204f2`). Native worker and real Rust service-process fixtures pass; Matter-enabled MIPS artifacts are 1,741,544-byte native and 2,158,276-byte service binaries. All five values pass `/api/status` and history. Live report flow through the installed gateway remains unverified; broader failure/backpressure gates remain open | Exercise the installed daemon's active CASE reports into native state; extend failure and backpressure coverage |
| [03 Identity](03-persistent-fabric-identity.md) | Persistent authority/host CASE recovery, allocator high-water, both simulated atomic-rename failure boundaries, and DNS-SD-based sparse legacy migration verified (`8bf8dc2` Rust); catalog remains bounded to 64 live devices; actual power-loss and target filesystem checks remain open | Verify real power-loss durability and target filesystem in plan 08 |
| [04 Native API/dashboard](04-native-api-dashboard.md) | Status, shared cards/history/alerts, bounded Matter job admission, common OnOff/Level controls, malformed/truncated IPC recovery, and commissioning PIN absence from job responses, `/api/status`, `/api/logs`, and process output verified; failures return 503 and recover (`1f8ebac`, `7618e0b`, `df7ae8e`, `9f8e562`, checkpoint 2026-10-05) | Cancellation/socket-loss and remaining cross-language/WebSocket redaction/failure gates |
| [05 Garage BLE relay](05-garage-ble-relay.md) | Foreground onboarding and parser/GATT/HTTP/WebSocket implemented; 60 tests and APK builds pass per flavor | Implementation checklist complete; physical acceptance stays in plan 08 |
| [06 Thread lifecycle](06-thread-network-lifecycle.md) | Host lifecycle/recovery fixtures pass; wire-level synthetic-RCP Spinel error proves service exit, dataset persistence and restore (`68d8e21`). Paired integration verifies native-worker recovery (`a90ada3`). The live gateway's Rust Thread instance now creates a private channel-11 dataset and reaches `attached:true` through Ziggurat while Zigbee remains online/formed. Physical accessory attachment, RF coexistence and failure recovery remain open; earlier reset cause is unknown | Preserve current network identities and verify first accessory joins the attached Thread network; broader physical RF acceptance remains excluded in plan 08 |
| [07 Devices/reports](07-device-model-and-reports.md) | Rust commits `f32a9bc`, `2876c02`, `40b6ee9`, `bc47fb0`, and `ae7ecbf` invalidate failed CASE addresses, schedule bounded rediscovery, add typed Boolean State reads/reports and shared contact mapping, and set a 12-slot per-endpoint subscription ceiling. Eight clusters are currently supported. Synthetic CASE-to-native contact reporting and the test-seeded daemon IPC/history path pass. MIPS builds succeed, but deployment preflight refused: 2,256 KiB free versus 4,340 KiB required with live executable inodes counted. No device files or processes changed. Real accessory behavior, installed-daemon CASE report delivery, and target resource validation remain open | Commission a real contact sensor when available; prove installed daemon CASE reports reach native state/history; resolve safe staging headroom and verify ICD deadlines |
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
