# Gateway Matter service runtime

The `thread-matter` executable is a single-threaded controller process. It owns
OpenThread, the Matter UDP transport, the private store, and a bounded Unix IPC
listener. It creates/restores a private controller authority and fabric;
Thread create/import/restore and commissioning/relay dispatch are implemented.
Production operational SRP/DNS-SD is wired and host-tested; remote catalog
recovery and the installed daemon's process-level report path remain pending.
The production Manager and IPC report path are composed with a live CASE
accessory fixture and native gateway process on the host. The optional native worker now
supplies protected coordinator observations and caches status for HTTP.

Build with `bash scripts/build-rs-matter-transport.sh host service` or
`bash scripts/build-rs-matter-transport.sh mips service`. The stripped target
artifact is `build/rs-matter/thread-matter-mips`. No build deploys or starts it.

The native report-integration test uses the host-only
`scripts/build-rs-matter-test-service.sh` command. It enables the
`controller-service-test-hooks` feature in the isolated
`build/rs-matter/service-test-target` directory, leaving the production host
and MIPS service outputs untouched. Both build commands enforce this boundary:
the test artifact must contain its fixture environment marker, while host
production and stripped MIPS artifacts must not. The checks use
`scripts/check-matter-service-test-hooks.py`. Never deploy this test-hook
executable.

Example invocation, once the coordinator provides a private Thread endpoint:

```sh
thread-matter --socket /tmp/microhad-matter.sock \
  --state-dir /tuya/native/matter \
  --radio-url 'spinel+hdlc+unix:///tmp/ziggurat/thread.sock?no-reset'
```

The adapter accepts only an absolute Unix HDLC endpoint with `?no-reset`; it
cannot open the UART. Unexpected platform reset requests abort this process.
Thread uses OpenThread's userspace IPv6/UDP stack, requiring neither a kernel
IPv6 interface nor TUN. It supports validated create/import/restore, gated by fresh private native
coordinator observations. It waits in `awaiting_coordinator` before initializing
OpenThread until the optional native worker supplies the observation.

The private state directory must be absolute. The store validates owner-only
directory/files, refuses symlinks, and locks out another store owner. The process
changes its working directory there so the pinned OpenThread POSIX `tmp`
settings subdirectory stays within private state. Entropy comes exclusively
from `/dev/urandom`; failure stops the service. No fixture credentials or open
accessory commissioning window are installed in this controller.

First start commits the complete root/intermediate/controller key and certificate
chain, IPK and allocator as one authority record, then installs the corresponding
upstream fabric. An interruption between those writes restores the same identity.
Reload verifies the chain, matching public/private keys, metadata and exact loaded
fabric materials; corrupt or mismatched records fail closed. Reads do not write.
`--administrator-vendor-id` takes a decimal u16 and defaults to 65521 (`0xfff1`,
the development administrator vendor). This metadata is not a manufacturer
certificate. Set it before first initialization; a different value on restart
is rejected. See the native `docs/matter-identity-store-v1.md` for the schema.

IPC uses the shared version-1 bounded frame format. Opcodes 1 and 2 accept empty
payloads and return redacted JSON. Matter reports initialization/fabric readiness;
Thread reports attachment. Opcodes 3–11 now decode strict schema-1 request bodies;
valid operations without implemented lifecycles return `unsupported`, and invalid
bodies return `malformed`, without echoing input. Opcode-12 requests close the peer.
Limits:
four clients, four accepts per loop, 4096 read/write bytes per client per loop,
eight queued responses per client, 2048 bytes per payload, and a two-second idle
deadline that partial requests cannot extend. OpenThread polls at most five
milliseconds between Matter and IPC turns.

The socket and permanent adjacent `.lock` file are owner-only. A held flock
rejects duplicate starts. An existing private socket is probed nonblocking and
removed only on connection refusal; regular files, symlinks, public sockets,
and live listeners are refused. Normal shutdown removes only the socket inode
created by this invocation. Retaining `.lock` prevents an inode replacement race.
SIGINT/SIGTERM unwind the transport and release storage and IPC ownership.

Stderr contains only static start/stop/failure messages. Request payloads,
CLI inputs, datasets and store contents have no logging path. A missing radio,
transport failure, or corrupt state exits the service. The native project's
optional paired supervisor implements independent bounded retries and backoff;
see `microhad-zigbee-native/MATTER_INTEGRATION.md`. It is not activated in production.

