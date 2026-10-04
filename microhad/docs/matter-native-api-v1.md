# Native Matter HTTP API v1

Matter-enabled native builds expose queued commissioning calls over the selected
trusted-LAN HTTP transport. HTTPS may use the same routes, but an installed
trusted gateway certificate is not a prerequisite. `MATTER=0` returns 404 for
these control/job routes and retains disabled read-only status responses.

## Commissioning calls

Send a JSON object with `schema:1` to these routes. Bodies follow the strict
[private IPC contract](matter-ipc-v1.md); duplicate/unknown fields, invalid PINs,
noncanonical IDs, trailing data and payloads over 2048 bytes are rejected before
queueing. No request body is returned or logged.

| Method/path | Body fields besides schema | Behavior |
| --- | --- | --- |
| `POST /api/matter/commissioning/start` | `discriminator`, `pin`, `timeout_seconds` | Submit a start call; the service returns a session snapshot or fixed error |
| `POST /api/matter/commissioning/status` | `session`, optional boolean `subscribe` | Submit a session status call; default subscription is false |
| `POST /api/matter/commissioning/cancel` | `session` | Submit cancellation; retained cancelled sessions are idempotent |
| `GET /api/matter/jobs/<id>` | None | Read an ephemeral native call snapshot without contacting the service |

Successful submission returns HTTP **202 Accepted**:

```json
{"schema":1,"ok":true,"job":1,"state":"queued"}
```

It proves queue acceptance only. Poll the job endpoint until `state` is
`complete` or `failed`. A completed call contains its validated IPC `response`:

```json
{"job":1,"state":"complete","response":{"schema":1,"ok":true,"result":{"session":"0000000000000001","state":"waiting_ble","deadline_remaining_seconds":30}}}
```

`complete` means the service replied to this call. Accessory commissioning has
its own session state, initially `waiting_ble`; only a later service session
snapshot can establish commissioning completion. A completed call may instead
contain `response.ok:false` with a fixed service error such as `busy` or
`stale_session`. A native transport failure is `state:"failed"` with
`error:"transport"`; mutations are never replayed automatically.

Malformed requests/job IDs return 400; an unavailable worker/service or a full
queue returns 503 with fixed `not_ready`/`capacity` errors. Unknown or evicted job
IDs return 404 `not_found`. IDs are nonzero canonical decimal u32 values. They
are RAM delivery identifiers, not session identity or authentication; native
restart clears them. Sixteen snapshots are retained, with terminal entries
eligible for eviction. Eight pending calls are allowed.

Read-only `GET /api/matter/status` and `GET /api/thread/status` remain cached
service snapshots. Commissioning calls run exclusively on the Matter worker;
HTTP/coordinator threads never wait for PASE, BLE, Thread or a service reply.

## Current scope

The routes reach the real Rust session owner through the persistent worker.
Waiting-BLE, busy/status/cancel/stale-session behavior is verified with the
actual service and synthetic radio peers. Thread dataset create/import public
routes, BLE WebSocket transport, Garage onboarding, production DNS-SD and device
operations remain separate open work. A successful start does not establish
end-to-end accessory support; production CASE completion still needs discovery.
