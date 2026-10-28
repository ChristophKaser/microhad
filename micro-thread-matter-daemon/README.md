# Thread/Matter controller service

`mthread-matter` owns the gateway's Matter fabrics, commissioning, sessions,
subscriptions and Thread state. It uses the pinned rs-matter dependency and an
OpenThread C ABI adapter built from upstream OpenThread v2026.10.0.
MicroHAD consumes its private IPC API and applies devices and reports to the common dashboard model.

Configure compiler, gateway and SSH credentials in the private root
`workspace.local.env`; see [shared host settings](../docs/workspace-settings.md).
Host tools load it automatically. Device-only commands inherit the configured
gateway; dual-mode fixtures require explicit `--host` or `--device` (without a
value to use `GATEWAY_IP`). Omit that flag for a host-only run.

From the repository root:

```sh
bash micro-thread-matter-daemon/scripts/build.sh host service
bash micro-thread-matter-daemon/scripts/build.sh mips service
bash micro-thread-matter-daemon/scripts/build.sh host commissioning
bash micro-thread-matter-daemon/scripts/build.sh host transport
```

The default is `host service`. Production outputs are
`build/micro-thread-matter-daemon/target/release/mthread-matter` and
`build/micro-thread-matter-daemon/mthread-matter-mips`. Synthetic fixtures retain the names
`ot-matter-commissioning-probe` and `ot-matter-transport-probe`.
Always use the build wrapper before integration checks; plain Cargo builds can
leave the fixture harness using a stale executable.

MIPS builds select RSDK by default, or crosstool-NG without RSDK.
After `python3 scripts/build-toolchain.py`, run
`TOOLCHAIN=crosstool-ng bash micro-thread-matter-daemon/scripts/build.sh mips service`.
Its output is `build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips`;
OpenThread target artifacts are under `build/crosstool-ng/openthread-unix/`.
RSDK OpenThread uses Clang 18; crosstool-NG uses its native GCC/G++.
Host builds need neither MIPS SDK and keep their fixture paths. See
[toolchain selection](../config/TOOLCHAINS.md) for source credits and limits.

Build scripts prepare `build/micro-thread-matter-daemon/service-source/` from
`dependencies/rs-matter/` and apply the controller patch series. OpenThread
sources remain under `dependencies/openthread/`; generated adapter
archives and test fixtures are under `build/openthread-unix/` at the root.

Host checks include:

```sh
python3 micro-thread-matter-daemon/scripts/test-openthread-compatibility.py
python3 micro-thread-matter-daemon/scripts/test-matter-service.py
python3 micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py
python3 micro-thread-matter-daemon/scripts/test-rs-matter-controller.py
python3 microhad/scripts/test_matter_native_report_integration.py
```

The native integration builds a separate service artifact with test-only report
hooks. Production builds check that those hooks are absent.
See [runtime](../docs/matter-service-runtime.md).

OpenThread is pinned to release `v2026.10.0`, commit
`53e8322140dde7b26827332100504507cec9cc4e`. Its build initializes the nested
Mbed TLS dependency without requiring the Silicon Labs firmware installer.
The release and the GSDK 4.5.1 radio source both declare Spinel RCP API
version 11, minimum supported RCP API version 4, and protocol version 4.3.
Runtime compatibility checks remain enabled. Host synthetic tests validate the
shared-radio no-reset attachment. The deployed gateway uses the same radio path
for Matter-over-Thread temperature and air-quality sensors.

The private protocol is [length-prefixed protobuf v3](docs/matter-ipc-v3.md).
Only the length prefix is outside the protobuf envelope.

## Matter logging

`mthread-matter --log-level LEVEL` accepts `off`, `error`, `warn`, `info`,
`debug`, and `trace`, defaulting to `info`. Service and rs-matter diagnostics
use syslog under `mthread-matter`. Debug includes protocol exchanges and
retransmissions; trace also includes frame bytes and decoded payloads, which
can expose decrypted commissioning data. Payload tracing remains at trace.

