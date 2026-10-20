# MicroHAD and Ziggurat integration

MicroHAD owns durable Zigbee security state and the application/device model.
Ziggurat owns the running Zigbee stack and physical UART. The coordinator thread
owns the native COBS connection and gateway mutations; HTTP uses protected
snapshots and a command queue. Socket failure leaves MicroHAD offline and never
falls back to a second UART owner.

## Build and configure

Use [build setup](building.md) for feature combinations and artifact paths.
The default is `ZIGGURAT=1 MATTER=1`; `ZIGGURAT=1 MATTER=0` omits Matter.
The paired supervisor runs the multipan server with IID 1 and exposes
`/tmp/ziggurat/native.sock` to MicroHAD. `ziggurat_socket` selects that path;
private state lives in `<runtime_dir>/ziggurat-state.json`.

The COBS socket and parent directory must be private (0600/0700). Keep it distinct
from the HDLC Thread socket. [Shared-radio transport](hdlc-multipan-usage.md)
explains IID mapping and reset/channel policy. [Migration](ziggurat-deployment-plan.md)
initializes preserved network state; [deployment](deployment.md) updates applications.

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

## Durability limits

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


The protocol/state does not persist the incoming NWK replay-counter map, and
saved APS receive counters can lag after a crash. Fresh-network creation and
raw EZSP backup are unsupported on this backend. Thread/Matter is supplied by
the separate service, not by the COBS API.

## Verify changes

```sh
make -C microhad host test
make -C microhad mips
make -C microhad test-backends
python3 ziggurat/scripts/test-cobs-unix.py
python3 microhad/scripts/test_ziggurat_service.py
```

Prepare the patched host Ziggurat fixture using its [component README](../ziggurat/README.md).
These checks use synthetic radio traffic. Hardware acceptance must verify actual
reports/controls and preserved identity through restart.

## Troubleshoot

| Symptom | Check |
| --- | --- |
| Missing/corrupt state | Preserve the file; validate a prepared private snapshot rather than forming a replacement network |
| Missing socket | Supervisor/server health, configured path and private permissions |
| Protocol mismatch | Both sides use the pinned patched protocol-v2 implementation |
| Identity/counter mismatch | Inspect privately; never overwrite with another coordinator's state |
| State write failure | Permissions and flash headroom; best-effort freeze alone does not prove rollback safety |
| Cold-start RNG | Startup uses `/dev/urandom`; fresh keys require kernel RNG readiness |

Ziggurat reads `/dev/urandom` directly for startup APS sequence numbers,
address allocation and timing jitter. Fresh trust-center link keys use a
separate OS read after a nonblocking `/dev/random` readiness poll. If entropy
is unavailable, the key request is rejected without changing stored or pending
keys; the device must retry. Keys derived from an inherited TCLK seed do not
need fresh randomness. MicroHAD applies the same readiness gate before creating
new network keys and trust-center secrets, returning a retryable error without
blocking its coordinator. Restoring a network does not regenerate keys.

The paired launcher no longer restores or saves a persistent RNG seed, and
releases no longer include `rng-seed`. Updates leave any previously installed
helper and seed files untouched; they are unused by the new launcher. Security
frame-counter persistence is unchanged. This policy is specific to Zigbee;
the Matter service retains its own randomness handling.
