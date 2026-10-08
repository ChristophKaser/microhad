# Persistent Matter node catalog

The production commissioning owner records node identity in the existing private
TMKV credentials envelope under key `0x1002`. No additional flash file or daily
observation writes are introduced. This catalog contains public fabric/node IDs
and commissioning state; fabric keys remain solely in the authority/fabric
records. `0x1100`–`0x113f` remain reserved for future endpoint descriptors.

## Format and bounds

The maximum catalog holds 64 nodes in a 594-byte record:

| Field | Encoding |
| --- | --- |
| Magic/version | Eight bytes `TMNC`, `01 00 00 00` |
| Controller fabric ID | Little-endian u64, nonzero |
| Node count | Little-endian u16, at most 64 |
| Each entry | Little-endian u64 node ID followed by one phase byte |

Entries are strictly increasing, unique, at least node 2, and less than the
persisted authority's next-node counter. Unknown phases, wrong fabric, truncated
records, duplicate/nonmonotonic nodes and incompatible allocator state fail
closed. The enclosing TMKV format supplies its own CRC, bounds, atomic rename,
private ownership/permissions and post-rename durability poisoning.

| Phase byte | Meaning |
| --- | --- |
| 1 | Reserved before accessory fabric installation |
| 2 | Accessory fabric installation confirmed |
| 3 | Thread provisioning confirmed |
| 4 | Operational CASE and CommissioningComplete confirmed |
| 5 | Recovery required; remote state must be reconciled explicitly |

Endpoint IDs are stable strings `matter:<fabric hex16>:<node hex16>:<endpoint hex4>`.
The endpoint formatter only accepts catalogued nodes. It does not establish that
an endpoint exists; descriptor discovery must validate that in plan 07.

## Commissioning and recovery

The owner checks capacity before allocation. The existing allocator commits its
next-node counter before handing out an ID. The catalog then reserves that ID
before issuing PASE/fabric work. A crash between those two commits may create a
gap; it never permits reuse. A failed catalog commit prevents accessory work.
Successful fabric and Thread milestones advance the record. Only successful
CASE/CommissioningComplete permits phase 4 and a successful session result.

The reservation guard marks phase 5 when its future fails, is cancelled, times
out or is replaced by a new attach. A storage failure during this cleanup marks
the catalog fatal; the production service exits rather than continuing to
commission on uncertain durable state. Startup changes any remaining phases
1–3 to phase 5 once. Subsequent loads, reads and status observations do not
rewrite complete/recovery records. An orphaned catalog without its authority
never causes creation of replacement credentials.

Recovery-required records are retained and consume capacity; they are not
silently erased or assumed uncommissioned. Remote CommissioningComplete may
succeed immediately before a process dies, so recovery must inspect the same
authority/node rather than reuse its identity. On startup, a legacy authority
without a catalog remains pending until Thread is ready. The service browses
`_matter._tcp.default.service.arpa.` through OpenThread's DNS client, filters
instance labels to the current compressed fabric and IDs below the persisted
next-node counter, then commits only those live IDs as `RecoveryRequired`. The
normal authenticated CASE descriptor scheduler then decides which entries are
usable; no entry is fabricated as complete. Historical allocation gaps
therefore do not occupy catalog space. More than 64 matching operational
services remains outside the bounded catalog and is rejected without
committing a partial list. Browse errors leave the catalog absent and retry
with a capped delay; a successful empty browse commits an empty catalog.
Commissioning is disabled until reconciliation commits, preventing new IDs
from racing the migration. A crash before commit safely repeats the browse.

## Verification scope

Service-feature library tests, the actual running service IPC fixture and the
production-owner encrypted fixture exercise this implementation. Catalog tests
cover completion/reload/stable identity/no read writes, cancellation/crash
recovery, capacity, invalid phase order, wrong authority/counter/corruption,
cleanup storage failure and bounded legacy migration. An identity regression
rejects orphaned catalogs without changing credentials. The running service fixture independently decodes TMKV and
the catalog to verify three distinct recovery-required IDs after reattach,
cancellation and failed GATT writes. The encrypted owner fixture asserts the
completed catalog entry before testing persisted authority and fresh CASE.

These remain synthetic radio/accessory checks. The original encrypted fixture uses explicit rendezvous. A newer
`--service-discovery` mode exercises production SRP/DNS lookup; remote recovery,
descriptors, subscriptions and native device/report integration remain open.
No physical gateway/accessory acceptance is claimed.

Thread discovery must use SRP/DNS-SD over the userspace Thread network rather
than assume multicast DNS on the mesh. OpenThread's pinned API provides SRP
server and DNS client functionality; their build/runtime integration is now
implemented. See [operational discovery](matter-operational-discovery.md). References: [OpenThread SRP](https://openthread.io/reference/cli/concepts/srp)
and [Thread discovery architecture](https://openthread.io/codelabs/openthread-border-router).

Verified commands — 2026-10-04:

```sh
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/thread-matter/target" cargo +nightly-2026-10-02 test \
  --locked --release --manifest-path thread-matter/Cargo.toml \
  --features controller-service --lib
bash thread-matter/scripts/build.sh host service
bash thread-matter/scripts/build.sh host commissioning
python3 thread-matter/scripts/test-openthread-matter-transport.py --commissioning --ble --service-owner
python3 thread-matter/scripts/test-matter-service.py
bash thread-matter/scripts/build.sh mips service
bash thread-matter/scripts/build.sh mips commissioning
```

All 48 service-feature tests pass. A separate `controller-identity` feature-only
library run passes 23 tests, verifying catalog/authority support without service
IPC or OpenThread. Both host builds, the encrypted owned fixture
and the running-service catalog assertions pass. Initial sandboxed Unix socket
checks failed with permission errors; the elevated retries passed. Both stripped
artifacts pass ELF verification for static big-endian MIPS-I/o32 soft-float. The
service is 1,748,648 bytes and the commissioning probe is 2,084,544 bytes; both
increased 8,192 bytes from the preceding checkpoint. These are binary sizes,
not target RSS or CPU measurements. Hashes/scope are recorded in
[machine-readable validation](matter-node-catalog-validation.json). Existing
MIPS linker abicalls/non-abicalls warnings remain; cross-build/ELF checks do not
replace plan 08 physical execution. No deployment occurred.

## DNS-SD legacy migration checkpoint — 2026-10-05

Rust commit `9e0922e` initially migrated every allocated ID and failed closed
above the catalog's 64-entry bound. The current implementation defers migration
until OpenThread is running, browses actual operational Matter instances and
filters by compressed fabric plus allocator high-water. `Catalog::migrate_legacy`
accepts only sorted, unique, in-range IDs; it persists them as
`RecoveryRequired`, including a valid empty result, before enabling new
commissioning. A DNS timeout or browse error does not write an empty catalog
and uses capped retry delay. The fixed C ABI caps matching results at 64 and
signals overflow instead of truncating. Host regression covers sparse IDs up to
high-water 100,003, and the encrypted SRP fixture verifies the Browse callback
returns the registered service's node.

Validation: all 76 controller-service tests pass when run with Unix socket
permissions enabled; the Browse SRP/CASE fixture passes in 16.4 seconds on the
development host. The static MIPS-I production service cross-build passes at
2,129,600 bytes, SHA-256
`1eecdc912a00d22c1f7a1557f090fdaf2916a7a2f9accc72e5d3d4465f17ce82` (20,480
bytes over the previous checkpoint). Browse uses a fixed 520-byte result
buffer and scans at most 256 labels per response; no gateway execution or
reset-sensitive workload was run. Physical filesystem and power-loss
verification remain excluded in plan 08.
