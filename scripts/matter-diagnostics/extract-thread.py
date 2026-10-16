#!/usr/bin/env python3
"""Extract raw radio frames from spinel.trace into a JSON Lines thread.trace."""
import argparse
import json
import os
from pathlib import Path

from trace_extract import checksum, extract_thread, save_trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('spinel', type=Path)
    parser.add_argument('--output', type=Path, help='default: thread.trace beside input; must not exist')
    args = parser.parse_args()
    os.umask(0o077)
    output = args.output or args.spinel.with_name('thread.trace')
    summary_path = output.with_suffix(output.suffix + '.summary.json')
    for target in (output, summary_path):
        if target.exists():
            parser.error(f'output already exists: {target}')
    with args.spinel.open(encoding='utf-8') as source:
        rows, summary = extract_thread(json.loads(line) for line in source if line.strip())
    save_trace(output, rows)
    summary.update(source_sha256=checksum(args.spinel), output_sha256=checksum(output),
                   source=str(args.spinel), output=str(output))
    save_trace(summary_path, [summary])
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
