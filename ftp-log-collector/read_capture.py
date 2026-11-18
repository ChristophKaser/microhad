#!/usr/bin/env python3
"""Summarize gateway capture records and sequence gaps across pcapng segments."""
import argparse
import collections
import json
import struct
from pathlib import Path


def blocks(path):
    with path.open("rb") as stream:
        while header := stream.read(8):
            if len(header) < 8:
                return
            kind, size = struct.unpack("<II", header)
            if not 12 <= size <= 65536 or size % 4:
                raise ValueError(f"{path}: invalid pcapng block size")
            body = stream.read(size - 8)
            if len(body) != size - 8:
                return
            if struct.unpack("<I", body[-4:])[0] != size:
                raise ValueError(f"{path}: mismatched pcapng block size")
            if kind == 0x40000BAD:
                yield body[4:-4]


def summarize(paths):
    streams = {}
    collector_drops = []
    for path in paths:
        for data in blocks(path):
            if data.startswith(b"GPR1") and len(data) >= 8:
                collector_drops.append(struct.unpack_from("<I", data, 4)[0])
            if not data.startswith(b"GPC1") or len(data) < 48:
                continue
            source, kind = data[4:6]
            run, sequence, _, length, offset = struct.unpack_from("<QQQII", data, 8)
            records, counters = streams.setdefault((source, run), ({}, []))
            if kind == 0 and length == 16 and len(data) >= 64:
                counters.append(struct.unpack_from("<QQ", data, 48))
            elif sequence and 0 <= offset < length <= 4096 and offset % 1152 == 0:
                record = records.setdefault(sequence, (kind, length, set()))
                if record[:2] == (kind, length):
                    record[2].add(offset)
    results = []
    for (producer, run), (records, counters) in sorted(streams.items()):
        kinds = collections.Counter()
        for kind, size, offsets in records.values():
            if len(offsets) == (size + 1151) // 1152:
                kinds[kind] += 1
        dropped, failed = counters[-1] if counters else (None, None)
        results.append(dict(producer=producer, run=run, complete_by_kind=dict(kinds),
                            missing_between_observed=max(records)-min(records)+1-len(records) if records else 0,
                            incomplete=len(records)-sum(kinds.values()),
                            producer_drops=dropped, producer_send_failures=failed))
    return dict(streams=results, collector_drop_reports=collector_drops)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.archives), indent=2))


if __name__ == "__main__":
    main()