Validation: 36 Rust library tests pass, including six listener checks, three
strict-body groups, five session lifetime checks and four authority recovery checks.
`python3 scripts/test-matter-service.py`
starts the real host service against the synthetic RCP and verifies both JSON
statuses, typed-body rejection, duplicate exclusion, 20 timely calls, socket permissions,
clean SIGTERM, fabric restoration in a second process, unchanged credential file
on reload/status, corruption/vendor refusal, and no RCP reset. Socket tests need local permissions;
the workspace sandbox otherwise returns EPERM. These checks prove host behavior,
not physical mesh attachment or commissioning a real accessory.

The factory uClibc lacks `accept4`. The MIPS adapter supplies an `accept`/`fcntl`
compatibility wrapper; its descriptor flag update is valid for this single-threaded
process, which does not concurrently fork. Existing factory sleep/name shims remain.
With authority and guarded Thread lifecycle the package is 1,552,028 stripped bytes,
static big-endian MIPS-I/o32. Strict request decoding was 1,343,120 bytes and
the initial status-only checkpoint 1,228,412 bytes. These are package sizes, not RSS or
physical-device runtime measurements. ELF validation is recorded in the native plan.

The encrypted commissioning fixture now uses the same `Identity` authority,
signer and allocator as this service. Both host BTP and UDP runs pass PASE,
Thread provisioning, CASE and fresh CASE after committed credentials/authority
reopen, with the simulated phone detached. The allocator then returns a value
above the commissioned node. Timings were 217/264 ms on the development host;
the updated factory fixture builds but has not been executed on the gateway.
This does not yet wire the commissioner into the service's IPC job dispatch.

## Live CASE reports into native gateway state — 2026-10-05

The extended commissioning fixture now connects the actual native Matter
worker to the production Manager and `IpcServer` while the synthetic accessory
has active CASE subscriptions. It drives snapshot pagination through
`POST /api/matter/devices/snapshot`, then sends temperature, humidity, Air
Quality, CO2 and PM2.5 reports into the native common device state. The
end-to-end host command is:

```sh
python3 scripts/test-openthread-matter-transport.py --native-bridge \
  --sensor-reports --service-discovery --service-owner --ble --commissioning
```

All five expected values, units and measured-quality labels are asserted in
`/api/status`. This joins the live accessory's CASE `ReportData`, the
production Rust Manager report queue/IPC publisher, the native IPC worker and
the common gateway device model. It uses only a synthetic RCP and accessory;
it does not start the installed `thread-matter` service process or use hardware.
That daemon-process variant and physical accessory acceptance remain open.

## Gateway MIPS CASE sensor-report profile — 2026-10-05

The updated MIPS commissioning fixture was run on gateway `192.168.178.62`
with its production Ziggurat service left online. Command:

```sh
python3 scripts/test-openthread-matter-device.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --commissioning --ble \
  --service-owner --sensor-reports
```

The fixture used a synthetic RCP and accessory, never opened `/dev/ttyS1`, and
cleaned its isolated state and staged files. Production reported
`online=true`, `network_formed=true`, and backend `ziggurat` both before and
after. The 2,580,192-byte MIPS-I probe (SHA-256
`8fb69bd91ba9a7a550407f5a248ee507513b08408e0072dd2256b7a1f2caa138`) completed
encrypted PASE/CASE, production Manager discovery/reads/commands, seven
subscriptions and all five sensor reports in 10.94 seconds. Sampled peak RSS
was 2,204 KiB; minimum raw MemFree was 10,080 KiB; minimum free plus clean
inactive file cache was 19,216 KiB. Sampled process CPU was about 4.41 seconds
over 11.75 seconds of wall time (37.5% of one core on average). The run stayed
below its 3,072 KiB RSS limit and 4,096 KiB effective-free floor. Gateway uptime
advanced throughout; no reset occurred during this test.

An earlier run also enabled `--service-discovery` and stopped at the fixture's
45-second synthetic SRP-registration deadline. The simulated RCP does not
advertise/respond as an SRP registrar on the target. It peaked at 1,772 KiB RSS
and kept production online; no report-work conclusions are drawn from that
incomplete run. Host SRP/DNS-SD integration remains covered by the synthetic
host fixture. These target results cover the MIPS commissioning/Manager report
procedure, not the installed `thread-matter` daemon, native worker on target,
real accessories, or combined live Zigbee/Thread radio traffic.

