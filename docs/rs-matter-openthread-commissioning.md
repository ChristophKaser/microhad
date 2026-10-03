# Encrypted commissioning through OpenThread and a phone BTP relay

On 2026-10-03 the host and factory MIPS gateway passed commissioning against a
synthetic accessory using the actual rs-matter controller, RustCrypto and
userspace OpenThread transport. Two modes pass: PASE/provisioning over internal
UDP, and PASE/provisioning over gateway-owned BTP with simulated phone GATT
callbacks. Both complete CASE over OpenThread, toggle/read an OnOff endpoint,
and read positive, negative and null temperature values.

This is not factory-new ALPSTUGA acceptance. The radio is synthetic, both Matter
peers run inside the probe, and operational discovery uses a fixture resolver.
No physical BLE peripheral, Android GATT implementation or external Thread mesh
is involved. Native service/IPC, device discovery/reporting and real-device
acceptance remain pending. Native `MATTER=1` still rejects an incomplete backend.

## Implementation and reproduction

`rs-matter-probe/src/bin/ot_commissioning.rs` adapts the pinned upstream
commissioning fixture, preserving its Apache license. The device uses public
upstream development attestation fixtures; all generated CA, operational and
session keys use `/dev/urandom`. The owner-selected manufacturer-attestation
bypass is applied while PASE and CASE remain active. Synthetic Thread
NetworkCommissioning commands accept the public fixture dataset without
joining a real mesh. No private key, passcode or operational dataset is logged.

`src/ble.rs` adds a gateway BTP central-side relay with monotonically increasing
connection generations and one C1 write with response in flight. The phone must
report C2 indications and correlate `onCharacteristicWrite` callbacks with the
current connection and write sequence. Old callbacks are rejected; a failed or
10-second stalled write resets the BTP connection. The owner must cancel the old
commissioning future before reconnecting. A transport reset alone does not
cancel an already-running Matter exchange.

ATT MTUs from 23 through 517 are accepted; the pinned BTP stack negotiates at
most 247 (244-byte segments). Tests carry 1024-byte payloads in both directions
at MTU 23 and 247 through real handshake, fragmentation and ACK windows. Further
tests exercise old callbacks, failed writes and expiration. The phone app and
HTTP/WebSocket relay have not yet been wired to this module.

```sh
bash scripts/build-rs-matter-transport.sh host commissioning
bash scripts/build-openthread-relay.sh host
python3 scripts/test-openthread-matter-transport.py --commissioning
python3 scripts/test-openthread-matter-transport.py --commissioning --ble
bash scripts/build-rs-matter-transport.sh mips commissioning
bash scripts/build-openthread-relay.sh mips
python3 scripts/test-openthread-matter-device.py --commissioning --ble \
  --host 192.168.178.62 --password-file ../ssh_password.txt
python3 scripts/test-openthread-matter-device.py --commissioning \
  --host 192.168.178.62 --password-file ../ssh_password.txt
```

The dependency lock adds `static_cell`; `heapless` and `embassy-futures` were
already in the locked graph. The `controller-probe` feature is optional and
implies the OpenThread adapter; it does not change the native build flag.

## Measurements and memory-guard findings

The first UDP commissioning fixture took 197 ms on the host and 4170 ms on the
gateway. After adding the BTP relay, host times were 196 ms for UDP and 149 ms
for synthetic GATT/BTP. The corresponding gateway runs took **4390 ms** and
**4300 ms**. These timed sections exclude radio initialization and upload, and
include two peers plus offline CA/certificate generation. They are not a
real-accessory latency measurement or a steady-state controller benchmark.

The latter MIPS artifact was **1,912,488 bytes**, SHA-256
`96cd13d3f180da2211bb99a65d71d1c4f47aa4a8cc070bf5a841c55f741c51f2`.
Both gateway runs reached **1628 KiB sampled RSS**. Samples occur approximately
once per second, so this is an observed maximum, not a proven peak. Main-process
CPU counter differences over the sampled intervals indicate roughly 66–69%
of one core; they exclude separate LinuxThreads timer/relay tasks and do not
establish idle CPU use. The relay reports the factory clock tick rate as 100 Hz.

Early runs were stopped by resource guards rather than a protocol error:

- The original genuinely-free-RAM floor of 1024 KiB stopped one run at 908 KiB.
  Newly uploaded executables also occupy file cache, so that sample did not
  establish a 2 MiB private controller allocation.
