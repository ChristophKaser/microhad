# Host Test Plan

This is the complete automated, host-only test inventory for the gateway.  It
is a backlog as well as a coverage contract: every row must eventually be
implemented by `make host test`, with deterministic fakes for serial, time,
filesystem, TLS, HTTP, FCM, and Browser Push.  It deliberately excludes
physical coordinator and radio interoperability testing.

## Conventions

- **Level:** `U` is a unit test, `I` is a process/component integration test,
  `H` is a loopback HTTP/TLS integration test, and `B` is a headless-browser
  integration test.
- **Status:** `E` means the current suite covers the stated behavior at least
  once; `P` means partial coverage; `G` is an unimplemented gap.  `E` is not a
  reason to omit the listed error variants if they are marked `P` or `G`.
- Tests must use temporary runtime directories below `/tmp`; they must never
  require a device, external network service, credential, production TLS key,
  or persistent state.  Simulated time is required for all timeout, retry,
  join-window, reminder, and periodic-refresh tests.
- Assertions must verify observable results: return/error value, persisted
  data, emitted serial frame, outbound request, gateway view, or HTTP response.
  Do not assert private implementation details where an observable contract is
  available.

## Build, CLI, and lifecycle

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| LIF-01 | U | Valid default and `--config PATH` invocation | Selected configuration is read; defaults apply to omitted optional fields | G |
| LIF-02 | U | Invalid CLI arity or option | Exit code 2 and usage text; no daemon components start | DONE |
| LIF-03 | U | Missing, unreadable, empty, malformed, or wrong-type configuration fields | Clear startup failure; no leaked worker, runner, listener, or descriptor | G |
| LIF-04 | I | Optional config boundaries: serial baud/ports outside valid integer range, duplicate listener ports, unusable runtime path | Startup rejects invalid values or reports bind/open error safely | G |
| LIF-05 | I | Runtime directory absent, already present, readonly, or a non-directory | Create/reuse only valid directories; propagate failure without partial state | DONE |
| LIF-06 | I | Serial path absent, permission denied, invalid baud, and start/stop twice | Error is propagated; stop is idempotent and closes resources | G |
| LIF-07 | I | HTTP-only, HTTPS-only configuration error (one TLS path), valid TLS pair, unreadable/invalid cert/key | Correct listener selection; pair is mandatory; failure cleans all started components | P |
| LIF-08 | I | SIGINT/SIGTERM and ignored SIGPIPE during normal request writes | Main loop exits cleanly; peer close cannot terminate the process | G |
| LIF-09 | I | Host test build and target verification contract | `make host test` is self-contained; target check rejects non-MIPS/non-big-endian output | G |

## ASH transport, serial I/O, and EZSP coordinator lifecycle

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| ASH-01 | U | Encode/decode data containing each reserved byte | Exact escaping, CRC, control byte, and payload round trip | DONE |
| ASH-02 | U | Empty, consecutive, fragmented, and multiple frames in one feed | Only complete valid frames are popped in order | DONE |
| ASH-03 | U | Bad CRC, bad escape sequence, overlong/unbounded input, stray delimiters, and reset mid-frame | Invalid data is discarded; parser resynchronizes without unbounded memory | DONE |
| ASH-04 | I | Reset request/acknowledgement and reset timeout | Gateway emits reset, begins initialization only after valid ack, retries or reports timeout | DONE |
| ASH-05 | I | Serial short write, `EAGAIN`, interrupted read/write, EOF, and hard read/write error | Pending bytes are preserved when recoverable; offline/error state is published otherwise | G |
| ASH-06 | I | Data ACK, NAK, retransmit deadline, retry limit, duplicate frame, and sequence wrap | Correct ACK/retransmit behavior; duplicate payload is not applied twice | G |
| EZSP-01 | I | Supported version reply and negotiated-version reply | Correct protocol negotiation and subsequent initialization request | P |
| EZSP-02 | I | Unsupported, malformed, stale-sequence, failed-status, or absent version reply | Negotiation does not advance incorrectly; error/retry is observable | G |
| EZSP-03 | I | Coordinator initialization command sequence, token-write setup, coordinator IEEE lookup, and Tradfri bind group | Commands occur in required order and only after prerequisite responses | G |
| EZSP-04 | I | Initialization command failure, timeout, unexpected command, and delayed reply | No false online state; retry/failure is logged and deadlines remain bounded | G |
| EZSP-05 | I | Incoming APS callbacks with valid/invalid profile, cluster, endpoints, lengths, options, LQI/RSSI | Valid frames reach the correct handler; malformed/unsupported frames only log safely | G |
| EZSP-06 | I | Frame diagnostic logging | Decoded frame details and bounded hex fallback are available without leaking secrets | P |

