# Matter IPC v2: length-prefixed CBOR

MicroHAD and `mthread-matter` use a persistent private Unix stream at
`/tmp/microhad-matter.sock`. Every frame is a two-byte **little-endian** unsigned
length followed by one CBOR map. The length includes the entire CBOR envelope
and is limited to 2048 bytes. No other framing metadata lives outside CBOR.

The envelope requires `version:2`, `kind`, `opcode`, `id`, and `body`. There is no
reserved field. Kind is request 0, response 1, or event 2. Requests/responses use
nonzero u32 correlations; replies repeat the opcode and correlation. Events use
opcode 12 and correlation zero. Opcode assignments 1–11 retain the established
operations. Status request bodies are empty maps. Other bodies retain `schema:1`.

The [CDDL contract](../../protocol/matter-ipc-v2.cddl) describes the current
operations, including commissioning country/regulatory fields, Thread retry,
bindings, sensor reports, and button events. Existing lifecycle, authorization,
range, cardinality, job, and readback checks remain authoritative.

Session, node, service-job, and catalog revision fields use unsigned 64-bit
integers. BLE address/data and Thread datasets use CBOR byte strings. Stable
endpoint IDs remain strings. Public HTTP and phone WebSocket APIs continue to
use their existing JSON, sixteen-digit hexadecimal IDs, and hexadecimal bytes.
Persistence formats and state paths are unchanged. The wire adapters explicitly
convert between CBOR and the existing in-process JSON models; JSON is never
embedded as the private message body.

Receivers permit at most **16 nested containers**, **256 pairs per map**, and
**512 entries per array**, counting the envelope and body containers. The
independent 2048-byte frame bound still applies. Known operations retain their
stricter limits, such as one endpoint per snapshot page and 244 BLE bytes.
Container sizes are checked before allocation. Maps have unique UTF-8 text keys.
Indefinite lengths, tags, undefined/simple extensions, nonfinite floats, malformed
encoding, and trailing data are rejected. Known integer fields require integers;
IDs and binary fields cannot fall back to their old text encodings.
Integers cover unsigned 64-bit and negative signed 64-bit values.

Unknown fields are validated structurally, then skipped, including fields inside
known nested bodies. A warning contains only the opcode and ignored-field count;
it never includes supplied field names, values, credentials, or packet contents.
Known-field type/range errors and unsupported actions or schema versions still
fail. Malformed envelopes close the connection; malformed operation bodies use
the existing fixed-error response behavior. Status requests retain their existing
close-on-malformed-body behavior.

Framing remains bounded and handles fragmentation, joined frames, correlated
calls, and interleaved events. Queue limits, backpressure, idle/call deadlines,
subscriptions, ownership, and reconnect behavior remain unchanged. Interrupted
mutations are never automatically replayed.

## Dependencies and verification

MicroHAD builds TinyCBOR v0.6.0 as C99 from its pinned Git submodule at
`microhad/dependencies/tinycbor`. Rust uses Ciborium 0.2.2, preceded by a bounded
structural scan. Initialize dependencies with `git submodule update --init`.

TinyCBOR C compilation includes `matter_cbor_platform.h`, which supplies endian
macros from libc for the factory GCC 4.4 toolchain. This keeps multi-byte integers
and floating-point values in CBOR network byte order on the big-endian target.

`protocol/matter-wire-shapes.json` is the shared field/conversion table.
`scripts/generate-matter-wire-shapes.py` generates the native and Rust lookups.
`scripts/generate-matter-wire-vectors.py` generates shared interoperability and
hostile-input vectors consumed by Rust and native tests. Generated files are
checked in so target builds do not require code generators.

```sh
make -C microhad host test
make -C microhad mips
bash micro-thread-matter-daemon/scripts/build.sh host service
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
bash micro-thread-matter-daemon/scripts/build.sh mips service
python3 micro-thread-matter-daemon/scripts/test-matter-service.py
python3 microhad/scripts/test_matter_native_worker.py
python3 microhad/scripts/test_matter_native_report_integration.py
python3 microhad/scripts/test-matter-ipc-codec.py
```

## Release compatibility

Both daemons must use v2 together. The service executable is `mthread-matter`,
installed at `/tuya/native/mthread-matter`. Use the
[maintenance release](../../docs/deployment.md#maintenance-release) for coordinated
updates. Runtime configuration, network state, Matter identity and socket paths
are preserved. Production binaries exclude test-only report hooks.

## Wire sizes

Representative complete frames, including their length prefix:

| Message | CBOR v2 bytes |
| --- | ---: |
| Empty status request | 36 |
| BLE indication, 3 bytes | 92 |
| BLE indication, 244 bytes | 338 |
| Thread dataset import, 254 bytes | 338 |
| Correlated failure | 68 |
| Measurement event | 192 |

The shared vectors reproduce these CBOR sizes.
