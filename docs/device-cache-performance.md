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
