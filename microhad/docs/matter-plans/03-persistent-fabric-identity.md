# Plan: Persistent Matter fabric identity

## Current open work — 2026-10-04

Persistent authority and host CASE restore exist. Synthetic staged recovery now
has evidence for a completed catalog, stable endpoint identity, corrupt
candidate refusal, source preservation, authority restoration and allocator
high-water; full archive/Thread-settings recovery and target filesystem
verification remain open.

Native commit `7247c94` adds structural preflight for the plaintext tar produced
after successful backup decryption. Commit `7f5ac3d` adds staged extraction that
repeats preflight, requires a new absolute destination, writes only regular
files and directories with owner-only modes, and preserves source
archives/existing trees. Seven synthetic tests pass. Service identity validation,
wrong-permission/interrupted replacement, allocator rollback, and failed-restore
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
| 6 | Partial | Four identity/recovery test groups and 31 combined Rust tests pass; synthetic service process restart/corrupt-state refusal passes. The encrypted host fixture now uses this same authority and allocator, reopens committed credentials, reloads the authority and establishes fresh CASE with the phone detached, over both synthetic BTP and UDP. Current target filesystem/runtime validation remains. |
| 7 | Partial | Schema describes credential protection and encrypted-export policy; detailed operational backup/recovery instructions still need completion. |

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
   the last valid state or silently create a new fabric.
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
   separate wrong-mode/corrupt-store tests pass. Service-level validation and
   failed-restore preservation still need end-to-end coverage.
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