## Network formation and joining

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| NET-01 | U | Save/load formed and unformed network state | PAN, channel, extended PAN, network/preconfigured keys round trip | DONE |
| NET-02 | U | Missing, malformed JSON, invalid hex/length, incomplete network state | Safe defaults or explicit load failure; no partially trusted keys | DONE |
| NET-03 | I | Create fresh network while online with confirmation | New material is generated, formation commands run, state becomes forming then formed and persists | G |
| NET-04 | I | Create network offline, while forming, RNG failure, EZSP failure, and formation timeout | Operation is rejected or fails cleanly without overwriting known-good state | P |
| NET-05 | I | Restart after formed network and after interrupted formation | Persisted formed state restores; interrupted state does not masquerade as formed | G |
| JOIN-01 | U | Offline join request | Rejected with coordinator-offline error and log entry | DONE |
| JOIN-02 | I | Secure join preparation policy replies all succeed | Join permit opens for requested duration; status shows preparing then deadline | G |
| JOIN-03 | I | Policy reply rejection, unexpected reply, each policy timeout, and duplicate join during preparation | Preparation cancels within ten seconds; dashboard can request joining again | G |
| JOIN-04 | I | Join extension, expiry, permit renewal, and permit-close failure | Correct deadline/renewal commands; status remains truthful | G |

## Devices, discovery, interview, and ZCL

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| DEV-01 | U | Device persistence with every dashboard-visible field and endpoint descriptor | Complete round trip, including defaults and special brightness value `-1` | DONE |
| DEV-02 | U | Missing/corrupt device data, invalid descriptors, duplicate IEEE/node data, bad numeric values | Invalid records are rejected/skipped deterministically; no crash | G |
| DEV-03 | I | Device announce/register, EUI64 mapping, rejoin at same/different node, node reuse | IEEE identity is stable; volatile data and node maps update correctly | P |
| DEV-04 | I | IAS enroll request and zone-ID allocation/exhaustion/persistence | Standard response is emitted; zone IDs are stable and exhaustion is safe | G |
| DEV-05 | I | Node descriptor, active-endpoint, simple-descriptor, basic-identity, bind, read, and reporting interview | Operations are sequenced, persisted, and classified from discovered capabilities | P |
| DEV-06 | I | Each interview response malformed, failed, delayed, duplicate, wrong transaction/tag, or absent | Correct operation alone completes; retry/pause/failure does not corrupt interview state | P |
| DEV-07 | I | Sleepy-device wake, contact closure during interview, refresh-only interview, rejoin migration | Paused work resumes correctly and avoids duplicate/obsolete operations | P |
| DEV-08 | I | Standard measurement reporting configuration and fallback refresh scheduling | Five-minute defaults, configurable whole-minute interval, `0` disable, serial refresh after response | P |
| DEV-09 | I | Refresh unknown device, no supported readable endpoint, busy interview, and queue/send failure | Meaningful rejection with no bogus state mutation | G |
| ZCL-01 | U | Temperature, humidity, occupancy, battery, on/off, and level reports | Values, flags, last-seen, and brightness conversion are correct | DONE |
| ZCL-02 | U | Xiaomi `0xff01` measurement data | Supported tagged values update only their respective measurements | DONE |
| ZCL-03 | U | IAS open/closed status and read-attribute response records | Contact state and standard read responses are normalized | DONE |
| ZCL-04 | U | Empty/truncated/wrong-type/out-of-range/unknown ZCL and IAS payloads | Returns false/no-op with no partial values or crash | DONE |
| ZCL-05 | U | Light-attribute classifier and command payload encoding | On/off, level, transition/default arguments and endpoint selection are exact | DONE |
| ZCL-06 | I | APS message dispatch for reports, reads, default responses, IAS, remote events, and irrelevant clusters | Each valid message updates one intended subsystem; unsupported traffic is harmless | G |

