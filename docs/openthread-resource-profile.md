# OpenThread resource profile on the factory MIPS gateway

The factory gateway completed a 100-second OpenThread simulation on 2026-10-03:
20 seconds disabled idle, 60 seconds of repeated calls, and 20 seconds enabled
idle. Under this workload, OpenThread averaged **11.02% of one CPU core** and
reached **764 KiB sampled RSS**. This supports continuing the integration
experiment; it does not establish production capacity for a real Thread mesh or
a Matter controller.

## Results

| Phase | Samples | Measured CPU interval | OpenThread mean CPU | Highest interval CPU | Sampled RSS range |
| --- | ---: | ---: | ---: | ---: | ---: |
| Disabled idle | 18 | 19.60 s | 0.00% | 0.00% | 280–660 KiB |
| Loaded | 48 | 58.09 s | 11.02% | 14.63% | 740–764 KiB |
| Enabled idle | 18 | 19.50 s | 0.05% | 0.87% | 764 KiB |

The test relay was measured separately: 56 KiB maximum sampled RSS and 3.60%
mean CPU during load, with no measurable CPU increment during either idle
interval. OpenThread and the relay together averaged approximately 14.62% CPU
under load. These figures exclude SSH, the monitoring shell, and production
services.

Device MemFree before staging was 3,220 KiB; its minimum across the 84 samples
was 1,752 KiB. MemFree is a whole-device observation, not OpenThread's allocation
size: executable pages, SSH, monitoring, and existing services also contribute.
OpenThread's RSS stayed at 764 KiB during the final idle phase. That short window
does not establish the absence of memory leaks.

The workload completed 1,188 cycles over 60.034 seconds, or 19.79 cycles/second.
Each cycle performed `ccathreshold` and `txpower` radio GETs followed by sending
and receiving a 256-byte UDP payload at OpenThread's own Thread IPv6 address.
The harness verified every receive callback: 304,128 payload bytes and 2,376
loaded radio GETs in total. Cycle latency, including both GETs, CLI/SSH,
host-simulator networking, and UDP delivery, was:

| Median | Mean | p95 | p99 | Maximum |
| ---: | ---: | ---: | ---: | ---: |
| 25.18 ms | 28.39 ms | 48.28 ms | 63.11 ms | 108.29 ms |

The cycle rate was intentionally capped at 20/second; this is not a throughput
limit measurement. UDP delivery used the internal OpenThread IPv6 stack and
needed neither kernel IPv6 nor TUN. It did not send the UDP payload over RF.

## Measurement method and reproduction

The actual statically linked MIPS OpenThread CLI used its Unix HDLC adapter to
connect to a small test relay on the gateway. The relay forwarded frames over
IPv4 TCP to a synthetic RCP on the development host. The production Ziggurat
multiplexer and physical coordinator were outside this test path.

```sh
bash scripts/build-openthread-relay.sh host
bash scripts/build-openthread-relay.sh mips
python3 scripts/test-openthread-device.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --profile
```

Use the [documented OpenThread build](openthread-feasibility.md) first if its
executables are absent. Without `--host`, the runner checks local executables
and sockets; it cannot provide target CPU/memory measurements.
`--profile-load-seconds` accepts 5–120 seconds; both idle phases remain 20 seconds.

The relay reports `sysconf(_SC_CLK_TCK)` from the target, which returned 100.
Approximately once per second, the watchdog reads `/proc/<pid>/stat` user plus
system CPU ticks, `/proc/uptime`, RSS and thread count from process status, and
MemFree from `/proc/meminfo`. Relay counters are read separately. CPU percentage
is `100 × tick_delta / CLK_TCK / uptime_delta`. Phase means sum the intervals
between samples in that phase. The measured interval is shorter than the full
phase because boundary samples are excluded. Phase labels use host collection
time and can be shifted slightly by SSH buffering.

All 84 samples reported one OpenThread thread, so main-PID CPU counters cover
this tested process. CPU resolution is 10 ms; a reported idle 0.00% means no
counter increment between the retained samples. The highest interval CPU uses
roughly one-second intervals, not instantaneous CPU. Maximum sampled RSS can
miss shorter allocation peaks. Sampling itself consumes device CPU that these
process-specific figures exclude.

The CLI binary is unchanged from the original passing target validation:
919,456 bytes, SHA256
`27a55270564874449e0bff93a4c941da01deded960c94e32139fde68e22ae08f`.
The relay's SHA256 and all sample counters are retained in the
[key-free measurement record](openthread-resource-profile.json).

## Harness changes and safety checks

The runner now supports the three profiling phases, verifies sustained UDP
receives, and exports per-phase CPU/RSS summaries. The relay accepts a bounded
optional lifetime and reports the target clock rate. The synthetic RCP handles
raw radio transmit completion for Thread timers: broadcast completion is
fabricated, while transmissions requesting an ACK receive synthetic NO_ACK
because there is no peer. The passing run recorded 15 simulated radio TX
requests, no unsupported properties, no reset command, and no simulator error.

Two initial target profiling attempts exposed stdout interleaving in the
harness. The watchdog and CLI share the SSH output stream; a sample could split
`Done` or a UDP callback, causing a false timeout. The collector now extracts
samples and rejoins the interrupted CLI fragments. Both attempts cleaned up
and left the production dashboard responding. The corrected full target run
and a subsequent local smoke test passed.

Only unique `/tuya/native-test/ot-sim-*` and `/tmp/ot-sim-*` paths were used.
The CLI retained its 4 MiB virtual-memory limit. The watchdog aborts above
2 MiB RSS or below 1 MiB MemFree; the default profiling run permits at most
135 watchdog samples, while the relay has a 145-second alarm. Sampling includes
shell overhead, so its sample-count bound is not an exact wall-clock limit.
The test never opens `/dev/ttyS1`, stops services, or reads production keys.
The temporary dataset is newly generated, private, and removed with the test.

The passing CLI exited 0. Cleanup verified removal of both unique directories.
Production reported `online=true`, `network_formed=true`, and
`coordinator_backend=ziggurat` before and after. Those status checks establish
continued service availability, not a measurement of Zigbee traffic latency.

Tracked evidence is [openthread-resource-profile.json](openthread-resource-profile.json).
Detailed local evidence remains ignored under `logs/`:

- Passing target profile: `openthread-device-1791047991681340297.json` and `.log`.
- Passing local smoke test: `openthread-device-1791048064555120346.json` and `.log`.
- Earlier harness failures: `openthread-device-1791047674148556342` and
  `openthread-device-1791047860145656013` reports/logs.

Remaining work is real RCP access through Ziggurat, another Thread endpoint,
combined Zigbee/Thread radio load, longer runtime/ABI testing, and Matter
commissioning/control. This profile contains no real mesh routing, RF contention,
cryptographic Matter sessions, or Matter controller memory usage.
