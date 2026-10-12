# Gateway recovery and diagnostics

Application updates use [deployment](deployment.md). Radio replacement and
network restoration use [Zigbee migration](ziggurat-deployment-plan.md).
Keep configuration, network state, Matter identity and existing backups intact.
Use `/tmp` for transient gateway output; `/tuya` is flash.

## Check readiness and logs

From the host, with `GATEWAY_IP` set to your address:

```sh
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/status"
curl --connect-timeout 5 --max-time 8 -sS "http://$GATEWAY_IP/api/logs"
```

The dashboard reads BusyBox syslogd's 512 KiB RAM ring directly, with bounded
semaphore acquisition. It does not fork `logread`. Native and Matter use syslog;
Ziggurat tracing is forwarded to it. The deployed `logread` remains available
for console diagnostics. See [syslog IPC](../microhad/docs/syslog-ipc.md).
The paired launcher also forwards logs to a site-specific remote collector;
review that destination in `paired_start.sh` when setting up another site.

On the **gateway**, temporarily select a log level:

```sh
/tuya/native/paired_control.sh log-level debug
/tuya/native/paired_control.sh raw-frames on
/tuya/native/paired_control.sh matter-log-level debug
/tuya/native/paired_control.sh matter-raw-frames on
```

Levels are `off`, `error`, `warn`, `info`, `debug`, `trace`. The first command
restarts Ziggurat; Matter can also restart if its radio transport exits. The
second gracefully restarts only Matter, interrupting sessions/subscriptions
without consuming its failure budget. Levels last for the current supervisor
and reset to `ZIGGURAT_LOG_LEVEL` / `MATTER_LOG_LEVEL` (normally `info`) on a fresh
launch. Return to `info` after diagnosis. Matter trace can expose decrypted data.

Ziggurat's asynchronous logger queues at most 16 complete entries of up to
16 KiB each. A dedicated thread fragments and writes them to syslog, so a slow
syslog receiver cannot block the radio tasks. On overload it drops whole entries
and reports the number lost. Existing fragment markers and host collection remain
compatible. Event formatting still costs CPU; full `trace` can perturb timing.

For Zigbee packet investigation, the updated Ziggurat binary accepts
`--log-level info --raw-frames`. This enables only the `ziggurat_raw` trace target
alongside normal informational logs, without verbose stack or Spinel trace.
RX records contain PSDU bytes including FCS, channel, RSSI, LQI, radio timestamp,
and the radio's metadata flags (`0x0010` means ACK with frame-pending set).
TX records contain the submitted PSDU including FCS and the subsequent radio
result (`Ok`, `NoAck`, or `CcaFailure`), correlated by MAC sequence number.
TX timestamps describe host submission, not measured over-the-air transmission.
Hardware-generated ACKs are not individually captured. An `Ok` result confirms
an ACK only when the submitted frame requested one.

With the updated supervisor and control helper, use
`/tuya/native/paired_control.sh raw-frames on` or `raw-frames off` to restart
Ziggurat with packet capture enabled or disabled. The setting is independent
of the ordinary log level and survives child restarts for the supervisor's
lifetime. A fresh supervisor starts with `ZIGGURAT_RAW_FRAMES` (`off` by default;
accepted values are `on` and `off`). Repeating the current setting does not
restart anything, and diagnostic switches do not consume the failure restart
budget. As with a log-level change, the native daemon reconnects and Matter
may restart if its radio transport exits. Active state is recorded as
`active-raw-frames` in the private runtime control directory.

The binary option can also be selected with `RUST_LOG=info,ziggurat_raw=trace`.
The switch controls `--raw-frames`; an explicit `RUST_LOG` directive enabling
the raw target or a general `trace` level still produces raw records. These
features require the updated binary and supervisor to be deployed. The helper
refuses a raw-frame request against an older supervisor without the active
state file, rather than restarting it with an unsupported request.

Matter uses the same bounded asynchronous output design, retaining syslog
severity and the existing 174-byte fragment limit. The updated service accepts
`--log-level info --raw-frames` for packet-only diagnostics. It records incoming
and outgoing OpenThread UDP payloads, peer IPv6 addresses and ports, and host
timestamps in microseconds under the `matter_raw` target. Outgoing records
include the OpenThread send result: `0` means local acceptance, not an accessory
acknowledgment. Payloads are recorded at the transport boundary, before Matter
decryption on receive and after Matter encryption on send; unsecured Matter
messages remain unsecured. These are UDP records, not 802.15.4 radio frames or
Thread routing captures. Formatting still costs CPU, and overload drops whole
entries with an explicit warning.

Use `/tuya/native/paired_control.sh matter-raw-frames on` or `matter-raw-frames off`
to restart only Matter with capture enabled or disabled, independently of its
normal log level and the Zigbee capture switch. This does not consume the Matter
failure restart budget. The setting survives child restarts for the supervisor's
lifetime, resetting to `MATTER_RAW_FRAMES` (`off` by default, `on` or `off`) on a
fresh supervisor. Active state is recorded in `active-matter-raw-frames`. Both
the updated service and supervisor must be installed to use this control.

