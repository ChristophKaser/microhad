# Routing transition diagnostics

The route diagnostics patch adds info-level `ROUTE_INVALIDATED` records with
`destination`, `next_hop` and `reason` on the same line. Reasons are
`link_status_lost`, `link_status_timeout`, `unicast_delivery_failed` and
`relay_delivery_failed`. These records describe active routing-table entries
that actually became inactive; they do not imply a device left its network.

`NEIGHBOR_LINK` records identify the neighbor by network and IEEE addresses and
show its outgoing cost before and after a change. Link Status changes also
include the advertised entry count and fragment flags. Timeout transitions
retain warning severity and replace the previous full neighbor-table dump.
Unchanged advertisements retain the existing debug packet logs without new
info records. A zero outgoing cost means the link is unusable for routing.

Existing parent-movement, child-expiry, join/rejoin, remote-button and scene
logs remain the evidence for tracing a remote through its parent and gateway
automation. Existing received Network Status logs already name the reported
destination and status. The new records supplement those logs; they cannot
explain why another router stopped receiving radio traffic.

Build the synthetic host server as documented in README.md, then run:

```sh
python3 ziggurat/scripts/test-route-diagnostics.py
```

The test reuses the authenticated neighbor recovery fixture. It establishes a
route, injects link loss/recovery, and checks that the invalidated destination,
next hop and reason are correlated, with cost transitions and no unchanged-cost
records. It uses a loopback radio and synthetic keys, without device access.
