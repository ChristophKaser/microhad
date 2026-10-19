#!/usr/bin/env python3
"""Summarize verified dynamic artifacts, counting shared runtime files once."""
import argparse
import json
from pathlib import Path


def summarize(paths):
    artifacts, libraries = [], {}
    for path in paths:
        report = json.loads(Path(path).read_text())
        if report['link_mode'] != 'dynamic' or report.get('runtime_source') != 'stock image':
            raise ValueError('expected stock-verified dynamic linkage report; rerun verification: ' + str(path))
        binary = Path(report['binary'])
        if binary.stat().st_size != report['bytes']:
            raise ValueError('artifact changed; rerun ELF verification: ' + str(binary))
        static = Path(report['static_binary'])
        report['static_bytes'] = static.stat().st_size if static.is_file() else None
        artifacts.append(report)
        for name, library in report['libraries'].items():
            libraries[library['path']] = dict(library, soname=name)
    dynamic_bytes = sum(item['bytes'] for item in artifacts)
    runtime_bytes = sum(item['bytes'] for item in libraries.values())
    static_bytes = (sum(item['static_bytes'] for item in artifacts)
                    if all(item['static_bytes'] is not None for item in artifacts) else None)
    return dict(artifacts=artifacts, libraries=list(libraries.values()),
                static_bytes=static_bytes, dynamic_bytes=dynamic_bytes,
                runtime_bytes=runtime_bytes, bytes_with_runtime=dynamic_bytes + runtime_bytes,
                additional_runtime_bytes=0, runtime_source='stock image')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', nargs='+', help='EXECUTABLE.linkage.json files')
    parser.add_argument('--output', type=Path, help='write combined JSON report')
    args = parser.parse_args()
    try:
        result = summarize(args.reports)
    except ValueError as error:
        raise SystemExit(str(error))
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('Artifact | Static bytes | Dynamic bytes | Saved bytes')
    for item in result['artifacts']:
        static = item['static_bytes']
        print(f"{Path(item['binary']).name} | {static if static is not None else 'missing'} | {item['bytes']} | {static - item['bytes'] if static is not None else 'unknown'}")
    print(f"Combined dynamic executables: {result['dynamic_bytes']} bytes")
    print(f"Stock-provided runtime counted once (part of image): {result['runtime_bytes']} bytes")
    print(f"Executables plus existing stock runtime files: {result['bytes_with_runtime']} bytes")
    print('Additional shared-library upload: 0 bytes (all dependencies verified against the stock image).')
    print('Totals use uncompressed ELF files; JFFS2 compression is not measured.')
