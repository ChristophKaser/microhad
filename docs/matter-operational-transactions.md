# Operational transaction executor

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
