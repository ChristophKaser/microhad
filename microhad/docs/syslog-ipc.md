# Direct syslog buffer reader

MicroHAD reads the BusyBox syslog IPC buffer directly. It does not start a
`logread` process, use a BusyBox library, or include BusyBox implementation
code. The reader is independently written against the following interface
facts; upstream sources were inspected to establish the format and as a
compatibility reference, not copied or translated into the implementation.

## Supported interface

The device runs BusyBox 1.13.4. Official BusyBox 1.37.0 and 1.38.0 sources use
the same interface:

- System V shared memory and a two-element semaphore set use key `0x414e4547`.
- The segment begins with two native-endian signed 32-bit integers: ring
  capacity and next write position. Message bytes follow at offset eight.
- Capacity equals the segment size minus nine. One additional byte after the
  ring is a permanent NUL sentinel. The write position is within the ring.
- Records are NUL-terminated text. A record can cross the physical ring end.
  Data at the write position may be an overwritten partial oldest record.
- Semaphore zero counts readers; semaphore one indicates an active writer.
  A reader atomically verifies that there is no writer and increments the
  reader count. It decrements that count after copying a snapshot. `SEM_UNDO`
  balances reader operations if the process exits unexpectedly.

Source references:

- [BusyBox 1.13.4 release](https://busybox.net/downloads/busybox-1.13.4.tar.bz2)
- [BusyBox 1.37.0 release](https://busybox.net/downloads/busybox-1.37.0.tar.bz2)
- [BusyBox 1.38.0 release](https://busybox.net/downloads/busybox-1.38.0.tar.bz2)

The relevant interface definitions are in `sysklogd/syslogd.c` and
`sysklogd/logread.c` in each release. The interface is not a promised stable
BusyBox ABI: future releases and vendor forks require compatibility checks.

## Bounds and failure behavior

The segment is attached read-only; the reader never creates, resizes, clears,
or removes the shared memory or semaphores. It validates the segment size,
header, sentinel, latest record terminator and semaphore count. The current
implementation accepts segments up to 16 MiB. Allocation happens before
locking; parsing and JSON formatting happen after unlocking and detaching.

Lock acquisition uses nonblocking operations with at most twenty retries and
100 milliseconds of total retry sleep. Missing IPC resources, incompatible
metadata, malformed data, a restarted syslog daemon, or an unavailable writer
lock produce HTTP 503 from `/api/logs`. There is no child-process fallback.

This requires BusyBox `FEATURE_IPC_SYSLOG` and a running `syslogd -C` instance.
It does not apply to file-only logging, journald, or unrelated syslog daemons.
