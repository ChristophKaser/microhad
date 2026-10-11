# Operational transaction executor

`operations::Operation` supports opcode-10 reads of attribute 0 for OnOff (6),
Level (8), Boolean State (69), Air Quality (91), temperature (1026), humidity
(1029), CO₂ (1037) and PM2.5 (1066). Opcode 11 supports on/off/toggle and
MoveToLevel. Opcode 10 also accepts binding jobs for OnOff/Level targets;
bindings require the endpoint to advertise Binding cluster 30. Other paths
return `unsupported`; controller/reserved node IDs and wildcard endpoints are rejected. Level values are bounded to the
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
Tagged results contain the stable node, endpoint and cluster/attribute. OnOff
and Boolean State values are boolean; Level is U8 or null; sensors use the
[typed measurement/air-quality contract](matter-sensors.md). A Level
command whose post-command readback is null fails instead of claiming success.
Errors are fixed enum values without opaque SDK diagnostics or input data.

There is no retry or flash write. If cancelled or timed out after invocation,
the accessory may already have changed; callers must reconcile through a later
read, not replay toggle. A failed readback is an error rather than success.
