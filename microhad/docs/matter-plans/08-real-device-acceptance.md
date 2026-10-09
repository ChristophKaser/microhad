# Real-device acceptance

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

Physical gateway/accessory acceptance remains pending. This is a test plan;
deployment and device tests require an explicit request.

## Goal

Demonstrate end-to-end commissioning and sustained operation with the target
factory-new IKEA ALPSTUGA while preserving the existing Zigbee network.

## Steps

1. Finish and review the preceding plans. Record the exact native and Rust
   commits, binary hashes, service configuration, radio socket mapping, and
   rollback procedure before touching the gateway.
2. Capture a filtered baseline of Zigbee online/network-formed state, channel,
   free memory, CPU, flash free space, and relevant service processes. Back up
   the configuration and network state without exposing keys in reports.
3. Stage the native binary, Rust service, and required assets under a unique
   temporary target directory. Verify checksums and ensure the test path cannot
   start a second Zigbee UART owner.
4. Commission a factory-new ALPSTUGA through the Garage BLE relay. Record only
   redacted phase timings and success/failure codes; do not retain setup codes,
   datasets, or session secrets in validation reports.
5. Confirm CASE read/control operations, then disconnect the phone. Verify
   subsequent reports and commands continue over Thread and cover every
   advertised sensor value.
6. Restart the Matter service and then the gateway under the approved test
   procedure. Verify fabric/identity restore and CASE recovery without
   recommissioning or changing Zigbee state.
7. Exercise concurrent Zigbee traffic and Matter reports/commands. Measure
   CPU, peak RSS, free memory, packet loss/retries, and flash writes over a
   documented sustained interval. Apply production memory and reliability
   gates, including at least 1 MiB free RAM where required by the deployment
   acceptance criteria.
8. Verify failure recovery: phone loss during PASE, Thread socket loss, RCP
   restart, invalid dataset, service crash, low memory, and corrupt Matter
   state. Confirm Zigbee continues operating and errors remain recoverable.
9. Remove staging artifacts and temporary logs. Compare filtered production
   status to baseline, verify configuration/network files are unchanged, and
   publish redacted reports with commands, hashes, timings, resource samples,
   and cleanup results.

## Acceptance

- A factory-new ALPSTUGA commissions and is controllable through the native
  gateway path.
- It continues to report and respond after the phone is disconnected.
- Matter service and gateway restart recover the same fabric and device.
- Zigbee remains online and formed throughout the test.
- Resource, report reliability, recovery, and flash-write gates pass, and the
  rollback procedure is demonstrated.

## Dependencies

All preceding plans, especially persistent identity, Thread lifecycle, the
Garage BLE relay, and device report integration. This is the final release gate.
