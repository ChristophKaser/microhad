# Sensor history file format

This document is the format and migration contract for
`/tmp/microhad-history.bin`. The file is transient RAM-backed state:
it survives a native-service restart and is cleared by a device reboot. It
must remain under `/tmp`; it must never be moved to `/tuya` flash.

## Goals and ownership

History is a single-writer store. The coordinator thread owns appends, page
allocation, eviction, and the in-memory page catalog. HTTP request workers
read through a synchronized snapshot of that catalog and never call `Gateway`
directly. A read copies the page IDs for one IEEE-address/metric pair while
holding the catalog mutex, releases the mutex, then reads and validates those
pages. An append that races with the read may cause a torn or reallocated
sample to be skipped for that request; the record checksum rejects it and a
later request can see the completed write.

The HTTP route and JSON response remain unchanged. A device query combines all
metric pages for that address, sorts samples by global sequence, applies the
`before` cursor, and returns the existing page of up to 2,000 samples.

## V2 layout

All multi-byte integers are unsigned big-endian on disk unless the value field
is interpreted as signed for its metric. CRC is CRC-8/ATM: polynomial `0x07`,
initial value zero, no reflection, and no final XOR.

The 16-byte file header is:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `TZH2` |
| 4 | 1 | Format version, `2` |
| 5 | 1 | File-header size, `16` |
| 6 | 1 | Page-header size, `16` |
| 7 | 1 | Sample-record size, `11` |
| 8 | 2 | Samples per page, `128` |
| 10 | 2 | Baseline page-pool size, `1472` |
| 12 | 3 | Reserved; zero |
| 15 | 1 | CRC-8 over bytes 0 through 14 |

Each page is 1,424 bytes: a 16-byte header followed by 128 11-byte records.
The header and every page start are 16-byte aligned; the record area is 1,408
bytes, also divisible by 16. The initial pool therefore uses
`16 + 1472 * 1424 = 2,096,144` bytes, within the nominal 2 MiB budget. The
file grows as pages are allocated rather than preallocating unused pages.

The 16-byte page header is:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 8 | IEEE device address |
| 8 | 1 | Metric ID: temperature 1, humidity 2, contact 3, power 4, occupancy 5, battery 6 |
| 9 | 1 | State: `1` allocated; all-zero header means unused |
| 10 | 4 | Page-allocation generation |
| 14 | 1 | CRC-8 over bytes 0 through 13 |
| 15 | 1 | Reserved; zero |

The 11-byte sample record is:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | Global sample sequence |
| 4 | 4 | Unix timestamp in seconds |
| 8 | 2 | Metric value in the existing metric-specific scale |
| 10 | 1 | CRC-8 over page address, page metric, page generation, and record bytes 0 through 9 |

Temperature and humidity use hundredths of a unit; power uses tenths of a
watt; contact and occupancy are `0` or `1`; battery is a percentage. Signed
metrics use 16-bit two's complement. The sample CRC deliberately includes
address, metric, and generation, so changing page ownership or reusing a page
cannot make its old samples appear to belong to the new owner. The page-header
CRC separately detects damage to page metadata.

The baseline pool contains 1,472 pages, or 188,416 sample slots before
per-pair page slack. Pages are stored at fixed offsets after the file header.
The file length determines how many page slots have been materialized;
incomplete trailing pages are ignored and repaired before the next append.
When all baseline pages are materialized, the file may grow by aligned pages
to give a new active device/metric pair its required first page if no reusable
or evictable page exists. There is no hard maximum in that exceptional case;
allocation failure leaves existing pages intact and reports an append error.

## Append, retention, and recovery

A page belongs to exactly one address/metric pair. Pages are allocated on the
first accepted sample for a pair, not for every possible metric a device might
report. Existing value validation, duplicate-value suppression, once-per-
minute sampling for non-contact metrics, and contact-transition retention
remain unchanged.

Samples append into a page until it has 128 valid samples. A pair may own
multiple pages. Active pairs keep at least one page: when another page is
needed, use an unused slot or evict the globally oldest eligible page, chosen
by its newest valid sequence. An active pair's only page is never a victim.
If the incoming pair already has its only page and no eligible spare page is
available, overwrite the oldest sample slot within that page. If the incoming
pair has no page and every page is protected, extend the file by one page.

Removing a device from the gateway unpins all of its pages, making them
eligible for ordinary eviction; removal does not immediately erase history.
If the device returns before its pages are reused, those pages become pinned
again. A page reallocated to another pair is cleared, its generation advances,
and its header is written only after the new page contents are ready. Readers
validate both header and sample CRCs against their catalog snapshot.

On startup, scan materialized page headers and records to recover the global
sequence, valid-sample counts, page fullness, per-pair page lists, last values,
and minute-sampling state. Invalid headers and records are not returned. A
corrupt page is never silently attributed to a different pair. The sequence
is 32-bit, matching the existing API cursor. Sequence exhaustion resets
history as the current implementation does. Only V2 files are recognized;
an unknown or damaged file is preserved and initialization fails.
