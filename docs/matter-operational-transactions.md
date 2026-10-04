# Operational transaction executor checkpoint

`operations::Operation` decodes supported opcode-10 OnOff attribute reads
(cluster 6, attribute 0) and opcode-11 on/off/toggle commands. Other paths return
`unsupported`; controller/reserved node IDs and wildcard endpoints are rejected.
`devices::Manager::prepare` requires a Complete catalog entry, an online
descriptor snapshot and a matching endpoint advertising the OnOff server.
Unknown nodes/endpoints return `not_found`; offline nodes return `not_ready`.
Denied paths do not start an exchange.

The prepared future owns its operation and controller references, so it does
not keep the manager borrowed while the gateway pumps other futures. Execution
has a 30-second total timeout starting at first poll. On/off/toggle invocation
acknowledgement is followed by a separate OnOff read; returned values are actual
readback, never optimistic requested values. Tagged results contain the stable
node, endpoint, cluster/attribute and `{type:"bool",value:true|false}`. Errors
are fixed enum values without opaque SDK diagnostics or input data.

There is no retry or flash write. If cancelled or timed out after invocation,
the accessory may already have changed; callers must reconcile through a later
read, not replay toggle. A failed readback is an error rather than success.

Verification on 2026-10-04:

- 58 service-feature library tests pass, including strict path/identity rejection
  and a golden tagged-value reply.
- Host and MIPS commissioning-probe/service builds pass using
  `scripts/build-rs-matter-transport.sh`; existing MIPS linker warnings remain.
- The encrypted synthetic accessory fixture uses `Manager::prepare` for on,
  toggle and off, checking confirmed values after each. It rejects the root
  endpoint without OnOff support and rejects an operation while offline.
  Existing discovery, catalog, snapshot, temperature and fresh CASE checks pass.
- `scripts/test-openthread-matter-transport.py --commissioning --ble
  --service-owner --service-discovery` requires the operational executor marker.

This executor is not yet routed by the service's synchronous IPC dispatcher.
Production use next requires bounded asynchronous operation jobs, admission and
start-time readiness/capability revalidation, queue-inclusive deadlines and
failure/cancellation handling. Native typed replies/routes and UI control are
also pending. Level control and sensor decoding remain separate open work.
No physical gateway/accessory test or plan 08 acceptance was performed.