The paired supervisor uses `MATTER_LOG_LEVEL` (default `info`). Change it for
its current lifetime with:

```sh
/tuya/native/paired_control.sh matter-log-level trace
/tuya/native/paired_control.sh matter-log-level info
```

The command gracefully restarts only Matter, interrupting its current sessions
and subscriptions. It leaves native and Ziggurat processes running and does not
consume the Matter failure-restart budget. The selected level survives child
restarts and resets to `MATTER_LOG_LEVEL` on a fresh supervisor launch or reboot.
An unchanged level is a no-op; stopped or quiesced services reject the request.
The existing `paired_control.sh log-level LEVEL` continues to select Ziggurat.

Each new phone BLE attachment discards Matter transport sessions for that BLE
address before starting a fresh PASE handshake. The owned attempt also removes
those sessions on completion, failure, timeout, cancellation, or reconnection;
operational sessions and sessions for other peers are retained. Info logs name
the current commissioning command (`pase`, `arm_fail_safe`, `regulatory_config`,
`csr_request`, and credential installation), and failures log that step with the
error. These diagnostics keep the phone's existing wire protocol compatible.

Messages stream through a bounded splitter without truncating long entries.
Bodies are at most 174 bytes, reserving space for the libc header and longer
Matter tag within the stock gateway's 255-byte syslog limit. Long entries use
Ziggurat's `[entry/part+]` continuation and `[entry/part=]` final markers, with
150-byte payload fragments. UTF-8 boundaries are preserved and NULs sanitized.
Logs stay in syslog's RAM ring; no flash log files are created.

After building the host service and synthetic relay, run
`python3 micro-thread-matter-daemon/scripts/test-matter-logging.py` for CLI and
syslog integration checks using simulated radio traffic and a host-only libc
capture shim. It does not access the gateway.

Binding diagnostics include Thread role and child attachment, SRP host/service
updates (including deletions and leases), SRP response counters, DNS query
server/instance and OpenThread error names. Binding milestones and these
low-volume events remain visible at `info`; routine DNS traffic outside binding
uses `debug`. DNS UDP observation is passive and logs only metadata, never
packet bodies, TXT data, network keys or datasets. Raw frame logging remains
separate. Binding start/end also snapshot IPv6 and MAC security error counters.

OpenThread's internal SRP server events also use `info`, including parser,
signature and ownership failures before the application callback, response
failures, lease expiry and removal. Accepted updates log every host address and
the full service instance name, so operational identifiers can be compared
with DNS requests. Binding boundaries and network-data changes log the complete
Thread network-data TLVs (in offset-labelled hex chunks), including advertised
SRP/DNS services. These are network data, not the secret operational dataset.
Commissioning allows twelve operational CASE/discovery attempts (normally
about sixty seconds of discovery), still bounded by the session deadline.

`--diagnostic-on-mesh-prefix` and `--diagnostic-external-routes` independently
enable Thread network-data advertisements. The first publishes a stable ULA
on-mesh `/64` derived from the Thread mesh-local prefix, with SLAAC and the
default-route flag when external routes are also enabled. The second publishes
`::/0` and a separate NAT64 `/96`. The paired launcher enables both. This is
diagnostic network data only: the gateway has no upstream IPv6 or NAT64
translator. Off-mesh packets delivered to the service are logged at `debug`
with addresses, protocol, size, full bytes and a drop reason, then discarded.
Packets OpenThread rejects before delivery log their addresses and reason. The
advertisements and registration results are logged at `info`. These flags do
not enable a host network interface or Linux packet forwarding.

For diagnosis after the fact, poll the gateway log API into a private host file:

```sh
python3 micro-thread-matter-daemon/scripts/capture-thread-logs.py \
  --output diagnostics/thread-session.log
```

