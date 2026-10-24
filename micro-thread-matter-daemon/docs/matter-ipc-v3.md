# Matter IPC v3: length-prefixed protobuf

MicroHAD and `mthread-matter` communicate over `/tmp/microhad-matter.sock`.
Each frame has a two-byte little-endian length followed by a protobuf Envelope,
limited to 2048 bytes. Its version is 3. Kind, opcode, correlation ID, and body
are inside the envelope. The authoritative schema is
[`matter-ipc-v3.proto`](../../protocol/matter-ipc-v3.proto).

Requests/responses use nonzero u32 correlations; responses repeat the opcode
and correlation. Events use opcode 12 and correlation zero. Opcodes 1–11 keep
their existing operations. The Body oneof must match kind/opcode. Status
requests use empty messages. Other operation bodies retain `schema:1`.

Bodies and nested results are typed protobuf messages. Optional scalars preserve
presence independently of zero and false. Nullable readings and metadata use
explicit oneof null variants. Session, node, job, and revision IDs use uint64;
BLE address/data and Thread datasets use bytes. Public HTTP/WebSocket JSON and
saved configuration formats remain unchanged. Adapters retain hexadecimal IDs
and bytes at the existing in-process JSON boundaries; JSON is not embedded in
protobuf messages.

Both decoders bound nesting at 16 protobuf message levels and repeated fields
at 512 entries before allocating them. The 2048-byte envelope limit and stricter
application limits also apply. Unknown fields are skipped with count-only
warnings. Duplicate singular fields, conflicting oneofs, wrong wire types,
integer overflow, malformed UTF-8, nonfinite doubles, groups, and truncated
fields are rejected. Existing operation validation still enforces ranges,
cardinalities, authorization, lifecycle, and supported actions. Malformed
protobuf closes the connection; application-level body validation retains its
fixed-error behavior. No payloads enter codec diagnostics.

Framing, fragmentation, joined frames, interleaved events, queue limits,
backpressure, deadlines, subscriptions, inventory ordering, and reconnect
behavior retain their existing semantics. Interrupted mutations are never
replayed. Inventory bootstrap still sends identities before endpoints and
commits only complete node records.

## Bindings and builds

The native implementation uses pinned nanopb 0.4.9.1 wire primitives and generated
schema metadata, compiled as C99 with the legacy static-assert option. Rust uses
Prost with checked-in typed bindings. Python host fixtures use the same generated
schema metadata; generated standard Python protobuf bindings are also supplied.
The existing `matter-wire-shapes.json` table now serves only JSON/hex boundary
conversion and action-specific allowlists; protobuf types and tags come from the
`.proto` schemas.

Normal builds do not need generators. To regenerate in a private Python venv:

```sh
python3 -m pip install -r protocol/protobuf-requirements.txt
python3 scripts/generate-protobuf.py
python3 scripts/generate-matter-wire-vectors.py
python3 scripts/generate-protobuf.py --check
python3 scripts/generate-matter-wire-vectors.py --check
python3 scripts/test-protobuf-schema.py
```

Run the mandatory native host tests and MIPS build, host service and commissioning
builds, MIPS service build, and the codec/service/native-worker/report fixtures.
Shared vectors cover cross-language values and malformed input.

## Coordinated release

IPC v2 CBOR peers are incompatible. Update both daemons together using the
[maintenance release](../../docs/deployment.md#maintenance-release), keeping
configuration, Matter identity, and network state intact. Do not perform a
single-service update across this boundary. No dual-format negotiation is
provided. The new `/tmp/microhad-values.pb` RAM cache starts fresh; previous
CBOR/JSON RAM caches remain unused. Reverting binaries leaves the protobuf
cache unused as well. Device deployment requires an explicit request.

## Representative frame sizes

Complete frame sizes, including the length prefix, from the shared vectors:

| Message | Previous CBOR v2 | Protobuf v3 |
| --- | ---: | ---: |
| Empty status request | 36 | 14 |
| BLE indication, 3 bytes | 92 | 36 |
| BLE indication, 244 bytes | 338 | 284 |
| Thread dataset import, 254 bytes | 338 | 285 |
| Correlated failure | 68 | 33 |
| Measurement event | 192 | 102 |

The native host codec probe reports its peak process memory through `VmHWM`.
That includes the host runtime and is not a gateway RAM measurement. Native
IPC allocation is bounded by the frame limit, and cache encoding/decoding
materializes one device record at a time within the 4 MiB file limit.
