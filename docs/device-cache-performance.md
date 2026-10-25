# Device snapshot CPU investigation

Measured on 2026-10-24 after the protobuf migration in `5400e2e`.
The investigation used passive process counters, the existing opt-in timing
instrumentation, and synthetic snapshots. Production cache contents were not
copied, services were not restarted, and diagnostic files stayed in `/tmp`.

## Observed cost

Across a 52.49-second passive sample, `history-write` consumed 3.86 seconds of
CPU (7.35% of one core): 3.81 seconds in user space and 0.05 seconds in the
kernel. Two cache inode replacements coincided with approximately 1.9 seconds
of CPU each. A separate instrumentation window measured two actual cache
writes at 3.89 seconds CPU / 4.93 seconds wall time total. Capturing those two
snapshots on the coordinator took 10 milliseconds CPU total.

This identifies encoding in the history worker as the expensive operation.
The small RAM-file write and coordinator snapshot capture do not explain the
CPU cost. HTTP workers also consumed CPU independently; their work is outside
this cache fix.

## Cause and correction

The native writer converted the typed `Device` records into heap-allocated,
string-keyed `Proto::Value` trees. Each field then searched the complete IPC
and cache descriptor table linearly, allocated duplicate-key set entries, and
was revisited for protobuf sizing and encoding. Nested sizing also traversed
children unnecessarily when the output stream was only counting bytes.

The writer now binds `Device` fields to generated nanopb C structs. String,
byte, and repeated-field callbacks borrow the owned worker snapshot. Encoding
uses generated descriptors and numeric tags, bypassing the generic tree and
name lookup. UTF-8 checks, finite-number checks, limits, atomic replacement,
file permissions, batching, and worker ownership remain intact. The remaining
generic IPC adapter uses immutable binary-search indexes and skips redundant
nested sizing traversals.

## Controlled comparisons

The same synthetic 26-device snapshot was used for every implementation. CPU
and wall-clock time are measured separately; the probe runs at low priority.

| Writer | Host CPU per snapshot | Gateway CPU per snapshot | Snapshot bytes |
| --- | ---: | ---: | ---: |
| Generic protobuf before fix | 3.851 ms | 1,055.0 ms | 10,458 |
| Generic protobuf, indexed names | 0.864 ms | 231.7 ms | 10,458 |
| Generated typed nanopb structs | 0.136 ms | 12.3 ms | 10,458 |
| Previous direct CBOR writer | 0.115 ms | 23.3 ms | 40,336 |

The typed prototype was about 85 times faster than generic protobuf on the
gateway, and about twice as fast as the previous CBOR writer. Decoded snapshot
content matched the original protobuf implementation. Endpoint field ordering
changed, which is valid protobuf and does not change the decoded data. These
are synthetic comparisons; the 1.95-second observed production snapshot cost
must not be confused with the controlled 1.055-second fixture measurement.

The isolated nested-sizing correction alone changed the host result only from
3.851 to 3.752 milliseconds for this shallow cache fixture. The table lookup
and generic representation were the primary cache costs.

The implemented writer was rechecked through the committed benchmark harness:
12.7 milliseconds CPU / 21.0 milliseconds wall time per snapshot on the gateway,
and 0.126 milliseconds CPU on the host. A 256-device, 103,089-byte snapshot
also passed the buffer-growth and reload path (1.33 milliseconds CPU on the host).

## Repeat the benchmark

Normal host benchmark:

```sh
python3 microhad/scripts/benchmark-device-cache.py
```

An explicitly authorized gateway run:

```sh
python3 microhad/scripts/benchmark-device-cache.py --device GATEWAY_ADDRESS --iterations 20
```

The benchmark uses synthetic records, writes a private temporary RAM cache,
checks that it can be loaded, and removes its files. It never reads production
configuration/cache contents or restarts a service. Target execution has memory
and CPU limits. `--devices` selects 1–256 records for scaling checks.

