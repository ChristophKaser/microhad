# Sensor history file format

Sensor history is a single-writer store under `/tmp`. It is transient RAM-backed
state and survives a native-service restart. A smaller flash copy restores
committed history after reboot; the live store always remains in `/tmp`.

The coordinator owns appends, allocation, eviction, and the in-memory page
catalog. HTTP workers copy the requested device’s page list under a short
catalog lock, then release it before all disk reads and JSON construction.
They compare page identity and allocation generation against their snapshot,
validate header and record CRCs, and recheck the header after copying a page.
A page evicted during a request is skipped; a later request sees the new data.
Pages are visited by newest timestamp and position, skipping ranges outside
the cursor and stopping once the response and pagination sentinel are known. The API keeps
its existing response tuple `[cursor, timestamp, metric, value, unit]` and
2,000-sample response pagination.

## V3 file and page layout

All multi-byte integers are unsigned big-endian unless stated otherwise. CRCs
use CRC-8/ATM (polynomial `0x07`, initial value zero, no reflection or final
XOR). The file header is 16 bytes:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `TZH3` |
| 4 | 1 | Format version, `3` |
| 5 | 1 | File-header size, `16` |
| 6 | 1 | Page-header size, `86` |
| 7 | 1 | Sample-record size, `9` |
| 8 | 2 | Samples per page, `218` |
| 10 | 2 | Maximum page pool, `1023` |
| 12 | 3 | Reserved zero bytes |
| 15 | 1 | CRC over bytes 0–14 |

Each page is exactly 2,048 bytes: an 86-byte header followed by 218 records of
9 bytes each. The page header is:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Device-ID byte length, 1–64 |
| 1 | 1 | Metric code |
| 2 | 1 | State: `1` allocated; zero means unused |
| 3 | 4 | Page allocation generation |
| 7 | 64 | Device ID, zero-padded |
| 71 | 14 | Reserved zero bytes, including the five bytes moved into the header |
| 85 | 1 | CRC over bytes 0–84 |

The record is a full Unix timestamp (4 bytes), IEEE-754 binary32 value (4 bytes),
and CRC (1 byte). No sequence number is stored. The record CRC covers the page
identity, generation, and first 8 record bytes. Numeric values are stored as float32;
contact, occupancy, and light use 0 or 1, while air-quality readings use codes
0–5 for good through extremely poor.

The maximum file size is `16 + 1023 * 2048 = 2,095,120` bytes, below 2 MiB.
Pages are allocated on demand at fixed offsets. When the pool is full, the
oldest page is evicted, except that an incoming metric’s only page is protected
while another page can be reclaimed. Every allocation advances a global page
generation, and reallocated record areas are cleared before the new header is
published.
Generation checks and CRCs detect concurrent reuse; clearing prevents CRC-8
identity collisions from reviving stale records after restart. The final page has no
footer: all 2,048 bytes belong to its header and records.

## Metric codes and unit normalization

Metric codes 1–11 remain stable: temperature, humidity, contact, power,
occupancy, battery, CO₂ ppm, PM2.5 µg/m³, air quality, light, and brightness.
The unit is fixed by the metric code and returned by the API; no unit string or
unit code is stored in a record.

| Code | Measurement and stored unit | Accepted compatible input units |
| ---: | --- | --- |
| 7 | CO₂, ppm | ppm, ppb, ppt (converted to ppm) |
| 8 | PM2.5, µg/m³ | mg/m³, µg/m³, ng/m³ (converted to µg/m³) |
| 12 | CO₂, µg/m³ | mg/m³, µg/m³, ng/m³ (converted to µg/m³) |
| 13 | CO₂, particles/m³ | particles/m³, particles/ft³ (converted to particles/m³) |
| 14 | CO₂, Bq/m³ | Bq/m³ |
| 15 | CO₂, dB/m | dB/m |
| 16 | PM2.5, ppm | ppm, ppb, ppt (converted to ppm) |
| 17 | PM2.5, particles/m³ | particles/m³, particles/ft³ (converted to particles/m³) |
| 18 | PM2.5, Bq/m³ | Bq/m³ |
| 19 | PM2.5, dB/m | dB/m |