## Device controls, alerts, logging, and views

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| CTRL-01 | U | Device name, thresholds, contact timing, and sensor refresh settings | Valid settings persist and publish; defaults are retained when a setting is omitted | DONE |
| CTRL-02 | U | Unknown device, empty/oversize name, low >= high, invalid timing, overflow, negative/NaN thresholds | Request fails with no partial persistence | DONE |
| CTRL-03 | I | Light state/brightness control for on/off, dimmable, non-dimmable, unknown, and unsupported devices | Correct command or precise capability/brightness/state error | DONE |
| ALT-01 | U | Contact opens then closes before first delay | No alert is emitted; pending alert is cancelled | DONE |
| ALT-02 | I | Contact remains open through initial and reminder deadlines; duplicate open report | Exactly one first alert and scheduled reminders; duplicate reports do not reset incorrectly | DONE |
| ALT-03 | I | Temperature crosses low/high threshold, remains outside, returns inside, thresholds disabled/changed | Alerts occur only on entering configured alarm states | DONE |
| ALT-04 | I | Notifications disabled before contact/temperature/test event | No notification audit/outbox/queue/delivery path is touched | DONE |
| LOG-01 | U | Status and logs JSON with escaping, all feature fields, and immutable snapshot | Schema is valid, complete, stable, and a prior snapshot cannot change | DONE |
| LOG-02 | U | More than ring capacity, long/control-character messages, unknown device labels | Bounded RAM log retains newest valid entries and serializes safely | DONE |
| LOG-03 | I | Runtime log path, restart load, and failures writing transient files | Logging remains RAM-only under `/tmp`; no `/tuya` high-frequency writes | P |

## Scenes, groups, and remotes

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| SCN-01 | U | Save/load valid groups, members, scenes, states, and bindings | Configuration round trips exactly | DONE |
| SCN-02 | U | Empty/comment-only config and every malformed record, field count, ID/range, duplicate group/scene/state/member, bad brightness, unknown action | Configuration is rejected atomically with a useful error | P |
| SCN-03 | U | Binding references missing group/scene/remote; scene state missing/non-member; names with separators | Invalid cross-references and unrepresentable text are rejected | G |
| SCN-04 | I | Set valid configuration | Saves first, provisions native-capable members/groups/scenes, publishes view | G |
| SCN-05 | I | Activate/off unknown group/scene, empty group, offline/busy state, and send failure | No selected-state advance on failure; clear error is returned | P |
| SCN-06 | I | Native group/scene recall versus unicast fallback for unsupported lights | Correct multicast/unicast commands and per-light state outcomes | G |
| SCN-07 | I | Scene command ACK/failure/timeout, retry limit, queued retry, coalesced action, selected-scene runtime persistence | Retry state is bounded; newest intended action wins without stale execution | P |
| SCN-08 | I | Remote on/off/left/right events, held/duplicate/unknown event, no binding, and binding during pending scene | Correct mapped action; ignored events do not perturb selection | P |

## Storage, JSON, and notification delivery

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| STO-01 | U | Directory creation and every save path | Parent/create/write/rename failures propagate and leave previous file intact | G |
| STO-02 | U | JSON strings, numbers, escapes, Unicode pairs, malformed escapes, duplicate/missing fields, trailing data | Accepted JSON is decoded accurately; malformed input is rejected | DONE |
| NOT-01 | U | Notification worker initial enabled state, toggle, restart persistence | `notifications.json` is honored and updates atomically | DONE |
| NOT-02 | I | Start/stop twice, disabled enqueue, queue of 0/1/32/33 jobs, stop with queued work | Worker is race-safe; capacity/error/flush policy is deterministic | G |
| NOT-03 | I | FCM and Browser Push configured/unconfigured and one/both delivery failures | One transport failure cannot crash worker or prevent the other intended attempt | G |
| FCM-01 | U | Valid/minimal/missing/malformed Firebase account JSON, PEM escaping | Required account fields and private key are validated | DONE |
| FCM-02 | U | Base64url encode/decode padding, invalid alphabet/length, FCM message escaping, JWT claims/signing errors | Encodings and messages are exact; bad input returns errors | DONE |
| FCM-03 | I | Token cache hit/expiry/account change, token endpoint failure, FCM non-2xx/malformed response | Correct cache behavior and propagated/redacted error | G |
| PUSH-01 | U | VAPID key first create, reload, corrupted/partial keys, write failure | Stable valid public key or safe error; no accidental key replacement | DONE |
| PUSH-02 | U | Valid subscription, duplicate, invalid scheme/JSON/keys, limit boundary | HTTPS-only unique subscriptions; 32-entry bound and useful errors | DONE |
| PUSH-03 | I | Push request encryption/signing, bad CA/URL/connect/TLS/HTTP response, permanent rejection | Correct request or error; permanently rejected subscription is removed, transient one retained | G |
| HTTPS-01 | U | URL parser and HTTPS request wrapper | Valid host/path URL forms accepted; malformed/non-HTTPS URLs rejected without a connection | DONE |

