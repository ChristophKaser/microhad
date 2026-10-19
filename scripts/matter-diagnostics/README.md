# Host OTBR through Ziggurat: build, capture and commission

Run commands from the repository root on a Linux host with Docker, Bluetooth,
`sudo`, `tcpdump`, `btmon`, Python 3 and `python3-cryptography`. The capture
wrapper authenticates sudo before starting and runs CHIP Tool as your user.
It never flashes the gateway, resets the radio or changes IPv6 forwarding.

## Build the custom OTBR image

```sh
docker build --network=host \
  -f scripts/matter-diagnostics/Dockerfile.otbr \
  -t microhad-otbr-debug:local scripts/matter-diagnostics
```

Use `sudo docker` if your account cannot access Docker. The build pins OTBR
`cb4bdae242b22287cded07ae3aa10347c2d16d9b` and OpenThread
`cb18e0f598059d733b3382b81437c4138195d1ea`, including mbedTLS and its nested
framework. The runtime base is pinned by digest in the Dockerfile. Ubuntu
24.04 build packages follow their repository security updates; this is not a
bit-for-bit reproducible compiler image.

`attach-to-running-rcp.patch` adds an explicit `attach-to-running-rcp` radio URL
option that requires `no-reset`. Stock `no-reset` still waits for a fresh RCP
boot notification, which an already-running Ziggurat endpoint does not send.
The patch skips that wait while retaining version/capability checks. It does
not weaken Ziggurat's radio-control restrictions.

The image includes built-in OpenThread mDNS, SRP advertising and DNS-SD discovery
proxies, and border routing. REST, D-Bus, web, TREL and firewall manipulation
are disabled. It uses `OT_LOG_LEVEL=DEBG`, `OT_FULL_LOGS=ON`, dynamic logging
and agent `-d7`; runtime `ot-ctl log level` must return `5`.

### Start it with existing OTBR settings

Only one consumer can connect to Ziggurat's Thread endpoint. If replacing the
existing diagnostic container, first record and reuse its settings directory:

```sh
OTBR_STATE=$(docker inspect microhad-otbr-attach --format \
  '{{range .Mounts}}{{if eq .Destination "/var/lib/thread"}}{{.Source}}{{end}}{{end}}')
test -n "$OTBR_STATE"
docker stop microhad-otbr-attach
docker rename microhad-otbr-attach "microhad-otbr-previous-$(date -u +%Y%m%dT%H%M%SZ)"
```

For a first installation, choose a new private directory instead:
`OTBR_STATE="$PWD/build/matter-diagnostics/otbr-state"; install -d -m 700 "$OTBR_STATE"`.
Preserve existing settings on subsequent runs, including counters and identity.

The following command reads gateway/interface settings from the private host
[workspace configuration](../../docs/workspace-settings.md). The container
requires these two values explicitly; it never reads your SSH credentials.

```sh
. ./scripts/workspace-env.sh
workspace_load "$PWD"
: "${GATEWAY_IP:?Set GATEWAY_IP}" "${LAN_INTERFACE:?Set LAN_INTERFACE}"
docker run -d --name microhad-otbr-attach --restart=no \
  --network=host --read-only --cap-add=NET_ADMIN --cap-add=NET_RAW \
  --device=/dev/net/tun \
  --tmpfs /tmp:rw,nosuid,nodev --tmpfs /run:rw,nosuid,nodev \
  --mount "type=bind,src=$OTBR_STATE,dst=/var/lib/thread" \
  -e "GATEWAY=$GATEWAY_IP" -e THREAD_PORT=9998 \
  -e "BACKBONE_INTERFACE=$LAN_INTERFACE" -e THREAD_INTERFACE=wpan-otbr-test \
  microhad-otbr-debug:local
```

The gateway must already be in temporary Thread TCP mode, with local
`mthread-matter` stopped. Its launcher command is
`/tuya/native/paired_control.sh thread-tcp 192.168.178.62:9998`.
See the [diagnostic plan](../../docs/matter-otbr-commissioning-diagnostic-plan.md)
for ownership and Zigbee checks. A gateway reboot restores local Unix sockets.

