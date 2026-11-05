#!/usr/bin/env python3
"""Probe GCC 15 + stock dynamic runtime without enabling that backend globally."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import uuid
from workspace_config import ROOT, settings, ssh_command, gateway_host
from stock_runtime import inventory, resolve

SOURCES = ROOT / 'scripts/probes/stock-dynamic'


def execute(command, log, **kwargs):
    with log.open('w') as output:
        return subprocess.run(list(map(str, command)), stdout=output, stderr=subprocess.STDOUT, **kwargs).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx-compat', action='store_true', help='resolve missing old-ABI C++ helpers from GCC 15 static archive')
    parser.add_argument('--rust-compat', action='store_true', help='compile and link the existing stock-uClibc C shim with GCC 15')
    parser.add_argument('--only', action='append', choices=['c', 'cpp', 'cpp-bounds', 'rust'], help='select probes; default is all')
    parser.add_argument('--device', type=gateway_host, help='explicitly run validated probes from gateway RAM')
    args = parser.parse_args()
    os.umask(0o077)
    workspace = settings()
    compiler_root = Path(workspace['CROSSTOOL_NG_ROOT'])
    prefix = compiler_root / 'bin/mips-lexra-linux-uclibc-'
    cc, cxx, readelf = (Path(str(prefix) + suffix) for suffix in ('gcc', 'g++', 'readelf'))
    for tool in (cc, cxx, readelf):
        if not tool.is_file(): raise SystemExit('Missing crosstool-NG tool: ' + str(tool))
    (ROOT / 'build').mkdir(exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='stock-dynamic-probes-', dir=ROOT / 'build'))
    print('Probe output:', output, flush=True)
    values = {'STOCK_ROOTFS': Path(workspace['STOCK_ROOTFS'])}
    libs = {name: resolve(values, name) for name in inventory()}
    links = output / 'link'; links.mkdir()
    # No libutil archive or symlink is borrowed from the existing RSDK link dir.
    for name, path in libs.items():
        if name.startswith('lib') and '.so.' in name:
            (links / (name.split('.so.')[0] + '.so')).symlink_to(path)
    report = {'created_utc': datetime.now(timezone.utc).isoformat(), 'device': args.device,
              'compiler': subprocess.check_output([cc, '--version'], text=True),
              'sysroot': subprocess.check_output([cc, '-print-sysroot'], text=True).strip(),
              'production_modified': False, 'probes': {}, 'output': str(output)}
    report['stock_library_sha256'] = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in libs.items()}
    def save_report():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    save_report()
    spec = importlib.util.spec_from_file_location('verify_target', ROOT / 'scripts/verify-gateway-target.py')
    verifier = importlib.util.module_from_spec(spec); spec.loader.exec_module(verifier)
    runtime_values = dict(values, TARGET_READELF=readelf)
    flags = ['-march=lx4380', '-msoft-float', '-mabi=32', '-Os', '-g', '-fno-pie', '-no-pie', '-static-libgcc']
    link_flags = ['-L' + str(links), '-Wl,-rpath-link,' + str(values['STOCK_ROOTFS'] / 'lib'),
                  '-Wl,--dynamic-linker=/lib/ld-uClibc.so.0', '-Wl,-t']

    def validate(name, binary, command):
        entry = report['probes'][name]
        trace = (output / (name + '.build.log')).read_text()
        if re.search(r'rsdk[-/]', trace, re.I):
            entry['validation_error'] = 'RSDK file in build trace'; return
        try:
            header = subprocess.check_output([readelf, '-h', binary], text=True)
            attributes = subprocess.check_output([readelf, '-A', binary], text=True)
            if not all(v in header for v in ('ELF32', 'big endian', 'MIPS', 'mips1', 'o32')) or 'Soft float' not in attributes:
                raise ValueError('wrong target ABI')
            if not re.search(r'Type:\s+EXEC\b', header): raise ValueError('expected non-PIE executable')
            entry['runtime'] = verifier.dynamic_dependencies(runtime_values, binary)
            dump = subprocess.check_output([str(prefix) + 'objdump', '-d', binary], text=True)
            if re.search(r'\b(?:rdhwr|teq)\b', dump): raise ValueError('unsupported stock-kernel instruction')
            # Compiler trace lists all headers; the linker map/trace lists CRTs and libraries.
            source = SOURCES / ('probe.c' if name == 'c' else 'probe.cpp')
            if name != 'rust':
                trace_file = output / (name + '.headers.log')
                header_exit = execute([command[0], *flags, *(['-std=gnu++11', '-D_GLIBCXX_USE_CXX11_ABI=0'] if name != 'c' else []),
                         '-H', '-fsyntax-only', source], trace_file)
                if header_exit: raise ValueError('header provenance check failed')
                if re.search(r'rsdk[-/]', trace_file.read_text(), re.I): raise ValueError('RSDK header detected')
            entry['verified'] = True
        except (ValueError, subprocess.CalledProcessError) as error:
            entry['validation_error'] = str(error)

    for name, source, compiler, extra in [
        ('c', 'probe.c', cc, ['-lpthread', '-lrt', '-lm']),
        ('cpp', 'probe.cpp', cxx, ['-std=gnu++11', '-D_GLIBCXX_USE_CXX11_ABI=0', '-fno-sized-deallocation']),
        ('cpp-bounds', 'probe.cpp', cxx, ['-std=gnu++11', '-D_GLIBCXX_USE_CXX11_ABI=0', '-fno-sized-deallocation', '-DPROBE_BOUNDS'])]:
        if args.only and name not in args.only: continue
        if args.cxx_compat and name.startswith('cpp'):
            archive = subprocess.check_output([cxx, '-print-file-name=libstdc++.a'], text=True).strip()
            extra = [*extra, '-DPROBE_CXX_COMPAT', '-H', '-lstdc++', archive, '-Wl,--gc-sections']
        binary = output / name
        command = [compiler, *flags, SOURCES / source, *link_flags, *extra, '-Wl,-Map,' + str(output / (name + '.map')), '-o', binary]
        entry = report['probes'][name] = {'command': list(map(str, command))}
        entry['build_exit'] = execute(command, output / (name + '.build.log'))
        if entry['build_exit'] == 0: validate(name, binary, command)
        save_report()
        print(name, 'build_exit=', entry['build_exit'], 'verified=', entry.get('verified', False), flush=True)

    if not args.only or 'rust' in args.only:
        # Separate custom Rust target and Cargo cache; global backend/link-mode guards remain untouched.
        crate = output / 'rust-source'; (crate / 'src').mkdir(parents=True)
        shutil.copy2(SOURCES / 'probe.rs', crate / 'src/main.rs')
        (crate / 'Cargo.toml').write_text('[package]\nname="stock-dynamic-rust-probe"\nversion="0.0.0"\nedition="2021"\n[workspace]\n')
        env = dict(os.environ, CARGO_HOME=str(ROOT / 'tools/cargo'), RUSTUP_HOME=str(ROOT / 'tools/rustup'),
                   CARGO_TARGET_DIR=str(output / 'rust-target'), CARGO_INCREMENTAL='0')
        env['PATH'] = str(ROOT / 'tools/cargo/bin') + os.pathsep + env['PATH']
        toolchain = next(line.split('=',1)[1] for line in (ROOT / 'config/toolchains.mk').read_text().splitlines() if line.startswith('RUST_TOOLCHAIN='))
        rustc = ROOT / 'tools/cargo/bin/rustc'; cargo = ROOT / 'tools/cargo/bin/cargo'
        report['rustc_version'] = subprocess.check_output([rustc, '+' + toolchain, '-Vv'], env=env, text=True)
        target = json.loads(subprocess.check_output([rustc, '+' + toolchain, '-Z', 'unstable-options', '--print', 'target-spec-json', '--target', 'mips-unknown-linux-uclibc'], env=env, text=True))
        target.update(cpu='mips1', features='-mips32r2,+mips1,+soft-float', linker=str(cc), **{
            'has-thread-local': False, 'llvm-args': ['-mno-check-zero-division'], 'crt-static-default': False,
            'position-independent-executables': False, 'relocation-model': 'static'})
        target_path = output / 'gcc15-stock-probe.json'; target_path.write_text(json.dumps(target, indent=2))
        compat_args = []
        if args.rust_compat:
            source = ROOT / 'ziggurat/stock-uclibc-compat/native/stock_compat.c'
            nm = Path(str(prefix) + 'nm')
            exports = set()
            for library in ('libc.so.0', 'libpthread.so.0', 'librt.so.0'):
                symbols = subprocess.check_output([nm, '-D', '--defined-only', libs[library]], text=True)
                exports.update(line.split()[-1].split('@')[0] for line in symbols.splitlines() if line.split())
            missing = [name for name in ('epoll_create1', 'eventfd', 'accept4', 'clock_nanosleep',
                       'pthread_setname_np', 'bcmp', 'pthread_getattr_np', 'pthread_attr_getstack') if name not in exports]
            obj = output / 'stock-compat.o'
            compat_command = [cc, *flags, '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-H',
                              *('-DCOMPAT_NEED_' + name for name in missing), '-c', source, '-o', obj]
            shim_exit = execute(compat_command, output / 'rust-compat.build.log')
            report['rust_compat'] = {'source': str(source), 'sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                                     'missing_symbols': missing, 'command': list(map(str, compat_command)), 'build_exit': shim_exit}
            save_report()
            if shim_exit:
                raise SystemExit('Existing compatibility shim failed to compile; see rust-compat.build.log')
            if re.search(r'rsdk[-/]', (output / 'rust-compat.build.log').read_text(), re.I):
                raise SystemExit('RSDK header detected in shim compilation')
            compat_args = ['-C', 'link-arg=' + str(obj)]
        env['RUSTFLAGS'] = ' '.join(shlex.quote(v) for v in ['-C', 'link-arg=-static-libgcc', '-C', 'link-arg=-no-pie',
            *[v for f in link_flags for v in ['-C','link-arg=' + f]], '-C', 'link-arg=-Wl,-Map,' + str(output / 'rust.map'), *compat_args])
        command = [cargo, '+' + toolchain, 'build', '-Zjson-target-spec', '-Zbuild-std=std,panic_unwind', '--release', '--manifest-path', crate / 'Cargo.toml', '--target', target_path]
        entry = report['probes']['rust'] = {'command': list(map(str, command)), 'target': target, 'rustflags': env['RUSTFLAGS']}
        entry['build_exit'] = execute(command, output / 'rust.build.log', env=env)
        built = output / 'rust-target/gcc15-stock-probe/release/stock-dynamic-rust-probe'
        if entry['build_exit'] == 0:
            shutil.copy2(built, output / 'rust'); validate('rust', output / 'rust', command)
        save_report()
        print('rust build_exit=', entry['build_exit'], 'verified=', entry.get('verified', False), flush=True)
    for name, entry in report['probes'].items():
        log = (output / (name + '.build.log')).read_text()
        entry['rsdk_paths_detected'] = bool(re.search(r'rsdk[-/]', log, re.I))
        entry['missing_symbols'] = sorted(set(re.findall(r"undefined reference to [`']([^'`]+)", log)))
        entry['build_error_tail'] = log[-6000:] if entry['build_exit'] else ''
        save_report()
        if entry['rsdk_paths_detected']:
            entry['verified'] = False
            entry['validation_error'] = 'RSDK detected in build evidence'
        if args.device and entry.get('verified'):
            required = entry['runtime']['libraries']
            remote = '/tmp/stock-dynamic-probe-' + uuid.uuid4().hex
            # Verify exactly the required installed runtime before executing any probe.
            checks = subprocess.check_output(ssh_command(args.device) + ['md5sum ' + ' '.join('/lib/' + n for n in required)], text=True)
            actual = {line.split()[-1].split('/')[-1]: line.split()[0] for line in checks.splitlines()}
            if any(actual.get(n) != hashlib.md5(libs[n].read_bytes()).hexdigest() for n in required):
                entry['runtime_error'] = 'Installed runtime differs from pinned stock libraries'; save_report(); continue
            binary = output / name
            script = ('umask 077; cat > ' + remote + '; chmod 700 ' + remote + '; ' + remote +
                      ' & child=$!; (sleep 20; kill -KILL "$child" 2>/dev/null) & watch=$!; '
                      'wait "$child"; result=$?; kill "$watch" 2>/dev/null; rm -f ' + remote + '; exit "$result"')
            with binary.open('rb') as source:
                entry['run_exit'] = execute(ssh_command(args.device) + [script], output / (name + '.run.log'), stdin=source, timeout=35)
            entry['run_output'] = (output / (name + '.run.log')).read_text()
            print(name, 'run_exit=', entry['run_exit'], entry['run_output'].strip(), flush=True)
    save_report()
    print('Report:', output / 'report.json', flush=True)


if __name__ == '__main__': main()
