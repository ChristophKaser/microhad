#!/usr/bin/env python3
"""Host-only synthetic state, history and log CPU benchmarks. Never accesses a device."""
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
    parser.add_argument('--benchmark', choices=['state', 'history', 'logs'], default='state')
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
        for directory in ['protocol', 'third-party']:
            if source_root!=ROOT:
                (source_root / directory).symlink_to(ROOT / directory, target_is_directory=True)
        binary = tmp / 'bench'
        sources = {'state': ['gateway.cpp', 'gateway_runner.cpp', 'thermostat.cpp', 'automation_types.cpp', 'json_util.cpp'],
                   'history': ['sensor_history.cpp', 'gateway.cpp', 'storage.cpp', 'json_util.cpp', 'protobuf_value.cpp'],
                   'logs': ['syslog_log_reader.cpp', 'json_util.cpp']}[args.benchmark]
        driver = {'state': 'benchmark_state_conversions.cpp', 'history': 'benchmark_history_conversions.cpp', 'logs': 'benchmark_log_polling.cpp'}[args.benchmark]
        driver_path = ROOT/'microhad/test'/driver
        if args.revision:
            historical = subprocess.run(['git', '-C', str(ROOT), 'show', args.revision+':microhad/test/'+driver], capture_output=True)
            if historical.returncode == 0:
                driver_path = tmp/driver
                driver_path.write_bytes(historical.stdout)
        objects = []
        flags = ['-DMICROHAD_TESTING']
        if 'struct ThermostatValues' in (source_root/'microhad/include/thermostat.h').read_text():
            flags.append('-DTYPED_THERMOSTAT')
        if 'cachedStatus' in (source_root/'microhad/include/gateway_runner.h').read_text():
            flags.append('-DCACHED_STATUS')
        if 'appendNumeric' in (source_root/'microhad/include/sensor_history.h').read_text():
            flags.append('-DTYPED_HISTORY')
        if args.benchmark=='history':
            for source in [*(ROOT/'third-party/nanopb'/(name+'.c') for name in ['pb_common', 'pb_encode', 'pb_decode']), ROOT/'protocol/generated/device-cache.pb.c']:
                obj=tmp/(source.name+'.o')
                subprocess.run([settings()['HOST_CC'], '-O2', '-DPB_C99_STATIC_ASSERT', '-ffunction-sections', '-fdata-sections', '-I'+str(ROOT/'third-party/nanopb'), '-c', str(source), '-o', str(obj)], check=True)
                objects.append(str(obj))
        subprocess.run([settings()['HOST_CXX'], '-std=gnu++03', '-O2', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        *flags, '-I'+str(source_root/'microhad/include'), '-I'+str(ROOT/'microhad/test'), '-I'+str(ROOT/'third-party/nanopb'), str(driver_path),
                        *[str(source_root/'microhad/src'/name) for name in sources], *objects, '-lpthread', '-o', str(binary)], check=True)
        samples = {}
        for _ in range(5):
            for line in subprocess.check_output([str(binary)], text=True).splitlines():
                parts=line.split()
                key,value=parts if len(parts)==2 else (args.benchmark+'_cpu_us',parts[0])
                samples.setdefault(key, []).append(float(value))
        print(json.dumps({key: statistics.median(values) for key, values in samples.items()}, indent=2))


if __name__ == '__main__':
    main()