## Installed MIPS service with synthetic Thread activity — 2026-10-05

The device harness can now drive the installed MIPS `thread-matter` process
through its private IPC socket while a test-only relay connects it to a
synthetic Spinel RCP. It sends the same coordinator observation the native
worker uses, requests Thread dataset creation, checks the resulting lifecycle
status, and samples the service process. The command is:

```sh
python3 scripts/test-matter-service-device.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --seconds 8 --active-thread-simulation
```

The service started in 0.05 seconds after IPC socket creation. Its IPC replies
confirmed a persisted channel-15 dataset and `state=attaching`; it remained
`attached=false`, as expected from a synthetic RCP with no peer mesh. Peak RSS
was 1,468 KiB, the active test/sample interval consumed one process CPU tick,
minimum raw MemFree was 11,516 KiB and minimum MemFree plus clean inactive file
cache was 19,756 KiB. Production Ziggurat was online/formed before and after,
the MIPS process stopped cleanly, and all private paths were removed. This is
the first gateway execution of the installed daemon's IPC observation/create
path. It does not validate physical RF attachment or the daemon's CASE report
loop.

Three separate active synthetic runs completed with the gateway's production
status unchanged at cleanup; a fresh invocation shortly afterward found
production offline and a lower `/proc/uptime` value each time. After each
reboot, filtered current-boot `dmesg` checks found no watchdog, panic, OOM or
reset-cause message. The paired supervisor restored
Ziggurat online/formed roughly 20 seconds after one reboot and about 138
seconds after another. These events create a repeated timing association with
the active test, but do not prove it caused the resets; measured Matter process
load was low, and the simulator never opened the production UART. Read-only
checks found no `/dev/watchdog`, watchdog sysfs device, feeder process, or
watchdog references in the active init/paired-launcher scripts. The
`/proc/watchdog_reboot` entry was left unread because an earlier read blocked
SSH. There is no identified Tuya watchdog feeder or safe disable control.

The latest reboot was followed by a two-minute idle observation: uptime
advanced from 37.75 to 160.65 seconds without another reset, and Ziggurat
returned online/formed between 138 and 149 seconds. Reset cause and cadence
remain unknown; the uptime windows are discontinuous and cannot establish a
reset rate. Detailed observations are in
`docs/openthread-resource-profile.md`. The user authorized continued guarded
gateway tests after the earlier stop recommendation; this authorization does
not establish that Matter triggered the resets.

## Guarded Thread lifecycle — 2026-10-04

Opcode 9 accepts status/create/import and private coordinator `observe` bodies.
Observations expire after five monotonic seconds. Online channels must be 11–26
and match the validated persisted dataset; an offline observation stops Thread.
No observation is stored or inferred from configuration. The Matter-enabled native worker sends heartbeat observations about once per
second. Unknown/offline or coordinator progress older than 2.5 seconds is sent
as offline. Without an online observation startup sends no RCP traffic.

The owner-only store record at key 0x1001 contains magic `TMDT1`, channel byte,
little-endian u16 TLV length and up to 254 validated TLV bytes. Create uses OS
entropy and the observed channel. Imports reject malformed, duplicate, pending,
invalid-prefix/PAN and mismatched-channel datasets before mutation. Replacement
returns `busy`; identical import is idempotent. Persist-before-apply failure
retains the validated record. At the lifecycle boundary, a failed apply with a
successful disable is held in `apply_failed`; heartbeats do not repeat the
write, and an explicit retry may reapply it. A non-OK Spinel status during the
real OpenThread RCP property transaction can leave the pinned OpenThread
instance unusable: the service exits instead of replying on IPC. The host
synthetic-RCP regression verifies that the committed dataset survives this
exit and restores only after a fresh service process receives a matching
coordinator observation. The test launches that second process directly; it
does not exercise the paired launcher or physical RCP failure behavior.

Cached active/pending datasets cannot select a boot channel. A settings-get
linker wrapper suppresses their unchecked restore, preserves security counter
records and permits reads of newly validated datasets. Supported channels are
restricted to the observation. Private POSIX record envelopes are checked before
backend initialization, which otherwise truncates malformed files. Core dumps
are disabled before credentials are loaded.

