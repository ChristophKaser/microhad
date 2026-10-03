# Matter IPC version 1

This fixes the contract for the remaining implementation. Only status opcodes
1/2 currently dispatch in the service. The other rows are reserved definitions,
not claims of implemented commissioning, relay, or device control.

## Framing and compatibility

The ten-byte header is payload length u16 LE, version u8 (1), kind u8
(request 0, response 1, event 2), opcode u8, reserved u8 (0), correlation u32 LE.
Payload is at most 2048 bytes. Requests/responses have nonzero correlations;
events have zero. A response repeats the request opcode/correlation. Never
reuse a live correlation on a connection. Clients can reconnect and reuse IDs
after all outstanding calls on the old connection have been discarded.

Statuses retain their existing JSON objects and empty request body. New bodies
are strict UTF-8 JSON objects with `schema:1`. Reject duplicate/unknown fields,
unsupported schema/action, invalid numbers, lengths, or trailing JSON. Limits
apply to the encoded byte payload before JSON parsing. Hex strings are lowercase
ASCII with even length; IDs are fixed sixteen-character strings, preserving
64-bit values through JavaScript. No body has a logging or debug-format path.

Changing an assigned opcode's meaning, removing required fields, or changing
field types requires a new schema and explicitly negotiated support. Header
changes require a new wire version. A future receiver must reject incompatible
versions; it must never guess another body shape. Opcodes 13–255 are reserved
for a future frame version and rejected by the current decoder.

## Operations

All new requests include `schema:1`; omitted optional fields use only the stated
defaults. All numeric fields are integers. `session`/`node` use the ID encoding
above. No API caller can request arbitrary Matter cluster commands.

| Opcode | Request body (besides schema) | Success result | Limits |
| --- | --- | --- | --- |
| 1 Matter status | Empty | Existing enabled/available/controller_initialized/fabric_ready/state object | 2048 bytes |
| 2 Thread status | Empty | Existing enabled/available/attached/state/linux_ipv6_or_tun object | 2048 bytes |
| 3 Commission start | `discriminator`, `pin`, `timeout_seconds` | `session`, `state`, `deadline_remaining_seconds` | discriminator 0–4095; valid Matter PIN; timeout 30–900 seconds |
| 4 Commission status | `session` | Session state, remaining time, optional terminal `error` and commissioned `node` | One active session; latest terminal session retained until next start |
| 5 Commission cancel | `session` | Same session snapshot in terminal `cancelled` state | Idempotent for retained cancelled session; mismatched session is stale |
| 6 BLE attach | `session`, `address`, `mtu` | `generation` | Address exactly 12 hex characters; ATT MTU 23–517 |
| 7 BLE indication | `session`, `generation`, `sequence`, `data` | Accepted sequence | Generation/sequence u32 nonzero; data 1–244 decoded bytes, also at most negotiated MTU minus three |
| 8 BLE write complete | `session`, `generation`, `write`, `success` | Accepted write ID | Write ID u32 nonzero; success boolean; must match one outstanding C1 write |
| 9 Thread dataset | `action` plus fields below | Redacted Thread snapshot | Create/import require authoritative coordinator channel 11–26 |
| 10 Device data | `action` plus fields below | Bounded device page or single typed attribute value | At most four device summaries per page; 64 stored devices maximum |
| 11 Device command | `node`, `endpoint`, `command`, optional fields below | Accepted job ID and state; completion through event/snapshot | Endpoint u16; only enumerated commands |
| 12 Event | No request allowed | Event from service to client | kind 2, correlation 0; same 2048-byte limit |

Thread actions: `status` has no other fields. `create` takes `zigbee_channel`.
`import` takes `zigbee_channel` and `dataset`, 1–254 decoded hex bytes. Neither
response includes dataset credentials. Only the native coordinator owner may
supply the current channel; HTTP input must not override it. Existing attached
network replacement requires explicit lifecycle policy, implemented in plan 06.

Device actions: `snapshot` takes `cursor` (u16, default 0), returning `devices`
and optional `next_cursor`. Summaries contain stable node IDs, availability and
bounded names (at most 32 UTF-8 bytes); no credentials. `read` requires `node`,
`endpoint` (u16), `cluster` (u32), `attribute` (u32). Only discovered/supported
attribute paths are permitted. A value is tagged `null`, `bool`, `integer`,
`number`, or `text`; text is at most 128 UTF-8 bytes. Descriptor/measurement
discovery, scaling and report semantics are implemented under plan 07.

Commands: `on`, `off`, `toggle` have no additional fields. `move_to_level`
requires `level` (0–254) and `transition_deciseconds` (0–65535). Reject command
fields inappropriate to the selected command and endpoints that do not advertise
its cluster. Commissioning and commands return job acceptance immediately;
they never wait synchronously for phone, network, or accessory completion.

## Replies, errors and events

For opcodes 3–11, success is `{"schema":1,"ok":true,"result":{...}}`; failure
is `{"schema":1,"ok":false,"error":"CODE"}` with no dynamic message. Known
codes are `malformed`, `unsupported`, `not_ready`, `busy`, `stale_session`,
`stale_generation`, `not_found`, `timeout`, `cancelled`, `transport`, `storage`,
`capacity`, and `accessory`. No error includes rejected data or upstream text.
Malformed framing closes the connection. A well-framed malformed new body gets
`malformed`; a request for opcode 12 closes the connection. Invalid status bodies
retain the existing close behavior. Unexpected correlations or event kinds close
the native peer; response JSON must also validate before reaching an HTTP caller.

Events have `schema:1`, `event` and typed data: `commissioning` (session snapshot),
`ble_write` (session/generation/write/data), `thread` (redacted snapshot), or
`device` (node/path/value/availability). BLE write data has the same decoded
1–244-byte bound as indications. Each relay connection serializes C1 writes;
the gateway grants the next write only after completion of the outstanding one.
An event subscriber must keep polling/heartbeating before the two-second IPC idle
deadline. Backpressure or disconnect drops that peer, never a coordinator process.

## Session ownership and races

The gateway generates opaque unpredictable nonzero session IDs from OS entropy.
Correlations identify individual IPC calls, not sessions or authentication. A
trusted LAN route/phone relay is the deployment model; gateway TLS trust is not
required. Private Unix ownership is checked separately from user-facing routes.

States are `waiting_ble`, `pase`, `provisioning`, `waiting_thread`, `case`,
`completing`, then `complete`, `cancelled`, `failed` or `timed_out`. Only one
nonterminal session exists. Deadlines use monotonic time; status never extends
them. Cancellation drops its commissioning future, clears relay writes and
credentials, and advances the generation before a new session can start.

Every BLE attach increments the session's generation; exhaustion fails closed.
Indication sequence starts at one for each generation and advances exactly once
per accepted callback. Old generations, duplicate/out-of-order indications and
write completions cannot enter another session. An attach during active PASE
cancels that attempt before starting fresh transport. IPC disconnect alone does
not cancel a job; absent BLE acknowledgements retain the existing ten-second
relay deadline, and the commissioning deadline bounds abandoned sessions.

Secrets remain gateway-owned: the phone relays GATT C1/C2 bytes only. Manufacturer
attestation follows the owner's chosen bypass policy; PASE and CASE remain
encrypted/authenticated. Setup PINs, datasets, authority keys and BTP ciphertext
must never be placed in status, events outside the opaque relay, or logs.
