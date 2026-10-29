# Gateway TCP core capture

A small static MIPS helper receives Linux `core_pattern` stdin and streams it to
`receiver.py`. It accepts every eligible crashing process, with no executable
filter. The receiver runs as a persistent host user service. Dumps and metadata
are private under ignored `diagnostics/core-dumps/`; they can contain keys.

## Current production limitation

The stock 3.10.90 gateway exposes `core_pattern` but appears built without
`CONFIG_ELF_CORE`: `elf_core_dump` is absent from its full kernel symbol table,
`/proc/PID/coredump_filter` is absent, and isolated dumpable ELF crashes invoke
no pipe helper, even with an unlimited core limit. Direct MIPS-helper TCP
transfers succeed. The control script refuses activation on this kernel and
leaves kernel settings unchanged (deployment returns status 3 if activation
is refused; `--stage-only` returns success after installation). Installing these files alone cannot make the
kernel generate cores. A kernel with ELF core support or a separate user-space
capture implementation is needed. Do not replace the production kernel as part
of installing this helper.

## Build, test and install

From the repository root:

```sh
bash scripts/core-capture/build.sh
python3 scripts/core-capture/test_capture.py
python3 ziggurat/scripts/test-fault-report.py
python3 scripts/core-capture/install-host.py --bind HOST_LAN_IP --gateway GATEWAY_IP
python3 scripts/core-capture/probe-device.py --host GATEWAY_IP --receiver HOST_LAN_IP --transport-only
# This intentionally crashes only a disposable /tmp fixture and restores sysctls:
python3 scripts/core-capture/probe-device.py --host GATEWAY_IP --receiver HOST_LAN_IP
# Explicit production installation, only when requested:
python3 scripts/core-capture/deploy.py --host GATEWAY_IP --receiver HOST_LAN_IP
# Stage without attempting activation (appropriate for the stock kernel):
python3 scripts/core-capture/deploy.py --host GATEWAY_IP --receiver HOST_LAN_IP --stage-only
```

The receiver defaults to TCP port 9999. `--port` overrides it in all commands.
Host service: `systemctl --user status gateway-core-receiver.service`.
Its generated unit is under `build/`, linked into the host user's default target.
The installer enables user lingering so the receiver also starts before login
and survives logout. The repository must
remain mounted. Network binding retries automatically after address loss.

The device configuration `/tuya/native/core-capture.conf` is plain `IPv4 PORT`,
not shell code. No destination is hard-coded in tracked files. The paired
launcher calls adjacent `core-capture-control.sh start` on MIPS; configuration
and binaries survive gateway reboot. Manual `stop` restores previous sysctls
without removing pending fallback data. Remove/rename configuration to disable
on future starts. Firmware radio tracing remains off.

## Bounds and failure behavior

Streaming uses 16 KiB chunks with **no dump-size or total-duration limit**.
Connection timeout is 3 seconds; socket and pipe inactivity timeouts are
10 seconds. The receiver writes directly to disk and fsyncs before returning a
single-byte saved acknowledgement. There is no checksum protocol. TCP delivery
alone cannot confirm that a receiver saved a file.

One private RAM fallback is retained beneath `/tmp/gateway-core-capture/`.
The **entire serialized fallback is at most 1 MiB**, including metadata and
framing. It contains the beginning of the dump and is marked truncated if it
exceeded that allowance. An older pending fallback is preserved; newer crashes
can still stream. A successful stream deletes only its own fallback. A retry
worker retries every 30 seconds, deleting pending data only after a saved ACK.
Loss of an ACK can produce duplicate host files. A reboot loses RAM fallback.
Flash stores only the installed executable, script and small configuration.

`core_pipe_limit=1` and an advisory lock serialize captures; concurrent crashes
can be skipped and are logged. The pipe limit also keeps `/proc/PID` available
while metadata is collected. Executable and maps metadata are bounded to one
16 KiB frame. Kernel dump eligibility rules still apply (e.g. non-dumpable
processes and ordinary termination/OOM kill do not produce cores). Pipe dumps
ignore `RLIMIT_CORE`; regular core files remain disabled in the launcher.

Host directories contain `process.txt`, `capture.json`, and either `core`,
`core.truncated`, or `core.partial`. Truncated and interrupted transfers are
explicitly identified. A truncated ELF may expose notes/registers but is not a
complete core; GDB may reject it. Match production cores with their unstripped
executables and shared libraries. The isolated probe saves its matching ELF.

## Wire format

Header: ASCII `GCORE1 PID SIGNAL EPOCH\n`, maximum 127 bytes. Then one metadata
frame, followed by core-data frames. Each frame is an unsigned 32-bit network
byte-order length and that many bytes (maximum 16384). A zero-length frame ends
the core, followed by `C` (complete) or `T` (truncated), then TCP write shutdown.
The receiver verifies EOF, saves the capture and returns `K`. Missing framing,
EOF or acknowledgement counts as failure. No transport encryption is provided;
the receiver binds to the selected LAN address and admits the configured
source IPv4 only.
