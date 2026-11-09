# Internal JSON audit

JSON is permitted for JSON files, REST endpoints, and required WebSocket messages.
The following inventory records runtime conversions and their status while the
internal representation is being migrated; it is not a claim of completion.

- Electrical measurement metadata is held as typed integer fields in MicroHAD.
  JSON is produced by the status endpoint, not used to store metadata.
- Matter worker status and job responses are held as typed protobuf messages;
  the worker serializes them for HTTP clients and WebSocket peers.
- Matter OTA control uses a length-prefixed binary protocol over its private
  Unix socket. Its JSON status remains confined to the HTTP API.
- Thermostat schedule details are held as binary transition data. Persistent
  device JSON is decoded at file load and written when saving; dashboard JSON
  is generated for the HTTP response. Lua receives typed tables for cooling
  and mixed schedules; heat-only schedules retain their legacy text form.
- Matter thermostat schedule commands sent by MicroHAD use the structured
  protobuf `DeviceCommandRequest.schedule` field. The service validates the
  typed values before operation. Legacy REST schedule JSON is parsed at the
  HTTP boundary, and the service rejects JSON schedule strings over IPC.
- Lua thermostat actions still accept JSON schedule strings. Migrate these to
  typed Lua tables carried through coordinator validation and dispatch.
- The automation worker calculates encoded lengths for shared-state and log
  limits without constructing JSON; it serializes when writing state files.
- Gateway settings are held as typed snapshot fields and serialized on HTTP
  reads. Status responses also use a short-lived cached HTTP response body; it
  is not parsed or exchanged between services.
- The Matter WebSocket relay parses and serializes JSON as required by that
  external interface, then carries typed requests to the Matter worker.
  Test-only JSON fixtures and file-based configuration or
  persistence are external boundaries, not internal wire formats.

This list is incomplete until all runtime serializers, parsers, and cached JSON
values across MicroHAD, Matter, Ziggurat, and firmware have been classified.