Protocol messages now use descriptive operation names such as
`MatterStatusRequest`, `CommissioningStartRequest`, `BleIndicationRequest`,
`ThreadControlRequest`, and `DeviceCommandRequest`. Numbered acknowledgement
names were replaced by `BleAttachResult`, `BleIndicationResult`, and
`BleWriteCompleteResult`. Framing and protocol version remain v3; no dual-format
or legacy-name adapter is introduced.

## Host IPC adapter follow-up (2026-10-25)

Historical adapter-only pass, superseded by the typed IPC implementation below.
The first IPC optimization pass removes whole-subtree copies during native
normalization/conversion and Rust decoding, replaces full conversion-table
scans with generated sorted lookups, avoids constructing a Rust discriminator
probe tree, and formats byte strings into one allocation. The internal JSON
message boundary still exists; this measures adapter work only, not a complete
typed-message migration or end-to-end gateway CPU. No device was accessed.
Host: AMD Ryzen 7 7800X3D, Linux x86_64. Native builds use `-O2`, C++03;
Rust uses the repository release profile. Results are median process CPU
microseconds per frame over five samples. Native uses 5,000 iterations per
sample; Rust uses 3,000. Baseline is `00b6d03`. Inputs are fixed synthetic
shared protocol vectors, including nested data, maximum BLE/dataset bytes,
and a 512-item stress case. Encode and decode are measured separately; native
decode calls the protobuf decoder directly, Rust includes the framing decoder.
Both revisions use the same driver. Wire equality is checked while encoding;
semantic and malformed-input checks are supplied by the shared-vector tests.
| Adapter / message | Encode before → after (µs) | Decode before → after (µs) |
|---|---:|---:|
| Native / empty-status | 2.88 → 2.07 | 2.33 → 1.28 |
| Native / max-ble-bytes | 11.54 → 9.78 | 6.86 → 4.92 |
| Native / max-dataset-bytes | 11.44 → 9.36 | 7.00 → 4.42 |
| Native / measurement-event | 26.69 → 16.49 | 18.56 → 8.59 |
| Native / thermostat-schedule-job | 40.22 → 23.46 | 28.24 → 11.43 |
| Native / repeated-512 | 395.78 → 107.82 | 393.52 → 111.09 |
| Rust / empty-status | 0.79 → 0.77 | 0.90 → 0.86 |
| Rust / max-ble-bytes | 3.88 → 3.89 | 9.41 → 3.61 |
| Rust / max-dataset-bytes | 4.26 → 3.76 | 10.12 → 3.32 |
| Rust / measurement-event | 7.16 → 5.70 | 6.82 → 5.26 |
| Rust / thermostat-schedule-job | 9.70 → 7.52 | 10.51 → 6.93 |
| Rust / repeated-512 | 52.93 → 52.21 | 53.54 → 43.74 |

Reproduce the native comparison from the final checkout:

```sh
python3 microhad/scripts/benchmark-matter-ipc.py --revision 00b6d03 --iterations 5000
python3 microhad/scripts/benchmark-matter-ipc.py --iterations 5000
```

For Rust, use the same example on each revision (copy the benchmark example
to the baseline checkout), with no feature changes between runs:

```sh
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/micro-thread-matter-daemon/target" \
  cargo +$RUST_TOOLCHAIN run --locked --release \
  --manifest-path micro-thread-matter-daemon/Cargo.toml \
  --example benchmark-ipc -- 3000
```

These small host timings vary with frequency scaling and machine load. The
large native nested-message improvements and Rust byte-decoding improvement
are repeatable; small differences such as Rust BLE encoding are noise and
are not claimed as gains. Production request frequency and MIPS behavior
require separate measurement.

## Typed thermostat snapshots and report suppression

The thermostat automation snapshot now builds its Lua-facing value directly
from `Device`, instead of formatting the complete thermostat to JSON and
parsing it again. If the thermostat inputs and other snapshot fields have not
changed, capture reuses the previous immutable snapshot before building any
thermostat value. Schedule-detail JSON is still parsed at that stored-text
boundary when a changed snapshot needs it. Numeric limits and readings retain
full precision (the former JSON path rounded the feature-map maximum from
4294967295 to 4294970000).

