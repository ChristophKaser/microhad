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
usable; no entry is fabricated as complete. Prior allocation gaps
therefore do not occupy catalog space. More than 64 matching operational
services remains outside the bounded catalog and is rejected without
committing a partial list. Browse errors leave the catalog absent and retry
with a capped delay; a successful empty browse commits an empty catalog.
Commissioning is disabled until reconciliation commits, preventing new IDs
from racing the migration. A crash before commit safely repeats the browse.
