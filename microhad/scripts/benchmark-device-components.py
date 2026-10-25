#!/usr/bin/env python3
"""Compare synthetic Device component benchmarks on the host or an explicit gateway.

Uploads only temporary benchmark executables. Never opens production state,
uses controller sockets, deploys services or restarts the gateway.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings
from workspace_config import gateway_host, has_credentials, ssh_command


def build(directory, revision, device, accounting):
    source = ROOT
    if revision:
        source = directory / 'source'
        for name in ['microhad/include', 'microhad/src', 'config', 'protocol/generated']:
            for path in subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', revision, '--', name], cwd=ROOT, text=True).splitlines():
                target = source/path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(subprocess.check_output(['git', 'show', revision+':'+path], cwd=ROOT))
        (source/'third-party').symlink_to(ROOT/'third-party', target_is_directory=True)
    config = settings()
    cc, cxx = (config['TARGET_CC'], config['TARGET_CXX']) if device else (config['HOST_CC'], config['HOST_CXX'])
    flags = ['-O2', '-ffunction-sections', '-fdata-sections', '-DPB_C99_STATIC_ASSERT', '-I'+str(ROOT/'third-party/nanopb')]
    objects = []
    for path in [*(ROOT/'third-party/nanopb'/(name+'.c') for name in ['pb_common', 'pb_encode', 'pb_decode']), source/'protocol/generated/device-cache.pb.c']:
        obj = directory/(path.name+'.o')
        subprocess.run([str(cc), *flags, '-c', str(path), '-o', str(obj)], check=True, stdout=subprocess.DEVNULL)
        objects.append(str(obj))
    flags += ['-DMICROHAD_TESTING', '-I'+str(source/'microhad/include'), '-I'+str(ROOT/'microhad/test')]
    if (source/'microhad/include/device_components.h').exists():
        flags.append('-DDEVICE_COMPONENTS')
    if accounting:
        flags.append('-DDEVICE_BENCH_ALLOCATIONS')
    binary = directory/'benchmark'
    sources = ['gateway.cpp', 'gateway_runner.cpp', 'thermostat.cpp', 'automation_types.cpp', 'storage.cpp', 'json_util.cpp', 'protobuf_value.cpp']
    subprocess.run([str(cxx), '-std=gnu++98' if device else '-std=gnu++03', *flags,
                    '-Wl,--gc-sections', *(['-static'] if device else []), str(ROOT/'microhad/test/benchmark_device_components.cpp'),
                    *[str(source/'microhad/src'/name) for name in sources], *objects, '-lpthread', '-lrt', '-o', str(binary)], check=True, stdout=subprocess.DEVNULL)
    if device:
        subprocess.run([str(config['TARGET_STRIP']), str(binary)], check=True)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', default='3a59035')
    parser.add_argument('--device', type=gateway_host, help='explicit gateway address; never inferred')
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--resume', action='store_true', help='resume matching cases from --output')
    args = parser.parse_args()
    if args.samples < 1 or args.samples > 9:
        parser.error('--samples must be 1..9')
    if args.device and not has_credentials():
        parser.error('--device requires configured SSH credentials')
    ssh = ssh_command(args.device) if args.device else None
    if args.resume and (not args.output or not args.output.is_file()):
        parser.error('--resume requires an existing --output file')
    digest = hashlib.sha256()
    for path in sorted([*(ROOT/'microhad/include').glob('*'), *(ROOT/'microhad/src').glob('*.cpp'), ROOT/'microhad/test/benchmark_device_components.cpp', ROOT/'protocol/generated/device_cache_encode.inc']):
        if path.is_file():
            digest.update(str(path.relative_to(ROOT)).encode())
            digest.update(path.read_bytes())
    remote = []
    results = {'mode': 'device' if args.device else 'host', 'baseline_revision': args.baseline,
               'synthetic': True, 'production_state_access': False, 'samples': args.samples,
               'device': args.device, 'after_source_digest': digest.hexdigest(), 'complete': False, 'cases': {}}
    if args.resume:
        existing = json.loads(args.output.read_text())
        for key in ['mode', 'baseline_revision', 'samples', 'device', 'after_source_digest']:
            if existing.get(key) != results[key]:
                parser.error('resume metadata differs: '+key)
        results['cases'] = existing['cases']
    def checkpoint():
        if args.output:
            temporary = args.output.with_suffix(args.output.suffix+'.tmp')
            temporary.write_text(json.dumps(results, indent=2)+'\n')
            temporary.replace(args.output)
    with tempfile.TemporaryDirectory(prefix='microhad-components-bench-') as work:
        try:
            binaries = {}
            for version, revision in [('before', args.baseline), ('after', None)]:
                for accounting in [False, True]:
                    directory = Path(work)/(version+('-alloc' if accounting else ''))
                    directory.mkdir()
                    binary = build(directory, revision, bool(args.device), accounting)
                    if ssh:
                        target = '/tmp/microhad-components-bench-'+uuid.uuid4().hex
                        remote.append(target)
                        subprocess.run(ssh+['umask 077; (set -C; cat > '+target+') && chmod 700 '+target], input=binary.read_bytes(), check=True, timeout=30, stdout=subprocess.DEVNULL)
                        binary = target
                    binaries[version, accounting] = str(binary)
            def run_many(jobs):
                commands = [f'{binaries[version, accounting]} {op} {kind} {count}' for version, accounting, op, kind, count in jobs]
                if ssh:
                    # One SSH connection per batch; each executable retains its own CPU limit.
                    output = subprocess.check_output(ssh+['ulimit -c 0; ulimit -v 16384; ulimit -t 20; '+ ' && '.join(commands)], timeout=60, text=True)
                    reports = [json.loads(line) for line in output.splitlines()]
                else:
                    reports = [json.loads(subprocess.check_output(command.split(), timeout=30, text=True)) for command in commands]
                if len(reports) != len(jobs):
                    raise RuntimeError('incomplete benchmark batch')
                return reports
            for kind, fixture in enumerate(['empty', 'sensor', 'thermostat', 'multifunction', 'mixed26']):
                for op in ['construct', 'copy', 'snapshot', 'unchanged', 'capabilities', 'cache']:
                    if op == 'construct' and kind:
                        continue
                    if fixture+'/'+op in results['cases']:
                        continue
                    # Calibration also warms each variant. Keep each timed run bounded.
                    pilot_count = 10
                    while True:
                        pilots = run_many([(v, False, op, kind, pilot_count) for v in ['before', 'after']])
                        if (min(r['cpu_us'] for r in pilots)*pilot_count >= 100000 and min(r['wall_us'] for r in pilots)*pilot_count >= 100000) or pilot_count >= 10000:
                            break
                        pilot_count *= 10
                    fastest = min((r['cpu_us'] for r in pilots if r['cpu_us'] > 0), default=1)
                    count = max(1, min(100000, int(250000/max(fastest, 1))))
                    while True:
                        jobs = []
                        for sample in range(args.samples):
                            for version in (['before', 'after'] if sample % 2 == 0 else ['after', 'before']):
                                jobs.append((version, False, op, kind, count))
                        jobs += [(v, True, op, kind, 1) for v in ['before', 'after']]
                        reports = run_many(jobs)
                        case = {'iterations': count}
                        for version in ['before', 'after']:
                            timed = [r for job, r in zip(jobs, reports) if job[0] == version and not job[1]]
                            case[version] = {key: statistics.median(r[key] for r in timed) for key in ['cpu_us', 'wall_us', 'inline_bytes']}
                            case[version]['memory'] = reports[-2 if version == 'before' else -1]
                        if all(case[v]['cpu_us'] > 0 for v in ['before', 'after']):
                            break
                        if count == 100000:
                            raise RuntimeError('CPU timing unresolved at maximum iteration count')
                        count = min(100000, max(1000, count*10))
                    case['cpu_change_percent'] = 100*(case['after']['cpu_us']/case['before']['cpu_us']-1)
                    results['cases'][fixture+'/'+op] = case
                    checkpoint()
                    print(f"{fixture}/{op}: {case['before']['cpu_us']:.3f} -> {case['after']['cpu_us']:.3f} CPU us", file=sys.stderr, flush=True)
        finally:
            for target in remote:
                subprocess.run(ssh+['rm -f '+target], check=True, timeout=15, stdout=subprocess.DEVNULL)
    results['complete'] = True
    output = json.dumps(results, indent=2)+'\n'
    if args.output:
        args.output.write_text(output)
    else:
        print(output, end='')


if __name__ == '__main__':
    main()
