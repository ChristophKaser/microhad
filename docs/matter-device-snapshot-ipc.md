# Device snapshot IPC

The private wire format is [CBOR v2](../microhad/docs/matter-ipc-v2.md).
Examples below use the public/in-process JSON representation: private-wire
node and revision IDs are unsigned 64-bit integers; the JSON representation
uses sixteen-digit hexadecimal strings. Unknown private-wire fields are skipped
with safe warnings. Historical validation checkpoints retain their original
service names and measurements; the current executable is `mthread-matter`.

Opcode 10 accepts a CBOR map equivalent to `{"schema":1,"action":"snapshot"}`
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
device UI remain pending. Native commit `8fff04c` implements strict opcode-10
request/reply validation and `POST /api/matter/devices/snapshot` through queued
HTTP jobs. Both synthetic paginated pages and real-service empty/stale-revision
HTTP checks pass. Clients poll the returned job, then request the next page
explicitly; no aggregate or retry is implicit. Native default/enabled host
suites and both MIPS builds pass. No credential or setup-code fields are present.

Verified 2026-10-04: 57 service-feature library tests, host/MIPS service and
commissioning-probe builds, real service IPC, encrypted descriptor/snapshot
fixture, and native real-service HTTP/WebSocket regression all pass. Commands
are the existing `micro-thread-matter-daemon/scripts/build.sh` host/mips service and
commissioning variants, `micro-thread-matter-daemon/scripts/test-matter-service.py`, and
`micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py --commissioning --ble --service-owner
--service-discovery`; native compatibility uses
`microhad/scripts/test_matter_native_worker.py --real-service`.

## Live CASE reports in the native gateway — 2026-10-05

The composed synthetic integration command
`python3 ../microhad/scripts/test_matter_native_report_integration.py`
starts the Matter-enabled native host gateway and encrypted accessory fixture
together. A test-only reports-sent/acknowledgement handshake holds the Rust
probe's IPC session open after it publishes all five reports. The integration
confirms there is no native Matter device yet, then requests snapshot pages
through the native HTTP API, polling each bounded job and carrying the
revision/cursor until endpoint 1 is registered. The early reports then flow
through the production Rust `Manager` and `IpcServer`; the native worker applies
them to its shared device model after registration. The check verifies `/api/status` values and units for temperature
(-5.25 °C), humidity (64%), air quality (`moderate`), CO2 (800 ppm), and PM2.5
(25.5 ug/m3), each with measured quality. The validation JSON records these
observed native values.

The native worker bounds pre-registration readings at 128 entries and applies
them after snapshot reconciliation. The run passed on the development host using a synthetic RCP/accessory. It
does not use gateway radio or UART, and it is not the installed
`thread-matter` service process. The daemon's own active report loop remains a
separate process-level verification item. Native API snapshot pagination is
required before checking the shared device catalog; the initial page in this
fixture contains the root endpoint and advertises another cursor. The test
markers are enabled only through environment variables and do not alter
production service behavior.

Stripped MIPS artifacts (not deployed):

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `mthread-matter-mips` | 1,969,848 | `7ecc50492c6a9dce68c50f3c8324faa67ee21ca68fbe0b052dc063a35261b97b` |
| `ot-commissioning-mips` | 2,309,840 | `d8f6dfa4940857e82173a82ff48eedb650fd839ecb196e5c49457880e0817e58` |
