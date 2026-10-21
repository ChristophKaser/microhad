# Native Matter HTTP API v1

Matter-enabled native builds expose queued commissioning calls over the selected
trusted-LAN HTTP transport. HTTPS may use the same routes, but an installed
trusted gateway certificate is not a prerequisite. `MATTER=0` returns 404 for
these control/job routes and retains disabled read-only status responses.

## Commissioning calls

Send a JSON object with `schema:1` to these routes. HTTP bodies retain strict
JSON validation: duplicate/unknown fields, invalid PINs,
noncanonical IDs, trailing data and payloads over 2048 bytes are rejected before
queueing. No request body is returned or logged.

The daemon converts accepted JSON into [private CBOR IPC v2](../../micro-thread-matter-daemon/docs/matter-ipc-v2.md).
Private-wire unknown fields are skipped with safe warnings; this does not relax
the public HTTP validator. IDs and binary data retain their JSON encodings here
and become unsigned integers and byte strings on the private wire.

| Method/path | Body fields besides schema | Behavior |
| --- | --- | --- |
| `POST /api/matter/commissioning/start` | `discriminator`, `pin`, `timeout_seconds` | Submit a start call; the service returns a session snapshot or fixed error |
| `POST /api/matter/commissioning/status` | `session`, optional boolean `subscribe` | Submit a session status call; default subscription is false |
| `POST /api/matter/commissioning/cancel` | `session` | Submit cancellation; retained cancelled sessions are idempotent |
| `GET /api/matter/jobs/<id>` | None | Read an ephemeral native call snapshot without contacting the service |
| `POST /api/matter/devices/read` | `node`, `endpoint`, `cluster`, `attribute:0` | Queue a capability-checked actor or sensor read |
| `POST /api/matter/devices/command` | OnOff: `command:"on"|"off"|"toggle"`, `node`, `endpoint`; Level: `command:"move_to_level"`, `node`, `endpoint`, `level`, `transition_deciseconds` | Queue a capability-checked command; result is a separate CASE readback |

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

HTTP control/job errors use `{"ok":false,"error":"CODE"}` without a `schema`
field; the nested IPC response and WebSocket envelopes retain schema 1.

Malformed requests/job IDs return 400; an unavailable worker/service or a full
queue returns 503 with fixed `not_ready`/`capacity` errors. Unknown or evicted job
IDs return 404 `not_found`. IDs are nonzero canonical decimal u32 values. They
are RAM delivery identifiers, not session identity or authentication; native
restart clears them. Sixteen snapshots are retained, with terminal entries
eligible for eviction. Eight pending calls are allowed.

Read-only `GET /api/matter/status` and `GET /api/thread/status` remain cached
service snapshots. Commissioning calls run exclusively on the Matter worker;
HTTP/coordinator threads never wait for PASE, BLE, Thread or a service reply.

## Support and verification

The routes use the real Rust session/device owner through the persistent native
worker. Synthetic fixtures cover commissioning, BLE callbacks, Thread operations,
CASE reads/commands and report handoff. The deployed path receives temperature
and air-quality reports from Matter-over-Thread sensors. Successful request
admission is not proof that an individual commissioning or delivery operation completed.

## Cached common device inventory

`GET /api/status` returns Matter nodes alongside every other device in the
normal `devices` array. Each physical node has one stable ID,
`matter:<fabric>:<node>`. All endpoints, including administrative and unsupported
endpoints, are included in `endpoint_descriptors` with 16-bit endpoint numbers
and 32-bit `input_clusters` / `output_clusters`. Nested endpoint IDs remain valid
for commands, settings and history. The node ID selects its default usable endpoint
for settings and history; explicit node/endpoint operation targets are unchanged.

`commissioning_state` distinguishes complete, commissioning and recovery nodes.
`endpoint_metadata_state` is `missing`, `stale` or `ready`. Known nodes remain
visible during discovery and service outages. Cached metadata is restored from
`devices.json` immediately; background interviews update it independently of HTTP.
Listing performs no radio or IPC calls and creates no jobs. The old snapshot
route is removed. Jobs remain for commissioning, Thread setup and operations.

The daemon automatically streams bounded identity, endpoint and node-commit
records to its native status subscriber. Identities arrive first; only a complete
verified node update replaces endpoints. Reconnects retain the previous cache.
Structural changes are coalesced for 60 seconds and flushed at clean shutdown.
Unchanged interviews, availability, reports and status reads do not schedule
metadata writes. Existing endpoint records preserve settings/history aliases.

## Device reads and commands

Device read and command routes use the same native job queue and polling
contract. `node` is a 16-character lowercase hexadecimal Matter node ID;
`endpoint` is 0–65534. Reads accept attribute 0 for OnOff (cluster 6), CurrentLevel
(cluster 8), Air Quality (91), Temperature Measurement (1026), Relative Humidity
Measurement (1029), CO2 Concentration Measurement (1037), and PM2.5 Concentration
Measurement (1066). Boolean State (69) reads return a typed boolean and map
into shared contact device state. Sensor replies use tagged `measurement` values with unit and
quality fields or tagged `air_quality` values; null values carry `unknown`
quality. Native validation rejects units or value ranges that do not match the
cluster. Sensor reads map into the shared Zigbee/Matter/Shelly device model and dashboard.

