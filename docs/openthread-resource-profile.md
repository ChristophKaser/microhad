# OpenThread resource profile on the factory MIPS gateway

## Latest constrained profile — 2026-10-05

The gateway completed the 100-second OpenThread CLI simulation while the
production service remained online and formed. The constrained profile began
with 3,004 KiB MemFree and used stricter limits than the standard profile:
1,024 KiB sampled CLI RSS maximum and 1,536 KiB MemFree abort threshold. The
lowest sampled system MemFree was 1,544 KiB, only 8 KiB above the guard; this
passes the harness but is too close to treat as broad resource headroom.

| Phase | Samples | CLI mean CPU | Peak interval CPU | CLI sampled RSS | Minimum system MemFree |
| --- | ---: | ---: | ---: | ---: | ---: |
| Disabled idle | 18 | 0.00% | 0.00% | 476–704 KiB | 1,568 KiB |
| Loaded | 49 | 11.80% | 17.65% | 748–808 KiB | 1,556 KiB |
| Enabled idle | 17 | 0.00% | 0.00% | 808 KiB | 1,544 KiB |

The separate simulator relay reached 56 KiB sampled RSS and averaged 3.27%
CPU during load. The constrained workload completed 1,166 cycles in 60.046
seconds (19.42 cycles/s), including 2,332 radio GETs and 298,496 bytes of
256-byte internal UDP payloads. Cycle latency was 25.58 ms median, 30.19 ms
mean, 52.42 ms p95, 86.71 ms p99, and 168.75 ms maximum. These timings include
SSH and synthetic-host relay delivery, not RF or Matter scheduling.

Production status was `online=true`, `network_formed=true`, and
`coordinator_backend=ziggurat` both before and after. The harness verified
cleanup, no radio reset command, and no simulator error. It never accessed
`/dev/ttyS1` or stopped the service. This measures the OpenThread CLI only; it
does not measure the Rust Matter daemon, Matter CASE, or combined Zigbee/Matter
load. The 1-second sampling can miss brief CPU or memory peaks.

## Short constrained recheck — 2026-10-05

A later five-second synthetic-RCP run, after the DNS-SD catalog migration
cross-build, used the same on-device OpenThread CLI profile while the production
service stayed online and formed. Preflight showed 14,188 KiB `MemFree` plus
6,044 KiB clean inactive file cache (20,232 KiB effective free). Across 39
samples, CLI RSS peaked at 808 KiB, raw free memory bottomed at 12,696 KiB, and
effective free memory at 19,608 KiB. The 98 loaded cycles ran at 19.57/s with
54.20 ms p95 and 76.60 ms maximum cycle latency. Sampled CLI CPU averaged
about 10.8% of one core during the loaded interval; no CPU ticks accumulated in
the two idle windows.

The production API reported online/formed on Ziggurat before and after. The
simulation issued no radio reset command, cleaned its temporary settings,
binary and socket, and a follow-up uptime read advanced beyond the test window.
Current-boot `dmesg` had no watchdog/reset/panic matches. This is still an
OpenThread CLI/RCP simulation, not the Rust Matter controller or the new DNS
Browse callback; it does not establish Matter CASE memory or CPU cost. Detailed
machine-readable measurements are in
[`openthread-resource-profile.json`](openthread-resource-profile.json); the
raw test report remains in ignored `logs/`.

## Historical standard profile — 2026-10-03

The earlier factory profile completed a 100-second OpenThread simulation:
20 seconds disabled idle, 60 seconds of repeated calls, and 20 seconds enabled
idle. Under this workload, OpenThread averaged **11.02% of one CPU core** and
reached **764 KiB sampled RSS**. This supports continuing the integration
experiment; it does not establish production capacity for a real Thread mesh or
a Matter controller.

### Results

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

When MemFree is below the standard 3,072 KiB preflight, the explicit
`--constrained-memory-profile` keeps the normal profile's guard unchanged while
using 2,560 KiB preflight, 1,024 KiB CLI RSS, and 1,536 KiB runtime MemFree
limits. The 2026-10-05 run used that mode. Do not infer safety margin from a
barely passing sample; retry with more free memory for stronger headroom.

