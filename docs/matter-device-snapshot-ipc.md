# Device snapshot IPC

The private wire format is [protobuf v3](../micro-thread-matter-daemon/docs/matter-ipc-v3.md).
Examples below use the public/fixture JSON representation: private-wire
node and revision IDs are unsigned 64-bit integers; the JSON representation
uses sixteen-digit hexadecimal strings. Unknown private-wire fields are skipped
with safe warnings. The executable is `mthread-matter`.

Opcode 10 accepts a protobuf message equivalent to `{"schema":1,"action":"snapshot"}`
with optional `cursor` (u16, default zero) and `revision` (nonzero unsigned 64-bit
integer on the wire, sixteen-digit lowercase hex string in JSON).
It responds immediately from the private catalog and in-memory descriptors;
no read triggers CASE, DNS, BLE, flash writes or a radio wait.

Successful replies have `schema:1`, `ok:true` and this `result`:

```json
{
  "revision": "123456789abcdef0",
  "devices": [{
    "node": "0000000000000002",
    "state": "complete",
    "online": true,
    "id": "matter:0000000000000001:0000000000000002:0001",
    "endpoint": 1,
    "servers": [6, 29],
    "clients": []
  }],
  "next_cursor": null
}
```

Each page contains at most one endpoint, with at most 64 server and 64 client cluster IDs.
Node/endpoint ordering is canonical. A node whose descriptors are not yet known
still gets one entry: `id:null`, `endpoint:null`, `servers:[]`, `clients:[]`, `online:false`.
Its state is `complete`, `recovery_required` or `commissioning` according to the
catalog. Incomplete entries are visible for recovery, never silently trusted.
An empty catalog returns `devices:[]`, `next_cursor:null` at cursor zero.
Maximum catalog cardinality is 64 × 32 = 2048 pages.

Start at cursor zero without a revision. Carry the returned revision and
`next_cursor` into each subsequent request. Missing revision for nonzero cursor
or a changed revision returns `stale_revision`; restart pagination explicitly
from zero. Out-of-range cursors return `malformed`. The revision starts from OS
randomness on each manager/service startup and increments on observable catalog,
descriptor or availability changes. Refreshes with identical descriptors and
availability preserve it. Counter exhaustion returns `capacity` permanently
instead of wrapping. Revisions are opaque tokens, not persistent IDs or clocks.

Snapshots are available before Thread initialization and while running or
detached. `POST /api/matter/devices/snapshot` queues a native HTTP job. Clients
poll the returned job and explicitly request the next page; aggregation and
retry are not implicit. Snapshots expose no credentials or setup codes.

See [descriptor discovery](matter-device-discovery.md), [sensor values](matter-sensors.md)
and the [HTTP API](../microhad/docs/matter-native-api-v1.md) for device integration.
