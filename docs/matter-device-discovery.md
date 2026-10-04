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

This is a reusable discovery helper exercised by the synthetic accessory probe.
It is not yet scheduled by the service or exposed through device IPC/API.
Production node recovery, device types/features, overall job deadlines, retry
policy, capability snapshots and native controls remain open in plan 07.
The helper does not write flash or mark catalog recovery complete. Host fixture
coverage does not establish real accessory interoperability; plan 08 is excluded.
