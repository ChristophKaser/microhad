# Gateway TCP core capture

Shared clients in MicroHAD, Matter and Ziggurat capture fatal-signal registers
and ask an external collector to produce a userspace ELF core. A small MIPS
transport helper streams it to `receiver.py`. The same transport can accept
kernel `core_pattern` input on kernels that provide ELF dumping. Dumps and
metadata are private under ignored `diagnostics/core-dumps/`; they can contain keys.
The host receiver runs as a persistent user service.

## Stock-kernel capture

The stock 3.10.90 ELF loader has a null core-dump callback. This was confirmed
by decompressing the gateway's own read-only kernel partition and inspecting
its ELF format descriptor and `do_coredump` instructions. The replacement
rtl8196e-gateway kernel configs are unrelated to that finding.

`CRASH_SNAPSHOTS=true` builds a shared signal-triggered userspace client into
MicroHAD, Ziggurat and Matter. The tracked default is `false`; this workspace
opts in through private `workspace.local.env`. Environment/Make overrides win.
The target build flag invalidates Make objects and Cargo outputs. Host
MicroHAD fixtures remain uninstrumented. Disabled target builds have no
snapshot registration, thread wrapping or collector dependency.

The paired supervisor starts `gateway-snapshot` before the applications when
`core-snapshots.enabled` contains `true` and `core-capture.conf` exists. Each
application registers private shared state beneath `/tmp/gateway-snapshots`.
A pthread wrapper registers application threads and provides alternate signal
stacks. Existing SIGSEGV/SIGILL logging remains active. The handler records
original registers in preallocated storage, sends a nonblocking notification
through a pre-opened socket, and stops the faulting task. Notification failure
falls back to ordinary fatal-signal behavior; an unregistered thread never
stops waiting for capture. Registration supports up to 128 application threads.

The watchdog starts a separate collector child, which attaches only to the
registered application's tasks. LinuxThreads manager tasks are discovered
through validated worker/parent relationships; ordinary thread groups are
also enumerated. The ELF core includes original fault registers, other task
registers, readable writable mappings, and descriptors for readable mappings.
No FPU state is collected on this soft-float target. Matching host executables
and shared libraries provide omitted executable/read-only bytes.

After capture, tasks are killed so the existing supervisor can restart them.
The shell therefore records exit status 137 for a captured crash; the original
signal is retained in the core, shared job record and snapshot syslog event.
The watchdog enforces a 120-second collection deadline. It kills a failed
collector and its transport group and cleans up held tasks. If the watchdog
itself dies, the paired supervisor runs recovery and restarts the clients to
reconnect. PID start times guard against PID reuse. Missing collector startup
leaves services operational without snapshots. Explicit stop/quiesce still
controls service recovery. Kernel core settings and firmware tracing are untouched.

The isolated demo verifies main/worker crashes, LinuxThreads manager capture,
GDB stack/heap markers, absent host, collector death and stalled-collector
recovery. An 8.44 MiB snapshot streamed from this gateway in 9.44 seconds;
smaller 449 KiB snapshots took about two seconds including SSH overhead.
The 120-second deadline bounds service recovery even on a slow network;
a capture that cannot finish is incomplete. There is no byte-size limit.

## Build, test and install

From the repository root:

```sh
bash scripts/core-capture/build.sh
python3 scripts/core-capture/test_capture.py
python3 ziggurat/scripts/test-fault-report.py
python3 scripts/core-capture/install-host.py --bind HOST_LAN_IP --gateway GATEWAY_IP
python3 scripts/core-capture/test-snapshot-device.py --host GATEWAY_IP --receiver HOST_LAN_IP
# Explicit production installation, only when requested:
python3 scripts/core-capture/deploy.py --host GATEWAY_IP --receiver HOST_LAN_IP
# Staging never restarts services; deploy the matching paired supervisor next:
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
launcher starts and monitors the userspace collector; configuration and binaries
survive gateway reboot. Set `core-snapshots.enabled` to `false` and restart the
paired supervisor to disable runtime capture. `core-capture-control.sh` remains
a separate legacy kernel-pipe control tool; it is not used on the stock kernel. Firmware radio tracing remains off.

## Bounds and failure behavior

Streaming uses 16 KiB chunks with **no dump-size limit**. The transport has no
total-duration limit; userspace collection has the separate 120-second deadline.
Connection timeout is 3 seconds; socket inactivity timeout is 10 seconds. Userspace producer-pipe waits
allow 120 seconds and the host receiver allows 130 seconds of inactivity, so
CPU-heavy collection is governed by the capture watchdog rather than an
earlier pipe/receiver timeout. Raw kernel-pipe input retains its 10-second wait. The receiver writes directly to disk and fsyncs before returning a
single-byte saved acknowledgement. There is no checksum protocol. TCP delivery
alone cannot confirm that a receiver saved a file.

One private RAM fallback is retained beneath `/tmp/gateway-core-capture/`.
The **entire serialized fallback is at most 1 MiB**, including metadata and
framing. It contains the beginning of the dump and is marked truncated if it
exceeded that allowance. An older pending fallback is preserved; newer crashes
can still stream. A successful stream deletes only its own fallback. The watchdog retries every 30 seconds, deleting pending data only after a saved ACK.
Loss of an ACK can produce duplicate host files. A reboot loses RAM fallback.
Flash stores only the installed executable, script and small configuration.

The watchdog and transport lock serialize captures; concurrent crashes can be
skipped and are logged. Tracing keeps application memory available during
userspace capture. Kernel-pipe mode separately uses `core_pipe_limit=1`. Executable and maps metadata are bounded to one
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

Userspace ELF production prefixes the local transport pipe with an eight-byte
big-endian expected core length (`gateway-core --snapshot PID SIGNAL EPOCH`).
This prefix is removed before TCP framing. A short or long body is marked
truncated, so a failed memory read cannot masquerade as a complete core.

Interrupted transport prefixes are recovered under the transport lock, trimmed
to the last complete frame and marked truncated before retry. The 1 MiB bound
includes framing and metadata even when the watchdog aborts collection.