The command route accepts `on`, `off`, `toggle`, or
`move_to_level`. Level commands require `level` in 0–254 and
`transition_deciseconds` in 0–65535. Unsupported or malformed requests return
400 before queueing. Submission returns 202 with a native job ID. Rust checks
that the discovered online endpoint advertises the requested cluster. The
native worker polls Rust operation status through the persistent IPC connection;
the native job becomes `complete` only after Rust returns a typed value from a
separate CASE readback. OnOff values are boolean; CurrentLevel values are U8
and may be null for an unknown read. A command that cannot confirm its resulting
level fails. An operation failure becomes a fixed native `failed` error. A
disconnected or restarted service never causes an operation replay. Callers poll
`GET /api/matter/jobs/<id>` after queue acceptance.

## Thread dataset mutations

`POST /api/thread/create` accepts exactly `{"schema":1}`.
`POST /api/thread/import` accepts exactly `schema:1` and a lowercase hexadecimal
`dataset` string encoding 1–254 bytes. Both return the same 202/job contract as
commissioning. Unknown/duplicate fields, a supplied `zigbee_channel`, bad hex
or an oversized dataset are rejected with 400 before queueing.

The queued body contains no channel. At execution, the Matter worker obtains a
fresh protected coordinator observation and adds its confirmed channel to the
private service request. Offline, unknown or stale coordinator state completes
the call with `response.ok:false,error:"not_ready"` without forwarding a dataset
mutation. The Rust lifecycle independently checks observation freshness, dataset
TLVs and channel compatibility before persistence/radio apply.

A repeated create returns service `busy` rather than replacing an existing
network. Import of an identical active dataset is idempotent; a different valid
existing dataset is refused. Invalid TLVs or an incompatible dataset channel
return service `malformed` in the completed job. Successful results contain only
the redacted Thread snapshot, never dataset/key bytes. Dataset inputs remain
transient and are removed from queued storage on execution/failure/shutdown.
Cached `GET /api/thread/status` continues to expose lifecycle/readiness state.

## Owned BLE relay WebSocket

Matter-enabled builds accept a plain HTTP upgrade at `ws://GATEWAY/api/matter/relay`.
Only one phone connection can be owned at a time; a second receives 503 `busy`.
The HTTP worker hands a nonblocking close-on-exec duplicate to the existing
Matter worker, which sends the 101 reply and owns all subsequent I/O. No extra
thread is created. The current profile supports `ws://`; a TLS upgrade is rejected
with 400. Existing HTTPS HTTP routes continue to work normally.

The first text message must bind a retained commissioning session using opcode 4:

```json
{"schema":1,"id":1,"opcode":4,"body":{"schema":1,"session":"0000000000000001"}}
```

The native owner forces `subscribe:true` in that private status call. A successful
reply binds the phone to that session. Later messages use the same envelope with
opcodes 4–8 and the strict bodies from [IPC v2](../../micro-thread-matter-daemon/docs/matter-ipc-v2.md). IDs are nonzero
u32 and must increase on the connection; replies have schema/id/response:

```json
{"schema":1,"id":1,"response":{"schema":1,"ok":true,"result":{"session":"0000000000000001","state":"waiting_ble","deadline_remaining_seconds":30}}}
```

Opcode 6 attaches the BLE address/MTU and returns a connection generation.
Opcode 7 forwards ordered C2 indications; opcode 8 confirms the matching C1 GATT
write. A failed matching write returns `transport` and fails the session; a
successful completion returns the accepted write ID. Confirm only after the matching GATT callback, and report `success:false`
for a failed write. Stale generations/sequences/write IDs are rejected by the
Rust session owner. Commissioning start/setup input stays on the HTTP route;
Thread and device operations cannot be sent through this relay profile.

Commissioning progress and opaque `ble_write` events use the existing IPC event
shape directly as text messages, without an id. The worker forwards only the
bound session and forwards writes only for the latest successful attach's
generation. Reattach must complete before writing under the new generation.
The phone performs GATT I/O only; the gateway retains BTP, PASE, Thread provision,
authority and CASE ownership.

Messages are at most 2048 bytes. Incoming and outgoing queues each hold at most
eight frames/messages. Ping receives pong; valid client frames renew a 30-second
idle deadline, so clients should send ping at least every 20 seconds. A frame
that cannot finish sending within two seconds closes the connection. Protocol
errors, repeated IDs, wrong sessions, overflow or loss close it with bounded
cleanup and best-effort close frames. Service failure closes the phone and
invalidates native caches; callbacks/mutations are never replayed automatically.

Phone disconnect cancels the bound active session through the Rust owner.
Cancelling a retained terminal session preserves that terminal result and does
not remove a commissioned node. Reconnect opens a new socket, binds status again
and explicitly attaches BLE anew. It must not reuse an old generation or assume
a lost C1 write was delivered. Native shutdown closes both pending handoffs and
active sockets; bound-session cancellation is subject to the existing one-second
IPC deadline. A disconnected/unbound phone cannot cancel another session.