Auto-attach is disabled. For **new settings only**, import the validated hex-only
Operational Dataset, whose channel must match Zigbee:

```sh
docker exec microhad-otbr-attach /opt/ot-ctl -I wpan-otbr-test \
  dataset set active "$(cat "$INTENDED_DATASET_HEX_FILE")"
```

Then enable Thread and wait for `leader`, `router` or `child`:

```sh
docker exec microhad-otbr-attach /opt/ot-ctl -I wpan-otbr-test ifconfig up
docker exec microhad-otbr-attach /opt/ot-ctl -I wpan-otbr-test thread start
docker exec microhad-otbr-attach /opt/ot-ctl -I wpan-otbr-test state
```

For a capture attempt, the wrapper subsequently restarts this container **after
capture has started**, preserving its existing dataset and settings. It waits
for attachment, SRP, border routing and validated Spinel traffic before pairing.
Host-local CHIP Tool uses the direct Thread TUN route and does not require
kernel IPv6 forwarding. Controllers on other LAN hosts require forwarding,
firewall permission and return routes as a separate setup step. No mDNS
reflector is required on this single backbone link.

## Build the custom CHIP Tool and fixture

The installed strict snap cannot write directly to this repository's trace
directory and has no guaranteed structured session-key exporter. Use the pinned
native host build below. On a fresh host, install the SDK's Linux build
prerequisites (Clang, Ninja, pkg-config, Python development/venv packages,
OpenSSL, GLib, D-Bus and Avahi development packages), then prepare its checkout:

```sh
git clone https://github.com/project-chip/connectedhomeip.git build/connectedhomeip/source
git -C build/connectedhomeip/source checkout 43c0a7d29f8d251d3eef0a7333d3b68e421eed81
(
  cd build/connectedhomeip/source
  python3 scripts/checkout_submodules.py --shallow --platform linux
  source scripts/bootstrap.sh
)
WITH_FIXTURE=1 bash scripts/matter-diagnostics/build-controller.sh
```

Skip checkout/bootstrap if the pinned checkout and `.environment` already exist.
The helper verifies the revision, applies the two tracked patches without
discarding other SDK changes, installs the exporter header and builds with the
existing Pigweed Python/ZAP tools. ZAP may write its normal `~/.zap` cache.
`CHIP_SOURCE_DIR`, `CHIP_BUILD_DIR` and `CHIP_BUILD_JOBS` override source,
Ninja output and parallelism; defaults are in the helper. It writes:

```text
build/matter-diagnostics/chip-tool
build/matter-diagnostics/chip-all-clusters-app
build/matter-diagnostics/controller-build.json
```

The original generated `DataModelLogger.cpp` has 29,549 lines and an 11,829-line
attribute dispatcher. `split-logger.py` retains all 531 original functions and
158 attribute clusters while distributing them across 18 translation units,
at most 1,822 lines each. It moves each attribute case into a class member,
retains unknown-attribute behavior, and leaves the pinned generated input intact.
It refuses an unexpected input shape rather than silently dropping cases.

### Rehearse key export before pairing the accessory

```sh
docker build --network=host -f scripts/matter-diagnostics/Dockerfile.fixture \
  -t microhad-matter-fixture:diagnostic scripts/matter-diagnostics
python3 scripts/matter-diagnostics/rehearse-controller.py
```

The fixture uses a network-none Docker container with a private dummy IPv6
interface and no host D-Bus/Bluetooth access. NET_ADMIN creates only that
container's interface; commissioning processes then drop to your UID/GID.
It commissions a disposable Linux accessory, performs operational reads,
exercises CASE resumption and authenticates saved AES-CCM messages in both
directions. This rehearses the real PASE/CASE implementations over IP; the real
accessory attempt additionally captures BLE HCI and the same Matter crypto
boundary. It does not access the gateway or real accessory.

