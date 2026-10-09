# Native API and dashboard

The source component is `micro-thread-matter-daemon/` and the executable is
`mthread-matter`. Private messages use [CBOR IPC v2](../matter-ipc-v2.md).
Public HTTP/WebSocket APIs remain JSON.

MicroHAD queues Matter mutations through its worker and serves protected status
snapshots. The coordinator thread owns common gateway/device state. Reports
arriving before endpoint registration are retained for later mapping; offline
readings do not fabricate measurements.

The [HTTP API](../matter-native-api-v1.md) and [dashboard](../matter-dashboard.md)
describe public JSON routes, jobs, controls and commissioning handoff. Phone
transport uses the owned WebSocket relay. Matter-disabled builds retain their
documented unavailable behavior.

Check admission limits, unavailable-service responses, session ownership,
reconnect/cancellation, control readback, nullable sensors and report-driven
presence/history. Run native host tests and MIPS builds for C++ changes; run the
Web UI build for UI changes. Physical acceptance must verify dashboard behavior
against a commissioned accessory while Zigbee remains healthy.
