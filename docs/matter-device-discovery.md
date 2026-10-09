# Operational descriptor discovery

`micro-thread-matter-daemon/src/devices.rs` reads root Descriptor PartsList over operational
CASE, then reads Descriptor ServerList for endpoint zero and every advertised
endpoint. It copies response data before the SDK releases its response buffer.
The result contains only advertised endpoint and server-cluster IDs; no device
type or capability is inferred from a product name.

The snapshot has at most 32 endpoints including root and 64 server clusters per
endpoint. Duplicate entries, endpoint zero in PartsList, the wildcard endpoint,
malformed lists, missing Descriptor support and excess capacity fail explicitly.
IDs are sorted for stable snapshots. A failed read returns an error rather than
publishing a partial snapshot. Each attribute read has its own exchange.

## Service scheduler

The service now polls a `devices::Manager` alongside transport, discovery and
IPC. It loads only Complete catalog entries and observes newly completed nodes
without a restart. The catalog's limit of 64 nodes bounds retained snapshots.
Only one discovery job runs at a time, with a total 30-second deadline. A
successful snapshot refreshes after 300 seconds. Failures mark it offline and
retry after 5, 10, 20, 40, 80, 160 and finally 320 seconds. Old descriptors remain
available as cached metadata, with `online=false`; no partial result replaces
them. Oldest due jobs run first so an unavailable node does not block others.

Commissioning preempts discovery by dropping its future. Radio detach also
cancels the job, marks snapshots offline and makes them due for refresh when
attachment returns. The manager does not mutate flash, promote incomplete
catalog entries or assume interrupted commissioning succeeded. Worst-case
cluster-ID storage alone is 524,288 bytes (64 nodes × 32 endpoints × 64 IDs × 4),
plus vectors and bookkeeping; target CPU/RSS remain unmeasured.

## Catalog recovery

Legacy catalogs are now initialized from the persisted authority node allocator:
all prior IDs up to the saved high-water mark become `RecoveryRequired` and are
validated by the existing production DNS-SD/CASE scheduler. Migration is
bounded to 64 allocated IDs; larger missing-catalog histories fail closed
without writing. See [catalog migration details](matter-node-catalog.md).
Address invalidation after an established CASE failure and administrative
pruning of unreachable migrated entries remain open.
Host fixture coverage does not establish real accessory interoperability;
plan 08 remains excluded.

## Paginated snapshots

Opcode 10 now exposes cached catalog/endpoint snapshots before radio startup and
while running. [The snapshot contract](matter-device-snapshot-ipc.md) defines
one endpoint per page, stable IDs, incomplete-node placeholders and revision
validation across pagination. Revisions start from OS randomness, advance only
for observable changes, and fail closed on exhaustion. Worst-case pages with
64 full-width cluster IDs fit the 2048-byte IPC limit. Reads do not write flash
or wait for accessory traffic.
