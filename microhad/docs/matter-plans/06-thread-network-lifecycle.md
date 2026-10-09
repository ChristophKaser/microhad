# Thread network lifecycle

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

Thread uses the private Spinel socket supplied by Ziggurat; the Matter service
never opens the radio UART. A fresh protected coordinator observation gates
Thread creation/import, attachment and commissioning. The selected Thread
channel must match the Zigbee coordinator channel.

The service validates datasets before committing or applying them. Invalid or
stale observations, channel mismatch, radio detach and dataset-apply failure
block unsafe progress. Failed apply is suppressed until explicit retry or a
valid recovery transition. Radio failures retain committed state for restart.

Verify create/import/restore, no-write status reads, observation renewal/expiry,
channel mismatch, explicit retry, service exit on transport failure and dataset
restoration through the supervisor. The synthetic fixtures cover these paths;
physical coexistence, accessory attachment and sustained RF recovery require
[real-device acceptance](08-real-device-acceptance.md).
