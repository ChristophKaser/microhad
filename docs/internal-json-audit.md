# Internal JSON audit

JSON is permitted for JSON files, REST endpoints, and required WebSocket messages.
The inventory below records the remaining boundary conversions and the typed
representations used within the runtime.

- Electrical measurement metadata is held as typed integer fields in MicroHAD.
  JSON is produced by the status endpoint, not used to store metadata.
- Matter worker status and job responses are held as typed protobuf messages;
  the worker serializes them for HTTP clients and WebSocket peers.
- Matter OTA control uses a length-prefixed binary protocol over its private
  Unix socket. Its JSON status remains confined to the HTTP API.
- Matter time-zone preferences use a bounded binary representation in the
  credentials store. Loading still accepts the old embedded JSON encoding so
  existing installations retain their preferences; new writes use binary.
- Thermostat schedule details are held as binary transition data. Persistent
  device JSON is decoded at file load and written when saving; dashboard JSON
  is generated for the HTTP response. Lua receives typed tables for cooling
  and mixed schedules; heat-only schedules retain their legacy text form.
- Matter thermostat schedule commands sent by MicroHAD use the structured
  protobuf `DeviceCommandRequest.schedule` field. The service validates the
  typed values before operation. Legacy REST schedule JSON is parsed at the
  HTTP boundary, and the service rejects JSON schedule strings over IPC.
- Lua thermostat actions carry schedule tables through coordinator validation
  and dispatch as typed transitions.
- The thermostat UI retains submitted schedules as objects and encodes them
  only when sending an HTTP request. Dashboard trigger selection uses an
  encoded identifier instead of serializing an array as JSON.
- The automation worker calculates encoded lengths for shared-state and log
  limits without constructing JSON; it serializes when writing state files.
- Gateway settings are held as typed snapshot fields and serialized on HTTP
  reads. Status responses also use a short-lived cached HTTP response body; it
  is not parsed or exchanged between services.
- The Matter WebSocket relay parses and serializes JSON as required by that
  external interface, then carries typed requests to the Matter worker.
  Test-only JSON fixtures and file-based configuration or
  persistence are external boundaries, not internal wire formats.
- Configuration, dashboard, backup, notification, and automation-state JSON
  is consumed from or written to files. Shelly, Firebase, browser push, and
  firmware-catalog JSON is handled at their HTTP interfaces. The Matter Rust
  JSON wire adapter is compiled only for tests and the commissioning probe.
- The Ziggurat multipan source and repository-owned firmware sources contain
  no runtime JSON serializer or parser.
