#!/usr/bin/env python3
"""Extract HDLC/Spinel from a classic PCAP into a JSON Lines trace."""
import argparse
import json
import os
from pathlib import Path

from trace_extract import checksum, extract_spinel, save_trace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pcap', type=Path)
    parser.add_argument('--gateway', default='192.168.178.62')
    parser.add_argument('--port', type=int, default=9998)
    parser.add_argument('--output', type=Path, help='default: spinel.trace beside PCAP; must not exist')
    args = parser.parse_args()
    os.umask(0o077)
    output = args.output or args.pcap.with_name('spinel.trace')
    summary_path = output.with_suffix(output.suffix + '.summary.json')
    for target in (output, summary_path):
        if target.exists():
            parser.error(f'output already exists: {target}')
    rows, summary = extract_spinel(args.pcap, args.gateway, args.port)
    save_trace(output, rows)
    summary.update(source_sha256=checksum(args.pcap), output_sha256=checksum(output),
                   source=str(args.pcap), output=str(output), gateway=args.gateway, port=args.port)
    save_trace(summary_path, [summary])
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
