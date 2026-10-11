# Zigbee backup and state conversion

These tools preserve a network when moving between the EZSP NCP and Ziggurat
backends. Use [the migration runbook](ziggurat-deployment-plan.md) for cutover
and [recovery tooling](recovery-tooling.md) for frozen exports. Host commands
below run from the repository root. Python dependencies are installed by
[build setup](building.md#prepare-installation-and-migration-tools).

## What must be preserved

A usable logical snapshot includes coordinator EUI64, PAN/channel, extended
PAN, network key/sequence, trust-center key/hashed seed, NWK and APS outgoing
security counters, device keys, sleepy children and address cache.
`network.json` or the application inventory alone is insufficient.

The EZSP v13 exporter reads APS counter evidence through `getApsKeyInfo` with
a trust-center-link-key context. The installed NCP lacks raw token enumeration;
logical export is not a complete token or radio flash backup. Reads are
sequential rather than an atomic freeze: close joining, and take a fresh final
capture under exclusive handover before starting the new stack.

Keep snapshots outside Git, in mode-0700 directories with mode-0600 files.
Never print key-bearing contents. The running native exporter schedules reads
through its coordinator queue, writes bounded transient output below
`/tmp/microhad-network-backup`, and returns only acceptance through HTTP.
Successful SSH collection removes the remote copy.

## Collect and prepare offline

Set `MIGRATION_PYTHON` from build setup and choose new absolute output filenames:

```sh
"$MIGRATION_PYTHON" microhad/scripts/network_backup.py collect \
  --host "$GATEWAY_IP" --password-file "$PASSWORD_FILE" --output "$PRIVATE_EXPORT"
"$MIGRATION_PYTHON" microhad/scripts/network_backup.py inspect --input "$PRIVATE_EXPORT"
"$MIGRATION_PYTHON" microhad/scripts/network_backup.py convert \
  --input "$PRIVATE_EXPORT" --output "$PRIVATE_STATE" \
  --coordinator-output "$PRIVATE_NCP_BACKUP"
"$MIGRATION_PYTHON" microhad/scripts/ziggurat_migration.py prepare-import \
  --state "$PRIVATE_STATE" --output "$PRIVATE_IMPORT"
"$MIGRATION_PYTHON" microhad/scripts/ziggurat_migration.py prepare-native \
  --state "$PRIVATE_IMPORT" --output "$PRIVATE_NATIVE_STATE"
```

`collect` uses the existing native UART owner, avoiding a second radio client.
`inspect` prints counts/statuses. Conversion produces open-coordinator JSON and
supplemental Ziggurat state. The import bundle includes Configure and complete
table loads, without StartNetwork. Supplemental EUI64 strings are little-endian
wire bytes, distinct from displayed IEEE address order.

Preparation advances NWK and APS outgoing counters by 10000 and checks overflow.
It translates incoming counters from NCP last-accepted to Ziggurat
next-acceptable semantics. Missing APS evidence or unauthorized device keys
cause refusal. `--aps-upper-bound` requires independent evidence, never a guess.
Prepared state becomes stale while the source coordinator runs.

## Final capture

For handover, suspend the verified old supervisor, gracefully stop its native
child, then start exactly one maintenance serial bridge. Use
`ezsp_final_capture.py` with the verified bridge/suspended-supervisor PIDs,
`--exclusive-radio --capture --enter-bootloader`, and the running export as
`--compare`. The script compares identity/security and monotonic counters,
saves new private state, and enters bootloader on the same connection.
ASH initialization makes this an exclusive maintenance operation. If interrupted,
inspect actual radio state and recapture before using a prepared import.
[Installation Mode B](installation.md#mode-b-native-service-with-ziggurat)
contains the full command sequence.

## Prepare return to NCP

After Ziggurat transmits, use a newly frozen complete snapshot:

```sh
"$MIGRATION_PYTHON" microhad/scripts/ziggurat_migration.py prepare-rollback \
  --original "$PRIVATE_FINAL_STATE" --last-stopped-state "$PRIVATE_FROZEN_STATE" \
  --output "$PRIVATE_ROLLBACK_BACKUP"
"$MIGRATION_PYTHON" microhad/scripts/ezsp_restore.py --backup "$PRIVATE_ROLLBACK_BACKUP"
```

Preparation requires post-freeze evidence, current keys/children, unchanged
identity/security and monotonic counters. It does not reuse raw token bytes.
Without `--restore`, the restore script validates offline. Live restoration
writes state and advances both outgoing counter spaces again; follow
[rollback](ziggurat-deployment-plan.md#rollback) before enabling it.

## Verify without hardware

```sh
"$MIGRATION_PYTHON" microhad/scripts/test_network_migration.py
"$MIGRATION_PYTHON" microhad/scripts/test_ziggurat_import.py
"$MIGRATION_PYTHON" microhad/scripts/test_ziggurat_import.py --freeze-running
"$MIGRATION_PYTHON" microhad/scripts/test_ziggurat_import.py --state "$PRIVATE_STATE"
```

The private-state import check loads a stopped synthetic stack and does not
transmit. The running fixture uses synthetic keys/radio traffic. Neither
establishes live reverse NCP restoration or power-loss counter safety.
