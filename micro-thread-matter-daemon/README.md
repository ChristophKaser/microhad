# Thread/Matter controller service

`mthread-matter` owns the gateway's Matter fabrics, commissioning, sessions,
subscriptions and Thread state. It uses the pinned rs-matter dependency and an
OpenThread C ABI adapter built from upstream OpenThread v2026.10.0.
MicroHAD consumes its private IPC API and applies devices and reports to the common dashboard model.

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

MIPS builds select RSDK by default, or experimental crosstool-NG without RSDK.
After `python3 scripts/build-toolchain.py`, run
`TOOLCHAIN=crosstool-ng bash micro-thread-matter-daemon/scripts/build.sh mips service`.
Its output is `build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips`;
OpenThread target artifacts are under `build/crosstool-ng/openthread-unix/`.
RSDK OpenThread uses Clang 18; the candidate uses its native GCC/G++.
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
See [runtime](../docs/matter-service-runtime.md) and
[physical acceptance](../microhad/docs/matter-plans/08-real-device-acceptance.md).

OpenThread is pinned to release `v2026.10.0`, commit
`53e8322140dde7b26827332100504507cec9cc4e`. Its build initializes the nested
Mbed TLS dependency without requiring the Silicon Labs firmware installer.
The release and the unchanged GSDK 4.5.0 radio source both declare Spinel RCP API
version 11, minimum supported RCP API version 4, and protocol version 4.3.
Runtime compatibility checks remain enabled. Host synthetic tests validate the
shared-radio no-reset attachment; physical radio compatibility requires a
separately authorized gateway acceptance run.

The private protocol is [length-prefixed CBOR v2](docs/matter-ipc-v2.md).
Only the length prefix is outside the CBOR envelope.

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

Deploy only a changed service with `./deploy.sh --services matter --target
/tuya/native --password-file ../ssh_password.txt --matter-service
build/micro-thread-matter-daemon/mthread-matter-mips`. Selective maintenance
retains the installed launcher and restarts the paired stack. Rollback copies
are opt-in via `--rollback-dir DIR`; existing backups are preserved.
