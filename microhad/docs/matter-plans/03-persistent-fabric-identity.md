# Plan: Persistent Matter fabric identity

## Current open work — 2026-10-05

Persistent authority and host CASE restore exist. Synthetic staged recovery now
has evidence for a completed catalog, stable endpoint identity, corrupt
candidate refusal, source preservation, authority restoration and allocator
high-water. The real service fixture also verifies an allocated node is retained
as `recovery_required` after active commissioning is interrupted by RCP loss.
Archive preflight, private extraction and matching-service identity validation
pass together on synthetic state, including failed candidate validation without
changing the source tree. Rust commits `8db4897` and `980db77` cover both sides
of atomic rename: an orphaned fully synced stage before rename preserves the
prior identity, and a simulated directory-sync error after rename poisons the
current owner while restart loads the complete replacement envelope. Actual
power-loss behavior and target filesystem verification remain open.

Native commit `7247c94` adds structural preflight for the plaintext tar produced
after successful backup decryption. Commit `7f5ac3d` adds staged extraction that
repeats preflight, requires a new absolute destination, writes only regular
files and directories with owner-only modes, and preserves source
archives/existing trees. Seven synthetic tests pass. Rust commit `1ec79fd` adds
offline, read-only candidate validation for the authority, installed fabric,
catalog, Thread dataset and OpenThread settings. The synthetic service harness
passes valid, incomplete and wrong-permission cases and proves each candidate
is unchanged. Rust commit `0e133b4` extends the synthetic service harness
through native archive preflight and extraction, then validates the staged
state. A structurally valid archive with a corrupt credential envelope is
rejected without changing the candidate. Seven native archive-helper tests
pass. Interrupted replacement, allocator rollback, and failed-restore
preservation remain open.

Consistent backup/staged restore procedures are documented in
[the recovery runbook](../matter-backup-recovery.md). Next deliverable: exercise
incomplete, corrupt and interrupted restores on synthetic state, including
the plan 07 catalog.

Follow the ordered steps and execution checklist below. Record verification
commands, results and the owning repository commit after each completed step.
Earlier checkpoint sections are historical; use the latest evidence when
assessing completion. Keep physical acceptance in plan 08.

## Goal

Preserve the gateway's Matter authority, fabric, node allocation, and device
state safely across service and gateway restarts.

## Steps

1. Define the production key/value schema and reserve stable keys for the
   root-authority private key and certificate, intermediate authority
   materials if used, fabric metadata, node-allocation counter, and version.
2. Reuse the bounded atomic `PrivateStore` implementation. Confirm that all
   files and directories remain owner-only on the target filesystem, that
   symlinks and corrupt envelopes fail closed, and that writes do not happen
   during normal status polling.
3. Implement first-start identity creation with cryptographically secure
   entropy. Persist the complete authority record atomically before exposing a
   commissioning session; recover safely from an interrupted first write.
4. Load and validate the same authority on restart. Refuse mismatched fabric
   metadata or partial records instead of silently creating a new fabric.
5. Allocate unique operational node IDs monotonically. Persist allocation
   before use so crashes cannot reuse an ID; define exhaustion and migration
   behavior.
6. Add tests for first start, reload, interrupted/corrupt state, concurrent
   owner exclusion, permission failures, node allocation across restart, and
   allocator exhaustion.
7. Document backup/restore and recovery without exposing private keys or
   operational credentials. Keep any export flow explicitly encrypted and
   outside routine logs.

## Acceptance

- A restart restores the identical fabric and authority and can establish CASE
  with an already commissioned node.
- Repeated allocation and restart never reuse a node ID.
- Corrupt or partially written state fails closed and does not overwrite the
  last committed identity.
- Flash writes are limited to actual state changes and commissioning events.

## Status quo — 2026-10-04

Step 1 is complete: [authority schema 1](../matter-identity-store-v1.md) reserves
vendor keys outside upstream rs-matter's fabric/singleton space and fixes one
bounded atomic authority record containing the RCAC/ICAC/controller keys and
certificates, IPK, fabric/vendor metadata and monotonic allocator. Interrupted
first-write recovery, exhaustion and explicit-version migration behavior are
defined. The authority is now implemented and connected to service startup.

