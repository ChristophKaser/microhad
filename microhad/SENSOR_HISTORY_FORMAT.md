# Sensor history file format

Sensor history is a single-writer store under `/tmp`. It is transient RAM-backed
state, survives a native-service restart, and is cleared by a device reboot. It
must never be moved to `/tuya` flash.

The coordinator owns appends, allocation, eviction, and the in-memory page
catalog. HTTP workers copy the requested device’s page list under a short
catalog lock, then release it before all disk reads and JSON construction.
They compare page identity and allocation generation against their snapshot,
validate header and record CRCs, and recheck the header after copying a page.
A page evicted during a request is skipped; a later request sees the new data.
Pages are visited by newest sequence, skipping ranges outside the cursor and
stopping once the response and pagination sentinel are known. The API keeps
its existing response tuple `[sequence, timestamp, metric, value, unit]` and
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
| 6 | 1 | Page-header size, `85` |
| 7 | 1 | Sample-record size, `13` |
| 8 | 2 | Samples per page, `151` |
| 10 | 2 | Maximum page pool, `1023` |
| 12 | 3 | Reserved zero bytes |
| 15 | 1 | CRC over bytes 0–14 |

Each page is exactly 2,048 bytes: an 85-byte header followed by 151 records of
13 bytes each. The page header is:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Device-ID byte length, 1–64 |
| 1 | 1 | Metric code |
| 2 | 1 | State: `1` allocated; zero means unused |
| 3 | 4 | Page allocation generation |
| 7 | 64 | Device ID, zero-padded |
| 71 | 13 | Reserved zero bytes, including the five bytes moved into the header |
| 84 | 1 | CRC over bytes 0–83 |

The record is sequence (4 bytes), Unix timestamp (4 bytes), IEEE-754 binary32
value (4 bytes), and CRC (1 byte). The record CRC covers the page identity,
generation, and first 12 record bytes. Numeric values are stored as float32;
contact, occupancy, and light use 0 or 1, while air-quality readings use codes
0–5 for good through extremely poor.

The maximum file size is `16 + 1023 * 2048 = 2,095,120` bytes, below 2 MiB.
Pages are allocated on demand at fixed offsets. When the pool is full, the
oldest page is evicted, except that an incoming metric’s only page is protected
while another page can be reclaimed. Reallocated pages advance their generation
and their record areas are cleared before the new header is published.
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

Startup scans page headers and contiguous valid records to rebuild the sequence
and page catalog, sorting each pair’s pages by sequence so appends resume in
the newest partially filled page after allocation wraps. Invalid headers and checksummed records are skipped; an
incomplete record tail is ignored. V3 is written fresh without migration. A
recognized former `TUV1` values journal is reset; unknown or damaged formats are
preserved and initialization fails closed.

Samples append until a page fills, then a new page is allocated for that
device/metric pair. Duplicate values are suppressed; non-discrete metrics keep
the existing once-per-minute sampling rule, while contact and other discrete
transitions are retained. API reads sort by global sequence and apply the
existing `before` cursor and 2,000-sample response limit.
