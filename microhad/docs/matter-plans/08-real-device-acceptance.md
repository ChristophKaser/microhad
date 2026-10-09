# Plan: Real-device acceptance

Current private IPC uses [CBOR v2](../matter-ipc-v2.md); the component is
`micro-thread-matter-daemon/` and the executable is `mthread-matter`. The v2
rename/migration has host and build verification only and has not been deployed.
Dated implementation and device checkpoints below retain historical names,
measurements and v1 protocol details. JSON examples describe the public or
in-process models; private-wire IDs and binary data use CBOR integers and byte
strings. Current wire rules, unknown-field warnings and limits are in the v2
contract.

## Current open work — 2026-10-04

Physical gateway/accessory acceptance remains pending; this document is a test plan
only.

Next deliverable: Collect evidence and artifact hashes from plans 01–07, then prepare
the private backup, baseline, staging and rollback manifest. Physical testing requires
its separately arranged scope.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

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

## Status quo — 2026-10-04

Pending. Existing transport/commissioning fixtures and build measurements do
not prove real-accessory commissioning or production coexistence. This file
documents the final gate; creating the plan does not initiate deployment or
pairing.

## Remaining checkpoints

1. Review plans 01–07 against their acceptance criteria and collect a manifest
   of exact source commits, artifact hashes and unresolved limitations.
2. Arrange the physical test scope and backups, then stage artifacts using the
   repository's deployment procedure without an extra UART owner.
3. Execute the commissioning, phone-detached operation, restart and failure
   scenarios above, recording redacted results against the baseline.
4. Measure sustained resource/RF behavior, verify rollback and cleanup, and
   publish a pass/fail report before considering permanent activation.

## Dependencies

All preceding plans, especially persistent identity, Thread lifecycle, the
Garage BLE relay, and device report integration. This is the final release gate.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [ ] Review acceptance evidence from plans 01–07 and list remaining risks.
   Assemble exact commits, artifact hashes, configuration, radio mapping and
   rollback commands into a test manifest.
2. [ ] Arrange the physical test scope; take private consistent backups and
   redacted Zigbee/resource baselines before staging anything on the gateway.
3. [ ] Stage and verify artifacts in a temporary directory. Establish sole
   UART ownership and cleanup/rollback before starting the test services.
4. [ ] Commission the factory-new accessory using Garage; record redacted
   phase timings and failures, then verify advertised values and controls with
   the phone disconnected.
5. [ ] Verify service/gateway restart, identity/catalog restore, fresh CASE and
   the listed BLE/radio/state failure scenarios without losing Zigbee state.
6. [ ] Measure sustained CPU, peak RSS, free RAM, RF/report reliability and
   flash writes against the manifest's acceptance gates; do not substitute
   development-host timings or binary sizes for these measurements.
7. [ ] Demonstrate rollback, remove temporary artifacts, compare to baseline
   and publish a redacted pass/fail report. Treat permanent launcher activation
   as a separate deployment decision after acceptance.

Validation: backed-up physical gateway/accessory testing. This is a planning
file only; no deployment, pairing or cutover is initiated by this update.
