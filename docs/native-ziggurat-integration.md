# Native service ↔ Ziggurat integration

## Current state and plan — 2026-10-03

**Implemented and deployed on the real gateway.** The native COBS/Unix socket
backend, native-owned state store and paired supervision are running against
the flashed HDLC RCP. Both backend host suites and MIPS builds pass; actual
host simulation and real existing-network traffic passed. Controlled reboot
returned online automatically with 23 inventory entries. See
[deployment and rollback](ziggurat-deployment-plan.md) and
[hardware findings, timings and limitations](real-radio-migration.md).

Cold-boot readiness took 121 seconds; RNG readiness is suspected, not proven.
Sleepy-child stimulation was waived and remains physically unverified.
Broadcast terminal verdicts remain intermittent. The 1000-frame restart margin
follows ZHA's policy and does not guarantee safety after arbitrary outages.

Next: local Thread/OTBR on the separate HDLC endpoint and standalone Matter,
with coordinated channel/reset/scans and combined gateway resource measurements.
IID 2 is unused. Thread, OTBR and Matter are not implemented by this deployment.

## Ownership and processes

The native service owns the durable Zigbee state, following ZHA/zigpy's model.
Ziggurat owns the running Zigbee stack and RCP link but does not persist state.
The native coordinator thread owns its socket and all gateway mutations. There
is no additional broker or client worker thread. Ziggurat is started separately;
the native service retries its socket and never starts a second radio owner.

The compile flag selects the production backend:

```sh
make ZIGGURAT=0 host test   # existing EZSP/ASH (default)
make ZIGGURAT=1 host test   # Ziggurat enabled; legacy unit/integration tests retained
make test-backends         # always runs both variants
make ZIGGURAT=0 mips
make ZIGGURAT=1 mips
```

Disabled outputs remain `build/host` and `build/mips`. Enabled outputs live in
`build/host-ziggurat` and `build/mips-ziggurat`. No executable is deployed by make.
The native project requires its stock RSDK and C++03. The production gateway runs `build/mips-ziggurat/microhad-zigbee-native`.

## Local API and configuration

The experimental server invocation is:

```sh
ziggurat --api cobs-unix --listen unix:/tmp/ziggurat/native.sock \
  --device tcp://HOST_SIMULATOR:PORT --spinel-iid 1 --log-level error
```

Create the socket's parent with mode 0700. The server creates a 0600 socket,
refuses existing regular files/symlinks and live listeners, and can reclaim a
verified stale socket. Keep the Zigbee COBS socket distinct from `--thread-socket`,
which carries HDLC Spinel. WebSocket and stdio modes remain available.

Native configuration adds `ziggurat_socket` (default
`/tmp/ziggurat/native.sock`). The state file is kept in the configured runtime
folder. Production startup selects Ziggurat only in enabled builds; no socket
failure falls back to EZSP or opens the UART.

## COBS and protocol v2

Each binary protocol frame is COBS-encoded and followed by `00`. A byte stream
can split frames or combine several frames in one read. COBS supplies boundaries,
not encryption or a checksum. Local socket permissions protect key-bearing API
messages. Logs must never contain raw frames or key material.

The decoded header is three bytes: command byte, then a little-endian 16-bit
word `(request_id << 2) | frame_type`. Type 0 is request, 1 response, 2 scan event,
3 notification. IDs are 1..16383 for requests; unsolicited notifications use 0,
while send confirmations carry the original send ID. Responses begin with status.
Wire integers are explicitly little endian even on the big-endian Tuya CPU.

The client limits pending requests to 64, encoded frames to 16384 bytes and queued
output to 65536 bytes and received events to 256. Linux SO_PEERCRED requires
the server to run as the same user as the native service. Malformed or oversized input is discarded to the next zero.
Control/admission requests time out in ten seconds; admitted send confirmations
have a separate thirty-second deadline. The socket reconnect interval is one
second. Snapshot refresh is every sixty seconds and after table changes.

## Startup and state recovery

The private state format `microhad-ziggurat-native`, version 1, stores the protocol's
82-byte network state and counted key, child and address tables as lowercase hex.
A SHA256 checksum covers their concatenated decoded bytes; it detects corruption,
not malicious edits by the service user.
It preserves the coordinator IEEE, PAN/channel, extended PAN, network key and
sequence, trust-center link key and hashed derivation seed/flavor, transmit power,
NWK/APS outgoing counters, and per-device APS receive counters. Receive counters
use Ziggurat's next-acceptable convention. It is separate from application device
configuration and is never committed.

