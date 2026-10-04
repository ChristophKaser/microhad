# Operational transaction executor checkpoint

`operations::Operation` decodes supported opcode-10 OnOff attribute reads
(cluster 6, attribute 0) and opcode-11 on/off/toggle commands. Other paths return
`unsupported`; controller/reserved node IDs and wildcard endpoints are rejected.
The service now admits opcode-10 reads and opcode-11 on/off/toggle commands as
bounded asynchronous jobs. `devices::Manager::submit` requires an attached
Thread radio, no active commissioning exchange, a Complete catalog entry, an online
descriptor snapshot and a matching endpoint advertising the OnOff server.
Unknown nodes/endpoints return `not_found`; offline nodes return `not_ready`.
Denied paths do not start an exchange.

The service returns a job ID immediately; opcode 10 `job` polls its
queued/running/complete/failed state. At most eight jobs are admitted and one
CASE operation runs at a time. Job IDs use a random process seed plus a
monotonic counter. Start-time catalog/capability checks run again; queued and
active jobs fail on radio loss. The 30-second deadline includes queue wait and
execution. On/off/toggle invocation
acknowledgement is followed by a separate OnOff read; returned values are actual
readback, never optimistic requested values. Tagged results contain the stable
node, endpoint, cluster/attribute and `{type:"bool",value:true|false}`. Errors
are fixed enum values without opaque SDK diagnostics or input data.

There is no retry or flash write. If cancelled or timed out after invocation,
the accessory may already have changed; callers must reconcile through a later
read, not replay toggle. A failed readback is an error rather than success.

Verification on 2026-10-04:

- 59 service-feature library tests pass, including queue bounds, status lookup,
  radio-loss failure, strict path/identity rejection and a golden tagged-value
  reply. The IPC tests require local Unix socket permission.
- Host and MIPS commissioning-probe/service builds pass using
  `scripts/build-rs-matter-transport.sh`; existing MIPS linker warnings remain.
- The encrypted synthetic accessory fixture uses `Manager::prepare` for on,
  toggle and off, checking confirmed values after each. It rejects the root
  endpoint without OnOff support and rejects an operation while offline.
  Existing discovery, catalog, snapshot, temperature and fresh CASE checks pass.
- `scripts/test-openthread-matter-transport.py --commissioning --ble
  --service-owner --service-discovery` requires the operational executor marker.

The service IPC now routes asynchronous OnOff jobs, but native typed job
validation, HTTP read/control routes and dashboard controls remain pending.
Level control, sensor decoding, subscriptions, and richer cancellation and
restart recovery remain separate open work. Job interruption after invocation
is never replayed: callers must reconcile through a later read.
No physical gateway/accessory test or plan 08 acceptance was performed.