Success writes `build/matter-diagnostics/rehearsal.json`, tied to the controller
binary hash, and retains its fixture bundle beside it. Rebuilding invalidates
that certificate; repeat the rehearsal before running the wrapper.

## Capture and commission

Place the accessory in commissioning mode, then run:

```sh
./scripts/commission-host-otbr-via-ziggurat.sh --payload 'MT:YOUR_SETUP_PAYLOAD'
```

Supply the QR payload given for the accessory; it is intentionally not committed
in the script. Gateway logging polls the gateway's HTTP `/api/logs` endpoint;
no SSH credentials or additional gateway executable are required. Failed gateway
requests are recorded in `logs/gateway.log` and retried. Packet, Bluetooth or
OTBR collector failures stop the attempt.

The terminal shows timestamped phases, periodic OTBR readiness updates, and the
full live CHIP Tool stdout/stderr. CHIP output is also saved unchanged under
`logs/commission.log` or `logs/read.log`; phase messages are in `logs/runner.log`.

Defaults: container `microhad-otbr-attach`, gateway `GATEWAY_IP:9998`, backbone
`LAN_INTERFACE` from the shared host settings, Thread interface `wpan-otbr-test`, new controller node ID `1`, 300-second
commissioning limit and 10-second observation tail. `--help` lists overrides.
`--dataset-file` additionally compares the active dataset against an intended
hex file. The channel comes from that dataset; no channel override is sent.
Production PAA roots from the pinned SDK are used; `--paa-trust-store` overrides
that directory. Attestation verification is never bypassed.

Collectors start before OTBR restarts. The wrapper saves all host-observed
packets on `any` without a BPF filter, full-length and packet-buffered, plus
Bluetooth HCI, OTBR DEBUG logs and available gateway syslog. It validates fresh
TCP handshakes, HDLC CRCs, IID 0, bidirectional raw Spinel frames, no reset
commands and no TCP gaps before commissioning. These are host observations,
not an independent RF sniffer or a physical multipan UART capture.

Pairing uses `pairing code-thread` with the active dataset, followed on success
by an endpoint-0 Basic Information vendor-name read using the same controller.
Each run creates a fresh controller fabric; do not rerun against an already
commissioned device without opening an appropriate commissioning window.
The script never factory-resets or unpairs the accessory automatically.

On success, failure, timeout, Ctrl-C or SIGTERM, the script flushes and stops only
its collectors and writes a result. Privileged collectors have control-pipe
supervisors, so cleanup does not depend on a still-valid sudo ticket. OTBR stays
running in TCP mode; local gateway Matter stays stopped. Return to local Matter
explicitly using the diagnostic plan's finish procedure when the experiment ends.
Sudo launchers retain the invoking terminal session so they can use the initial
authentication; only the collectors inside their supervisors get new process groups.

## Trace bundle and decoding

All attempt output goes under
`./matter-traces/host-otbr-via-ziggurat/<UTC-timestamp>/` by default:

| Location | Contents |
| --- | --- |
| `capture/host.pcap` | Host Spinel TCP, Thread IPv6, Wi-Fi mDNS/Matter, ICMPv6 and other host packets. |
| `capture/ble.btsnoop` | Bluetooth HCI traffic before connection through teardown. |
| `capture/matter-messages.jsonl` | Encrypted controller traffic, actual nonce/AAD/tag and outbound plaintext. |
| `capture/matter-decrypted.jsonl` | Offline authenticated plaintext reconstructed from exported keys. |
| `keys/` | Every derived PASE/CASE/resumed session's traffic keys and attestation challenge; active Thread datasets/key-sequence samples. |
| `logs/` | Collector diagnostics/drop counts, OTBR logs, CHIP Tool logs/decoded traces/events and gateway logs. |
| `state/chip-tool/` | Persistent controller fabric and resumption state for later commands. |
| `metadata/` | Build/rehearsal identity, commands, Docker config, addresses/routes, state snapshots, validation and checksums. |
| `result.json` | Separate commissioning/read exits, key verification, collection errors/warnings and overall success. |