State reads require a bounded regular file owned by the service user with no group
or other permissions. Writes use a private temporary file, fsync, rename and
parent-directory fsync. A lock file prevents multiple service processes from
writing the same network state; loading happens after taking that lock. Symlinks,
invalid counts, duplicate entries, invalid keys and checksum mismatches are refused.
Identical snapshots do not cause another write. A missing/corrupt file blocks startup; nothing
silently forms a replacement network.

On connection, validate Hello version 2 and query GetNetworkInfo. If unconfigured,
advance both outgoing counters by 1000 (APS at least above all saved key transmit
counters), check overflow, and save that state **before** Configure/LoadKeyTable/
LoadChildren/LoadAddressCache/StartNetwork. A reconnect to a running stack checks
identity and counter consistency and reads the live tables without reconfiguration.
A stopped/frozen stack is loaded afresh with the same identity and advanced counters.

Counter notifications monotonically update durable state; link-key notifications
replace the corresponding device key and trigger table refresh. Successful scan
responses must agree with the received and advertised table counts; partial scans
are not published. A key update racing
a snapshot causes a new scan so an older row cannot overwrite a newly saved key.
Orderly service stop uses the existing FreezeNetwork extension, scans stopped
state and saves it before disconnecting, with a five-second overall stop bound. Failure to freeze is not proof of a safe
rollback; use the existing migration tool's freeze/export checks.

## ZHA-style durability assumptions

The current ZHA adapter (`zigpy-ziggurat`) stores counter/key notifications in
zigpy's database and adds 1000 to NWK and APS counters on stateless restart. This
implementation follows that policy, without adding persistence to Ziggurat.

Upstream emits counter notifications every 100 frames and bounds its notification
queue to 64 events; older events may be dropped. The 1000 margin is a policy,
**not an unlimited crash-safety guarantee**. Long disconnects, notification loss,
storage/commit lag and a crash during a key update can defeat saved-state freshness.
A best-effort freeze on state-write failure cannot be guaranteed if the connection
is already broken. Do not describe simulated recovery as guaranteed no-repair
hardware migration or unconditional counter safety.

Incoming replay counters are captured in table checkpoints and on orderly freeze.
After an unclean stop they can lag live values. Strict replay protection across
power loss is not supplied. There are no per-frame flash writes; security
notifications and table checkpoints cause persistence writes. Device simulations
use synthetic state in `/tmp`, which is RAM-backed and not power-loss durable.

Reference implementations:
- https://github.com/zigpy/zigpy-ziggurat/blob/dev/zigpy_ziggurat/zigbee/application.py
- https://github.com/zigpy/zigpy/blob/dev/zigpy/appdb.py
- https://github.com/zigpy/ziggurat

## Network import

Use the existing migration preparation with fresh, exclusively handed-over state.
Never start actual-network state against a simulator. Actual backup verification
remains stopped-only. The preparation margin of 10000 is separate from the 1000
native restart margin; converting an already prepared state does not advance again.

```sh
migration_python=../ziggurat-compilation/tools/network-migration-venv/bin/python
"$migration_python" scripts/ziggurat_migration.py prepare-import \
  --state PRIVATE_CAPTURED_STATE.json --output PRIVATE_IMPORT_BUNDLE.json
"$migration_python" scripts/ziggurat_migration.py prepare-native \
  --state PRIVATE_IMPORT_BUNDLE.json --output PRIVATE_NATIVE_STATE.json
```

Neither command connects, starts a stack or flashes firmware. Current network
migration/recovery prerequisites remain in [NETWORK_MIGRATION.md](network-migration.md).

## Validation and device boundaries

The following checks passed on 2026-10-03. Run native host variants sequentially;
some existing legacy tests share temporary paths. Python test helpers require
Python 3.8 or later. Real service simulations use private synthetic fixtures and
loopback sockets, never production state or the UART.

| Check | Result |
| --- | --- |
| `make ZIGGURAT=0 host test` | Legacy EZSP unit/integration suite passes |
| `make ZIGGURAT=1 host test` | Legacy suite, COBS/state units and 13 scripted-peer cases pass |
| `make ZIGGURAT=0 mips`; `make ZIGGURAT=1 mips` | Both stock-RSDK builds pass |
| `python3 scripts/test_ziggurat_service.py` | Six actual native/Ziggurat/RCP lifecycle scenarios pass |
| Migration Python tests | 15 cases pass, including native conversion and private-file rules |
| Rust transport/protocol/server tests | 15 transport cases pass; protocol/server have no unit cases |
| Host HDLC / COBS simulations | Eight / seven scenario checks pass |
| Tuya COBS simulation | Eight checks pass, including unchanged production and cleanup |