## Runner, HTTP, TLS, and dashboard

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| RUN-01 | I | Runner start success/failure and stop before/after start | Startup error crosses thread boundary; resources and gateway ownership are correct | G |
| RUN-02 | I | Every command type: join, network, settings, refresh, light, scene config/activate/off, test notification, notification toggle | Command reaches only coordinator thread and relays result/error | G |
| RUN-03 | I | Concurrent readers and mutations, publish while lease held, release after stop, wake-pipe failure | Snapshot leases are safe/immutable; no deadlock, use-after-free, or direct gateway access | G |
| HTTP-01 | H | `GET /`, `/api/status`, `/api/logs`, `/api/scenes/config`, `/sw.js`, `/api/push/vapid-key` | Correct status, content type, body/schema, and missing-dashboard/config behavior | DONE |
| HTTP-02 | H | `POST /api/join`, `/api/network/create`, `/api/notifications/test`, `/api/notifications/enabled`, `/api/push/subscriptions` | Success and every body validation/coordinator/disabled/worker error serialize correctly | P |
| HTTP-03 | H | `POST /api/devices/{ieee}/settings`, `/refresh`, `/light` | URL decoding, valid command, missing body/name, invalid decimals/minutes/brightness/state, and gateway errors | DONE |
| HTTP-04 | H | `POST /api/scenes/config`, `/{group}/{scene}/activate`, `/{group}/off` | Valid dispatch plus malformed/missing/out-of-range IDs/actions/configuration | DONE |
| HTTP-05 | H | `POST /api/service/restart` | Reply is flushed before SIGTERM; supervisor-facing exit is graceful | G |
| HTTP-06 | H | Unknown routes/methods, malformed request line/headers, no terminator, partial reads, oversized request, duplicate/conflicting length, chunked transfer, invalid content length | 400 or 413 exactly as applicable; server continues serving later clients | DONE |
| HTTP-07 | H | Acceptor/workers under multiple simultaneous clients, peer closes during read/write, queue pressure, bind/listen failure | Request handling remains bounded and service survives client faults | G |
| TLS-01 | H | Valid loopback TLS status/API requests | Certificate, hostname validation, request, response, and HTTP parity work | DONE |
| TLS-02 | H | Invalid/mismatched cert/key, handshake failure, malformed encrypted request, oversize request, peer close | TLS resource cleanup and 400/413 behavior are correct; plain listener unaffected | G |
| UI-01 | B | Dashboard status/log polling and rendering of offline/forming/formed/join-preparing/joining states | Controls, countdown, badges, and errors reflect API response | G |
| UI-02 | B | Device cards, settings, refresh, light controls, descriptors, escaping, and empty states | Correct fields/actions are rendered; user input is encoded and API errors displayed | G |
| UI-03 | B | Network confirmation, notification toggle/test, restart prompt, service-worker/VAPID enrollment failure | Dangerous actions require confirmation; disabled/development/browser-unsupported paths are clear | G |
| UI-04 | B | Scene editor and text editor: add/remove groups/scenes/bindings, validation, serialization, API errors | UI prevents invalid drafts and sends valid configuration without unsafe HTML injection | G |

## Deployment and local development helpers

| ID | Level | Feature / stimulus | Expected result | Status |
| --- | --- | --- | --- | --- |
| DEP-01 | I | `deploy.sh` valid binary-only, webui-only, and both deployments against a fake SSH/SCP endpoint | Selects exactly the requested assets and preserves runtime configuration by default | G |
| DEP-02 | I | Missing/unreadable password, binary, dashboard, config, TLS, CA, or target inputs; unknown/incomplete flags | Exits 2 before any remote mutation and reports the invalid option/input | G |
| DEP-03 | I | `--config`, `--activate`, or `--restart` with webui-only; activate/restart outside `/tuya/native` | Rejects unsafe combinations before connection | G |
| DEP-04 | I | Transfer, staging, validation, rename, activation, and supervised-restart failures | Does not replace live assets until all selected transfers succeed; never reboots or alters Zigbee state | G |
| DEV-UI-01 | I | Local development server static files, `GET`/`POST` API proxy, request body/headers, upstream errors/timeouts | Serves dashboard safely, proxies only `/api/`, and relays appropriate upstream results | G |
| DEV-UI-02 | B | Dashboard on the development proxy | Browser-push enrollment is disabled and ordinary dashboard API actions use the proxied gateway | G |

## Completion criteria and implementation order

1. Split the current broad `unit_gateway.cpp` tests into behavior-focused cases
   while retaining the existing `test_main.cpp` target; add fakes for time,
   randomness, filesystem write faults, serial transport, outbound HTTPS, and
   notification delivery.
2. Extend `integration_gateway.cpp` with scripted ASH/EZSP sessions for every
   `I` row, including failures and deadlines.  Extend `integration_http_server.cpp`
   with route and malformed-transport matrices for every `H` row.
3. Add a host headless-browser target for `B` rows and make it hermetic using a
   loopback fake API; it must not contact the development gateway.
4. A row may move to `E` only when its test is deterministic, asserts the stated
   result, runs through `make host test`, and has no production credential,
   device, radio, external FCM, or external Push dependency.
