# Real-radio Ziggurat migration — 2026-10-03

> **Reporting correction (2026-10-03):** cached-endpoint rejoins previously
> skipped binding/reporting setup. The native fix is tested for both backends
> and deployed. Küche accepted setup and sent four reports, but sustained
> reporting after a later rejoin remains unresolved.
> [Current thermometer status](THERMOMETER_RECOVERY.md).

> **Post-deployment correction (2026-10-03):** investigation of Küche TH01
> reproduced and fixed child loss on relayed own parent announcements, and fixed
> rejection of TH01 signed humidity reads. Both fixes are deployed; the child
> survives startup. [Evidence and current verification](THERMOMETER_RECOVERY.md).


## Current checkpoint

Permanent paired production startup is installed under `/tuya/native` and
controlled reboot returned online automatically. All 12 Zigbee deployment steps are complete;
final frozen recovery state and offline-validated NCP rollback backup are archived. The original
EZSP launcher/binary/config and NCP recovery image remain available. Follow
[the deployment plan](ZIGGURAT_DEPLOYMENT_PLAN.md) for current checkpoint and
orderly rollback; restoring the old launcher alone cannot undo an RCP migration.
Thread IID 2 remains unused; standalone Thread/Matter is a separate next phase.

The preserved inventory has **23 entries: 17 Zigbee, four Shelly and two FRITZ!Box**.
Inventory preservation alone does not prove radio communication. The real light
test below and fresh Zigbee reports provide separate physical-radio evidence.
The user waived physical sleepy-child stimulation; report that check as assumed,
not verified. The imported temperature child was already silent for over two
hours, and the imported remote requires a button press to wake.

## What the hardware revealed

1. The native daemon exits after SIGTERM, but remains a zombie while its verified
   parent launcher is SIGSTOPped. A zombie with no descriptors does not own the
   UART. Check its state and the device-side UART ownership helper rather than
   waiting for its `/proc` directory to disappear.
2. Bellows connection initialization resets the NCP. This NCP defaults to stack
   profile 0. Set runtime security level 5 and Zigbee PRO profile 2, then
   `networkInit(0)` restores the saved network. Without that runtime profile,
   initialization returns `NOT_JOINED`. The final capture helper now performs
   this guarded initialization and never forms a replacement network. It can
   transmit after initialization, so capture and final bootloader entry share a
   connection and migration counters advance with margins.
3. TCP urgent-data flow switching did not reliably expose this bootloader menu.
   Stop the exact bridge and use the direct UART helper to verify/run the menu;
   for upload, restart `/tuya/serialgateway -f -p 8888` to set no hardware flow
   explicitly. Confirm the recognizable menu before any upload.
4. In this SDK's multi-PAN RCP, IID 0 is reserved and `IidToInstance(0)` returns
   null. Both bootloader entry and normal radio reset must address active IID 1.
   The original helper and Ziggurat owner reset sent IID 0; the physical radio
   rejected them, while the simulator incorrectly accepted them. Both are fixed.
   Broadcast reset notifications on IID 0 are still dispatched to consumers.

The helper's corrected reset bytes are `90 01 03` before HDLC framing/FCS. Its
host/MIPS build and four PTY tests pass, followed by physical bootloader entry,
menu recognition and run without upload. Ziggurat's reset correction passes 16
Rust transport tests, seven simulator checks, eight multi-PAN host scenarios and
eight actual native/Ziggurat/simulator lifecycle scenarios. The MIPS binary was
rebuilt and uploaded; this correction did not require another radio flash.

## Firmware and first network start

The bootloader accepted the application-only 103800-byte RCP image. Program
range was `[0x4000,0x1d52c)`; the bootloader and NVM3 region were excluded.
SHA256: `5e835cf429dbc25ded9e6217f620d0d2243c11c98935de0e29e387657063bb48`.
The retained recovery NCP GBL and original service/application archive remain
privately on the host, together with the final pre-cutover network export.

Real RCP hardware identity matched the NCP backup before Configure/StartNetwork.
After the corrected normal reset, native became online/formed with backend
`ziggurat`. HTTP and HTTPS pass. Identity, PAN/channel, key metadata and advanced
NWK/APS counters match privately. Native owns persistent state at
`/tuya/native/ziggurat-state.json`; Ziggurat owns `/dev/ttyS1` at 460800 with RTS/CTS.
Thread IID 2 remains unused.

Once this stack transmitted, rollback moved to checkpoint E: freeze the current
stack and export its latest counters/keys before preparing the NCP return. The
pre-cutover export is no longer a sufficient rollback snapshot by itself.

## Physical delivery measurements

An existing light's physical on/off value was first read with a COBS unicast
request requiring an APS acknowledgement. The light was toggled through the
native HTTP API, read back through the real radio, then restored in a `finally`
block and read back again. This distinguishes HTTP command acceptance from
physical execution and end-to-end radio acknowledgement.

| Read | APS acknowledgement | Matching attribute response |
| --- | ---: | ---: |
| Initial state | 35.48 ms | 80.93 ms |
| Toggled state | 37.54 ms | 37.54 ms |
| Restored state | 35.61 ms | 35.61 ms |