The directory is private and `/matter-traces/` is ignored by Git. Setup payload,
dataset and session keys are retained in the bundle as requested.

Key/message export is opt-in through `MATTER_DIAGNOSTIC_DIR` and
`MATTER_DIAGNOSTIC_BUILD`; no export happens without the former. The key file is
`keys/matter-session-keys.jsonl`, schema 1. Correlate records by `(pid, context)`;
the context counter is never reused within a process. `keys` records are flushed
with `fsync` immediately after successful derivation; subsequent `session`
records add actual session IDs, type, node IDs and peer address. Unavailable
pre-activation fields are null. `tx_key` and `rx_key` are from this controller's
perspective; role identifies the initiator/responder mapping. Resumption keys
are exported at every fresh derivation, not copied from an earlier session.
Export failure aborts the diagnostic controller instead of silently losing keys.

Message records contain exact `nonce`, `aad`, `ciphertext` and `tag` hex. Nonce
bytes encode security flags, little-endian message counter and actual nonce node
ID; this retains PASE's zero-node-ID behavior. RX is saved before in-place
decryption, including unauthenticated incoming packets; TX is saved after
encryption together with its original plaintext. Keys and messages use the
same attempt/build identity. AES-128-CCM tags are authenticated offline:

```sh
python3 scripts/matter-diagnostics/verify-export.py "$SESSION"
```

The wrapper invokes this automatically. Overall success requires commissioning,
the CASE read, PASE/CASE decryption in both directions, valid Spinel capture and
zero reported capture drops. Invalid received authentication tags remain in
the capture and are reported, not hidden. Failed attempts retain partial keys
and messages even when complete verification is impossible.

For later device commands, use the same instrumented controller and
`--storage-directory "$SESSION/state/chip-tool"`. To export those new sessions,
also set `MATTER_DIAGNOSTIC_DIR="$SESSION"` and `MATTER_DIAGNOSTIC_BUILD` to the
saved binary hash and start fresh collectors; a one-off command does not extend
the stopped packet capture automatically.

## Extract Spinel and raw Thread radio frames offline

These tools use only the Python 3.11+ standard library; neither Docker, sudo nor
session keys are needed. They read a stopped capture without contacting the gateway:

```sh
SESSION="$PWD/matter-traces/host-otbr-via-ziggurat/20261016T073813.011529Z"
python3 scripts/matter-diagnostics/extract-spinel.py "$SESSION/capture/host.pcap"
python3 scripts/matter-diagnostics/extract-thread.py "$SESSION/capture/spinel.trace"
```

The first tool selects TCP traffic to/from `192.168.178.62:9998`; override with
`--gateway ADDRESS --port PORT`. Both accept `--output PATH` and refuse to
overwrite an existing trace or summary. Outputs have private permissions.
Each prints counts and writes a separate `*.trace.summary.json` containing
source/output SHA-256 hashes. The original capture and bundle manifest stay unchanged.

Both `.trace` files contain schema-1 JSON Lines with one-based `record` IDs:

| Output | Records and fields |
| --- | --- |
| `capture/spinel.trace` | `spinel` records retain exact `hdlc_hex` (delimiters/escaped FCS included), unescaped `spinel_hex` (FCS removed), command/property/payload, IID and TID. `diagnostic` records preserve malformed/boundary bytes and report CRC failures, TCP gaps and conflicting overlaps. Header parse failures keep the original frame and add `parse_error`. |
| `capture/thread.trace` | `thread` records have `kind: tx`, `rx` or `ack`, complete `psdu_hex` and available `radio` metadata. `tx_done` records correlate completions to `tx_spinel_record`; they do not duplicate the TX frame. A structurally complete ACK-bearing status can be extracted even without its request; its reference is null. `unmatched_status` retains other LAST_STATUS responses without claiming they are transmissions. Metadata parse failures add `diagnostic` records and retain any already-extracted PSDU. |

