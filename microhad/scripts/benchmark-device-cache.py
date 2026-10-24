#!/usr/bin/env python3
"""Benchmark synthetic device-cache writes locally or on an explicitly selected gateway."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings
from workspace_config import gateway_host, has_credentials, ssh_command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=gateway_host, help='explicit gateway hostname/address; never inferred')
    parser.add_argument('--iterations', type=int)
    parser.add_argument('--devices', type=int, default=26)
    args = parser.parse_args()
    iterations = args.iterations if args.iterations is not None else (20 if args.device else 400)
    if not 1 <= iterations <= 10000 or not 1 <= args.devices <= 256:
        parser.error('iterations must be 1..10000 and devices 1..256')
    if args.device and not has_credentials():
        parser.error('--device requires configured SSH credentials')
    config = settings()
    cc = str(config['TARGET_CC']) if args.device else config['HOST_CC']
    cxx = str(config['TARGET_CXX']) if args.device else config['HOST_CXX']
    with tempfile.TemporaryDirectory(prefix='microhad-cache-benchmark-') as tmp:
        directory = Path(tmp)
        objects = []
        for source in [*(ROOT / 'third-party/nanopb' / (name + '.c') for name in ('pb_common', 'pb_encode', 'pb_decode')),
                       ROOT / 'protocol/generated/device-cache.pb.c']:
            output = directory / (source.name + '.o')
            subprocess.run([cc, '-std=gnu99', '-O2', '-ffunction-sections', '-fdata-sections', '-DPB_C99_STATIC_ASSERT',
                            '-I' + str(ROOT / 'third-party/nanopb'), '-c', str(source), '-o', str(output)],
                           check=True, stdout=subprocess.DEVNULL)
            objects.append(str(output))
        binary = directory / 'benchmark'
        sources = ['test/benchmark_device_cache.cpp', 'src/gateway.cpp', 'src/storage.cpp',
                   'src/json_util.cpp', 'src/protobuf_value.cpp']
        subprocess.run([cxx, '-std=gnu++98' if args.device else '-std=gnu++03', '-O2', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-I' + str(ROOT / 'microhad/include'), '-I' + str(ROOT / 'third-party/nanopb'),
                        *(['-static'] if args.device else []), *(str(ROOT / 'microhad' / source) for source in sources),
                        *objects, '-lpthread', '-lrt', '-o', str(binary)], check=True, stdout=subprocess.DEVNULL)
        if not args.device:
            result = subprocess.check_output([str(binary), str(iterations), str(args.devices)], text=True)
        else:
            subprocess.run([str(config['TARGET_STRIP']), str(binary)], check=True)
            remote = '/tmp/microhad-cache-benchmark-' + uuid.uuid4().hex
            ssh = ssh_command(args.device)
            try:
                subprocess.run(ssh + ['umask 077; (set -C; cat > ' + remote + ') && chmod 700 ' + remote],
                               input=binary.read_bytes(), check=True, stdout=subprocess.DEVNULL, timeout=30)
                result = subprocess.check_output(ssh + [f'ulimit -c 0; ulimit -v 8192; ulimit -t 20; {remote} {iterations} {args.devices}'],
                                                 text=True, timeout=120)
            finally:
                subprocess.run(ssh + ['rm -f ' + remote], check=True, stdout=subprocess.DEVNULL, timeout=15)
        report = json.loads(result)
        report.update(mode='device' if args.device else 'host', synthetic=True, production_state_access=False)
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