| Step | Status | Evidence / remaining work |
| --- | --- | --- |
| 2 | Partial | PrivateStore reused with private ownership, atomic replacement, symlink/corruption and owner-exclusion host tests; current authority has not been exercised on the target filesystem. |
| 3 | Complete | Secure OS entropy generates all authority materials; full record committed before fabric installation; abandoned-stage and interrupted-install host checks pass. |
| 4 | Complete | Restore verifies the certificate chain, all three key pairs, fabric/node/vendor metadata, counter and exact upstream fabric materials. Missing authority with a fabric or an unloaded corrupt fabric is rejected. Real host service process restart passes without rewriting state. |
| 5 | Complete | Counter committed before an ID is returned; reload continues above prior IDs, failed writes return no ID, exhaustion is persisted and never wraps. |
| 6 | Partial | Offline candidate validation refuses active owners, does not create paths or write records, and checks authority/vendor, installed fabric, catalog, Thread dataset and settings framing (`1ec79fd`). Controller-service library tests pass 73/73; synthetic service integration passes valid/incomplete/unsafe cases. Archive preflight/extract/validate passes end to end; corrupt credentials fail validation without changing the candidate or source (`0e133b4`). Catalog high-water refusal, interrupted commissioning reservation recovery, pre-rename orphan-stage recovery, and post-rename owner poisoning/reload are covered. Actual power-loss behavior remains untested. The encrypted host fixture reloads authority and establishes fresh CASE over synthetic BTP and UDP. Target filesystem validation remains in plan 08. |
| 7 | Partial | Schema and operational backup/restore policy are documented; the recovery runbook invokes the matching binary's `--validate-state-dir` command. Both rename-boundary error paths are host-tested; actual power-loss and target filesystem durability evidence remain. |

The service now reports `fabric_ready:true`/`running` after initialization,
with Thread still `detached` until plan 06. Administrator vendor metadata
defaults to development ID `0xfff1`, with an explicit first-start CLI override;
changing that metadata on restart fails closed. This is not a manufacturer
certificate or gateway HTTPS trust requirement. Static stripped factory
MIPS-I/o32 service is 1,527,452 bytes. No physical gateway test was run.

The production-authority encrypted host fixtures complete in 217 ms over
synthetic BTP and 264 ms over internal UDP. These are development-host timings,
not gateway performance. The corresponding factory MIPS fixture build passes;
it has not yet been executed on the gateway in this checkpoint.

## Remaining checkpoints

1. Write an operational backup procedure: stop the store owner, copy the entire
   consistent private state, encrypt the backup, restrict permissions and
   verify it without displaying credential contents.
2. Document restore into a separate private directory, validation before use,
   rollback and retention. Do not merge stores or reset an allocator to repair
   corruption; preserve the rejected state for diagnosis.
3. Add recovery exercises for wrong permissions, incomplete backup, corrupt
   state and interrupted replacement; prove failed restore does not replace
   the last valid state or silently create a new fabric. Offline staged-state
   validation (`1ec79fd`), archive-to-validator success/corrupt-envelope
   refusal (`0e133b4`), incomplete archive rejection and source preservation
   pass. A crash during replacement itself remains untested.
4. When target testing is arranged, verify owner-only filesystem behavior and
   crash/restart persistence on factory uClibc. Measure writes during repeated
   status reads and compare restored identity/allocator without exposing keys.
5. Record fresh CASE recovery with the same authority and a persisted node;
   keep synthetic fixture, target fixture and real-accessory evidence distinct.

## Dependencies

[Rust service runtime](01-rust-service-runtime.md) and its single-owner store.
Complete this before real accessory commissioning.

The encrypted fixture now also exercises the production-owned commissioning
future with this persisted signer/allocator, then verifies authority/fabric
reopen and fresh CASE with the phone detached. This strengthens host integration
evidence; it does not replace target filesystem checks or the outstanding
operational backup/restore documentation.

## Execution checklist for open work — 2026-10-04

All unchecked items below remain open. Earlier checkpoint measurements
are historical; the latest status/evidence section above takes precedence.

1. [x] Write a backup runbook covering owner shutdown, consistent copying of
   the entire private state, encrypted storage, permissions and verification
   without displaying credential contents.
2. [x] Write a restore runbook using a separate private directory, validation
   before activation, rollback and retention. Forbid merging stores or
   lowering the allocator; preserve rejected state for diagnosis.
3. [ ] Complete the synthetic recovery matrix for incomplete backup, corruption,
   wrong permissions and interrupted replacement. Archive preflight/staging now
   covers incomplete roots, unsafe/duplicate members, size limits, private modes
   and destination/source preservation (`7247c94`, `7f5ac3d`); staged authority/catalog recovery with
   allocator high-water, corrupt candidate refusal, abandoned-stage recovery and
   separate wrong-mode/corrupt-store tests pass. Service-level validation
   rejects corrupt staged credentials without changing the source tree; a crash
   during atomic replacement is simulated before rename by Rust commit
   `8db4897`; post-rename sync failure poisons the owner and reloads the complete
   envelope (`980db77`). Actual power-loss durability remains untested.
4. [ ] After plan 07 persists the node catalog, include it in backup/recovery
   tests and prove restart reaches the same node through fresh CASE.
