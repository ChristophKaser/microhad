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
silently erased or assumed uncommissioned. Automatic operational discovery/CASE
reconciliation and administrative removal remain future plan 07 work. Remote
CommissioningComplete may succeed immediately before a process dies, so recovery
must inspect the same authority/node rather than reuse its identity. Legacy
stores without a catalog retain their allocator/authority but cannot infer the
state of old allocated nodes; an explicit migration/reconciliation path remains
open. No legacy node is fabricated as complete.

## Verification scope

Service-feature library tests, the actual running service IPC fixture and the
production-owner encrypted fixture exercise this implementation. Catalog tests
cover completion/reload/stable identity/no read writes, cancellation/crash
recovery, capacity, invalid phase order, wrong authority/counter/corruption and
cleanup storage failure. An identity regression rejects orphaned catalogs without
changing credentials. The running service fixture independently decodes TMKV and
the catalog to verify three distinct recovery-required IDs after reattach,
cancellation and failed GATT writes. The encrypted owner fixture asserts the
completed catalog entry before testing persisted authority and fresh CASE.

These remain synthetic radio/accessory checks. The encrypted fixture still uses
explicit rendezvous, and production operational discovery, remote recovery,
descriptors, subscriptions and native device/report integration remain open.
No physical gateway/accessory acceptance is claimed.

Thread discovery must use SRP/DNS-SD over the userspace Thread network rather
than assume multicast DNS on the mesh. OpenThread's pinned API provides SRP
server and DNS client functionality; their build/runtime integration is still
needed. References: [OpenThread SRP](https://openthread.io/reference/cli/concepts/srp)
and [Thread discovery architecture](https://openthread.io/codelabs/openthread-border-router).

Verified commands — 2026-10-04:

```sh
source scripts/rust-env.sh
CARGO_TARGET_DIR="$PWD/build/rs-matter/target" cargo +nightly-2026-10-02 test \
  --locked --release --manifest-path rs-matter-probe/Cargo.toml \
  --features controller-service --lib
bash scripts/build-rs-matter-transport.sh host service
bash scripts/build-rs-matter-transport.sh host commissioning
python3 scripts/test-openthread-matter-transport.py --commissioning --ble --service-owner
python3 scripts/test-matter-service.py
bash scripts/build-rs-matter-transport.sh mips service
bash scripts/build-rs-matter-transport.sh mips commissioning
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
