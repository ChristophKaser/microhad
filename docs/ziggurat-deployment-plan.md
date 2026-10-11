# Migrate an existing Zigbee network to Ziggurat

This runbook replaces the EZSP radio application with the two-instance HDLC RCP
while preserving coordinator identity, keys and existing pairings. Application
updates on an already migrated gateway use [deployment](deployment.md).
A fresh gateway uses [installation](installation.md).

## Prepare the handover

1. Build the Ziggurat backend, multipan server, maintenance helper and
   application-only RCP GBL using [build setup](building.md). Initially use
   `ZIGGURAT=1 MATTER=0`; add Matter after Zigbee migration is validated.
2. Retain the original application/launcher/configuration, a private logical
   network backup and the pinned NCP recovery GBL on the host. Record selected
   artifacts and hashes. Check flash/RAM before staging.
3. Close joining. Collect a running EZSP export and validate/import it offline
   using [state conversion](network-migration.md). This is preparation; the
   source network remains active and that export cannot be the final cutover state.
4. Stage under an isolated directory without activating a launcher. Verify
   maintenance ownership checks and prepared state permissions.
5. Arrange exclusive maintenance: identify/suspend the old supervisor, stop
   native gracefully and confirm UART release. Save the application/runtime
   archive after native stops. Start only the maintenance serial bridge.
6. Take a fresh final capture with `ezsp_final_capture.py`, compare it with the
   running export, then prepare import/native state from that final capture.
   Do not resume the old coordinator after preparing handover.

[Installation Mode B](installation.md#mode-b-native-service-with-ziggurat)
provides the staging, final-capture, flash and launcher commands. For an existing
network, substitute the current network for its empty-network setup; do not run
**Create fresh network** or reset existing devices. All backup paths must be new
and private. Keep the normalized final pre-cutover state for future rollback.

## Flash and activate

Verify the expected Gecko menu before uploading an inspected application-only
GBL at 115200 baud without flow control. Stop the bridge after upload and verify
UART is free. Ziggurat runtime uses 460800 baud, RTS/CTS and Spinel IID 1.
A working RCP can re-enter the bootloader through the maintenance helper;
an unresponsive RCP has no verified software recovery path.

Install the prepared private `ziggurat-state.json`, explicit Ziggurat native
binary, server/helper and paired supervisor/control scripts. Install the paired
launcher only after prerequisites are present. Retire the suspended old
supervisor before starting the new launcher once. `deploy.sh --activate`
selects the EZSP launcher and must not be used for this stack.

## Accept the migrated network

Require backend `ziggurat`, online/formed status, preserved coordinator/PAN/channel
identity, and appropriately advanced counters. Verify real control of an existing
router/light and actual reports from a sleepy device after waking it. Confirm
joining expires, a native restart reconnects, and a full supervised restart
restores the same network. A cached inventory alone is insufficient.

Do not replace configuration or device inventory to resolve a missing report.
Use [sensor diagnostics](sensor-reporting.md). Keep the private recovery
artifacts until communication and restart checks pass. Add Thread/Matter through
a coordinated [maintenance release](deployment.md#maintenance-release).

## Rollback

Choose recovery according to the last completed checkpoint:

| Checkpoint | Recovery input |
| --- | --- |
| Before radio replacement | Original NCP application/launcher and network |
| RCP installed, before migrated Zigbee transmission | Fresh final pre-cutover coordinator backup, advanced by restore tooling |
| After Ziggurat transmits | Complete current frozen Ziggurat snapshot and original normalized final state |
| Crash/power loss or unresponsive radio | Inspect available evidence before restoration; an old backup is not proof of current counters/keys |

For orderly return after transmission:

1. [Quiesce, freeze/export and stop](recovery-tooling.md#orderly-stop-and-frozen-zigbee-export).
   Require complete post-freeze evidence; native wire-state JSON alone is insufficient.
2. Prepare/validate the NCP rollback backup with the commands in
   [state conversion](network-migration.md#prepare-return-to-ncp).
3. Verify exclusive UART ownership. Enter the RCP bootloader at 460800/RTS-CTS,
   then use the 115200/no-flow bridge to flash the pinned NCP application:

   ```sh
   "$MIGRATION_PYTHON" microhad/scripts/ezsp_flash.py --image "$NCP_IMAGE" \
     --sha256 05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d \
     --host "$GATEWAY_IP" --exclusive-radio --already-in-bootloader --flash
   ```

4. Confirm EZSP v13 with the exclusive bridge, then restore current logical state:

   ```sh
   "$MIGRATION_PYTHON" microhad/scripts/ezsp_restore.py \
     --backup "$PRIVATE_ROLLBACK_BACKUP" --host "$GATEWAY_IP" \
     --exclusive-radio --restore --output "$PRIVATE_VERIFIED_NCP_BACKUP"
   ```

   This writes state and advances counters. If verification fails, inspect with
   `--verify-only` and a new output before deciding on another write.
5. Stop the exact bridge, restore the saved EZSP application/launcher as needed,
   start it once, and verify identity plus real existing-device traffic.

Never flash a raw `.bin`, bootloader/full-chip image or erase NVM to repair this
migration. Live reverse NCP restoration remains unverified. Native checkpoints
and the 1000-frame restart margin have bounded freshness assumptions;
[Ziggurat durability](native-ziggurat-integration.md#durability-limits) explains why
power loss and stale keys can prevent a justified logical rollback.
