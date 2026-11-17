# Zigbee topology queries

`POST /api/devices/<ieee-address>/topology` starts read-only Zigbee ZDO
Management LQI and Management Routing requests for one known Zigbee device.
The coordinator accepts at most eight concurrent device queries. A successful
HTTP response only confirms that the requests were queued. Each request is
paginated independently, with a 20-second timeout per page. Sleepy devices may
not respond. Query results are held in memory and discarded on restart.

Poll `GET /api/status` and locate the device in `topology`. The
`neighbors_status` and `routes_status` fields are `waiting`, `complete`,
`timed out`, `unsupported or rejected`, or `malformed response`. Neighbor
entries include IEEE and network addresses, Zigbee device type, relationship,
depth and LQI as reported by the queried device. Route entries include the
network destination, next hop, and Zigbee route status. Partial entries may be
present after a timeout or failure; only a `complete` table is a full response.
Querying again replaces previous results. Read-only viewer access can inspect
status; starting a query requires administrator access.

The Topology tab shows devices with at least one completed table, connecting
responding devices where a neighbor table reports a relationship. A reported
neighbor link is directional and does not establish which copy of a multicast
reached a device. Network addresses and routes can change, and some devices do
not support these queries. Matter inventory does not expose Thread neighbor or
routing tables through the current service, so it is not shown as topology.
