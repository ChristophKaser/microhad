# Operational transaction executor checkpoint

`operations::Operation` supports opcode-10 OnOff (`cluster 6`, attribute 0) and
Level Control (`cluster 8`, attribute 0) reads, opcode-11 on/off/toggle, and
Level Control MoveToLevel. Other paths return `unsupported`; controller/reserved
node IDs and wildcard endpoints are rejected. Level values are bounded to the
Matter `u8` command range excluding 255; transition time uses bounded `u16`
deciseconds. The service admits these requests as bounded asynchronous jobs.
`devices::Manager::submit` requires an attached Thread radio, no active
commissioning exchange, a Complete catalog entry, an online descriptor snapshot
and a matching endpoint advertising the cluster needed by that operation.
Unknown nodes/endpoints return `not_found`; offline nodes return `not_ready`.
Denied paths do not start an exchange.

The service returns a job ID immediately; opcode 10 `job` polls its
queued/running/complete/failed state. At most eight jobs are admitted and one
CASE operation runs at a time. Job IDs use a random process seed plus a
monotonic counter. Start-time catalog/capability checks run again; queued and
active jobs fail on radio loss. The 30-second deadline includes queue wait and
execution. On/off/toggle invocation acknowledgement is followed by a separate
OnOff read; MoveToLevel acknowledgement is followed by a separate CurrentLevel
read. Returned values are actual readback, never optimistic requested values.
Tagged results contain the stable node, endpoint, cluster/attribute and either
`{type:"bool",value:true|false}` or `{type:"u8",value:0..254|null}`. A Level
command whose post-command readback is null fails instead of claiming success.
Errors are fixed enum values without opaque SDK diagnostics or input data.

There is no retry or flash write. If cancelled or timed out after invocation,
the accessory may already have changed; callers must reconcile through a later
read, not replay toggle. A failed readback is an error rather than success.

Verification on 2026-10-04:

- 59 service-feature library tests pass, including queue bounds, status lookup,
  radio-loss failure, strict path/identity rejection and a golden tagged-value
  reply. The IPC tests require local Unix socket permission.
- Host and MIPS commissioning-probe/service builds pass using
  `thread-matter/scripts/build.sh`; existing MIPS linker warnings remain.
- The encrypted synthetic accessory fixture uses the production manager for
  OnOff and Level reads/commands, checking confirmed values after each. It rejects
  root endpoints that do not advertise the requested cluster and rejects
  operations while offline. Existing discovery, catalog, snapshot, temperature
  and fresh CASE checks pass.
- `thread-matter/scripts/test-openthread-matter-transport.py --commissioning --ble
  --service-owner --service-discovery` requires the operational executor marker.

The Rust service IPC routes asynchronous OnOff and Level jobs. A dispatcher gap
left MoveToLevel implemented in the manager but unmatched by the production
service binary; service commit `1b7ae98` adds that final command variant. The
composed native HTTP-to-real-service fixture verifies that MoveToLevel reaches
the manager and receives the expected `not_ready` result when no Thread
accessory is attached, rather than `unsupported`. Native typed validation,
HTTP Level routes and shared dashboard controls are already implemented and
host-tested. Sensor decoding, live subscription reports from a commissioned
accessory, and richer cancellation/restart recovery remain open. Job
interruption after invocation is never replayed; callers must reconcile by a
later read.
No physical gateway/accessory test or plan 08 acceptance was performed.
