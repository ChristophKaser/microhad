# Persistent Matter authority schema 1

This specifies gateway-owned identity records, separate from upstream rs-matter
fabric blobs. Upstream reserves keys below `VENDOR_KEYS_START = 0x1000`; its
fabric records occupy 0–255. The existing private atomic store wraps all records
in its bounded version-1 CRC envelope. CRC detects corruption; files themselves
are plaintext credentials protected by owner-only permissions.

| Key | Record |
| --- | --- |
| `0x1000` | Complete controller authority, identity and next-node counter below |
| `0x1001` | Reserved for Thread active-dataset lifecycle |
| `0x1002` | Reserved for device catalogue index/version |
| `0x1100`–`0x113f` | Reserved for at most 64 device catalogue records |

The authority is one blob so first-start creation never exposes a partial key
or counter record. It contains an RCAC and ICAC, their P-256 private keys, the
controller operational key/NOC, fabric IPK, fabric/controller IDs, vendor ID and
next operational node ID. No field may be independently replaced. Certificates
are Matter TLV, with each at most the pinned upstream 400-byte certificate bound.

| Order | Encoding |
| --- | --- |
| Magic/version | Eight bytes `TMID` followed by `01 00 00 00` |
| Fabric ID | Nonzero u64 little-endian, generated securely once |
| Controller node ID | u64 little-endian, fixed 1 for schema 1 |
| Next operational node ID | u64 little-endian, initial 2 |
| Administrator vendor ID | u16 little-endian, fixed at creation |
| RCAC private key | 32 canonical P-256 bytes |
| ICAC private key | 32 canonical P-256 bytes |
| Controller operational private key | 32 canonical P-256 bytes |
| Fabric IPK | 16 bytes |
| RCAC, ICAC, NOC | Each u16 little-endian length followed by exact TLV bytes |

Maximum record size is 1,352 bytes; no trailing bytes or empty/oversized
certificates are accepted. Version changes require explicit migration, not
reinterpretation or resetting the fabric. Manufacturer credentials and accessory
attestation data do not belong in this record. Administrator vendor ID is a
controller metadata choice, not a trusted manufacturer certificate.

On first start, create all materials from OS entropy and commit the complete
authority through `PrivateStore` before exposing commissioning. Restore verifies
certificate signatures/chain, public/private key correspondence, fabric and node
IDs, vendor metadata, bounds and allocator state. If an upstream controller
fabric is present it must match these exact materials. Missing authority with
an existing fabric, malformed records or a mismatch must fail closed.

An interrupted first write leaves either no authority or the complete committed
record. An authority committed before its upstream fabric blob is installed
can reinstall that same identity; it must never generate new keys on restart.
Ordinary successful reload and status polling write nothing.

Allocation returns the current counter only after atomically persisting its
successor in the authority blob. Failed/cancelled commissioning can leave gaps;
IDs are never recycled. Operational range is 2 through `0xffff_ffef_ffff_ffff`,
matching the pinned Matter stack; the successor of that maximum is the exhausted
sentinel. Exhaustion rejects further allocation. A failed durability commit
returns no ID; the store's poisoned-owner rule requires reopening committed
state before further use.

Backup/restore procedures must include the complete private credential envelope
and Thread settings, preserving owner-only permissions. Routine IPC/status/logs
must never export this record. Any later export feature must encrypt the backup
explicitly; there is no plaintext export endpoint in this schema. Restore must
not lower the allocator counter below any value already used in the fabric.

This document defines the schema. Implementation/restart/CASE evidence is tracked
separately in [plan 03](matter-plans/03-persistent-fabric-identity.md).
