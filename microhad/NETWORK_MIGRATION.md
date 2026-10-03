# Preserve the Zigbee network when moving to Ziggurat

## Current state — 2026-10-03

**The native read-only exporter is implemented, tested and deployed. Radio
firmware remains EmberZNet 7.5.1.0 / EZSP v13; no firmware was flashed.**
The production network remains formed. The exporter deployment/restart described
here occurred on 2026-10-02. On 2026-10-03 the optional native Ziggurat backend
and ZHA-style persisted state were implemented and host-tested; only Ziggurat
was temporarily deployed for a synthetic-radio test. The production daemon was
not replaced/restarted and no radio was accessed. See [integration results](ZIGGURAT_INTEGRATION.md).

A fresh private export contains operational coordinator EUI64, PAN/channel,
network key/sequence, hashed trust-center seed, NWK and APS security counters,
and one direct sleepy child. All 12 link-key slots are currently empty; this
is the radio's exported table, not the application's device inventory.
Identity, PAN/channel, network key and trust-center secret match the retained
post-upgrade backup. The actual snapshot round-trips through a **stopped host
Ziggurat stack with a loopback RCP**, including the child table. It sends no
frames and never accesses `/dev/ttyS1`.

**Orderly rollback preparation is implemented and simulated.** The Ziggurat
experiment now has `FreezeNetwork` (experimental protocol-v2 opcode `0x04`):
it waits for admitted API operations, cooperatively stops all tracked stack
tasks, and retains readable counters, keys and child tables. Mutating requests
and restart are rejected until a new configuration is supplied. A frozen
snapshot can produce a logical NCP recovery backup with both NWK and APS
counters advanced. The restore tool verifies both counters and device keys.
No live NCP restoration or bootloader entry was performed in this work.

**Limitations and next steps:**

1. Rehearse firmware migration/recovery and real device communication on a
   spare radio/network before flashing the production coordinator. Simulator
   results do not establish that existing devices will communicate after migration.
2. Validate the implemented native protocol-v2 adapter and native-owned state
   against real hardware. Production still speaks EZSP. The ZHA-style 1000-frame
   restart margin and checkpoints pass simulation but do not provide unconditional
   crash safety; notification loss, stale keys or long outages remain limits.
3. Establish recovery if an RCP stops responding. A working RCP has a compiled
   Spinel bootloader-entry path; the current setup has no verified SWD recovery
   procedure or full radio flash dump. A valid image does not guarantee recovery.
4. Continue standalone Thread/OTBR and Matter integration in the sibling
   `ziggurat-compilation` project. This work does not provide a Thread stack.

## Findings from the actual NCP

- `getNetworkKeyInfo` exports live NWK sequence/counter metadata.
- `getApsKeyInfo` with a **TC_LINK** context succeeds and its metadata has
  `KEY_HAS_OUTGOING_FRAME_COUNTER`. It supplies the live APS counter. The older
  `getValue(VALUE_APS_FRAME_COUNTER)` query is unsupported (`0x37`).
- `getTokenInfo` returns **LIBRARY_NOT_PRESENT (`0xb5`)**. Raw token enumeration
  is unavailable in the installed firmware, even though token command IDs exist.
  The exporter preserves that capability failure and completes the logical
  export. It does not silently claim a complete token or flash backup.
- Keys and coordinator identity are exported twice and compared; network key
  rotation, counter regression and inconsistent snapshots are rejected. Joining
  must be closed. Export is a sequence of read commands, not an atomic freeze of
  the NCP; take a new export immediately before an exclusive handover.
- `network.json` alone is insufficient: it lacks EUI64, key sequence, security
  counters and device-key records. The new export reads these from the radio.
- Raw UART receive bytes and late sensitive EZSP response bytes are redacted
  from logs. Backups are separate mode-0600 files in a mode-0700 directory,
  outside the web root. HTTP returns only export acceptance, never secrets.

Private files are under ignored `diagnostics/network-migration/`; do not copy
keys into documentation, issues, logs or Git. The deployed service writes
exports only below `/tmp/microhad-network-backup`, with a 256 KiB file bound,
10-second response timeout and 120-second overall bound. Failed exports do not
reset the radio. Successful collection removes the remote copy. Existing files
are never overwritten. Transient exports do not write the `/tuya` flash volume.

