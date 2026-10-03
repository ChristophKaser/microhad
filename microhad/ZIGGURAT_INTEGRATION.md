# Native service ↔ Ziggurat integration

## Current state and plan — 2026-10-03

**Implementation in progress.** EZSP remains the default build and the gateway's
production service/radio remain untouched. A bounded C++03 COBS client and private
native state store are implemented; service wiring and integration validation are
pending. Ziggurat has an experimental COBS Unix listener. Completed results will
be recorded here before delivery.

The goal is a standalone Tuya gateway: native device/application handling talks
to Ziggurat over a Unix stream socket; Ziggurat speaks HDLC/Spinel to the radio.
The later Thread consumer uses a separate HDLC endpoint, not this Zigbee API.
This work does not implement Thread, OTBR or Matter.

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
The native project requires its stock RSDK and C++03. Do not deploy these native
binaries during this task.

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
output to 65536 bytes. Malformed or oversized input is discarded to the next zero.
Control/admission requests time out in ten seconds; admitted send confirmations
have a separate thirty-second deadline. The socket reconnect interval is one
second. Snapshot refresh is every sixty seconds and after table changes.

## Startup and state recovery

The private state format `microhad-ziggurat-native`, version 1, stores the protocol's
82-byte network state and counted key, child and address tables as lowercase hex.
It preserves the coordinator IEEE, PAN/channel, extended PAN, network key and
sequence, trust-center link key and hashed derivation seed/flavor, transmit power,
NWK/APS outgoing counters, and per-device APS receive counters. Receive counters
use Ziggurat's next-acceptable convention. It is separate from application device
configuration and is never committed.

State reads require a bounded regular file owned by the service user with no group
or other permissions. Writes use a private temporary file, fsync, rename and
parent-directory fsync. A lock file prevents multiple service processes from
writing the same network state. A missing/corrupt file blocks startup; nothing
silently forms a replacement network.

On connection, validate Hello version 2 and query GetNetworkInfo. If unconfigured,
advance both outgoing counters by 1000 (APS at least above all saved key transmit
counters), check overflow, and save that state **before** Configure/LoadKeyTable/
LoadChildren/LoadAddressCache/StartNetwork. A reconnect to a running stack checks
identity and counter consistency and reads the live tables without reconfiguration.
A stopped/frozen stack is loaded afresh with the same identity and advanced counters.

Counter notifications monotonically update durable state; link-key notifications
replace the corresponding device key and trigger table refresh. Successful scan
responses complete snapshots; partial scans are not published. A key update racing
a snapshot causes a new scan so an older row cannot overwrite a newly saved key.
Orderly service stop uses the existing FreezeNetwork extension, scans stopped
state and saves it before disconnecting. Failure to freeze is not proof of a safe
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
# Extract bundle.state to a new private file (0600); then:
"$migration_python" scripts/ziggurat_migration.py prepare-native \
  --state PRIVATE_PREPARED_STATE.json --output PRIVATE_NATIVE_STATE.json
```

Neither command connects, starts a stack or flashes firmware. Current network
migration/recovery prerequisites remain in [NETWORK_MIGRATION.md](NETWORK_MIGRATION.md).

## Validation and device boundaries

Host tests must retain existing EZSP coverage and add native-client scripted peers
and actual-Ziggurat/host-RCP simulation. Test notification persistence, cold starts,
reconnects, partial frames, malformed input, send outcomes and filesystem failure.

Tuya testing uploads **only Ziggurat and a test socket probe**, uses a host RCP
simulator over TCP, and synthetic network keys. Check MemFree against upload sizes
plus 8 MiB because `/tmp` is ramfs. Use unique paths, a bounded watchdog and cleanup.
Record `top` CPU, RSS and host-observed response timing (including the SSH bridge).
The probe transfers COBS bytes without implementing Zigbee service behavior; full
native adapter tests run on the host. Check production status/checksum before and
after. No device state files, launcher changes, service restart or physical UART.

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
