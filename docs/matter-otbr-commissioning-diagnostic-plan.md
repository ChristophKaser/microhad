# Matter commissioning diagnostic with host OTBR

## Goal and scope

This runbook tests the accessory with the repository's host-based OpenThread
Border Router (OTBR) setup, using the factory-kernel gateway only as a remote
radio through `/tuya/serialgateway` on TCP port 8888. It stops
the current paired MicroHAD/Ziggurat/Matter stack, temporarily flashes the
standard OT-RCP firmware, runs OTBR and a Matter controller on the host, records
the RCP traffic and Thread credentials, commissions the accessory, then restores
the exact Ziggurat radio firmware and paired services.

This is a controlled radio-firmware cutover, not a side-by-side test. The
standard OT-RCP image replaces the current multipan RCP while OTBR runs. The
current gateway's Ziggurat and Matter network state must remain untouched and
the exact Ziggurat-compatible RCP image must be available for return.

The test can capture all Spinel/HDLC traffic exchanged between OTBR and the
EFR32, including raw 802.15.4 frames exposed by the RCP, and preserve the Thread
dataset needed to decrypt Thread MAC traffic. With no independent 802.15.4
sniffer, this is not proof of every frame on the air: hardware-generated ACKs,
frames filtered inside the radio, and traffic not reported by the RCP are not
recoverable from the host TCP stream. Matter PASE/CASE application encryption
is a separate layer; the Thread Network Key does not decrypt Matter payloads.
This diagnostic requires **both saved Matter session keys and decoded controller
traces**. Export every PASE and CASE session's derived traffic keys before its
first encrypted message, including resumed and replacement sessions. Persist
them in the host bundle before session teardown; controller storage
and decoded logs alone do not satisfy this requirement. Offline decryption
must be demonstrated with the saved keys and matching captured messages.