## Prerequisites

The native C++03 implementation needs the prescribed RSDK, with mandatory
`make host test` and `make mips`. Host integration tests bind localhost sockets.
Host backup/migration tools need Python 3, `sshpass`, SSH, Git, and these pinned
Python dependencies in an isolated venv:

```sh
cd .
python3 -m venv tools/network-migration-venv
tools/network-migration-venv/bin/pip install \
  bellows==1.0.2 zigpy==2.2.0 universal-silabs-flasher==1.1.0
```

The real-server tests additionally need the patched host Ziggurat and its
simulator from the experiment checkout. The experiment's
`patches/ziggurat-network-migration.patch` applies after its HDLC transport patch;
`ZIGGURAT_MULTIPAN=1 bash scripts/build-ziggurat.sh` builds the MIPS version.

## Fresh export, conversion and offline import

Run from the native repository. Choose **new filenames** for every export;
files below are examples, not permission to reuse an old backup for cutover.
The installed daemon must be online on EZSP v13 with joining closed.

```sh
cd ../microhad-zigbee-native
umask 077
mkdir -p diagnostics/network-migration
chmod 700 diagnostics/network-migration
migration_python=../ziggurat-compilation/tools/network-migration-venv/bin/python

"$migration_python" scripts/network_backup.py collect \
  --host 192.168.178.62 --password-file ../ssh_password.txt \
  --output diagnostics/network-migration/new-export.json
"$migration_python" scripts/network_backup.py inspect \
  --input diagnostics/network-migration/new-export.json
"$migration_python" scripts/network_backup.py convert \
  --input diagnostics/network-migration/new-export.json \
  --output diagnostics/network-migration/new-state.json \
  --coordinator-output diagnostics/network-migration/new-coordinator-backup.json
"$migration_python" scripts/ziggurat_migration.py prepare-import \
  --state diagnostics/network-migration/new-state.json \
  --output diagnostics/network-migration/new-ziggurat-import.json
```

`collect` schedules fixed read-only commands through the daemon's coordinator
queue and retrieves the private result over SSH. It never opens a second UART
connection. `inspect` prints only counts/statuses. Conversion creates standard
open-coordinator JSON and a supplemental Ziggurat migration format.
The import bundle contains Configure, LoadKeyTable, LoadChildren and
LoadAddressCache commands; **StartNetwork is deliberately absent**. All EUI64
strings in the supplemental format are little-endian wire bytes, not
human-readable IEEE address order. Signed radio power is converted explicitly.

NWK and APS outgoing counters advance by 10000 with overflow checks. Incoming
key counters are translated from NCP last-accepted to Ziggurat next-acceptable
semantics. Keys lacking the authorized bit are refused, because Ziggurat's
current restoration API must not silently promote an unverified device key.
Missing APS counter evidence refuses preparation. `--aps-upper-bound` is only
for an independently established bound; guessing is not a substitute for an
export. A prepared snapshot becomes stale while the original coordinator runs.

## Validation that does not flash anything

```sh
"$migration_python" scripts/test_network_migration.py
"$migration_python" scripts/test_ziggurat_import.py
"$migration_python" scripts/test_ziggurat_import.py --freeze-running
"$migration_python" scripts/test_ziggurat_import.py \
  --state diagnostics/network-migration/new-state.json
"$migration_python" scripts/ezsp_restore.py \
  --backup diagnostics/network-migration/new-coordinator-backup.json
"$migration_python" scripts/inspect_network_recovery.py \
  --ncp-image diagnostics/ezsp-upgrade-recovery/ncp-uart-hw-7.5.1-115200-hw.gbl \
  --rcp-map ../ziggurat-compilation/build/firmware-hdlc/build/debug/ot-rcp-multipan.map
```

Evidence: 13 Python migration/restore tests pass; actual-server stopped imports
pass with both synthetic and actual private state. The synthetic running test
transmits one simulated startup frame, then freezes; counters/state remain
stable, restart/key mutation are rejected, and rollback preparation advances
both counters. The actual private snapshot test never starts the network and
transmits zero frames. Native host tests, integration tests and static MIPS
verification pass. Focused Rust tests pass (10 codec + 5 multi-PAN tests); the
new Ziggurat server also compiles for host and stock MIPS.