5. [ ] Prepare target filesystem checks for atomic replacement, fsync,
   ownership, crash recovery and unchanged state during status polling; run
   them within plan 08 and record redacted pass/fail evidence.

Validation: host recovery exercises and encrypted fixture CASE recovery first;
target persistence semantics require separately recorded target evidence.

## Backup/restore runbook checkpoint — 2026-10-04

[The recovery runbook](../matter-backup-recovery.md) now specifies complete-state
scope, private permissions, owner/supervisor quiescence, encrypted SSH-to-GnuPG
backup, separately staged restore, identity/schema validation, counter rollback
refusal, controlled activation, retention and cleanup. It distinguishes document
instructions from executed recovery evidence. Quiesce stops Matter and native
while leaving Ziggurat alive; removing the marker does not resume the existing
supervisor. This behavior was checked in the actual launcher before documenting.

## Pre-rename credential replacement crash checkpoint — 2026-10-05

Rust commit `8db4897` adds a `PrivateStore` regression that first commits an
identity, then leaves behind a complete owner-only replacement envelope after
`write_all`/`sync_all` but before rename, matching the filesystem state of a
process crash at that boundary. Reopening ignores the orphan stage and reads
the prior committed identity; a later update and second restart preserve that
identity while committing the new counter.

The focused pre-rename test and all 73 controller-service library tests pass
with local Unix socket permission. This verifies process-crash recovery on the
host filesystem. It does not simulate power loss or prove target filesystem
durability.

## Post-rename sync failure recovery — 2026-10-05

Rust commit `980db77` adds a test-only fault point after rename and before the
parent-directory sync completes. The simulated sync failure returns an error
and poisons the live `PrivateStore`, preventing reads or writes from a possibly
stale in-memory view. After restart, the store validates and loads the complete
replacement envelope. The focused store tests pass 7/7 and the complete
controller-service library suite passes 73/73.

This verifies fail-closed owner behavior and recovery from a post-rename error
on the host. It cannot establish which directory entry survives sudden power
loss; actual filesystem durability and target validation remain open in plan
08.

No credentials or gateway state were read/exported/restored. The shell template
and relative links were checked locally. Archive preflight/staging automation,
offline service validation, synthetic failed/interrupted restore tests and node
catalog recovery remain open. Target filesystem/physical acceptance stays in
plan 08. The Garage failed-GATT-confirmation fix is separately uncommitted;
its required Android build/TCP checks were not executed because automatic
approval review reported exhausted workspace credits, not an unsafe action.

## Catalog/authority consistency checkpoint — 2026-10-04

Rust commit `5002f88` adds the bounded node catalog to the same private envelope.
Its fabric/node IDs are checked against persisted authority/allocation, and an
orphaned catalog cannot silently create replacement credentials. A regression
verifies refusal leaves the envelope unchanged. Full service tests pass 48;
identity-only tests pass 23. Backup/recovery documentation now includes catalog
metadata. Synthetic backup/restore failure tests remain open in this plan.

## Synthetic staged catalog recovery checkpoint — 2026-10-04

Rust commit `6e0d445` copies the private credentials envelope into a separate
staging directory and verifies that the completed catalog and stable endpoint
identity reopen. The test corrupts a separate candidate and proves the source
envelope is unchanged, then reopens the valid source with an abandoned staging
file present. Existing store tests cover wrong directory permissions and
corrupt/truncated envelopes. This was the catalog-only first checkpoint; the
follow-up below adds authority and allocator recovery to the same staged copy.

Rust commit `78f4573` closes the authority/catalog gap: it stages the same
private envelope in a separate directory, validates the original fabric and
completed node, then confirms allocation resumes above the restored high-water
mark. The `controller-identity` feature library suite passes 25/25 with both
recovery tests. The test uses synthetic directories and does not export or
activate live state. Archive completeness, OpenThread `tmp/` preservation and
end-to-end rejection of an incomplete archive remain open, so checklist item 3
is still unchecked.

## Interrupted commissioning allocation recovery — 2026-10-04

Rust commit `34b4839` extends the real service restart fixture to allocate a
node for an active commissioning attempt, then terminate the RCP connection.
On service restart the catalog retains that node in `recovery_required`; the
Thread dataset bytes remain unchanged, and the service rejects the old
commissioning session callback. This confirms that an interrupted attempt
does not recycle its operational node ID or overwrite the active Thread
credentials.

`python3 scripts/test-matter-service.py` passes this scenario. It is host
synthetic-RCP evidence, not crash-at-every-write filesystem acceptance.
Interrupted archive replacement, corrupt/incomplete failed-restore preservation,
and target filesystem behavior remain open; plan 08 was not run.