On the same host, five samples of 3,000 synthetic captures each measured:

| Thermostat capture | Before (µs CPU) | After (µs CPU) |
|---|---:|---:|
| Unchanged | 130.64 | 0.41 |
| Changed setpoint | 150.39 | 92.50 |

Reproduce with `python3 microhad/scripts/benchmark-state-conversions.py
--revision 4438d79` and the same command without `--revision`.
The fixture populates every thermostat attribute, so changed-snapshot cost is
higher than for sparse devices. Public thermostat JSON is unchanged.

Matter subscription suppression now compares typed normalized report values
instead of allocating JSON bytes. It includes node, endpoint, cluster,
attribute, value variant, unit and quality, with regression coverage preserving
JSON's signed-zero and nonfinite/null comparison behavior. Five samples of
100,000 comparisons measured 0.418 → 0.0078 µs CPU for unchanged readings and
0.425 → 0.0123 µs for alternating readings, including replacement of the saved
value. These are isolated suppression costs, not full report ingestion.

Reproduce the old serialization expression and new typed expression together:

```sh
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/micro-thread-matter-daemon/target" \
  cargo +$RUST_TOOLCHAIN run --locked --release \
  --manifest-path micro-thread-matter-daemon/Cargo.toml \
  --features controller-service --example benchmark-report-dedup
```

## Numeric history and HTTP polling follow-up

Native history producers now call a numeric entry point. Text input keeps its
bounded parser; both entry points share numeric/unit validation, exclusion of
actuator and FRITZ!Box presence history, deduplication and persistence. Zigbee,
Matter and Shelly no longer format numeric readings only to parse them again.
The Matter thermostat-temperature path now keeps its actual Celsius value/unit
instead of overwriting them with the transport reading's scalar/unit.

Status grouping borrows immutable endpoint records and copies only merged
nodes. Each published view also keeps a mutex-protected lazy serialization
cache. It expires when the wall-clock second changes, so contact-trigger
expiry still follows the original time-dependent behavior. Every newly
published snapshot starts with an empty cache.

Log polling retains one immutable ring snapshot and line index for rings up to
512 KiB, with at most 16,384 line entries. A full byte comparison determines a
hit, preserving correctness after wrap, same-cursor changes and syslog restart.
Readers hold a shared lease; timestamp conversion and query filtering are still
performed for each request. Larger rings/indices bypass caching. On the
32-bit target this retains about 640 KiB at the configured upper bounds,
including the ring and index (plus small allocator/object overhead).

Same host/profile as above, medians of five samples, baseline `a080600`:

| Operation | Before (µs CPU) | After (µs CPU) |
|---|---:|---:|
| Duplicate numeric history submission | 0.288 | 0.048 |
| Fresh status serialization, 26 endpoints | 19.01 | 12.32 |
| Repeated status read, same published view and second | 18.89 | 0.080 |
| Log polling, unchanged 512 KiB ring | 290.77 | 235.88 |
| Log polling, ring changed for every request | 298.68 | 285.34 |

History samples contain 100,000 submissions and include the old number
formatting plus the production append call, or the new numeric append call.
Duplicate submissions exercise validation/deduplication without disk I/O; this
does not measure the cost of writing a new history record. Status samples
contain 3,000 calls; warm-cache numbers describe repeated reads of one immutable
view within a second, not the cost of every production HTTP request. Log
samples contain 1,000 requests filtering old synthetic entries from a private
SysV ring. Changing-ring differences are small and are not claimed as a clear
gain. Actual gateway savings depend on publication/log/request rates.

Run each host-only comparison with and without `--revision a080600`:

```sh
python3 microhad/scripts/benchmark-state-conversions.py --benchmark state
python3 microhad/scripts/benchmark-state-conversions.py --benchmark history
python3 microhad/scripts/benchmark-state-conversions.py --benchmark logs
```

The log benchmark needs local SysV IPC permissions. All fixtures use synthetic
input and private temporary resources; none accesses the gateway.