The synthetic process test covers deferred boot without RCP requests, create,
malformed/mismatched imports without mutation, observation expiry, persisted
restart, conflicting cached channel and unchanged credentials on status/reload.
All recorded synthetic channel writes are 15 and no reset is issued. This does
not prove physical RF attachment, coexistence or target runtime resource use.

The native worker checkpoint (`microhad-zigbee-native` implementation documented in
plan 04) passes an actual native-to-Rust IPC/HTTP host round trip with synthetic
Ziggurat and Thread RCP peers. Native observations authorize dataset create;
all recorded writes match the confirmed Zigbee channel. Coordinator loss moves
Thread to `awaiting_coordinator`, and service shutdown produces cached HTTP
503 without stopping native. Commands: `python3
../microhad-zigbee-native/scripts/test_matter_native_worker.py --real-service` from
this project (requires both host builds). This is synthetic host evidence only.

## Targeted IPC events — 2026-10-04

Private opcode-9 Thread status accepts optional `subscribe:true` (default false).
The correlated response is followed by an opcode-12 event carrying schema 1,
event `thread`, and the redacted snapshot in `data`; later events are emitted
only when that snapshot changes. Existing status/observation callers do not
subscribe and keep their request/reply behavior. Subscription is tied to the
connection, not a reusable correlation or session identifier.

The listener assigns monotonically increasing, nonzero internal peer IDs and
never reuses them; exhaustion fails closed. Targeted publish is bounded by the
existing eight-frame reply/event queue and 4096-byte per-turn write budget.
Full queues, expired/closed connections and malformed requests affect only
that peer. Outgoing events do not renew the two-second idle deadline; a
subscriber must send requests to remain connected. No broadcast, flash write
or payload logging is introduced.

`responses` provides bounded schema-1 reply serialization and typed Thread,
commissioning-snapshot and opaque BLE-write event bodies. BLE bytes serialize
as lowercase hex, bounded at 244 decoded bytes, with nonzero session/generation/
write IDs. Exact vectors and secret-free session snapshots are covered by tests.
Commissioning/BLE events are building blocks only: production commissioner
dispatch and the native persistent event consumer are still pending.

Verification: 41 controller-service library tests, 24 control-ipc-only library
tests, the synthetic service test (subscription before OT startup, then channel
observation/change event), and native-to-real-Rust synthetic HTTP regression
pass. The factory build/artifact is recorded in native plan 02 after verification.

The factory event-enabled service build is static big-endian MIPS-I/o32 and
1,556,124 stripped bytes. No target execution or real-device test was performed.

## Owned commissioner dispatch — 2026-10-04

The current runtime dispatches commissioning/relay opcodes 3–8; device opcodes
10/11 remain unsupported. Start requires a freshly observed matching channel
and active validated dataset; no accessory responder or manufacturer/gateway
certificate is introduced. Attach installs the actual owned commissioner future,
using persisted authority, monotonic allocation and zeroizing setup-code copies.
BTP and userspace UDP share the single Matter pump. Targeted phase/write events
are produced on the selected persistent commissioning peer. See
`matter-session-lifecycle.md` for ownership and callback rules.

Native event consumption, phone BLE transport and operational DNS-SD remain
open. A job may reach CASE only after encrypted PASE and Thread provisioning;
without a real resolver its commissioning deadline still bounds completion.
No production fixture address injection or fake success is used. The encrypted
probe's new `--service-owner` mode exercises this same owner/future with a
synthetic accessory and explicit fixture rendezvous; it passes PASE, provisioning,
CASE completion, controls and fresh CASE after persisted authority/fabric reopen.

Checks: 42 service-feature tests, 24 control-only tests, the running-service IPC
harness including a real 30-second timeout, native-to-real-Rust HTTP regression,
and encrypted owner/direct BTP/direct UDP fixtures pass. Host timings were
206/199/246 ms respectively; they are not gateway performance measurements.
Both factory builds pass: service 1,740,456 stripped bytes, commissioning probe
2,076,352 bytes, static big-endian MIPS-I/o32. Neither updated artifact was run
on the physical gateway.

## Persistent node catalog checkpoint — 2026-10-04

The owned commissioner now persists bounded node reservations and fabric/Thread/
completion milestones in TMKV key 0x1002. Dropped tasks become recovery-required;
startup reconciles interrupted local stages without reusing IDs. Cleanup storage
failure makes the owner fatal and exits the service. An orphaned catalog cannot
generate replacement authority. Reads introduce no flash writes.