Enable `/tuya/native/paired_control.sh log-level trace` for all available Spinel
RX/TX events, including the Thread IID, and `matter-raw-frames on` for Matter UDP
payloads. Keep Matter at `debug`; registration decisions remain visible at
`info`. Ziggurat's `raw-frames` setting alone covers Zigbee, not the Thread
interface. Trace settings restart the relevant supervised services and last
until the paired supervisor restarts. The host collector polls `/api/logs` once
per second, retries failed requests, and avoids repeating entries at the
inclusive timestamp boundary.
The RAM ring and asynchronous log queues are bounded: capture is best effort,
and overload/drop markers or connection gaps must be checked before treating
an absent event as evidence. Frames filtered or dropped by the RCP are not a
promiscuous over-the-air capture. No capture files are written to gateway flash.

To distinguish malformed accessory fragments from receive-path problems, replay
an authenticated plaintext 6LoWPAN FRAG1 payload (including its four-byte
fragment header):

```sh
python3 micro-thread-matter-daemon/scripts/replay-thread-fragment.py /path/to/fragment.bin
# Explicit isolated MIPS run; files and synthetic settings live only in /tmp:
python3 micro-thread-matter-daemon/scripts/replay-thread-fragment.py /path/to/fragment.bin \
  --device
```

Build the host or MIPS OpenThread transport archives and synthetic relay first.
The helper tests header decoding, full decompression, message conversion and
the first-fragment reassembly handler with synthetic secured receive metadata.
It does not replay radio authentication, production neighbor state, or the
complete datagram. Successful isolated replay therefore establishes validity
in that test context, not successful delivery through the live radio path.
First-fragment failures log the actual first 32 bytes, mesh-local prefix,
message length and failing stage at INFO. Stages include conversion, the
post-conversion size check, message extension and ingress filtering. Separate
bounded events retain the complete header within OpenThread's own log limit.
These allow comparing the live post-decryption header against the authenticated
offline capture. The synthetic `lowpan-oversized-first-fragment.hex` vector
exercises successful conversion followed by rejection at the message-size check.

The MIPS/Lexra OpenThread build uses portable Mbed TLS arithmetic. Its assembly
path rejected a valid captured P-256 SRP public key with `ECP_INVALID_KEY` even
though host and gateway calculated the same signed hash; portable arithmetic
verified the same update on the gateway. This changes the crypto implementation
without relaxing signature checks. The host build retains its normal settings.

The isolated signature regression checks a valid synthetic SRP SIG(0), a
tampered signature, and the exact signed hash:

```sh
python3 micro-thread-matter-daemon/scripts/test-srp-signature.py
python3 micro-thread-matter-daemon/scripts/test-srp-signature.py \
  --device
```

Build the relevant OpenThread transport archives and synthetic relay first.
The device test requires explicit `--device` and sufficient free RAM; it keeps
all probe files and synthetic settings beneath a private `/tmp` directory and
never accesses the production radio or state. `--fixture PATH` accepts a private
captured vector with hex `public_key`, `dns`, and expected `sha256` fields.

For explicitly requested deployment of a changed service, run from the root:

```sh
./deploy.sh --services matter --target /tuya/native \
  --matter-service build/micro-thread-matter-daemon/mthread-matter-mips
```

This example uses the RSDK artifact; select the crosstool-NG output path when
using crosstool-NG. Selective maintenance
retains the installed launcher and restarts the paired stack. Rollback copies
are opt-in via `--rollback-dir DIR`; existing backups are preserved.

## Sleepy-device traffic and subscription recovery

Incoming subscription reports use at most 256 concurrent asynchronous exchanges,
polled on the existing service thread. No report worker threads are created.
Exchange futures and their bounded payload buffers are allocated as reports
arrive; an idle service retains only one acceptor. Existing transport/session
limits still apply. An incomplete report or a delayed acknowledgement does not
block unrelated reports. Valid chunks refresh subscription liveness, and a
complete validated report reaches the device manager before the final response
acknowledgement. Capacity and receive failures produce rate-limited warnings.
An admitted exchange with no valid chunk progress for 60 seconds is reclaimed;
this does not change the negotiated subscription deadline or idle parent polls.

