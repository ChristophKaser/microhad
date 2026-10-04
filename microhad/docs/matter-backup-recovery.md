# Matter identity backup and recovery

This is an offline maintenance runbook for the complete private Matter state.
It does not initiate a gateway connection, stop a service, restore state or
change the launcher. Physical execution belongs to plan 08. The host recovery
exercises in plan 03 must still verify these procedures before deployment.

## State and consistency

The configured `MATTER_STATE_DIR` defaults to `/tuya/native/matter`. Confirm the
actual invocation rather than assuming the default. Include the whole directory:

- `credentials.bin`: the atomic TMKV version-1 envelope, including authority,
  fabric, allocator, canonical Thread dataset and node catalog records.
- `tmp/`: OpenThread POSIX settings, including persistent network/security state.
- All additional private files/subdirectories belonging to the same state tree.

`owner.lock` is a permanent lock inode, not a credential. Do not unlink it in a
running state directory to bypass ownership. A restored, separate directory can
have its own lock inode; the new owner must acquire it normally. Never copy only
key 0x1000, merge two stores, transplant certificates or edit the allocator.

The Rust store requires the directory and credential/lock files to belong to the
service UID and deny group/other access. Use directories 0700 and regular files
0600. Symlinks, public state, corrupt envelopes and mismatched authority/fabric
records must fail closed. Certificate/key validation belongs to the exact Rust
service, not a tar listing or checksum. CRC detects storage corruption; encryption
protects the backup. Neither proves that a stale backup is safe to activate.

## Preparation and owner shutdown

1. Record exact native/Rust commits and executable hashes, state/socket/radio
   paths, administrator vendor metadata and supervisor invocation in a private
   maintenance manifest. Record the filtered Zigbee baseline and the intended
   rollback. Keep credentials/setup codes out of that manifest and logs.
2. Arrange a maintenance window. Block new commissioning/control operations
   and prevent the supervisor from respawning Matter while state is copied.
   Killing only the Matter child is insufficient: `paired_start.sh` retries it.
3. For the paired supervisor, create its `quiesce` marker and wait for the
   matching `quiesced` acknowledgement under its actual `RUNTIME_CONTROL` path
   (default `/tmp/ziggurat-supervisor`). The implementation stops Matter and
   native, leaves Ziggurat alive, and does not respawn clients while quiesced.
   HTTP/control availability is interrupted during this maintenance window.
   Do not use an arbitrary PID or start another UART owner.
4. Confirm no Matter owner remains, no other process writes the state tree,
   and shutdown was orderly. A forced shutdown requires corruption/durability
   validation before treating the snapshot as a valid recovery point.
5. On a host filesystem, additionally hold an exclusive nonblocking `flock`
   on the original `owner.lock` during snapshotting. Refusal means an owner is
   still present; stop and investigate. Do not replace the lock file. On the
   factory gateway, use the verified supervisor/owner shutdown procedure;
   availability of a standalone `flock` executable has not been established.

Do not remove `quiesce` and assume that resumes service. The current supervisor
exits when a quiesced state loses that marker. Recovery requires its explicit
stop and a fresh supervisor start using the recorded invocation. That transition
can restart Ziggurat, so it belongs to the arranged maintenance/rollback scope.

## Encrypted consistent backup

Use a private backup directory on the operator host and an existing, verified
OpenPGP recipient. Verify access to the corresponding decryption key before
relying on the backup. Do not put a password/passphrase in a command argument,
script, report or source file. GnuPG can encrypt to the public recipient without
exporting its private key.

The following host-side Bash template transfers the already-quiesced state
through SSH directly into encryption. It does not create a plaintext archive
on gateway flash or the operator's disk. Run only after the preparation above;
replace the backup directory and recipient with the reviewed manifest values.
If the state directory differs, review the fixed remote tar path as well.

```bash
set -euo pipefail
umask 077
MATTER_BACKUP_DIR=/secure/private/matter-backups
MATTER_BACKUP_RECIPIENT=VERIFIED_OPENPGP_FINGERPRINT
MATTER_BACKUP_NAME=matter-REVIEWED_CHECKPOINT.tar.gpg
MATTER_BACKUP_PART="$MATTER_BACKUP_DIR/$MATTER_BACKUP_NAME.partial"
MATTER_BACKUP_FINAL="$MATTER_BACKUP_DIR/$MATTER_BACKUP_NAME"

# Directory already exists, is owned by the operator and has mode 0700.
# Both output names must be absent; never overwrite an earlier recovery point.
test -d "$MATTER_BACKUP_DIR"
test ! -e "$MATTER_BACKUP_PART"
test ! -e "$MATTER_BACKUP_FINAL"

MATTER_SSH=(sshpass -f ../ssh_password.txt ssh
    -o ConnectTimeout=8 -o StrictHostKeyChecking=yes
    -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa
    -o PreferredAuthentications=password -o PubkeyAuthentication=no
    root@192.168.178.62)

"${MATTER_SSH[@]}" 'tar -C /tuya/native/matter -cf - .' |
    gpg --encrypt --recipient "$MATTER_BACKUP_RECIPIENT" --output "$MATTER_BACKUP_PART"
chmod 600 "$MATTER_BACKUP_PART"
# Only publish after SSH/tar and GnuPG both succeeded under pipefail.
mv -n "$MATTER_BACKUP_PART" "$MATTER_BACKUP_FINAL"
test ! -e "$MATTER_BACKUP_PART"
sha256sum "$MATTER_BACKUP_FINAL"
```

