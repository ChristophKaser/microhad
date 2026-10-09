#!/usr/bin/env python3
"""Compile identical isolated probes with host, RSDK, or crosstool-NG."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / 'build/toolchain-proof'


def run(args, env=None, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), env=env, check=True, **kwargs)


def output(args, env=None):
    return subprocess.check_output(list(map(str, args)), env=env, text=True)


def verify(binary, readelf):
    header = output([readelf, '-h', binary])
    segments = output([readelf, '-l', binary])
    dynamic = output([readelf, '-d', binary])
    attributes = output(['readelf', '-A', binary])
    assert 'ELF32' in header and 'big endian' in header and 'MIPS' in header, header
    assert 'o32' in header and 'mips1' in header, header
    assert 'INTERP' not in segments and 'There is no dynamic section' in dynamic
    # Old RSDK does not always emit floating-point ABI attributes.
    assert 'Hard float' not in attributes
    assembly = output(['llvm-objdump-18', '-d', '--mcpu=mips32r2', binary])
    binary.with_suffix('.disassembly').write_text(assembly)
    counts = {name: len(re.findall(r'\t' + name + r'\s', assembly))
              for name in ['rdhwr', 'teq', 'll', 'sc', 'sync', 'lwl', 'lwr', 'swl', 'swr']}
    assert counts['rdhwr'] == 0 and counts['teq'] == 0, counts
    return {'size': binary.stat().st_size, 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'instructions': counts, 'elf_header': header, 'attributes': attributes}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('backend', choices=['host', 'rsdk', 'crosstool-ng'])
    parser.add_argument('--rsdk-root', type=Path)
    args = parser.parse_args()
    destination = BUILD / 'probes' / args.backend
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'report.json').unlink(missing_ok=True)
    env = dict(os.environ)
    for variable in ['RUSTFLAGS', 'RSDK_ROOT', 'TARGET_CC', 'TARGET_CXX', 'TARGET_AR',
                     'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                     'PROOF_NATIVE_ARCHIVE_DIR', 'PROOF_CC']:
        env.pop(variable, None)
    settings = dict(line.split('=', 1) for line in (ROOT / 'config/toolchains.mk').read_text().splitlines()
                    if line and not line.startswith('#'))
    env.update(CARGO_HOME=str(ROOT / 'tools/cargo'), RUSTUP_HOME=str(ROOT / 'tools/rustup'),
               CARGO_TARGET_DIR=str(destination / 'rust-target'), CARGO_INCREMENTAL='0')
    env['PATH'] = str(ROOT / 'tools/cargo/bin') + ':' + env['PATH']
    if args.backend == 'host':
        cc, cxx, ar, readelf = 'gcc', 'g++', 'ar', 'readelf'
        flags = ['-O2']
    else:
        prefix = 'mips-lexra-linux-uclibc-' if args.backend == 'crosstool-ng' else settings['TARGET_PREFIX']
        compiler_root = ROOT / 'tools/lexra-proof' if args.backend == 'crosstool-ng' else (
            args.rsdk_root or Path(os.environ.get('RSDK_ROOT', ROOT / settings['RSDK_RELATIVE']))).resolve()
        cc, cxx, ar, readelf = [str(compiler_root / 'bin' / (prefix + name)) for name in ['gcc', 'g++', 'ar', 'readelf']]
        flags = ['-O2', '-msoft-float', '-mabi=32']
    native_object = destination / 'native.o'
    run([cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', *flags, '-c', HERE / 'native.c', '-o', native_object], env)
    run([cxx, '-std=gnu++98', '-Wall', '-Wextra', '-Werror', *flags,
         HERE / 'native.cpp', native_object, '-o', destination / 'native', '-static', '-lpthread', '-lrt', '-lm'], env)
    report = {'backend': args.backend, 'compiler': output([cc, '--version'])}
    if args.backend != 'host':
        report['sysroot'] = output([cc, '-print-sysroot'], env).strip()
        report['runtime_archives'] = {library: output([cc, '-print-file-name=' + library], env).strip()
                                      for library in ['libc.a', 'libpthread.a', 'libstdc++.a', 'libm.a']}
        if args.backend == 'crosstool-ng':
            for path in [report['sysroot'], *report['runtime_archives'].values()]:
                assert Path(path).resolve().is_relative_to(compiler_root.resolve()), path
        symbols = set()
        for library in ['libc.a', 'libpthread.a', 'librt.a']:
            path = output([cc, '-print-file-name=' + library]).strip()
            symbols.update(line.split()[-1] for line in output(
                [str(Path(ar).with_name(prefix + 'nm')), '--defined-only', path]).splitlines() if line.split())
        missing = [name for name in ['epoll_create1', 'eventfd', 'accept4', 'clock_nanosleep', 'pthread_setname_np',
                                     'bcmp', 'pthread_getattr_np', 'pthread_attr_getstack']
                   if name not in symbols]
        report['compatibility_wrappers'] = missing
        report['unsupported_stack_introspection'] = [name for name in missing if name in ['pthread_getattr_np', 'pthread_attr_getstack']]
        run([cc, '-std=gnu99', '-Wall', '-Wextra', '-Werror', *flags,
             *['-DPROOF_NEED_' + name for name in missing], '-c', HERE / 'compat.c', '-o', destination / 'compat.o'], env)
        run([cc, '-std=gnu99', *flags, '-c', ROOT / 'ziggurat/stock-uclibc-compat/native/fault_report.c',
             '-o', destination / 'fault_report.o'], env)
        archive = destination / 'libproof_compat.a'
        archive.unlink(missing_ok=True)
        run([ar, 'rcs', archive, destination / 'compat.o', destination / 'fault_report.o'], env)
        env['PROOF_NATIVE_ARCHIVE_DIR'] = str(destination)
        env['PROOF_CC'] = cc
    cargo = ['cargo', '+' + settings['RUST_TOOLCHAIN']]
    # Generate the lock once; subsequent builds use exactly the same dependencies.
    manifest = HERE / 'rust/Cargo.toml'
    if not (manifest.parent / 'Cargo.lock').exists():
        run([*cargo, 'generate-lockfile', '--offline', '--manifest-path', manifest], env)
    build = [*cargo, 'build', '--locked', '--offline', '--release', '--manifest-path', manifest]
    if args.backend == 'host':
        rust_binary = destination / 'rust-target/release/lexra-toolchain-proof'
    else:
        spec = json.loads(output(['rustc', '+' + settings['RUST_TOOLCHAIN'], '-Z', 'unstable-options',
                                 '--print', 'target-spec-json', '--target', 'mips-unknown-linux-uclibc'], env))
        spec.update(cpu='mips1', features='-mips32r2,+mips1,+soft-float', linker=cc)
        spec['has-thread-local'] = False
        spec['llvm-args'] = ['-mno-check-zero-division']
        spec['metadata'] = {'description': 'Isolated stock Lexra compiler proof', 'tier': 3, 'host_tools': False, 'std': True}
        target = destination / 'lexra-proof.json'
        target.write_text(json.dumps(spec, indent=2) + '\n')
        env['RUSTFLAGS'] = '-C target-feature=+crt-static -C relocation-model=static'
        build += ['--target', target, '-Z', 'json-target-spec', '-Z', 'build-std=std,panic_abort']
        rust_binary = destination / 'rust-target/lexra-proof/release/lexra-toolchain-proof'
    run(build, env)
    run(['cp', rust_binary, destination / 'rust'])
    for name in ['native', 'rust']:
        binary = destination / name
        if args.backend == 'host':
            run(['timeout', '60', binary])
            report[name] = {'size': binary.stat().st_size}
        else:
            original_size = binary.stat().st_size
            run(['cp', binary, binary.with_suffix('.unstripped')])
            run(['llvm-strip-18', '--keep-section=.gnu.attributes', binary])
            report[name] = verify(binary, readelf)
            report[name]['unstripped_size'] = original_size
    (destination / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS build and verification:', args.backend)


if __name__ == '__main__':
    main()