CO₂ and PM2.5 always have distinct metric codes even when their units match.
Other sensor metrics retain their fixed existing units. Unsupported units are
rejected rather than mixed into an incompatible series.

## Recovery and retention

Startup scans page headers and contiguous valid records to rebuild the page
catalog and highest allocation generation. Each pair’s pages are sorted by
allocation generation so appends resume in the newest partially filled page
after allocation wraps, including when the clock moves backwards. The oldest
allocation generation determines eviction. Invalid headers and checksummed
records are skipped; an incomplete record tail is ignored. V3 is written fresh
without migration or legacy-file removal. Unknown or damaged formats are
preserved and initialization fails closed.

Samples append until a page fills, then a new page is allocated for that
device/metric pair. Full timestamps support devices reporting once per day
without forcing pages to close early. Duplicate values are suppressed;
non-discrete metrics keep the existing once-per-minute sampling rule, while
contact and other discrete transitions are retained.

The API derives each cursor as `(timestamp << 18) | (page_index << 8) | slot`.
This provides distinct cursors for same-second samples without storing extra
bytes. The maximum cursor is below 2^50 and is an exact JavaScript integer.
Reads sort by timestamp and position, apply the exclusive `before` cursor,
and return at most 2,000 samples. Generation checks and CRCs validate the copied
pages independently of these cursors. Cursors identify retained samples; a
reused physical slot at the same timestamp can reuse its cursor.

`GET /api/devices/<id>/history` accepts optional `since` (an inclusive Unix
timestamp in seconds, 0 through 4294967295) and `before` (an exclusive cursor).
Parameter order does not matter. Omit `since`, or set it to zero, for the
existing unbounded read. Invalid, duplicate, and unknown parameters return 400.

A range read includes records at or after `since`, plus the latest retained
record before `since` for each metric across the requested endpoint IDs. Those
predecessors retain their original timestamps; the UI carries their values to
the chart's left edge. Predecessors are selected independently of `before`,
then filtered by it, so pagination cannot return successively older baseline
values. The 2,000-sample limit and `next_before` apply to this eligible set.
Catalog bounds skip older pages once they cannot improve a predecessor.

The history dialog and dashboard request only their selected day/week range,
publish each page as it arrives, and cap client retention at 10,000 samples.
“All retained” omits `since`. Changing device/range or closing a dialog aborts
outstanding requests. Dashboard refreshes run 30 seconds after the preceding
load finishes, merge the newest page by cursor, and do not overlap.

## Excluded readings

Actuator on/off states and brightness are not recorded. Metric codes 10 and 11
remain reserved. Actual sensor measurements, including smart-plug power, are
still recorded. FRITZ!Box Wi-Fi presence devices receive no history for privacy
reasons: connection transitions can expose arrivals, departures, and routines.
Only their current presence state is maintained.

## Flash persistence

Production uses `<runtime_dir>/history.values` (normally `/tuya/native/history.values`).
Its maximum is 255 pages plus the existing header: 522,256 logical bytes. The
v3 header remains unchanged; the smaller pool limit is enforced by persistence.
Filesystem overhead and garbage collection are outside this file-size ceiling.

A dedicated worker writes full 2,048-byte page images when pages fill. Dirty
sparse pages are eligible every four hours on a monotonic schedule starting at
initialization. There is no shutdown flush. Existing RAM history takes precedence;
only an absent RAM file triggers validation and compact restoration from flash.
Restored RAM indices, and therefore API cursors, can change after reboot.

Flash residents retain their exact slots across updates and restarts. Unchanged
pages are never rewritten. Pages evicted from RAM remain in flash until replaced;
there is no flash compaction or separate clearing/zero-fill operation. Incoming
pages use unused slots first, then replace victims directly. Retention prioritizes
each device/metric's newest eligible page, then newest allocation generations
overall. If more than 255 pairs qualify, the newest 255 win.

The worker snapshots the RAM catalog briefly under its mutex, then reads and
validates page identities, CRC8 checksums and header rechecks outside it. It
synchronizes write batches before acknowledging them; concurrent new records
remain dirty. Failures are reported through the coordinator and retried after
60 seconds. Invalid flash headers are preserved and disable persistence for the
run; invalid pages are skipped. CRC8 is the chosen, limited protection against
torn writes, without a new format, migration, or additional checksums.