## Supervisor and startup

`/tuya/tuya_start.sh` starts the paired supervisor once. Its private control
directory defaults to `/tmp/ziggurat-supervisor`. Native failures restart the
native child; unexpected Ziggurat failures recover the pair. Matter has its own
bounded restart budget. Inspect the supervisor PID and actual processes before
stopping or cleaning runtime paths.

Startup refuses live or unsafe socket paths. It removes stale socket names only
when `/proc/net/unix` proves they are inactive. Never unlink a live socket or
delete a control directory merely to bypass a duplicate-supervisor error.

The supervisor restores a one-use RNG seed from
`/tuya/native/ziggurat-rng.seed` when the optional helper is present. It saves a
fresh seed after 180 seconds once the kernel RNG is ready. Preserve readiness
checks; do not bypass entropy waits. The private 44-byte seed costs one small
flash write per successful boot. A missing helper/seed retains the normal wait.

## Orderly stop and frozen Zigbee export

Use this sequence when preserving final Zigbee security state for rollback.
Set `MIGRATION_PYTHON`, `GATEWAY_IP`, `PASSWORD_FILE` and private filenames on the
host; the normalized template is retained from migration.

On the **gateway**:

```sh
/tuya/native/paired_control.sh quiesce
```

Quiescence stops Matter/native, disables their respawn and leaves Ziggurat
available for export. Confirm acknowledgement before continuing. On the **host**:

```sh
"$MIGRATION_PYTHON" microhad/scripts/ziggurat_freeze.py \
  --template "$PRIVATE_TEMPLATE" --host "$GATEWAY_IP" \
  --password-file "$PASSWORD_FILE" \
  --bridge /tuya/native/gateway-maintenance \
  --quiesced --execute --output "$PRIVATE_FROZEN_STATE"
```

Require successful freeze, complete table counts and a saved new private file.
Native `ziggurat-state.json` lacks the normalized post-freeze evidence expected
by rollback preparation. On the **gateway**, only after export succeeds:

```sh
/tuya/native/paired_control.sh stop --frozen-snapshot-saved
```

The flag asserts that the operator saved the snapshot; it does not inspect the
host file. Wait for process cleanup and UART release. Resume with a fresh
`/tuya/tuya_start.sh` invocation, rather than removing the quiesce marker.
TERM to the supervisor is bounded shutdown, not proof of a frozen rollback point.

## Radio maintenance helper

Build with `bash firmware/scripts/build-maintenance.sh`. The installed helper
is `/tuya/native/gateway-maintenance`. These **gateway** commands require all
radio consumers and their supervisors to be stopped:

```sh
/tuya/native/gateway-maintenance check-free /dev/ttyS1
/tuya/native/gateway-maintenance bootloader-enter /dev/ttyS1 --exclusive-radio
/tuya/native/gateway-maintenance bootloader-run /dev/ttyS1 --exclusive-radio
```

Production accepts only `/dev/ttyS1`, requires root, checks `/proc` ownership
and fails on incomplete inspection. `bootloader-enter` sends Spinel reset reason
3 at 460800/RTS-CTS, then recognizes the Gecko menu at 115200/no-flow. It does
not upload or run an image. `bootloader-run` sends menu option 2 only.
Serial actions have a 25-second watchdog. An unresponsive RCP cannot use this
software entry path; hardware recovery remains unproven.

`check-owner /dev/ttyS1 PID` requires exactly that verified serialgateway owner.
`cobs-hello SOCKET` checks protocol-v2 Hello within five seconds;
`cobs-bridge SOCKET` forwards stdin/stdout with a 120-second watchdog. COBS tools
require a same-user mode-0600 socket and never open the UART.

## Event-loop and CPU diagnostics

Toggle native event-loop counters with form-encoded
`POST /api/debug/event-loop`, `enabled=true` or `enabled=false`. Enabled logging
adds an `EVENT_LOOP_DEBUG` line about every five seconds to the existing RAM
ring. Disable it after collecting a few intervals.

For a hot thread, build the register sampler on the host:

```sh
source scripts/toolchains.sh
"$TARGET_CC" -Os -static -o /tmp/gateway_cpu_sampler microhad/tools/gateway_cpu_sampler.c
```

Copy it to gateway `/tmp`, find the task ID with `top`, then run
`/tmp/gateway_cpu_sampler TID [samples]` there. It briefly ptrace-stops the task,
reads MIPS EPC and detaches; the default is 100 samples, bounded to 1–1000.
Translate addresses on the host with `addr2line -f -C -e MATCHING_UNSTRIPPED_BINARY`.
The unstripped binary must match the running revision. Counts estimate where
sampled time was spent; they are not precise CPU percentages.