## Firmware recovery assets and sequence

Use the **same 7.5.1 recovery image previously installed successfully**, not
an unverified downgrade or the raw RCP `.bin`:

- SHA256: `05c66bcc7895bc7db0b9648c12c40e387a1145eabe792413a2762d3d0ada262d`.
- 208036-byte GBL; its CRC is valid and its application program range is
  `[0x4000, 0x36c58)`. No bootloader or secure-element update is included.
- The RCP map reserves 36864 bytes at `[0x37000, 0x40000)` for NVM3, matching
  the retained NCP project. This does not prove that runtime storage changes
  preserve the old network, so recovery uses logical state restoration.
- The RCP enables `OPENTHREAD_CONFIG_PLATFORM_BOOTLOADER_MODE_ENABLE` and links
  the **strong** `bootloader_rebootAndInstall` implementation from
  `platform/bootloader/api/btl_interface.o`, not only the platform weak stub.
  Spinel reset reason `Bootloader = 3` invokes this path. This is a compiled
  recovery capability, not a hardware-tested round trip with Gecko 1.8.0.
- An executable-only copy of the pre-export native daemon was saved privately
  for service rollback. Production configuration remains on the gateway.

For a **future separately authorized** orderly return from Ziggurat:

1. Use `freeze_and_export(client, original_template)` in the migration module
   (or its eventual native adapter equivalent). Save the returned private state
   and verify the freeze operation succeeded before shutting down the process.
2. Prepare the NCP recovery backup from that final state:

   ```sh
   "$migration_python" scripts/ziggurat_migration.py prepare-rollback \
     --original diagnostics/network-migration/pre-migration-state.json \
     --last-stopped-state diagnostics/network-migration/final-frozen-state.json \
     --output diagnostics/network-migration/ncp-rollback-backup.json
   ```

   The tool requires post-stop counter evidence, rejects counter regression,
   changed identity/security material and overflow, and uses the latest device
   keys/children. It does not reuse old token bytes. If the key/seed changed,
   stop and evaluate that migration instead of overwriting current security.
3. Stop radio consumers and establish exclusive UART ownership. A responding
   RCP needs Spinel reset-to-bootloader at 460800 with RTS/CTS; the Gecko UART
   bootloader uses the previously verified 115200/no-flow-control path. The
   old EZSP bootloader-entry command cannot be sent to RCP firmware. Adapt the
   bridge/baud switch and verify the menu on spare hardware; do not assume the
   old bridge automatically handles both rates.
4. Only then upload the pinned application-only GBL using the existing guarded
   flash tooling, confirm EZSP v13, and restore the prepared logical network.
   `ezsp_restore.py --restore` is a state-writing operation and was **not run**
   here. It requires exclusive access, refuses EUI64 rewriting, additionally
   advances both counter spaces and verifies identity, keys, seed and counters.
   If verification fails, read with `--verify-only` rather than repeating writes.
5. Resume the native EZSP daemon and verify real router/sleepy-device traffic.
   Preserve its application device/configuration files throughout.

If the RCP is unresponsive, the Spinel entry path is unavailable. Without a
verified hardware entry/SWD method, rollback cannot be guaranteed. Likewise,
a crash before final export needs current keys and sufficiently advanced counters.
The native persistence layer now implements ZHA-style checkpoints and a 1000-frame
restart margin; this is not a durable reservation of every future counter.
See the integration guide for notification-loss and incoming replay-counter limits.

Sources: the retained [EZSP upgrade record](scripts/EZSP_UPGRADE.md), the
[Silicon Labs NCP-to-host migration note](https://docs.silabs.com/zigbee/9.0.0/multiprotocol-solution-linux/host-ncp-rcp-migration),
and the pinned local GSDK `misc.c`, `ncp_base.cpp`, `btl_interface.c` and linker
map. Silicon Labs' token-based migration requires a token interface that this
installed NCP lacks; our implemented route exports logical state instead.
