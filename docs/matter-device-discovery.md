# Operational descriptor discovery

`micro-thread-matter-daemon/src/devices.rs` reads root Descriptor PartsList over operational
CASE, then reads Descriptor ServerList and ClientList for endpoint zero and every advertised
endpoint. It copies response data before the SDK releases its response buffer.
The result contains only advertised endpoint and server/client-cluster IDs; no device
type or capability is inferred from a product name.

The snapshot has at most 32 endpoints including root and 64 clusters per list per
endpoint. Duplicate entries, endpoint zero in PartsList, the wildcard endpoint,
malformed lists, missing Descriptor support and excess capacity fail explicitly.
IDs are sorted for stable snapshots. A failed read returns an error rather than
publishing a partial snapshot. Each attribute read has its own exchange.

## Service scheduler

The service polls a `devices::Manager` alongside transport, discovery and
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
catalog entries or assume interrupted commissioning succeeded. Cluster lists are bounded by node/endpoint/list capacities, but vector storage
and bookkeeping also consume memory; target CPU/RSS require workload measurement.

## Catalog recovery

A missing legacy catalog is reconciled through bounded operational DNS-SD browse
and authenticated CASE recovery. See [catalog recovery](matter-node-catalog.md).
Incomplete nodes are not assumed commissioned. Physical accessory recovery and
resource/RF behavior require [hardware acceptance](../microhad/docs/matter-plans/08-real-device-acceptance.md).

## Paginated snapshots

Opcode 10 exposes cached catalog/endpoint snapshots before radio startup and
while running. [The snapshot contract](matter-device-snapshot-ipc.md) defines
one endpoint per page, stable IDs, incomplete-node placeholders and revision
validation across pagination. Revisions start from OS randomness, advance only
for observable changes, and fail closed on exhaustion. Worst-case pages with
64 full-width cluster IDs fit the 2048-byte IPC limit. Reads do not write flash
or wait for accessory traffic.