Descriptor interviews run at startup, after commissioning, for incomplete
metadata, and on explicit subscription recovery. A successful interview does
not schedule another timed interview. Transient failures retain verified
capabilities and recover operational addresses and subscriptions with backoff.
Topology changes are discovered after a service start or explicit recovery.
ICD features and idle durations are cached until metadata changes, a confirmed
startup event arrives, or explicit recovery invalidates them. Registered
Check-In devices retain wake-window gating; short-idle ICDs do not need
Check-In support to remain recoverable. Negotiated report intervals and the
response-time allowance remain authoritative for subscription liveness.

The original five-minute descriptor refresh (`378ae54`, October 4, 2026)
populated and refreshed persisted-device inventory. The original single report
future (`1d15bc9`, October 4) also serialized acknowledgement waits and chunked
reports. Subscription watchdog recovery (`ba8497a`, October 20) was added to
restore reports after accessory recovery, but it could also react to reports
that our busy receiver failed to accept within the transport's one-second
deadline. The ICD interval change (`492cb20`, October 25) matched matter.js
reporting semantics; lengthening those intervals would conceal the receiver
problem. The replacement removes recurring full interviews and allows reports
to progress independently while retaining genuine loss/reboot recovery.

Host regressions cover the 256-exchange cap on one calling thread, lazy
allocation, failed response acknowledgements, an hour of healthy reporting,
and targeted recovery. The synthetic CASE fixture also keeps one report
incomplete while another report arrives on the same authenticated session.
These checks use synthetic traffic and do not access the gateway.

## Licenses and attribution

Original project code is MIT licensed; upstream components retain their own terms.
See the repository-wide [third-party inventory](../THIRD_PARTY.md) for attribution,
notice generation and release requirements.

### Accessory clocks and daylight saving time

The dashboard's time-sync action saves the selected IANA time zone in the
controller's private store. The Matter service resolves that zone using bundled
AceTime C data for the dashboard’s 26 zones and sends UTC, the standard offset,
and bounded DST intervals to the
accessory. It refreshes clocks daily, at transitions, and after service startup,
without requiring an open dashboard. Failed updates retry after one minute,
backing off to at most one hour. When a configured accessory returns online,
its clock is synchronized again immediately through the operational queue.
Clock recovery subscribes to Basic Information `StartUp` and Time
Synchronization `TimeFailure` events, after reading the accessory's EventList
attributes to select supported paths. Live events request urgent reporting;
retained events in subscription priming recover missed restarts. Event numbers
suppress duplicate and historical replay. Re-establishing a lost clock event
subscription also schedules a sync if its startup event was evicted.

There is no minute-by-minute UTC polling. The event subscription requests a
ten-minute maximum keepalive interval; if the accessory loses subscriptions on
reboot, detection waits for the negotiated keepalive timeout and recovery, rather
than promising immediate notification. Daily/DST refresh and offline-to-online
recovery remain active. Accessories advertising neither event use those fallback
paths, with a warning instead of repeatedly subscribing to unsupported events.
The service logs the actual Basic Information and Time Synchronization EventList
IDs when establishing clock recovery. Event cursors and refresh deadlines stay
in RAM; no periodic gateway flash writes are added. Devices still need one
initial dashboard sync to save their chosen zone.

Accessories with room for multiple DST entries can change their clocks while
the gateway is offline, until their supplied schedule expires. Single-entry
accessories require the gateway at the transition. Time-zone rule updates ship
with service updates. After upgrading from fixed-offset synchronization, sync
each accessory once to save its zone; prior selections cannot be recovered.

Clock scheduling supports UTC dates from 2025-01-01 through 2099-12-31. The
lookahead is capped at 2100-01-01. Unsupported zone names or dates fail explicitly;
an unsupported saved zone is named in the startup error. Existing supported
preferences and IPC requests remain compatible. The processor uses one shared,
serialized workspace, independent of device count. No libc `TZ` changes or
32-bit `time_t` conversions are used.

The database is pinned to IANA 2025b and excludes unused zones and display data.
See [AceTime subset provenance](../third-party/acetimec/README.md) for regeneration
and offline validation. The half-hour DST test zone is a separate host fixture
and is excluded from the production service.