The gateway check now estimates effective free memory as `MemFree` plus clean
`Inactive(file)` pages, subtracting `Dirty`, `Writeback`, and `Shmem` from the
inactive-file amount conservatively. It reports raw MemFree, counted inactive
file cache, and their sum separately. Preflight and runtime availability checks
use that sum, while an independent raw-MemFree floor prevents cache from
masking a low-free-RAM condition. The profile above predates this accounting;
its recorded minimums and pass/fail result used the stricter raw-MemFree guard.
New JSON reports include both memory figures and both guardrails. The guarded
Matter-service startup check now uses 512 KiB raw MemFree and 1,536 KiB
effective-free floors for both preflight and runtime; its reported deployment
acceptance raw-MemFree floor is also 512 KiB.

## Matter service startup profile — 2026-10-05

The MIPS service started with unique `/tmp` state and IPC paths and an
unavailable synthetic radio socket, so it waited for coordinator observation
and never initialized Thread or accessed `/dev/ttyS1`. It remained alive for
15 seconds and logged orderly start/stop. The observed service RSS was 788 KiB;
sampled CPU ticks did not increase. Raw MemFree was 928–944 KiB while clean
inactive file cache was 10,484 KiB, giving 11,412–11,428 KiB effective free.
The test used a 512 KiB raw-free abort floor and a 1,536 KiB effective-free
floor. Those are now also the check script's preflight floors, replacing 1,536
KiB raw and 2,560 KiB effective. The earlier run would pass this memory gate,
though the observed reboot still blocks further device testing and deployment.

Within the subsequent gateway status check, uptime had reset and production
process IDs had changed; `/api/status` briefly reported Zigbee offline and
unformed. The running supervisor brought it back online and formed within the
next four-second status poll. The post-reboot kernel ring buffer contains only
the new boot, so the cause cannot be tied conclusively to this profile or
classified as OOM. Treat the reboot as a possible test-related failure: no
further on-device Matter load should run until the cause is understood. A later
read-only check found uptime had reset again between observations without a
Matter workload running. It found no `/dev/watchdog*`, `/sys/class/watchdog`,
watchdog process, module, or init feeder; the current boot log contains no
watchdog initialization or reset reason. `/proc/watchdog_reboot` exists as a
read-only, zero-length proc entry, but reading it blocked SSH, so its contents
were not retried. No watchdog has been identified that can safely be disabled.

Read-only monitoring confirmed two uptime resets between observations. The
earlier sequence changed from about 3,247 to 390 seconds; later, uptime read
722.42 seconds and then 252.93 seconds about a minute later. Production process
IDs changed after the latter reset; the supervisor restored the Zigbee
processes and `/api/status` was online/formed. Other samples (285.50 and 411.18
seconds) were separated by uncertain elapsed time and do not establish another
reset. A later calibration increased uptime from 2,496.97 to 2,507.01 over a
10-second device sleep. The two confirmed resets show that the earlier Matter
profile was not the only possible trigger, but do not establish a continuing
reset cadence. The conventional Linux watchdog interface and userspace feeder
remain absent, so a Tuya/board-level reset source is possible but unproven. No
further gateway workloads should run until the reset source is identified.