## Typed IPC: JSON detour removed (2026-10-25)
Both native and Rust frames now own generated protobuf message types. Native
worker jobs/events, Lua/device/thermostat/observation request producers, response
validation and reading extraction use those types directly. Rust request
dispatch and reply/event builders construct or consume generated Prost structs.
There is no JSON serialization/parser or generic value-tree conversion in
either IPC codec or dispatcher. IDs and opaque bytes stay numeric/binary across
IPC. Public HTTP/phone and persistence adapters keep their existing formats;
the old Rust JSON adapter is compiled only for tests/the explicit host probe.
Native decoding uses direct generated numeric-tag switches and presence flags.
Rust checks outbound bounds directly on typed structs, and validates received
wire data with integer schema IDs, presence bits and fixed counters before
Prost allocation. This preserves duplicate-field/oneof, finite-number, UTF-8,
repeated-count, depth, framing, correlation and payload-size checks without
string field lookups or per-message hash-table allocations.
Baseline: `877478c` (the previous optimized JSON bridge). Same Ryzen host and
compiler/release settings as above. Each result is median process CPU
microseconds per frame over five samples of 5,000 iterations. The identical
benchmark driver prepares messages by decoding fixed synthetic wire fixtures
outside the timed region, so it works on both old JSON frames and new typed
frames. Native decode excludes the stream prefix; Rust includes stream framing.
Do not compare the two languages as equivalent whole-request costs.
| Adapter / message | Encode before → after (µs) | Decode before → after (µs) |
|---|---:|---:|
| Native / empty-status | 1.96 → 0.44 | 1.33 → 0.90 |
| Native / max-ble-bytes | 10.22 → 0.59 | 5.25 → 1.09 |
| Native / max-dataset-bytes | 9.37 → 0.55 | 4.63 → 1.04 |
| Native / measurement-event | 16.64 → 1.31 | 8.98 → 1.53 |
| Native / thermostat-schedule-job | 23.26 → 1.66 | 11.59 → 1.66 |
| Native / repeated-512 | 117.48 → 24.49 | 124.64 → 7.38 |
| Rust / empty-status | 0.76 → 0.10 | 0.85 → 0.22 |
| Rust / max-ble-bytes | 3.85 → 0.30 | 3.45 → 0.38 |
| Rust / max-dataset-bytes | 3.62 → 0.23 | 3.23 → 0.38 |
| Rust / measurement-event | 5.60 → 0.53 | 5.19 → 0.65 |
| Rust / thermostat-schedule-job | 7.26 → 0.56 | 6.82 → 0.69 |
| Rust / repeated-512 | 52.04 → 8.36 | 42.90 → 9.37 |

Reproduce from the final checkout:

```sh
python3 microhad/scripts/benchmark-matter-ipc.py --revision 877478c --iterations 5000
python3 microhad/scripts/benchmark-matter-ipc.py --iterations 5000
python3 micro-thread-matter-daemon/scripts/benchmark-ipc.py --revision 877478c --iterations 5000
python3 micro-thread-matter-daemon/scripts/benchmark-ipc.py --iterations 5000
```

The Rust revision wrapper copies only tracked baseline sources into a private
temporary directory and uses the same benchmark driver and installed pinned
dependencies offline. These are host codec CPU measurements, not device CPU or
end-to-end HTTP timings. No device was accessed or deployment performed.

The native target retains its 64 KiB Matter-worker stack; host builds reserve
128 KiB because their owned C++ strings/messages are larger. Target assembly
stack-frame inspection was performed offline, and the periodic status loop
reuses typed request/response storage to keep stack use bounded.

Validation: native host unit/integration suites and 20 synthetic peer tests;
111 Rust service library tests; the shared native codec corpus and 21 vectors
against standard protobuf bindings; deterministic generator checks; native
worker/HTTP, service runtime and real Rust-to-native report fixtures; and both
native/Rust MIPS builds with target ABI/instruction verification. Service job
and BLE session tokens also remain numeric in worker state and polling; hex
formatting happens only at the external boundary.