These timings include the host SSH/Unix-socket bridge. They are real-radio samples,
not synthetic throughput measurements or a general latency guarantee.

Non-actuating group/broadcast reads were also exercised. An initial group read
reported terminal status 8 (relay quorum incomplete); a later group read returned
status 0 in 68.4 ms. Broadcast descriptor reads for an existing router obtained
matching successful descriptor responses on all three attempts, while terminal
statuses were 8, 5, 8 (incomplete relay quorum / CCA failure). Delivery occurred,
but complete broadcast acknowledgement is intermittent. Preserve these weaker
results; do not convert HTTP admission or a device reply into a claim that every
relay acknowledged. Native scene handling retains bounded retry/unicast fallback.
Warnings also identify a next hop that sometimes does not MAC-ACK. This needs
continued observation after cutover and comparison with physical connectivity.

## Memory and CPU interpretation

This BusyBox `top` displays **VSZ** and computes `%MEM` from virtual address space.
Native reserves roughly 20 MB of virtual space, including thread stacks. Its
measured RSS was about 3600 KiB; Ziggurat RSS was about 2012 KiB. LinuxThreads
appear as separate PIDs sharing an address space: sum their CPU, but do not sum
repeated RSS/VSZ figures as if each were a separate daemon.

A kernel sample showed 27892 KiB total, 2664 KiB free, 15820 KiB cached, 6948 KiB
inactive file pages, and 416 KiB reclaimable slab. Much file cache is reclaimable;
RAM-backed files and anonymous memory are not. Do not equate `MemFree` alone with
allocatable headroom or treat all cache as reclaimable. The stock kernel has no
`MemAvailable` field. Diagnostic files totalled under 9 KiB at this checkpoint;
the existing native log is a bounded 512 KiB RAM ring. Executables reside on flash,
not on `/tmp` ramfs.

Early second-frame `top` samples showed summed CPU ranges of native 0–13% and
Ziggurat 0–1%. `top` itself creates measurable load, so discard its first frame.
State checksum changes were approximately once per minute, consistent with the
native checkpoint timer; this is deliberate security persistence, not flash
logging. Final window/restart/reboot measurements belong in the execution record.
These measurements do not establish capacity for a standalone Matter/Thread host.

## Records and privacy

Private host evidence is under ignored `diagnostics/ziggurat-deployment/`, with
0700 directories and 0600 files. It includes immutable original application
archives, raw/final network exports, prepared import/native/recovery state, live
identity checks, light readbacks, observation samples and final frozen snapshots.
Do not publish those files, keys, configuration, TLS credentials or device data.
Only sanitized execution status, artifact hashes and measurements are mirrored
into the private experiment repository.

### Child-table checkpoint

The post-start frozen snapshot contains zero direct children, versus two in the
original NCP export. Original child records remain in immutable private backups.
No child-timeout warnings appeared in the recorded warning log; the cause of the
table difference is not certified. Some devices may have moved to router parents,
but that is an inference, not a verified explanation. Network credentials remain
unchanged. The user waived physical sleepy-device validation; retain this explicit
limit and inspect polling/rejoin behavior when those devices are next available.

## Permanent startup and controlled reboot

The paired supervisor owns startup order, bounded restarts and maintenance
quiesce. Production uses `/tuya/native/paired_start.sh`, `paired_control.sh`,
`ziggurat`, `gateway-maintenance` and the enabled native binary. The boot wrapper
is `/tuya/tuya_start.sh`; original EZSP files are retained. Private configuration
and checksummed wire state remain mode 0600. Child console output is discarded;
native logs use the bounded RAM ring and must be read through `/api/logs`.

Quiesce → complete freeze/export → clean stop → restart passed before reboot.
`/bin/reboot` then automatically started supervisor 126, Ziggurat 134 and native
149. Native logged readiness 121 seconds after startup, following lifecycle/hello
timeouts; it recovered without manual restart. HTTP/HTTPS, pinned binary MD5s,
23-entry inventory, actual light read/bind/reporting replies and a humidity report
passed after reboot. The initial host observation deadline expired before the
confirmation. Cold-boot Configure blocking requires further investigation;
kernel RNG readiness is suspected, not established, and randomness checks remain.

After reboot memory was 12844 KiB free and 8136 KiB cached out of 27892 KiB.
The earlier low free-memory reading included filesystem caches and testing
artifacts; it did not establish a production heap leak. Do not sum LinuxThreads
RSS entries sharing the same address space.

## Final recovery checkpoint

The final controlled quiesce/export saved complete frozen network, key, child
and address state plus native persisted state privately. Rollback preparation
verified identity/security and nonregressing NWK/APS counters and applied safety
margins. The standard NCP backup passed offline validation. The pair then stopped
and restarted through the permanent launcher; HTTP/HTTPS returned online with
23 inventory entries. Artifact hashes and validation evidence are archived in
Git-ignored diagnostics. This certifies backup preparation, not a performed live
reverse restore. Any later rollback needs a newly frozen snapshot after subsequent
traffic. Original two child records remain archived; the post-start child table
was empty, so direct-child continuity/wake/rejoin remains unproven.
