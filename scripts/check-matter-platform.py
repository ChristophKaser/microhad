#!/usr/bin/env python3
"""Check necessary Matter compiler/library prerequisites, never claim a working controller.

No network access or device access. Full SDK/platform builds and device acceptance
remain separate gates even when these small prerequisite probes succeed.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = '3bcdd56ba54fb88b2afb4bfef575014671df7aa7'


def run(command):
    result = subprocess.run([str(part) for part in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=60)
    return {'command': [str(part) for part in command], 'exit_code': result.returncode,
            'output': result.stdout}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('host', 'mips'))
    parser.add_argument('--sdk', type=Path, default=ROOT / 'dependencies/connectedhomeip')
    parser.add_argument('--rsdk', type=Path,
                        default=ROOT.parent / 'rsdk-4.4.7-4181-EB-2.6.30-0.9.30-m32u-140129')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    args.sdk = args.sdk.resolve()
    args.rsdk = args.rsdk.resolve()
    output = (args.output or ROOT / 'build/matter-sdk' / args.mode).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {'mode': args.mode, 'sdk_tag': 'v1.6.1.0', 'sdk_revision': REVISION,
              'scope': 'compiler and C++ library prerequisites only', 'probes': [],
              'controller_validated': False, 'device_access': False}
    try:
        identity = run(['git', '-C', args.sdk, 'rev-parse', 'HEAD'])
        if identity['exit_code'] or identity['output'].strip() != REVISION:
            raise RuntimeError('Missing or incorrect SDK pin; run bash scripts/fetch-matter-sdk.sh')
        dirty = run(['git', '-C', args.sdk, 'status', '--porcelain'])
        if dirty['exit_code'] or dirty['output'].strip():
            raise RuntimeError('SDK checkout must be unmodified')
        # Verify the prerequisite selection against actual pinned upstream files.
        setup = (args.sdk / 'src/controller/SetUpCodePairer.h').read_text()
        if '#include <optional>' not in setup or 'std::optional<' not in setup:
            raise RuntimeError('Pinned SDK optional-library assumption changed')
        if args.mode == 'host':
            compilers = [('host-cxx', [shutil.which('g++') or 'g++', '-std=c++17'])]
        else:
            cxx = args.rsdk / 'include/c++/4.4.7'
            compilers = [
                ('stock-rsdk', [args.rsdk / 'bin/mips-linux-g++', '-std=c++17', '-msoft-float']),
                ('clang-stock-rsdk', [shutil.which('clang++-18') or 'clang++-18',
                    '--target=mips-linux-gnu', '-std=c++17', '-march=mips1', '-mabi=32',
                    '-msoft-float', '-fno-pic', '-mno-abicalls', '-G0',
                    '-mllvm', '-mno-check-zero-division', '--sysroot=' + str(args.rsdk),
                    '-nostdinc++', '-isystem', cxx, '-isystem', cxx / 'mips-linux-uclibc/4181'])]
        for compiler, flags in compilers:
            for name in ('matter-type-traits', 'matter-standard-library'):
                obj = output / f'{compiler}-{name}.o'
                obj.unlink(missing_ok=True)
                result = run(flags + ['-fno-exceptions', '-fno-rtti', '-I', args.sdk / 'src',
                                      '-c', ROOT / 'probes' / f'{name}.cpp', '-o', obj])
                result.update({'compiler': compiler, 'probe': name})
                report['probes'].append(result)
                if result['exit_code'] == 0:
                    binary = output / f'{compiler}-{name}'
                    binary.unlink(missing_ok=True)
                    linker = ([flags[0]] if args.mode == 'host' else [args.rsdk / 'bin/mips-linux-g++', '-msoft-float', '-static'])
                    linked = run(linker + [obj, '-o', binary])
                    linked.update({'compiler': compiler, 'probe': name + '-link'})
                    report['probes'].append(linked)
                    if linked['exit_code'] == 0:
                        verified = run([binary] if args.mode == 'host' else ['readelf', '-h', binary])
                        verified.update({'compiler': compiler, 'probe': name + '-run' if args.mode == 'host' else name + '-elf'})
                        report['probes'].append(verified)
        report['compiler_results'] = {
            compiler: all(item['exit_code'] == 0 for item in report['probes'] if item['compiler'] == compiler)
            for compiler, _ in compilers
        }
        report['prerequisites_passed'] = any(report['compiler_results'].values())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        report['error'] = str(error)
        report['prerequisites_passed'] = False
    path = output / 'report.json'
    path.write_text(json.dumps(report, indent=2) + '\n')
    for result in report['probes']:
        print(('PASS' if result['exit_code'] == 0 else 'FAIL'), result['compiler'], result['probe'])
        if result['exit_code']:
            print(result['output'][:1500].rstrip())
    if report.get('error'):
        print('FAIL', report['error'])
    print('Report:', path)
    print('Controller/platform/device validation remains pending.')
    return 0 if report['prerequisites_passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
