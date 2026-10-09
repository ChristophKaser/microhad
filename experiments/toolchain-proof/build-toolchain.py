#!/usr/bin/env python3
"""Build an isolated compiler; production compiler configuration is never read."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BUILD = ROOT / 'build/toolchain-proof'
DOWNLOADS = BUILD / 'downloads'
BOOTSTRAP = ROOT / 'tools/crosstool-ng-proof'
PREFIX = ROOT / 'tools/lexra-proof'


def run(args, cwd=BUILD, env=None):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env, check=True)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=str(BOOTSTRAP / 'bin') + ':' + os.environ['PATH'])
    for variable in ['RSDK_ROOT', 'TARGET_CC', 'TARGET_CXX', 'TARGET_AR', 'CC', 'CXX', 'AR',
                     'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH', 'C_INCLUDE_PATH',
                     'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH', 'LD_LIBRARY_PATH']:
        env.pop(variable, None)
    # Checksums are pinned independently of a cached file's presence.
    sources = json.loads((HERE / 'sources.json').read_text())
    for name, metadata in sources.items():
        archive = DOWNLOADS / name
        if not archive.exists():
            temporary = archive.with_suffix(archive.suffix + '.part')
            run(['curl', '-fL', '--retry', '2', '--connect-timeout', '20',
                 '--max-time', '600', metadata['url'], '-o', temporary])
            temporary.rename(archive)
        if digest(archive) != metadata['sha256']:
            raise SystemExit('source checksum mismatch: ' + str(archive))

    for package in ['help2man-1.49.3', 'libtool-2.5.4', 'crosstool-ng-1.28.0']:
        source = BUILD / package
        if not source.exists():
            run(['tar', '-xf', DOWNLOADS / (package + '.tar.xz')])
        executable = {'help2man': 'help2man', 'libtool': 'libtool', 'crosstool-ng': 'ct-ng'}
        binary = executable[package.rsplit('-', 1)[0]]
        if not (BOOTSTRAP / 'bin' / binary).exists():
            args = ['./configure', '--prefix=' + str(BOOTSTRAP)]
            if package.startswith('help2man'):
                args += ['--disable-nls']
            run(args, source, env)
            run(['make', '-j8'], source, env)
            run(['make', 'install'], source, env)

    patches = ROOT / 'firmware/dependencies/rtl8196e-gateway/1-Build-Environment/10-lexra-toolchain/patches'
    binutils = BUILD / 'binutils-2.45.1'
    marker = binutils / '.lexra-proof-patched'
    if not marker.exists():
        if binutils.exists():
            raise SystemExit('incomplete binutils preparation; remove only ' + str(binutils))
        run(['tar', '-xf', DOWNLOADS / 'binutils-2.45.1.tar.xz'])
        run(['patch', '-p1', '-i', patches / 'binutils/2.45.1/600-Lexra.patch'], binutils)
        marker.write_text(digest(patches / 'binutils/2.45.1/600-Lexra.patch') + '\n')
    elif marker.read_text().strip() != digest(patches / 'binutils/2.45.1/600-Lexra.patch'):
        raise SystemExit('binutils patch changed; remove only the prepared binutils source and rebuild')
    gcc_patch_dir = BUILD / 'patches/gcc/15.2.0'
    gcc_patch_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(patches / 'gcc/15.2.0/970-Lexra.patch', gcc_patch_dir / '970-Lexra.patch')
    patch_records = {str(path.relative_to(ROOT)): digest(path) for path in [
        patches / 'binutils/2.45.1/600-Lexra.patch', patches / 'gcc/15.2.0/970-Lexra.patch']}
    (BUILD / 'source-manifest.json').write_text(json.dumps({
        'sources': sources, 'patches': patch_records,
        'upstream_commit': subprocess.check_output(['git', '-C', str(patches.parents[2]),
                                                   'rev-parse', 'HEAD'], text=True).strip(),
    }, indent=2) + '\n')
    config = (HERE / 'crosstool.config').read_text().replace('@ROOT@', str(ROOT)).replace('@BUILD@', str(BUILD))
    (BUILD / '.config').write_text(config)
    run([BOOTSTRAP / 'bin/ct-ng', 'olddefconfig'], env=env)
    resolved = (BUILD / '.config').read_text()
    for required in ['CT_ARCH_ARCH="lx4380"', 'CT_THREADS="linuxthreads"', 'CT_GCC_VERSION="15.2.0"',
                     'CT_UCLIBC_NG_VERSION="1.0.54"', '# CT_CC_GCC_CONFIG_TLS is not set']:
        if required not in resolved:
            raise SystemExit('configuration lost required setting: ' + required)
    fingerprint = hashlib.sha256((resolved + (HERE / 'uclibc.config').read_text() +
                                  json.dumps(sources, sort_keys=True) + json.dumps(patch_records, sort_keys=True)).encode()).hexdigest()
    complete = PREFIX / '.proof-complete'
    if complete.exists() and complete.read_text().strip() == fingerprint:
        print('Toolchain already built; configuration prepared but rebuild skipped.')
        return
    run([BOOTSTRAP / 'bin/ct-ng', 'build'], env=env)
    libc_config = (PREFIX / 'mips-lexra-linux-uclibc/sysroot/usr/include/bits/uClibc_config.h').read_text()
    for unsupported in ['__UCLIBC_HAS_TLS__', '__UCLIBC_USE_TIME64__', '__UCLIBC_HAS_FPU__']:
        if '#define ' + unsupported + ' 1' in libc_config:
            raise SystemExit('installed libc has an incompatible feature: ' + unsupported)
    if '#define __UCLIBC_HAS_LINUXTHREADS__ 1' not in libc_config:
        raise SystemExit('installed libc does not use LinuxThreads')
    for required in ['__UCLIBC_SUSV3_LEGACY__', '__UCLIBC_SUSV4_LEGACY__', '__UCLIBC_USE_NETLINK__', '__UCLIBC_SUPPORT_AI_ADDRCONFIG__']:
        if '#define ' + required + ' 1' not in libc_config:
            raise SystemExit('installed libc lost application API support: ' + required)
    complete.write_text(fingerprint + '\n')


if __name__ == '__main__':
    main()