48 service-feature tests, actual-service IPC/catalog assertions and the encrypted
owned fixture pass. Both host and MIPS service/probe builds pass; stripped sizes
are 1,748,648 and 2,084,544 bytes, respectively. This adds durable catalog metadata,
not production DNS-SD, remote reconciliation or device/report dispatch. No physical
execution occurred. See [catalog format and evidence](matter-node-catalog.md).

## Operational SRP/DNS checkpoint — 2026-10-04

The service now runs a bounded OpenThread resolver alongside Matter/IPC and
advertises an SRP server through standard Network Data publication. Validated
SRV/TXT/AAAA answers drive operational CASE without injected rendezvous. The
separate synthetic fixture uses signed SRP registration and the production
resolver, including fresh CASE after authority/fabric reopen. Query callbacks
use static generation cookies and survive dropped futures safely.

54 service library tests, encrypted resolver/cancellation/error fixture,
running-service IPC/catalog and native real-service HTTP/WebSocket checks pass.
Both host and MIPS builds pass; service/probe sizes are 1,924,788/2,264,780 bytes.
Fixture symbols are absent from the service. Remote catalog recovery, production
descriptors/operations/subscriptions and reports remain open. No deployment or
physical performance test occurred. See [resolver details and evidence](matter-operational-discovery.md).

## Current Matter service MIPS artifact — 2026-10-05

`bash scripts/build-rs-matter-transport.sh mips service` succeeds for Rust
source commit `d141586`. The stripped output
`build/rs-matter/thread-matter-mips` is a static ELF32 big-endian MIPS-I
executable with no `PT_INTERP`, 2,109,120 bytes, SHA-256
`6ba2fb6cf7629fdcd40c50bffeaac968e774e1e877055abff810c7c7d9762c93`.

Commit `4c02d8a` compiles OpenThread's C/C++ archives with abicalls enabled,
matching Rust and the final `cpic`, o32 executable; the mixed-ABI warnings no
longer appear. The RSDK still warns that GCC 4.4 does not recognize `-no-pie`,
but the verified output is ET_EXEC with no interpreter or dynamic section.
Target execution is still needed to prove runtime compatibility. Artifact size
does not measure peak RSS, CPU load, or radio scheduling deadlines; none were
measured on the gateway.

## Legacy catalog migration verification — 2026-10-05

The controller-service suite passes 76/76 tests. New catalog tests simulate a
legacy authority with allocated IDs but no catalog, verify each ID is persisted
as `RecoveryRequired` across reopen, and verify a history exceeding the 64-node
bound fails without a partial catalog write. The production manager's existing
recovery scheduler remains responsible for DNS-SD and authenticated CASE before
promoting each node. The MIPS service cross-build passes at 2,109,120 bytes;
SHA-256 `bed049e5f9ea5195bfbd52a991ba58a181ac2337405b9da5713e4ba98dd92965`.
This code performs at most 64 small catalog records at startup; it does not add
radio work before Thread attachment. No target CPU, memory or timing profile
was run because the bounded catalog initialization adds no complex target-side
procedure. More than 64 prior allocations fail closed pending broader DNS-SD
browse migration; no physical acceptance or deployment occurred.

## Actual MIPS service startup profile — 2026-10-05

`python3 scripts/test-matter-service-device.py --host 192.168.178.62
--password-file ../ssh_password.txt --seconds 10` ran the current
2,129,600-byte `thread-matter-mips` on the gateway with unique private state,
IPC and radio paths. It waited for coordinator observation, so it did not open
the RCP or production UART. Production stayed online/formed on Ziggurat. The
service created its private socket, reached `Matter controller service started`
1.07 seconds later and shut down cleanly on SIGTERM. Peak RSS was 796 KiB, raw
MemFree stayed at or above 10,896 KiB, and effective free memory stayed at or
above 19,620 KiB. CPU ticks did not increase across the 10-second awaiting-
coordinator sample. Uptime advanced 34.89 seconds across the whole transfer and
profile; no reset occurred, and unique paths were removed.

This verifies target executable startup, IPC socket creation, idle waiting and
shutdown. It does not attach the service to a target radio or measure active
CASE, DNS-SD, native-worker coexistence or deadlines. The initial profile
attempt exposed an immediate-marker race in the harness; it now waits up to 15
seconds after IPC socket creation for the start marker and reports that interval.
