# FTP log collector

`ftp-log-collector` receives local BusyBox syslog UDP datagrams and the existing
`GCORE1` TCP crash stream, and uploads them to an FTP directory. Both listeners
bind only to loopback. The paired supervisor starts it when the executable and
configuration exist next to MicroHAD and restarts it if it exits. The local
BusyBox 512 KiB log ring remains the dashboard's log source.

## Configuration

Create `/tuya/native/ftp-log-collector.conf` as a root-owned mode 0600 regular
file. It uses one `key=value` per line; values are literal, not shell code.
The configuration is separate from the FRITZ!Box integration; do not commit it
or include credentials on command lines. Required settings:

```text
ftp_host=192.0.2.1
ftp_user=example
ftp_password=example
ftp_directory=/
```

Optional settings and defaults:

```text
ftp_port=21
udp_port=5514
tcp_port=9999
queue_bytes=131072
log_file_bytes=1048576
log_files=100
rotate_seconds=3600
```

The local installation uses `log_file_bytes=10485760`. To route logs to this
service, set `syslog_remote` in the existing native JSON configuration to
`127.0.0.1:5514` and restart the paired stack. To route snapshots here, set
`core-capture.conf` to `127.0.0.1 9999`. These destinations are independent;
the original host syslog collector or crash receiver can be selected instead.
`ftp-log-collector --check-config PATH` validates a private config without
starting listeners.

UDP packets are queued in bounded RAM while FTP is unavailable; once full,
new packets are dropped and the loss count is reported to local syslog.
Uploads stream directly to FTP. Log files are closed after the size or time
limit and renamed from `.partial` to `.log`; retention counts both completed
and interrupted collector logs and deletes only matching collector log names.
Failed transfers can leave `.partial` files until retention runs. The size
limit can be exceeded by one packet if that packet alone exceeds the limit.

Crash uploads stream directly without a full RAM copy. Metadata and the core
are published separately. The receiver sends `K` only after both uploads and
renames succeed; the existing transport's 1 MiB RAM fallback handles failures.
A lost acknowledgement can create duplicates. Partial uploads and orphaned
metadata may remain after failures, and dumps are never automatically deleted.
FTP acknowledgements do not guarantee a physical disk flush. Plain FTP sends
credentials and data without encryption and diagnostics can contain keys.

## Build and test

From the repository root:

```sh
bash ftp-log-collector/build.sh
python3 ftp-log-collector/test_collector.py
python3 microhad/scripts/test_paired_supervisor.py
```

The build writes `build/${TARGET_BUILD_SUBDIR}ftp-log-collector` and verifies
its MIPS target. The integration fixture uses local sockets and an in-memory
FTP server; it does not contact the gateway or FRITZ!Box.

For the explicitly selected first installation, `python3
ftp-log-collector/deploy.py --host GATEWAY_IP` copies the single configured
FRITZ!Box's host and credentials into a separate private collector configuration,
sets the destination folder to `/`, selects 10 MiB log files, directs both
diagnostic streams to loopback, and deploys the paired supervisor using the
guarded selective maintenance flow. It refuses to overwrite an existing
collector installation. Gateway access and the service restart require an
explicit deployment request.
