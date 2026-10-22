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

## Filtered and paginated reads

Each request still copies the complete ring while holding the reader semaphore.
The copy is arranged in chronological order using at most two bulk copies, so
wrapped records can be scanned as contiguous bytes after the lock is released.
Record and line boundaries are indexed as byte ranges; zero padding is skipped
in blocks. No shared cache or additional persistent state is used.

Pagination counts the indexed lines and materializes and converts timestamps
only for the requested page. `before` remains an exclusive positional index;
zero or a position beyond the current line count selects the latest page.
`next_before` and `has_more` retain their existing behavior, including the
possibility of positions changing when the ring overwrites older messages.

`since` checks every candidate timestamp once, includes equal timestamps, and
reuses that timestamp when formatting selected lines. It does not stop at an
older timestamp: syslog messages can arrive out of timestamp order. One date
and timezone context is captured for the request. Standard 15-byte syslog
headers use direct month and digit parsing instead of `sscanf`. For UTC and
explicit fixed-offset POSIX timezones, epoch values are computed with integer
calendar arithmetic using the request's year boundary; scanning old records
therefore requires no per-record libc calendar conversions. DST-aware and
named timezone files retain `mktime`, as do leap seconds and normalized invalid
dates. No timestamp ordering assumption or cross-request cache is introduced.
Unusual long timestamp headers retain the previous parser behavior through a
bounded fallback copy.

Unit tests compare the optimized output with the original byte-walk and
filter/page semantics, including physical wraparound, multiline records,
empty and sparse rings, timestamp equality, out-of-order timestamps and year
rollover. Test-only counters verify that pagination converts timestamps only
for returned lines and filtering converts each candidate at most once.
