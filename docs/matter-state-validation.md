# Offline Matter state validation

The controller binary can validate an already staged state tree before it is
considered for activation. It does not start the service, bind the IPC socket,
open an OpenThread radio, or create missing state:

```sh
thread-matter --validate-state-dir /absolute/private/staging/path
```

Pass `--administrator-vendor-id` when the staged authority was created with a
non-default administrator vendor ID. Validation requires an owner-only state
directory, `credentials.bin`, and `tmp/` OpenThread settings owned by the
current service UID. It rejects an active store owner, malformed settings
records, a missing or mismatched authority, invalid Matter fabric records,
catalog inconsistency, or an invalid Thread dataset. Successful validation
prints `Matter state validated`; failures print a fixed validation stage and
exit nonzero without including credentials or record contents.

The store is opened read-only and retains the owner lock for the duration of
the check. Validation does not recover transitional catalog phases or write
records. The synthetic service test snapshots file contents, modes, inodes,
timestamps, and sizes to verify valid and rejected candidates are unchanged. It
also uses the adjacent `microhad/scripts/matter_archive.py` helper to
preflight and extract a synthetic service archive, validates the extracted
candidate, and confirms a structurally valid archive with a corrupt credential
envelope is rejected without modifying the extracted tree.

This is a host-side candidate check; it does not test target filesystem
durability or prove that a physical Thread device can be reached.