Frame records include connection identity, host-relative direction (`tx` toward
the gateway), contributing one-based PCAP `packets`, and Unix nanosecond
timestamps. `timestamp_ns` is the latest contributing captured segment time;
`first_timestamp_ns` is the earliest. These are host capture times, not RF
transmission times. Thread records refer back through `spinel_record`;
receive metadata also preserves the RCP's separate `radio_timestamp_us`.

TCP reconstruction handles segmentation, out-of-order delivery, retransmission,
overlap and sequence wrap within a stream span shorter than 2 GiB. Connections
already active at capture start are included (`syn_seen: false`). A new client
SYN with a different sequence starts another connection epoch. Conflicting
overlaps retain lower-sequence bytes, then the first captured segment for ties,
and report the alternative bytes. Gaps break framing; extraction resumes at
the next HDLC boundary. Packet references describe bytes used in reconstruction,
not redundant retransmissions. Classic PCAP in either byte order and either
timestamp resolution is supported with Linux cooked v1/v2, Ethernet (including
VLAN) and raw IP link types. IPv4/IPv6 TCP is supported; fragmented IP and PCAPNG
input are not reassembled/supported. Target TCP payloads are buffered in memory.

Radio layouts follow the pinned OpenThread `radio_spinel.cpp` implementation:
STREAM_RAW TX/RX and ACKs inside successful LAST_STATUS TX completions. RX
retains RSSI, noise, channel, LQI, flags, receive error and optional MAC security
metadata; TX retains scheduling, retries, channel and security-processing flags.
Unknown trailing bytes stay in `extra_hex` (including completion security
updates); nested unknown metadata stays in the radio fields. No frame-type or
Thread classification filter is applied: ACKs and other radio observations
remain available. PSDU bytes, including any FCS bytes/placeholders, are preserved
exactly as transported. TX requests can precede radio security processing and
do not prove an over-the-air transmission. This is not an independent RF capture.
Protocol decoding, decryption and Wireshark/PCAPNG export are deferred.

On session `20261016T073813.011529Z`, extraction found 262 valid Spinel frames:
258 from the fresh connection and four from the earlier connection. The radio
trace contains 177 frames: 57 TX, 88 RX and 32 embedded ACKs, plus 57 successful
TX completion records and seven unmatched ordinary LAST_STATUS responses.
There were no extraction diagnostics or malformed Spinel headers; every TX
request had a completion and all RX receive-error values were zero.

## Host-only checks

```sh
python3 scripts/matter-diagnostics/test-diagnostics.py
python3 scripts/matter-diagnostics/test-trace-extract.py
bash -n scripts/commission-host-otbr-via-ziggurat.sh scripts/matter-diagnostics/*.sh
```

These tests cover capture reconstruction/retransmissions, gap/reset rejection,
authenticated decryption/tampering, success/failure/timeout/interruption bundles
and cleanup that leaves unrelated processes running. The fixture rehearsal is
the separate real-stack acceptance check; mock tests do not certify key export.

The OTBR image also has a synthetic startup check that does not access the radio:

```sh
python3 scripts/matter-diagnostics/test-otbr-init.py --image microhad-otbr-debug:local
```

Verified on 2026-10-16: the custom OTBR image built and passed running-RCP
initialization without a boot notification or reset; the split CHIP Tool and
Linux accessory built; the isolated fixture authenticated 22 PASE TX/11 RX,
6 CASE TX/3 RX and 2 resumed CASE TX/1 RX messages, with no authentication
failures. The saved live gateway attach capture also passed all 214 HDLC/Spinel frames.
The later real commissioning session `20261016T073813.011529Z` completed BLE/PASE,
Thread attachment, operational CASE commissioning and a separate vendor-name
read (`IKEA of Sweden`). All 34 exported encrypted messages across one PASE
and two CASE sessions authenticated offline; that run did not use CASE
resumption.
