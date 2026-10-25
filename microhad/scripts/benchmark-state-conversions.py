#!/usr/bin/env python3
"""Host-only synthetic thermostat/status CPU benchmark. Never accesses a device."""
import argparse
from pathlib import Path
import json
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', help='Compile implementation from this Git revision')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='state-conversions-benchmark-') as tmp:
        tmp = Path(tmp)
        source_root = ROOT
        if args.revision:
            source_root = tmp / 'source'
            for directory in ['microhad/include', 'microhad/src', 'config']:
                paths = subprocess.check_output(['git', '-C', str(ROOT), 'ls-tree', '-r', '--name-only', args.revision, '--', directory], text=True).splitlines()
                for path in paths:
                    target = source_root / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(subprocess.check_output(['git', '-C', str(ROOT), 'show', args.revision+':'+path]))
        binary = tmp / 'bench'
        subprocess.run([settings()['HOST_CXX'], '-std=gnu++03', '-O2', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I'+str(source_root/'microhad/include'), str(ROOT/'microhad/test/benchmark_state_conversions.cpp'),
                        *[str(source_root/'microhad/src'/name) for name in ['gateway.cpp', 'thermostat.cpp', 'automation_types.cpp', 'json_util.cpp']], '-o', str(binary)], check=True)
        samples = {}
        for _ in range(5):
            for line in subprocess.check_output([str(binary)], text=True).splitlines():
                key, value = line.split()
                samples.setdefault(key, []).append(float(value))
        print(json.dumps({key: statistics.median(values) for key, values in samples.items()}, indent=2))


if __name__ == '__main__':
    main()
