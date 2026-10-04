# Device snapshot IPC

Opcode 10 accepts `{"schema":1,"action":"snapshot"}` with optional `cursor`
(u16, default zero) and `revision` (nonzero 16-character lowercase hex string).
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
    "servers": [6, 29]
  }],
  "next_cursor": null
}
```

Each page contains at most one endpoint, with at most 64 server cluster IDs.
Node/endpoint ordering is canonical. A node whose descriptors are not yet known
still gets one entry: `id:null`, `endpoint:null`, `servers:[]`, `online:false`.
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

The service supports snapshots before Thread initialization as well as while
running or detached. Descriptor-based reads, commands, sensor values and native
API routing remain pending. Native clients must first add strict opcode-10
request/reply validation; the current native HTTP surface does not expose this
operation. No credential or setup-code fields are present.

Verified 2026-10-04: 57 service-feature library tests, host/MIPS service and
commissioning-probe builds, real service IPC, encrypted descriptor/snapshot
fixture, and native real-service HTTP/WebSocket regression all pass. Commands
are the existing `scripts/build-rs-matter-transport.sh` host/mips service and
commissioning variants, `scripts/test-matter-service.py`, and
`scripts/test-openthread-matter-transport.py --commissioning --ble --service-owner
--service-discovery`; native compatibility uses
`microhad-zigbee-native/scripts/test_matter_native_worker.py --real-service`.

Stripped MIPS artifacts (not deployed):

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `thread-matter-mips` | 1,969,848 | `7ecc50492c6a9dce68c50f3c8324faa67ee21ca68fbe0b052dc063a35261b97b` |
| `ot-commissioning-mips` | 2,309,840 | `d8f6dfa4940857e82173a82ff48eedb650fd839ecb196e5c49457880e0817e58` |