On any failure, do not publish or activate the partial output. Preserve the last
valid encrypted backup. Remove the failed encrypted partial only after diagnosis;
never print/decode its contents into logs. Verify encryption/decryption exit
status and stage/validate a recovery copy before resuming allocation. A checksum
is an artifact identity, not proof of successful decryption or valid fabric.
SSH host-key handling and the password file follow the repository deployment
procedure; no SCP subsystem is used and the password file is never copied.

The template assumes a private, single-writer backup directory. Its shell
existence checks are not a concurrent-writer lock; do not run two exports under
one checkpoint name. Retain at least the last verified recovery point until a
new backup has passed restore validation. Apply the operator's encrypted-backup
retention policy rather than routinely writing copies to gateway flash.

## Stage restore without replacing live state

1. Keep the existing service owner stopped and preserve the original private
   tree. Restore into a new absolute directory; do not extract over the current
   state or delete its `credentials.bin` to force first-start initialization.
2. Decrypt to a private RAM-backed workspace on the operator host, using
   `gpg --decrypt --output PRIVATE_RAM_PATH/state.tar ENCRYPTED_BACKUP`. Confirm
   the workspace filesystem is actually tmpfs and its directory mode is 0700.
   The archive is plaintext credentials while there; never put it in Git,
   ordinary `/tmp` on disk, a shared folder or a diagnostics attachment.
3. Require successful decryption/integrity verification before extraction. Do
   not activate output produced by a failed decrypt, and do not assume a
   streaming extractor's successful exit proves GnuPG succeeded upstream.
4. Preflight archive members before extraction: relative canonical paths only;
   no traversal, symlinks, hard links, devices, sockets or duplicate targets.
   Bound the archive/member sizes and allow only the private state tree. The
   automated archive preflight/staging helper remains an open recovery exercise;
   do not use unrestricted tar extraction on an unreviewed archive.
5. Extract into a new private staging tree on RAM or an encrypted host filesystem,
   restrict directories/files to 0700/0600, and assign the intended service UID
   when staging onto the gateway. Verify the entire tree is present, including
   OpenThread settings and the versioned node catalog.
6. Validate the TMKV magic/version, bounded record count/lengths, unique keys,
   exact payload length and CRC without printing records. Then use the matching
   service's identity/store validation to verify certificates, key pairs,
   fabric/vendor metadata and allocator. A newer service/schema requires its
   explicit migration procedure, never reinterpretation or record edits.
7. Exercise fresh CASE to a retained node against the restored catalog/authority
   in an isolated synthetic test before considering physical activation. Do
   not start a second service against the production Thread socket for this
   check. The target may need separately arranged radio startup validation.

The credential-envelope format and authority fields are defined in
[the identity schema](matter-identity-store-v1.md). The exact runtime validation
exists in companion `rs-matter-probe/src/store.rs` and `src/identity.rs`.
A dedicated offline service validation command and automated staged-restore
harness remain open; this runbook does not claim either exists.

## Counter rollback and activation

A backup contains its captured allocator and OpenThread security counters.
An older backup can therefore be unsafe even when its keys and CRC are valid.

- Compare the candidate against the latest trusted state/high-water recovery
  checkpoint. Fabric/authority must match and the node allocator cannot be
  lower than any value already used by that fabric.
- Preserve Thread security counters as part of the complete settings snapshot.
  An earlier settings backup must not reset counters already used on the mesh.
  A tar/CRC match cannot prove this when later traffic occurred.
- Do not manually raise one field, merge newer counters into older blobs or
  recycle abandoned node IDs to make an old backup pass. Those operations need
  an explicit versioned recovery policy and tests; none is provided here.
- If current state is corrupt/lost and no trusted high-water evidence proves
  the backup current, fail closed. Preserve both sources for diagnosis. Do not
  silently create a replacement fabric or claim seamless device recovery.

Only after all checks pass, choose the new staged tree as `MATTER_STATE_DIR`
under the reviewed restart procedure. Keep the original tree at a separate
private path; never rename a directory while its owner is running. Atomic
replacement of the credential file is supplied by PrivateStore; selecting an
entire staged tree is an offline maintenance operation, not an online rename.

Start exactly one Matter owner with the recorded administrator metadata,
no-reset private Thread socket and native coordinator observations. Check
redacted Matter/Thread states, the same fabric/node identity, fresh CASE and
continued Zigbee formation/channel. A failed startup must not trigger automatic
store deletion, first-start key generation or repeated dataset replacement.

Rollback stops the new owner and returns to the preserved tree only if that tree
is still a safe counter high-water point. Once the new owner allocates IDs or
transmits Thread traffic, an earlier tree is no longer automatically safe.
Take a fresh consistent recovery point before switching again. Finish by removing
plaintext RAM staging, recording redacted validation/cleanup results and retaining
only private encrypted recovery copies under the operator policy.

## Verification and remaining work

The paths, owner lock, permission rules, atomic credential replacement and paired
supervisor quiesce/resume behavior were checked against the current sources.
All document links and shell-template syntax are checked on the host. No actual
credentials were read, encrypted, exported or restored, and no gateway was
contacted. These are documented procedures, not executed recovery evidence.

Plan 03 still requires synthetic tests for incomplete/corrupt archives, wrong
permissions, interrupted replacement, invalid identity, allocator rollback and
failed restore preserving the last good tree. Extend the tests to the node
catalog remote reconciliation once plan 07 implements it. Factory filesystem durability and physical
CASE/reception/resource measurements remain in excluded plan 08.

[GnuPG command reference](https://www.gnupg.org/documentation/manuals/gnupg/Operational-GPG-Commands.html)
describes public-key encryption and explicit-output decryption. The runbook's
archive, owner-lifetime and counter rules derive from this repository's state
and supervision implementation.