- Flushing/dropping only the unique test binaries' upload cache allowed execution
  to start but the same floor stopped a run at 964 KiB.
- Counting clean inactive file pages conservatively allowed crypto to run;
  the initial 1536 KiB RSS ceiling stopped the two-peer fixture at 1548 KiB.
  Its usable-memory estimate was still 11940 KiB. That failure is retained in
  `rs-matter-commissioning-memory-limit.json`.

Kernel inspection showed roughly 9 MiB `Inactive(file)`, no inactive anonymous
pages, and no dirty/writeback pages. The revised probe keeps at least 4096 KiB
of `MemFree + max(0, Inactive(file) - Dirty - Writeback - Shmem)`, plus a 512 KiB
hard `MemFree` floor. This is a conservative test estimate, not the kernel's
modern `MemAvailable` field. The commissioning fixture has a 3072 KiB RSS cap;
the UDP smoke probe retains 1536 KiB. Both have an 8 MiB virtual-memory limit.
Commissioning is killed after 100 seconds. Preflight still requires 3000 KiB
actually free RAM and at least 1 MiB free flash after binary staging.

`probes/drop-test-cache.c` fsyncs and advises `DONTNEED` only on `/probe` or
`/relay` under an exact `/tuya/native-test/ot-matter-<32 hex digits>` directory.
It checks regular-file ownership and refuses a symlink at the final component.
It never opens the UART or drops global caches. Production services and network
state are not changed. Before/after status stayed online, formed and on
Ziggurat; each run removed its unique flash and RAM paths.

Function/data section flags were tried for unused OpenThread code removal.
The commissioning binary size did not shrink. They remain enabled for the
adapter build; no memory saving is claimed from this change.

## Persistent controller state work

`src/store.rs` provides a bounded, private `KvBlobStore` envelope with CRC32
corruption detection, atomic rename after file fsync, directory fsync,
single-owner flock, and 0700 directory/0600 file permissions. It rejects
corruption, truncation, public permissions, and symlink credentials. A
post-rename durability error poisons the owner so it cannot continue using
stale in-memory state. Limits are 64 KiB total, 128 entries, 4096 bytes per blob.
CRC32 detects accidental corruption; it is not authentication or encryption.

Thirteen host unit tests pass: three shared commissioning tests, three BLE
lifecycle/fragmentation tests, four persistence tests and three IPC framing tests.

The final fixture closes every original store handle, reopens the committed
credential envelope from the unique flash staging directory, initializes a
fresh Matter instance, restores its fabric, establishes a fresh CASE session,
and reads the existing OnOff value with the simulated phone detached. The
accessory and OpenThread stay running. The initial restart fixture reused an
in-memory store clone; it was corrected before recording the final reports.

This stronger check passed on both host and MIPS. Host times were 183 ms for
BTP and 231 ms for UDP. Gateway times were **5030 ms** for BTP and **5160 ms** for
UDP, including commissioning, operations, physical file reopen and new CASE.
Sampled RSS maxima were **1692 KiB** and **1700 KiB** respectively. Approximate
main-process sampled CPU was 67% and 62%; no idle figure is inferred.
The final MIPS artifact was **1,953,452 bytes**, SHA-256
`21cddc6c567f5af28a100577f9d2426d8999fd82e17fa8e2880f6b6d7fbf89bc`.

The retained final reports are `rs-matter-ble-commissioning-device-validation.json`
and `rs-matter-commissioning-device-validation.json`. They include guard limits,
resource counters, success markers, filtered production status and cleanup.
An occasional relay connection-reset diagnostic when the probe exits does not
indicate failed commissioning; the probe returned zero and all success markers
were present. No radio UART or production configuration was opened.

CA identity and node allocation still need runtime integration. These tests do
not establish complete daemon restart recovery, real accessory recovery or
power-cut tolerance. The controller and accessory currently share one process;
a production commissioner does not need the fixture accessory data model.


## Native IPC framing

The companion native `include/matter_protocol.h`/`src/matter_protocol.cpp` and
Rust `src/ipc.rs` implement the same version-1 bounded stream codec. Golden
vectors cover little-endian fields on host and factory big-endian MIPS,
fragmented/coalesced packets, invalid headers, maximum payload and queue
exhaustion. Details are in the native Matter document. This establishes the
framing boundary; daemon dispatch, native lifecycle and HTTP routes remain
pending. No native daemon or feature-enabled service was deployed.