The gateway retains its factory kernel. It needs neither IPv6/TUN nor an
in-kernel UART bridge: OTBR and its Linux networking run on the host. The
upstream compose file describes kernel 6.18, and its `flash_efr32.sh` requires
that kernel's bridge sysfs API. Those gateway instructions do not apply here.
Do not upgrade the kernel, flash userdata, edit `/userdata/etc/radio.conf`, or
reboot to enable a bridge for this experiment. See
[factory Linux constraints](openthread-feasibility.md) and the existing
[serialgateway flashing procedure](installation.md#b2-flash-the-application-only-rcp).

## Readiness and comparison boundary

This document is a preparation plan until the selected images, return reset
mechanism, dataset import and instrumented controller have passed preflight.
The repository does not currently supply the IID0 return tool or a demonstrated
host-controller key-export/decoder pipeline described below. Do not treat the
example commands as a ready-to-run automation script. Reviewing this plan does
not authorize a device cutover.

Record the original gateway commissioning failure and last successful stage
before changing the stack. Keep the accessory firmware, placement, power,
Thread dataset/channel and pairing method comparable where possible. OTBR plus
a different controller changes both Thread and Matter implementations; success
isolates the problem to that combined difference, not to one daemon by itself.

## Fixed inputs and permanent capture directory

Run from the repository root. Keep each attempt permanently under
`diagnostics/matter-otbr/`, independently of build outputs and cleanup. Use a
new session directory for each attempt and retain failed attempts too.
`diagnostics/` is already ignored by Git; keep captured device state and keys
out of commits as required by the repository instructions.

```sh
SESSION="$PWD/diagnostics/matter-otbr/session-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$SESSION"/{capture,keys,state,firmware,logs,metadata}
chmod 700 "$SESSION/state"
```

This is a debug setup: keys, setup codes, decoded payloads and device identifiers
may appear directly in the diagnostic files and logs. Preserve complete keys
in the bundle so analysis does not depend on log retention or a running process.
`ziggurat_freeze.py` requires its output parent directory to have no group/other
permissions, hence the explicit mode 0700 on `state` above. Its snapshot is
written mode 0600; do not relax those tool-enforced permissions.

Record the gateway address, host interface used to reach it, accessory QR/manual
pairing data, software versions and UTC start time in session metadata. Set
`GATEWAY_IP`, `PASSWORD_FILE`, `MIGRATION_PYTHON`, `PRIVATE_TEMPLATE`,
`OT_RCP_IMAGE`, `OT_RCP_SHA256`, `RETURN_RCP_IMAGE`, `RETURN_RCP_SHA256` and
`COMPOSE_PROJECT`, `COMPOSE_FILE` and `INTENDED_DATASET_HEX_FILE` explicitly. Image paths must refer to the retained session
copies. Use one authenticated `gateway_ssh` wrapper with the established SSH
options and password file for all gateway commands; plain `ssh` below is
shorthand. Export `SESSION` in each capture terminal. Disable shell tracing
when expanding credentials, and check host disk capacity for the capture window.

Before beginning, identify and record:

1. The **actual installed** EFR32 firmware version, baud and flow control from
   the paired launcher configuration and current service logs. Do not assume
   the local `ot-rcp-multipan.gbl` is installed. Local build manifests such as
   `docs/hdlc-build-manifest.json` describe artifacts, not live device state.
2. The exact application-only `.gbl` image compatible with the running Ziggurat
   build. Record its SHA-256 and retain it in the session workspace. The return
   image must match the running Ziggurat's expected multipan protocol and
   460800/RTS-CTS settings. Do not substitute a newly built or merely similarly
   named image without checking compatibility.
3. An explicitly selected application-only Lidl OT-RCP `.gbl`, its SHA-256,
   board compatibility, UART baud/flow control and Spinel compatibility with
   the pinned host OTBR build. The compose file does not select or flash an
   image. Resolve firmware under
   `firmware/dependencies/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/`;
   use 460800/RTS-CTS for the selected image in this plan. Validate both this
   image and the return image with `microhad/scripts/ezsp_preflight.py image`
   before stopping anything. Application-range validation does not establish
   NVM schema compatibility; establish that separately for the round trip.
   Also prepare and validate the return bootloader-entry tool: the current
   `firmware/tools/gateway-maintenance.c` hardcodes multipan IID1 (`0x90`),
   whereas the standard single-Thread RCP uses IID0. It cannot be assumed to
   reset that image. A bounded host Spinel/HDLC tool must send RESET reason 3
   to IID0 over the exclusive 460800/RTS-CTS serialgateway connection, then
   disconnect so the bridge can switch to 115200/no-flow for menu validation.
   Record that tool's path, source revision, invocation and successful rehearsal
   against the selected OT-RCP on a spare compatible radio before cutover.
   Do not flash the production radio merely to satisfy this prerequisite.
   If no rehearsal hardware or established recovery path exists, record this
   as an unresolved cutover blocker. This tool is a preparation prerequisite;
   the existing IID1 maintenance command is not its replacement.
4. The current Thread Operational Dataset and complete Matter/OpenThread
   persistent state. Store the dataset as a file. The gateway API returns
   redacted status, not an `ot-ctl` dataset export: locate the original import
   TLVs or validate a read-only export from the gateway's persisted dataset
   record (`thread_lifecycle.rs`, store key `0x1001`). Do not pass the wrapped
   store record to OTBR as dataset TLVs. If there is no configured production
   Thread network, record that fact and select a separate test dataset.
   Decide whether OTBR should attach to the existing Thread network or form a separate test
   network. Prefer the existing dataset for this comparison; never form a new
   network over the existing device without intentionally changing the test.
5. A pinned host controller build with working PASE/CASE key export.
   Record the source revision, instrumentation patch, build identity, export
   format and offline decoder version. Stock decoded tracing or debug logging
   must not be assumed to export keys. Complete the key-export rehearsal below
   before the radio cutover. A controller without demonstrated export is not
   eligible for this test.
6. The matching normalized migration recovery template and retained recovery
   artifacts. Validate the template offline with `ziggurat_freeze.py --template
   "$PRIVATE_TEMPLATE"` before shutdown. Phase 1 creates the fresh frozen
   snapshot; an additional earlier freeze is unnecessary and an old snapshot
   cannot replace it. The live `ziggurat-state.json` alone is not normalized
   post-freeze evidence. Do not improvise a template from that file.
7. The accessory's initial fabric/network state and return procedure. Use an
   unpaired test accessory for a BLE-to-Thread commissioning reproduction.
   An already commissioned accessory needs its existing administrator to open
   a commissioning window; adding a fabric on its current network exercises
   a different path and may skip Thread provisioning. Do not factory-reset an
   existing production accessory implicitly. Record any intended reset, the
   expected fail-safe rollback after failure, and how test-fabric removal or
   recommissioning will restore the accessory after the test.

Complete the compose/controller/capture preparation in Phase 3 steps 1–2,
including image pulls, builds, BLE permissions and logger/key-export rehearsal,
before Phase 1 stops the gateway. No dependency installation or first-time
controller instrumentation should be left for the radio downtime.

Retain the selected firmware images in `$SESSION/firmware` and record filenames,
sizes and SHA-256 hashes in the session manifest. Save the full datasets and
keys alongside their fingerprints for repeatable analysis.

## Capture design

Start capture **before** OTBR opens the gateway bridge and keep it running until
after commissioning and a short post-commissioning observation window. Do not
connect a second TCP client to port 8888: the gateway bridge allows one client,
and a second client can displace OTBR.

Use one host BLE commissioner (preferably the instrumented CHIP Tool) and
OTBR alone for the default reproduction. Home Assistant, a UI and a second
Matter server add no evidence to this path; start them only when reproducing
that specific integration. A second transport collector is also unnecessary
when controller transport instrumentation already saves complete encrypted
messages in both directions; HCI capture is useful for diagnosing BLE itself.

Capture these independent views:

| Artifact | What it proves / contains |
| --- | --- |
| Host-side PCAP of TCP port 8888 | Exact TCP byte stream to/from the gateway bridge. Contains HDLC/Spinel frames and may contain raw 802.15.4 frames. It is not an RF PCAP. |
| Timestamped OTBR debug/trace logs | RCP resets, Spinel protocol events, raw stream TX/RX records if supported by the selected OTBR build, Thread state changes, SRP/mDNS and border-routing events. |
| Matter session-key export | All PASE/CASE directional traffic keys, session identifiers and nonce context, flushed before teardown; required for offline authenticated decryption. |
| Host Matter IP capture and BLE HCI/BTP or controller transport capture | Encrypted CASE/IP and PASE/BLE messages for correlation with saved keys; UART capture alone does not include BLE. |
| Matter controller logs | BLE/PASE commissioning steps, Thread credentials delivery, attestation, CSR/NOC, CASE, operational discovery and CommissioningComplete result. |
| Thread dataset before and after | Network Key and the rest of the Operational Dataset, plus separately recorded runtime key sequence needed to interpret/decrypt Thread frames, including any newly generated/rotated dataset. |
| Host and gateway timeline | UTC timestamps, service stop/start, firmware image/hash, bridge connection, OTBR state, commissioning start/finish and failures. |

For encrypted layers, retain these key sources with the capture bundle:

- **Thread 802.15.4/MAC:** the Thread Network Key and key sequence from each
  active dataset used during the capture. These let an offline analyzer derive
  the Thread MAC/MLE keys and decrypt the Thread frames, subject to a compatible
  decoder and complete counters/headers.
- **Thread commissioning:** preserve the full Operational Dataset, including
  PSKc and timestamps, because it is the record of the network credentials that
  OTBR and the accessory were meant to use. PSKc is not a substitute for the
  Network Key when decrypting ordinary traffic.
- **Matter payloads:** preserve verbose controller logs from before commissioning
  through the observation window. Prefer CHIP Tool's decoded trace mode
  (`--trace_file PATH --trace_decode 1`, if supported by the installed build)
  or Matter.js Server debug logging. These traces are produced inside the live
  Matter stack, where received secure messages have already been decrypted;
  they can show commissioning exchanges, interaction model operations and
  decoded payload/TLV bytes. Validate the selected logger before the
  real attempt by confirming that a harmless Matter read produces decoded
  output. A raw TLV dump may still need a Matter TLV decoder to render fields
  and values in a human-readable form.
- **Matter fabric:** preserve a copy of the test controller database
  and its fabric credentials. This protects the test fabric and allows the
  controller to resume, but does not automatically reveal ephemeral PASE/CASE
  session keys. Save those separately for every session as required below.

### Mandatory Matter session-key export

Prepare the controller instrumentation before touching the gateway. Hook the
selected stack where PASE/CASE derives or installs a secure session, while the
derived material is accessible. Export both initiator-to-responder and
responder-to-initiator AES traffic keys, the attestation challenge key where
available, and the mapping from each key to its direction and session. Capture
fresh keys for CASE resumption and later sessions too; a resumption secret or
fabric private key alone does not replace the active traffic keys.

Write an append-only file at
`$SESSION/keys/matter-session-keys.jsonl`. Each record must identify the
attempt, UTC time, controller build, PASE/CASE type, initiator/responder roles,
local/peer session IDs, peer address and known node IDs, key names/bytes and
algorithm. Retain the packet security flags, message counters and source-node
ID/nonce context used by that stack, including its PASE-specific rules, in the
capture or associated metadata. Record unavailable fields explicitly;
do not guess IDs from another session. Define the exact record format with the
chosen offline decoder. Flush records as keys are installed, before messages
can be lost to a crash, and preserve the file before controller shutdown.

Use the pinned controller and a disposable host fixture/accessory to rehearse
PASE, CASE and CASE resumption if enabled. Reconstruct at least one encrypted
message in each direction for PASE and CASE from saved transport bytes and
exported keys, validate the authentication tag, and compare the plaintext to
the decoded trace. BLE PASE requires saved Matter-over-BLE message bytes: use a
host BLE HCI collector (for example `btmon`) plus BTP reassembly, or instrument
the controller's transport boundary to save complete encrypted Matter messages
in both directions. Capture operational Matter IPv6/UDP separately on the host
Thread interface. If a phone performs PASE, its keys/messages need their own
export; prefer a host BLE commissioner with controlled instrumentation.

Decoded tracing remains useful for correlation, but neither `--trace_decode`
nor `LOG_LEVEL=debug` proves key export. If the rehearsal fails, stop preparation
and implement/fix export before the real attempt. Key bytes may also appear in
debug logs, but save the structured export under `$SESSION/keys` for reliable
offline decoding. Keep gateway transient output in `/tmp` and retrieve it to
the permanent host session directory; do not write capture/key streams to
gateway flash.

Example host capture (replace the interface/filter with the actual route; run
in a dedicated terminal and stop it with Ctrl-C after the observation window):

```sh
sudo tcpdump -i any -s 0 -U \
  -w "$SESSION/capture/otbr-uart-spinel.pcap" \
  "host $GATEWAY_IP and tcp port 8888"
```

For host Matter IP capture, cover the OTBR Thread interface and backbone,
including UDP/5540, mDNS/5353 and ICMPv6; avoid a filter that loses discovery,
fragments or nondefault Matter ports. For example, capture IPv6 on `any` and
post-filter offline. Start `btmon -w "$SESSION/capture/ble.btsnoop"` before the
BLE connection if using HCI/BTP reconstruction. Save collector stderr/drop
counts and confirm the actual interfaces exist as OTBR creates them.

The capture must be started before `docker compose up`. Validate it after the
first OTBR startup: it must contain the full TCP handshake and sustained
bidirectional payload, and the OTBR logs or an offline HDLC/Spinel decoder must
show RCP initialization and `STREAM_RAW` transmit/receive frames. If the build
only logs parsed Spinel commands and the PCAP cannot be decoded to the raw
stream frames, stop before commissioning and improve the capture instrumentation.
Do not claim a complete frame trace based on ordinary OTBR debug logs alone.
Reassemble TCP separately by connection and direction, remove retransmissions,
then unescape HDLC and validate its FCS before decoding Spinel. Preserve RX
metadata, TX requests and matching TX completion/status records. TX submissions
may precede RCP security/header processing (`mIsSecurityProcessed` and
`mIsHeaderUpdated`); they are not necessarily the encrypted on-air PSDU. Record
RCP capabilities and returned key-index/frame-counter updates. Demonstrate
which RX/TX bytes can actually be decrypted; use post-security radio tracing
or an independent sniffer if the question requires final on-air TX bytes.

## Phase 1 — baseline and orderly service shutdown

1. Save a copy of the gateway's radio configuration, paired launcher
   metadata and service status. Record the currently running process IDs and
   verify the Matter service, native daemon and Ziggurat versions. Confirm the
   current coordinator identity, PAN/channel and Thread status before stopping.
2. Close Zigbee joining and stop initiating new Matter operations. Export the
   current Thread dataset before shutdown using the validated export source.
   Keep production state in place; the host controller uses its own test
   fabric (OTBR itself has no Matter fabric).
3. Use the supported paired control flow to quiesce MicroHAD and Matter while
   leaving Ziggurat available for its final state export:

   ```sh
   ssh root@"$GATEWAY_IP" '/tuya/native/paired_control.sh quiesce'
   ```

   Require the quiesce acknowledgement. Now archive the complete stopped
   Matter state directory (including OpenThread settings), native persistent
   state, configuration and launchers to the host session. A live database copy
   is not the final consistent backup. Use the documented
   `microhad/scripts/ziggurat_freeze.py` flow with the matching validated
   migration template, `--quiesced --execute`, and a **new output path
   under `$SESSION/state`**:

   ```sh
   "$MIGRATION_PYTHON" microhad/scripts/ziggurat_freeze.py \
     --template "$PRIVATE_TEMPLATE" --host "$GATEWAY_IP" \
     --password-file "$PASSWORD_FILE" \
     --bridge /tuya/native/gateway-maintenance \
     --quiesced --execute \
     --output "$SESSION/state/ziggurat-frozen.json"
   ```

   Verify successful freeze, complete table counts, matching network identity
   and a nonempty output. If any check fails,
   do not stop the supervisor or flash.
4. After the frozen snapshot is verified, stop the paired supervisor using the
   documented `paired_control.sh stop --frozen-snapshot-saved` flow. Confirm
   that MicroHAD, Matter and Ziggurat have exited, no process owns `/dev/ttyS1`,
   and the UART bridge is not connected to an old client. Keep all existing
   `/tuya/native` state and Matter identity files intact.
5. Stop any host OTBR, CHIP Tool, Matter Server or Home Assistant instance that
   could connect to the bridge. Do not remove Docker volumes. The planned host
   OTBR instance must be the sole radio client.

## Phase 2 — switch to the known OT-RCP image

Use the existing `/tuya/native/gateway-maintenance`, a verified factory-kernel
`/tuya/serialgateway` with the OOB flow-control commands expected by
`ezsp_flash.py`, and the host migration Python environment (bellows 1.0.2,
universal-silabs-flasher 1.1.0). Verify these are available in preflight.
`BRIDGE_PID` below must be the actual numeric PID of this attempt's bridge;
verify `/proc/PID/cmdline` and UART ownership before using it. SSH examples
assume the established gateway authentication/options.

1. Require the frozen snapshot, stopped paired supervisor and free UART. Enter
   the Gecko bootloader directly from the running multipan RCP using the
   supported Spinel reset flow, **before** starting serialgateway:

   ```sh
   ssh root@"$GATEWAY_IP" '/tuya/native/gateway-maintenance check-free /dev/ttyS1'
   ssh root@"$GATEWAY_IP" '/tuya/native/gateway-maintenance bootloader-enter /dev/ttyS1 --exclusive-radio'
   ```

   Require recognition of the 115200/no-flow Gecko menu. Do not use the
   flasher's default EZSP/NCP probing path on a Spinel RCP.
2. Start serialgateway at the **bootloader** settings, verify exclusive UART
   ownership, and flash the validated application-only image:

   ```sh
   ssh root@"$GATEWAY_IP" '/tuya/serialgateway -d /dev/ttyS1 -f -p 8888 -b 115200 > /tmp/matter-otbr-serialgateway.log 2>&1 < /dev/null & echo $! > /tmp/matter-otbr-serialgateway.pid'
   BRIDGE_PID=$(ssh root@"$GATEWAY_IP" 'cat /tmp/matter-otbr-serialgateway.pid')
   ssh root@"$GATEWAY_IP" "/tuya/native/gateway-maintenance check-owner /dev/ttyS1 $BRIDGE_PID"
   "$MIGRATION_PYTHON" microhad/scripts/ezsp_flash.py \
     --image "$OT_RCP_IMAGE" --sha256 "$OT_RCP_SHA256" \
     --host "$GATEWAY_IP" --port 8888 \
     --exclusive-radio --already-in-bootloader --flash
   ```

   Require successful upload acknowledgement and application-run request. An
   unexpected menu or partial upload requires inspection, not automatic retry.
3. Stop only the verified bridge PID and require UART release. Restart at the
   **application** settings; omit `-f` so RTS/CTS is enabled:

   ```sh
   ssh root@"$GATEWAY_IP" "kill -TERM $BRIDGE_PID"
   ssh root@"$GATEWAY_IP" '/tuya/native/gateway-maintenance check-free /dev/ttyS1'
   ssh root@"$GATEWAY_IP" '/tuya/serialgateway -d /dev/ttyS1 -p 8888 -b 460800 > /tmp/matter-otbr-serialgateway.log 2>&1 < /dev/null & echo $! > /tmp/matter-otbr-serialgateway.pid'
   BRIDGE_PID=$(ssh root@"$GATEWAY_IP" 'cat /tmp/matter-otbr-serialgateway.pid')
   ssh root@"$GATEWAY_IP" "/tuya/native/gateway-maintenance check-owner /dev/ttyS1 $BRIDGE_PID"
   ```

   Wait for process exit after TERM before `check-free`; a failed ownership
   check stops the procedure. Retrieve bridge logs before reusing their path.
4. Confirm a single TCP:8888 listener owned by that serialgateway, startup log
   settings of `/dev/ttyS1`, 460800 and HW flow, and no gateway OTBR, Ziggurat or
   Matter service. No reboot or gateway mode/configuration change is required.
   OTBR must be the only TCP client once capture begins. Do not probe the port
   with `nc` or a second client while OTBR is connected.

## Phase 3 — start OTBR and begin all captures

1. Prepare a test-specific copy of
   `firmware/dependencies/rtl8196e-gateway/2-Zigbee-Radio-Silabs-EFR32/26-OT-RCP/docker/docker-compose-otbr-host.yml`.
   Adapt its kernel-bridge comments to the serialgateway transport above. Keep
   its OTBR TCP transport, baud, logging and Thread settings. Give test services and persistent
   volumes unique names. Prefer only OTBR and the host CHIP Tool; do not
   collide with an existing `otbr`, `matter-server` or `homeassistant`
   container. A Compose project name alone does not isolate hard-coded
   `container_name` values or host-network ports: remove/rename those names
   and check port conflicts. Set `restart: "no"` for test services. Keep the
   Matter test database separate from the gateway's production Matter fabric. If using Matter.js Server, set `LOG_LEVEL=debug`
   in the test copy before it starts. If using CHIP Tool, prepare its
   trace output path and verify that this installed build accepts
   `--trace_file` and `--trace_decode 1`.
2. Set the actual `RCP_HOST`, `RCP_PORT=8888`, `RCP_BAUDRATE=460800`, correct
   host backbone interface and high OTBR log level. Pin the OTBR image by
   digest; the source compose uses the mutable `bnutzer/otbr-tcp` tag. Inspect
   that image's entrypoint/radio URL and auto-attach behavior rather than
   assuming the environment variables alone configure it correctly. Confirm
   host `/dev/net/tun`, IPv6 forwarding, RA acceptance, firewall rules and
   multicast/mDNS are ready, plus a powered BlueZ adapter and controller BLE
   access. Save any host settings that will need restoration. Preserve the
   rendered compose configuration, image digests and OTBR/OpenThread versions.
3. Start the UART TCP capture, host Matter IP and BLE/transport collectors,
   session-key exporter and timestamped log collectors first. Then start
   OTBR. Do not commission yet.
4. Initialize OTBR with an isolated settings volume and auto-attach disabled
   until the intended dataset is installed. Do not clone the gateway's full
   OpenThread settings into OTBR: share network credentials while letting
   OTBR retain its own node identity and counters. Record its extended address
   and verify it does not reuse an active production node's identity. With
   `COMPOSE_FILE` pointing
   to the saved test compose, use the selected image's supported CLI:

   ```sh
   docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" exec -T otbr ot-ctl thread stop
   docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" exec -T otbr ot-ctl ifconfig down
   docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" exec -T otbr ot-ctl dataset set active "$(cat "$INTENDED_DATASET_HEX_FILE")"
   docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" exec -T otbr ot-ctl ifconfig up
   docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" exec -T otbr ot-ctl thread start
   ```

   The input file must contain only hex Operational Dataset TLVs. Require
   `Done` for each command; never use `dataset init new` on the existing
   network. Verify the expected RCP/Spinel version and stable Thread role,
   Thread interface, routes and IPv6/mDNS readiness. An isolated network may
   elect this OTBR leader rather than attach to an existing leader. If the
   dataset is wrong, stop before commissioning.
5. Export `ot-ctl dataset active -x` (or the OTBR web/API equivalent) into a
   file in `$SESSION/keys`. Compare it with the intended dataset. Record the
   dataset file containing the Network Key and its SHA-256 fingerprint in
   the key manifest. If Thread key rotation is active, capture each
   active dataset and runtime key sequence observed during the trace. The key
   sequence is runtime state, not an Operational Dataset TLV: record
   `ot-ctl keysequence counter` separately, including before/after values and
   changes inferred from captured key identifiers or instrumented Thread logs.
6. Validate that the RCP traffic capture contains bidirectional HDLC/Spinel and
   raw frames before starting commissioning. Confirm the capture file is
   growing and the collector clock is synchronized. If no raw frames are
   observable, stop and fix instrumentation first.

## Phase 4 — commission and observe

1. Use the installed CHIP Tool as the Matter controller for the most explicit
   commissioning transcript, or use the test Matter Server/UI if matching the
   Home Assistant deployment is the goal. Do not run two commissioners against
   the same attempt. Store the controller's persistent database under the
   session's `state` directory, with verbose commissioning logs redirected to
   `$SESSION/logs`. For CHIP Tool, enable decoded tracing, for example:

   ```sh
   chip-tool pairing ble-thread "$NODE_ID" "hex:$(cat "$INTENDED_DATASET_HEX_FILE")" \
     "$SETUP_PIN" "$DISCRIMINATOR" \
     --storage-directory "$SESSION/state/chip-tool" \
     --trace_file "$SESSION/logs/chip-tool-trace.log" --trace_decode 1 \
     > "$SESSION/logs/chip-tool.log" 2>&1
   ```

   Set the node ID, PIN and discriminator from the selected accessory; use
   the same storage directory and commissioner identity for later reads and
   cleanup. Configure the instrumented key exporter according to its rehearsed
   invocation; this stock CLI example does not enable key export on its own.
   Check the installed command's help and pairing syntax before the test. For Matter.js Server, start the test container with
   `LOG_LEVEL=debug` and save timestamped logs from before commissioning, e.g.:

   ```sh
   docker compose -f "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" logs --timestamps --follow matter-server \
     > "$SESSION/logs/matter-server.log"
   ```

   Matter.js debug output can include decoded interaction-model activity and
   post-decryption Matter payload/TLV bytes. Confirm the available output
   during the fixture rehearsal; no extra accessory commissioning is needed. These stack logs
   complement the mandatory session-key export. Verify new PASE and
   CASE key records are durably written during the attempt; debug output alone
   does not fulfill the capture requirement. Use only a controller build that
   passed the key-export rehearsal, including when selecting Matter.js.
2. Record the exact start time, node ID, QR/manual-code source, controller
   version, active dataset fingerprint and setup PIN in the session metadata. For CHIP Tool, use the BLE-to-Thread pairing flow with
   the OTBR active dataset and the accessory's supplied discriminator/PIN.
   Do not create a new Thread dataset or change the radio channel during the
   attempt.
3. Keep all captures running through the entire commissioning sequence and for
   at least two minutes after success or terminal failure. Capture OTBR and
   Matter logs with timestamps, including attestation, PASE, operational
   credentials, Thread Network Configuration, mDNS/SRP resolution, CASE and
   CommissioningComplete. Mark each phase boundary in the timeline.
4. At success, verify all of the following rather than trusting a single
   `ConnectNetwork` or Thread attach response:
   - The accessory is attached to the expected Thread network.
   - The Matter controller completes operational discovery and CASE.
   - CommissioningComplete succeeds.
   - A basic Matter read/command succeeds after commissioning.
   - Record SRP host/service registration and operational DNS-SD publication
     when that is the accessory's discovery path. These diagnose discovery;
     a specific SRP implementation is not an extra Matter success criterion.
   - The host resolves the operational Matter service over mDNS and can reach
     the accessory over IPv6/Thread.
5. At failure, stop only after the terminal timeout/error and observation
   window. Record the last successful Matter commissioning stage and the
   corresponding Spinel TX/RX frames, Thread frame counters, OTBR state and
   DNS-SD observations. Preserve failed-attempt artifacts before another retry.
6. Confirm all session-key records have been flushed, including failed and
   replacement sessions. Stop the capture only after the observation window.
   Copy container logs and metadata into the session directory. Verify that PCAP and logs cover the
   same time interval and that neither collector reported drops or disk errors.

## Phase 5 — stop OTBR and restore the current gateway stack

Restore the gateway after every cutover, including failed captures or
commissioning. Do not delay restoration for offline analysis in Phase 6.

1. Preserve the final OTBR dataset and flush key exports/logs. Record the
   accessory's final fabric/network state. Perform the agreed accessory cleanup
   while the test controller and Thread network are reachable; retain its
   database until that finishes. If cleanup needs commissioning again on the
   original gateway, record that outstanding action explicitly.
   Stop the host controller cleanly, then make the final consistent database
   copy. Save timestamped container logs and counters **before** removing
   containers: `docker compose down` removes their accessible log history.
   Stop OTBR, archive persistent test data, then use `docker compose -f
   "$COMPOSE_FILE" -p "$COMPOSE_PROJECT" down`, **never** `down -v`. Keep the
   volumes and prevent any test service from auto-restarting.
2. Confirm OTBR has disconnected and the paired supervisor remains stopped.
   Keep serialgateway at 460800/RTS-CTS, with its verified PID as the sole UART
   owner. Run the preflight-validated IID0 Spinel bootloader-entry tool as the
   only TCP client. Save its acknowledgement/timeline and disconnect. Stop
   only the verified bridge PID, save its logs, and require
   `gateway-maintenance check-free /dev/ttyS1`. Do not use the existing
   `gateway-maintenance bootloader-enter` here: it addresses multipan IID1.
3. Start the 115200/no-flow serialgateway as in Phase 2, recognize the Gecko
   menu, and use `ezsp_flash.py` with
   `--image "$RETURN_RCP_IMAGE" --sha256 "$RETURN_RCP_SHA256"`
   and `--exclusive-radio --already-in-bootloader --flash`.
   If the reset/menu check fails, stop for documented recovery. Never fall
   back to EZSP probing, NVM erase or a kernel upgrade. Verify the return hash
   against the pre-test manifest, require the upload acknowledgement, then
   stop the bootloader bridge and require UART release.
4. Leave serialgateway stopped: the production Ziggurat process owns the UART
   directly. Start `/tuya/tuya_start.sh` once, without rebooting. Confirm the
   expected multipan RCP version/460800/RTS-CTS settings from Ziggurat startup,
   healthy paired supervisor, MicroHAD and existing Matter service, and correct
   IPC socket ownership. Preserve the original launcher and configuration.
5. Validate restoration against the frozen pre-test state: same Zigbee
   coordinator identity, PAN/channel, network key fingerprint, device tables
   and counters advanced as required by the saved state. Test a real existing
   Zigbee device report/control. Verify Matter resumes using the unchanged
   `/tuya/native/matter` identity and the previously saved Thread dataset.
   Do not create a replacement Zigbee or Thread network to make status appear
   healthy.
6. Restore temporary host networking/Bluetooth settings after captures stop,
   without deleting pre-existing interfaces, volumes or services. Finish any
   agreed accessory recommissioning and verify its original-controller access.
7. If identity, counters or service health differ, stop and use the documented
   Ziggurat recovery/migration procedure. Do not flash an NCP or erase EFR32
   NVM as an improvised repair. Keep the host OTBR stack stopped until the
   original gateway services are verified.

## Phase 6 — protect and analyze the evidence

Keep the following in the diagnostic bundle:

- TCP PCAP containing the complete OTBR↔RCP stream;
- encrypted Matter IPv6/UDP capture and BLE HCI/BTP or controller transport
  message capture covering PASE;
- PASE/CASE session-key export, format/decoder instructions, session
  mapping and nonce context, including resumed/replacement sessions;
- controller instrumentation revision/patch and authenticated offline
  decryption results in both directions for the sessions observed;
- raw Spinel/HDLC frame extraction and decoded 802.15.4 TX/RX frame list, if
  available;
- OTBR, verbose Matter controller (including decoded payload/TLV trace) and
  host mDNS logs;
- before/after active Thread datasets and key-sequence timeline;
- commissioning transcript, timestamps, firmware hashes and container image
  digests;
- radio and OTBR counters before and after the attempt.

Use the Thread Network Key and matching key sequence to decrypt captured Thread
MAC payloads offline where the capture contains secured frame bytes. Validate
MIC/authentication tags as well as headers and frame-counter progression;
plausible decoded headers alone do not establish successful decryption. Mark
TX requests affected by RCP security offload separately. Thread decryption does
not reveal Matter CASE/PASE application payloads. Use the saved Matter session
keys to decrypt the corresponding captured secure messages and verify their
authentication tags; compare the plaintext with decoded stack traces. Missing
session keys, missing BLE/transport messages or failed authenticated decryption
make the evidence incomplete even when decoded controller logs exist. Retain
the complete debug bundle, including network keys, Matter session keys,
controller databases, setup codes and device identifiers, for later analysis.

Before calling the frame capture complete, document its observation boundary:
it includes every frame exchanged with the host that the RCP reports through
Spinel. It does not independently capture hardware ACKs or prove the absence
of radio-side filtering. If a question depends on those missing frames, an
independent 802.15.4 sniffer/PTI capture is required; do not infer those frames
from TCP success or OpenThread counters.

## Exit criteria

The experiment is complete only when the artifact bundle is intact,
the commissioning outcome is tied to raw RCP-visible frame evidence and
saved PASE/CASE session keys with demonstrated authenticated offline decryption
and decoded Matter payload traces, and the original paired gateway stack
has been restored and verified on its original Zigbee network. Record
limitations explicitly: no passive capture of hardware ACKs/filtered RF frames
without a sniffer. Missing Matter session keys or undecipherable captured
sessions are unmet exit criteria; record the failure and restore the gateway
regardless. Decoded live traces supplement the required saved keys.

## Reference checks

- [Gateway shutdown, frozen export and maintenance helper](recovery-tooling.md#orderly-stop-and-frozen-zigbee-export).
- [Freeze output permission enforcement](../microhad/scripts/ziggurat_freeze.py),
  [flasher behavior](../microhad/scripts/ezsp_flash.py) and
  [IID1 bootloader reset](../firmware/tools/gateway-maintenance.c).
- [Gateway dataset persistence](../micro-thread-matter-daemon/src/thread_lifecycle.rs).
- [OpenThread dataset get/set commands](https://github.com/openthread/ot-docs/blob/main/site/en/reference/cli/concepts/dataset.md).
- [CHIP Tool BLE-to-Thread syntax and storage](https://github.com/project-chip/connectedhomeip/blob/master/docs/development_controllers/chip-tool/chip_tool_guide.md).
- [Spinel raw-frame metadata](../micro-thread-matter-daemon/dependencies/openthread/src/lib/spinel/spinel.h)
  and [RCP TX security/counter handling](../micro-thread-matter-daemon/dependencies/openthread/src/lib/spinel/radio_spinel.cpp).