Scripted peers exercise fragmented/malformed/oversized input, join/interview,
device reports and light sends, coordinator descriptors, admission failures,
control timeout, reconnect, counter/key commits, state corruption, storage failure,
inconsistent snapshots and a crash after saving counters but before Configure.
The peer asserts that Configure never precedes the durable state commit.
Actual-stack scenarios cover cold load, joining, freeze/save, frozen restart,
native SIGKILL with a running stack, and cold recovery after Ziggurat restarts.

Build the patched server in the sibling experiment checkout using its documented
Rust and stock-RSDK prerequisites:

```sh
cd ../ziggurat-compilation
ZIGGURAT_MULTIPAN=1 bash scripts/build-ziggurat.sh
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/host" cargo +nightly-2026-10-02 build \
  --manifest-path worktrees/ziggurat-hdlc/Cargo.toml -p ziggurat-server
python3 scripts/test-cobs-unix.py
cd ../microhad-zigbee-native
python3 scripts/test_ziggurat_service.py
../ziggurat-compilation/tools/network-migration-venv/bin/python \
  scripts/test_network_migration.py
```

Tuya testing uploads **only Ziggurat and a test socket probe**, using a host RCP
and synthetic keys. The full native adapter runs only on the host. Device testing
uses unique paths, a bounded watchdog, a free-memory check and cleanup. `/tmp` is
ramfs: upload plus an 8 MiB reserve did not fit, so the verified run temporarily
staged executables in an isolated `/tuya/ziggurat-simulation-*` flash directory,
with logs/socket in RAM, then removed both. No native state or launcher was changed.

```sh
cd ../ziggurat-compilation
bash scripts/build-hdlc-probe.sh
python3 scripts/test-cobs-unix.py --host 192.168.178.62 \
  --password-file ../ssh_password.txt --upload-storage flash
```

Measured Ziggurat RSS was **1,620 KiB**. `top` samples summed its LinuxThreads
processes and ranged **0–7% CPU** during 200 socket queries. Median/p95 response
times were **5.27/8.23 ms**, including the SSH probe. Host RCP processing median/p95
was **0.050/0.121 ms** across 30 simulator replies. These are synthetic query
measurements; they do not establish real-radio, Thread or Matter capacity.
Production remained online/formed with 23 devices and MD5
`7a508ac880516642572a70e87bdc5014` before and after.

The old device shell has no `rmdir`: an initial run left two empty test directories.
They were removed, cleanup was changed to remove only the owned unique directories,
and the complete rerun passed. No test uploads remain.

## Application mapping and remaining limits

| Native operation | Ziggurat operation / handling |
| --- | --- |
| Coordinator startup | Hello, GetNetworkInfo, Configure/table load when needed, StartNetwork |
| Snapshot | Network info, key/child/address scans and GetInfo; validate complete tables |
| Join window | PermitJoins duration in seconds; close on failure/expiry |
| Unicast/group/broadcast | Protocol-v2 send operations with distinct admission and delivery results |
| Device traffic | Shared native APS/ZDO/ZCL handlers; interview, reporting and controls |
| Joined/left/key/counter events | Update native application state or private security state |
| Orderly shutdown | Freeze, scan stopped state, persist; bounded timeout |

Request IDs and application send cookies are separate. Outstanding sends fail on
disconnect; they are not blindly replayed. The stack owns APS sequence numbers,
routing and encryption. The native endpoint 1 answers Node, Active Endpoint and
Simple Descriptor requests; the legacy Green Power endpoint 242 is not exposed by
this adapter. Fresh-network creation and raw EZSP `/api/network/backup` return
explicit unsupported errors on this backend; use private Ziggurat state and the
migration tools for recovery preparation.

The incoming NWK replay-counter map is absent from the current protocol/state;
incoming APS counters can lag after a crash. Real radio ACKs, sleepy-child behavior,
hardware restoration and recovery from an unresponsive RCP remain unverified.
The Thread endpoint transports Spinel; it does not supply an OTBR or Matter stack.
`deploy.sh` defaults to the EZSP binary. A future Ziggurat deployment would need
explicit `--binary build/mips-ziggurat/microhad-zigbee-native`; no such deployment was
performed or authorized for this test.

## Troubleshooting

- Missing state: explicitly initialize a private prepared state; do not create a
  new live network automatically.
- Missing socket: start the separate simulator/server, check the configured path
  and directory permissions. Native remains offline and retries.
- Protocol mismatch: use the pinned patched server; do not interpret another
  protocol version as v2.
- Identity/counter mismatch: stop the test and inspect private state offline.
  Do not overwrite it with live state from a different coordinator.
- State write failure: fix permissions/free space; the client fails closed and
  requests a best-effort freeze. Hardware rollback still needs a verified export.
- Cold-start RNG stall: preserve entropy checks; do not bypass randomness safety.
