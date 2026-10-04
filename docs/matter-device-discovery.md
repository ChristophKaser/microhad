# Operational descriptor discovery checkpoint

`rs-matter-probe/src/devices.rs` reads root Descriptor PartsList over operational
CASE, then reads Descriptor ServerList for endpoint zero and every advertised
endpoint. It copies response data before the SDK releases its response buffer.
The result contains only advertised endpoint and server-cluster IDs; no device
type or capability is inferred from a product name.

The snapshot has at most 32 endpoints including root and 64 server clusters per
endpoint. Duplicate entries, endpoint zero in PartsList, the wildcard endpoint,
malformed lists, missing Descriptor support and excess capacity fail explicitly.
IDs are sorted for stable snapshots. A failed read returns an error rather than
publishing a partial snapshot. Each attribute read has its own exchange.

Verification on 2026-10-04:

- Service-feature library tests: 55 passed. The new list test checks unsorted
  values, duplicates, capacity, decode errors and empty lists.
- `bash scripts/build-rs-matter-transport.sh host commissioning` passed.
- `bash scripts/build-rs-matter-transport.sh mips commissioning` passed; existing
  MIPS abicalls linker warnings remain.
- `python3 scripts/test-openthread-matter-transport.py --commissioning --ble
  --service-owner --service-discovery` passed. The production helper discovered
  endpoints zero and one and endpoint one's OnOff server over actual encrypted
  CASE, with production SRP/DNS discovery. The harness requires the descriptor
  success marker. Existing OnOff/temperature and persisted-authority reopen
  checks also passed.

## Service scheduler checkpoint — 2026-10-04

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

Verification: 57 library tests pass, including deadline expiry, unavailable-node
fairness, commissioning preemption, radio invalidation, retry cap, incomplete-node
exclusion and unchanged credentials after scheduler operations. The encrypted
fixture now exercises the production manager with a reopened catalog, then
detach and refresh, comparing both snapshots to the descriptor helper's result.
The harness requires the scheduler success marker. Host service/probe and MIPS
service/probe builds pass; existing MIPS linker warnings remain.
`python3 scripts/test-matter-service.py` passes real service IPC/lifetime,
fabric and guarded Thread restore checks. The native repository's
`python3 scripts/test_matter_native_worker.py --real-service` passes persistent
heartbeat, queued mutations, owned relay callbacks, cancellation, radio loss
and unavailable-service responses. Stripped MIPS artifacts are 1,961,656 bytes
for the service and 2,301,648 bytes for the probe; neither was deployed.

Device IPC/API exposure, remote reconciliation of recovery-required nodes,
legacy catalog migration, device types/features and native controls remain open.
Host fixture coverage does not establish real accessory interoperability;
plan 08 remains excluded.