The RTL8196E datasheet documents an on-chip watchdog reset indicator in
`WDTCNR` at `0xB800311C`; this hardware path could exist without `/dev/watchdog`
or a Linux watchdog driver. The target has no `/dev/mem` or `devmem` utility, and
reading `/proc/watchdog_reboot` blocked SSH, so the hardware reset indicator
could not be inspected safely. There is no known safe disable control. A later
latest read-only check observed uptime at 3,173 seconds with `/api/status`
online and formed. A ten-second device-side interval advanced uptime from
3,198.43 to 3,208.47 seconds, with no reset during that check. These healthy
observations do not show the earlier reset issue is resolved. The register
semantics are described in the [RTL8196E datasheet](https://www.elinfor.com/pdf/Realtek/RTL8196E-CG-Realtek.pdf),
section 8.2.9.

The guarded 15-second repeat service profile did not reset the gateway, but its
first attempt exited before collecting samples and emitted no service log. A
five-second retry passed: service RSS was 788 KiB, CPU ticks did not increase,
raw MemFree stayed between 11,508 and 11,536 KiB, and effective free stayed
between 19,884 and 19,920 KiB. Uptime advanced 29.38 seconds across 27.29 host
seconds; Zigbee remained online and formed. This startup-only test had no radio
access and is not evidence of Matter operation or Thread attach. The initial
silent launch failure did not reproduce. The production service was never
intentionally stopped, and all unique staging and temporary test paths were
removed.

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

## Initial repeat-profile refusal — 2026-10-05

An authorized repeat profile checked production status and found 2,980 KiB
MemFree, below the harness's existing 3,072 KiB minimum. The test therefore
did not upload or launch the CLI/relay workload. It cleaned its unique paths
and confirmed production stayed online, formed, and on Ziggurat. A later
read-only sample reported 2,960 KiB. The existing gate remains unchanged; no
No workload ran in this first attempt. The later constrained profile above
passed its stricter runtime checks while preserving the active service.

Tracked evidence is [openthread-resource-profile.json](openthread-resource-profile.json).
Detailed local evidence remains ignored under `logs/`:

- Passing target profile: `openthread-device-1791047991681340297.json` and `.log`.
- Passing constrained target profile: `openthread-device-1791183372177187582.json` and `.log`.
- Passing local smoke test: `openthread-device-1791048064555120346.json` and `.log`.
- Earlier harness failures: `openthread-device-1791047674148556342` and
  `openthread-device-1791047860145656013` reports/logs.

Remaining work is real RCP access through Ziggurat, another Thread endpoint,
combined Zigbee/Thread radio load, longer runtime/ABI testing, and Matter
commissioning/control. This profile contains no real mesh routing, RF contention,
cryptographic Matter sessions, or Matter controller memory usage.

## Authorized reset-frequency observation — 2026-10-05

The user authorized continued guarded gateway tests to measure how common the
resets are. A 13-minute observation sampled `/proc/uptime` and redacted
`/api/status` about every 15 seconds: a three-minute idle baseline, guarded
15-second Matter startup profiles separated by two-minute idle windows, and a
three-minute post-test idle window. Profiles used private paths, no physical
radio, and did not stop or replace the production Zigbee service. The memory
floors were 512 KiB raw free and 1,536 KiB effective free.

One confirmed reboot occurred during the second profile's idle cooldown:
uptime moved from 3,742.86 seconds at 09:29:45 UTC to 29.45 seconds at
09:30:31 UTC. The profile had already exited cleanly at 09:29:38 UTC, with no
reboot detected between its before/after samples. This is evidence the reset
was not caused by that just-completed startup-only profile. The exact reset
time is only bounded by adjacent polls. Earlier read-only observations had
already confirmed two separate resets, so this is the third confirmed reset
across the documented observations; the windows are discontinuous and do not
support a reliable reset rate or prove a periodic cadence.

The startup test had one harness failure (`SERVICE_START_MARKER_MISSING`) with
no service samples and verified cleanup. A repeat passed: RSS peaked at 788
KiB, CPU ticks did not increase, raw free memory bottomed at 11,008 KiB, and
effective free memory at 19,244 KiB. A third attempt was correctly refused
while production was still offline after reboot. The production API returned
online and formed about 135 seconds after the reboot and stayed healthy through
the remaining idle window. No second reset occurred in the observed 4.7
minutes after it.

A fresh `dmesg` capture returned no watchdog, WDT, reset, or reboot lines. This
still does not identify the reset source: the hardware watchdog indicator
cannot be inspected through available target interfaces, and no Tuya watchdog
feeder or safe disable control has been found. Do not infer that the RTL8196E
hardware watchdog caused this event from the uptime drop alone. Production
service remained running throughout; no reset or watchdog control was changed.

At 10:03 UTC a further read-only SSH check observed uptime increasing from
1,990.12 to 2,001.07 seconds, and the paired supervisor, Ziggurat and native
daemon were still present. The current `dmesg` ring buffer contains current
boot initialization messages and no reset reason or watchdog initialization.
Its final vendor messages read and write `0xb8000038`; no evidence ties that
register to the documented watchdog register at `0xb800311c` or to resets.
This short check confirms no reset during its interval and does not change the
unknown reset-source assessment.
