#!/usr/bin/env python3
"""Build static crosstool-NG binary/source releases; never access a gateway."""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts/release'))
from common import inventory, sha, verify_binary, verify_elf
import release_source

DEPENDENCIES = ['microhad/dependencies/mbedtls', 'microhad/dependencies/tinycbor',
                'micro-thread-matter-daemon/dependencies/openthread',
                'micro-thread-matter-daemon/dependencies/rs-matter', 'ziggurat/dependencies/ziggurat']
PREPARED = ['worktrees/ziggurat-multipan', 'build/openthread-unix/source-mailbox',
            'build/micro-thread-matter-daemon/service-source',
            *['build/patched-crates-mailbox/' + n for n in ('getrandom-0.4.3', 'nix-0.26.4', 'radium-0.7.0')]]
EXCLUDE = ['microhad/test/fixtures/tls/', 'io.github.microhad/', 'firmware/dependencies/', 'firmware/hdlc-multipan/',
           '.codex/', '.githooks/', '.gitmodules']


def run(command, cwd=ROOT, env=None, capture=False):
    # Never include the inherited shell environment or credentials in build logs.
    print('+ ' + shlex.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), cwd=cwd, env=env, check=True,
                          text=True, stdout=subprocess.PIPE if capture else None).stdout


def git(*args, cwd=ROOT):
    return subprocess.check_output(['git', '-C', str(cwd), *args], text=True).strip()


def environment(root, jobs, cargo_home, rustup_home, epoch):
    env = {k: v for k, v in os.environ.items() if k in ('PATH', 'HOME', 'TMPDIR', 'LANG', 'TERM')}
    env.update(TOOLCHAIN='crosstool-ng', LINK_MODE='static', WORKSPACE_CONFIG='/dev/null',
               CARGO_HOME=str(cargo_home), RUSTUP_HOME=str(rustup_home), BUILD_JOBS=str(jobs),
               CARGO_BUILD_JOBS=str(jobs), ZIGGURAT_MULTIPAN='1', SOURCE_DATE_EPOCH=str(epoch),
               npm_config_userconfig='/dev/null', npm_config_globalconfig=str(root / 'scripts/release/npmrc'), npm_config_audit='false',
               npm_config_fund='false', npm_config_cache=str(root / '.release/npm-cache'), MICROHAD_RELEASE_SOFTWARE_ONLY='1')
    env['PATH'] = str(cargo_home / 'bin') + ':' + env['PATH']
    return env


def excluded(name):
    return any(name.startswith(prefix) for prefix in EXCLUDE) or name.startswith('firmware/scripts/') and name != 'firmware/scripts/build-maintenance.sh'


def export_git(source, destination, recursive=True, filter_root=False, revision=None):
    """Export tracked bytes only. Never copy .git, caches, local settings or state."""
    destination.mkdir(parents=True, exist_ok=True)
    revision = revision or git('rev-parse', 'HEAD', cwd=source)
    for row in git('ls-tree', '-r', '-z', revision, cwd=source).split('\0'):
        if not row:
            continue
        fields, name = row.split('\t', 1)
        mode, _, oid = fields.split()
        if filter_root and excluded(name):
            continue
        target = destination / name
        if mode == '160000':
            if recursive:
                if git('rev-parse', 'HEAD', cwd=source / name) != oid:
                    raise ValueError('Dependency pin mismatch: ' + name)
                export_git(source / name, target, revision=oid)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        data = subprocess.check_output(['git', '-C', str(source), 'cat-file', 'blob', oid])
        if mode == '120000':
            target.symlink_to(data.decode())
        else:
            target.write_bytes(data)
            target.chmod(0o755 if mode == '100755' else 0o644)


def archive(root, destination, epoch):
    with tarfile.open(destination, 'w:xz', preset=3) as output:
        for path in [root, *sorted(root.rglob('*'))]:
            name = root.name if path == root else root.name + '/' + path.relative_to(root).as_posix()
            info = output.gettarinfo(str(path), name)
            if info.islnk():
                raise ValueError('Unexpected hardlink in release')
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mtime = epoch
            info.mode = 0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644
            if info.isfile():
                with path.open('rb') as stream:
                    output.addfile(info, stream)
            else:
                output.addfile(info)


def pinned_revisions(revision):
    result = {}
    def visit(repo, revision, prefix='', recurse=False):
        for line in git('ls-tree', '-r', revision, cwd=repo).splitlines():
            if line.startswith('160000 '):
                fields, name = line.split('\t', 1)
                oid = fields.split()[2]
                result[prefix + name] = oid
                if recurse or name in DEPENDENCIES:
                    visit(repo / name, oid, prefix + name + '/', True)
    visit(ROOT, revision)
    return result


def snapshot_sources(root):
    records = inventory(root, (p for p in root.rglob('*') if '.git' not in p.parts
        and 'node_modules' not in p.parts and not p.is_relative_to(root / '.release/npm-cache/_logs')
        and p != root / '.release/source-manifest.json'))
    (root / '.release/source-manifest.json').write_text(json.dumps({'schema': 1, 'files': records}, indent=2, sort_keys=True) + '\n')


def rust_setup(root, env, expected=None):
    config = dict(line.split('=', 1) for line in (root / 'config/toolchains.mk').read_text().splitlines()
                  if line and not line.startswith('#'))
    toolchain = config['RUST_TOOLCHAIN']
    version = run(['rustc', '+' + toolchain, '-Vv'], env=env, capture=True)
    if expected and version != expected:
        raise ValueError('Installed Rust compiler does not match the release')
    sysroot = Path(run(['rustc', '+' + toolchain, '--print', 'sysroot'], env=env, capture=True).strip())
    if not (sysroot / 'lib/rustlib/src/rust/library/Cargo.toml').is_file():
        raise ValueError('Install rust-src for ' + toolchain + ' before building')
    return toolchain, version, sysroot


def rust_source_sysroot(root, compiler_sysroot, env):
    """Build std from bundled source, while using a separately installed compiler."""
    destination = root / '.release/rust-sysroot'
    destination.mkdir(exist_ok=True)
    (destination / 'lib').mkdir(exist_ok=True)
    for path in (compiler_sysroot / 'lib').iterdir():
        if path.name != 'rustlib':
            target = destination / 'lib' / path.name
            if not target.exists(): target.symlink_to(path)
    rustlib = destination / 'lib/rustlib'
    rustlib.mkdir(exist_ok=True)
    for path in (compiler_sysroot / 'lib/rustlib').iterdir():
        if path.name != 'src':
            target = rustlib / path.name
            if not target.exists(): target.symlink_to(path, target_is_directory=path.is_dir())
    (rustlib / 'src').mkdir(exist_ok=True)
    source = rustlib / 'src/rust'
    if not source.exists(): source.symlink_to(root / '.release/rust-src', target_is_directory=True)
    wrapper = root / '.release/rustc-wrapper.sh'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(compiler_sysroot / 'bin/rustc')) +
                       ' --sysroot ' + shlex.quote(str(destination)) + ' "$@"\n')
    wrapper.chmod(0o755)
    env['RUSTC'] = str(wrapper)


def cargo_patches(root):
    args = []
    for name, version in [('getrandom', '0.4.3'), ('nix', '0.26.4'), ('radium', '0.7.0')]:
        args += ['--config', f'patch.crates-io.{name}.path="{root}/build/patched-crates-mailbox/{name}-{version}"']
    return args


def toolchain_evidence(prefix):
    compiler = prefix / 'bin/mips-lexra-linux-uclibc-gcc'
    result = {'compiler_version': subprocess.check_output([compiler, '--version'], text=True), 'runtime_archives': {}}
    for name in ('libc.a', 'libm.a', 'libpthread.a', 'libutil.a', 'librt.a', 'libdl.a', 'libstdc++.a', 'libgcc.a', 'libgcc_eh.a', 'crt1.o', 'crti.o', 'crtn.o'):
        path = Path(subprocess.check_output([compiler, '-print-file-name=' + name], text=True).strip()).resolve()
        if not path.is_relative_to(prefix.resolve()):
            raise ValueError('Runtime outside validated compiler: ' + name)
        result['runtime_archives'][name] = sha(path)
    return result


def collect(root, args, env, toolchain, sysroot):
    # Generated Rust workspaces and registry patches are validated by their existing helpers.
    run(['bash', 'ziggurat/scripts/build.sh', '--prepare-only'], env=env)
    run(['bash', 'micro-thread-matter-daemon/scripts/fetch-rs-matter.sh'], env=env)
    run(['python3', 'micro-thread-matter-daemon/scripts/prepare-openthread.py'], env=env)
    for manifest in ('micro-thread-matter-daemon/Cargo.toml', 'worktrees/ziggurat-multipan/Cargo.toml'):
        run(['cargo', '+' + toolchain, 'fetch', '--manifest-path', manifest], env=env)
    run(['python3', 'scripts/prepare-crate-patches.py', 'getrandom-0.4.3', 'nix-0.26.4', 'radium-0.7.0'], env=env)
    export_git(ROOT, root, recursive=False, filter_root=True, revision=args.revision)
    for name in DEPENDENCIES:
        export_git(ROOT / name, root / name)
    for name in PREPARED:
        export_git(ROOT / name, root / name)
        # Cargo.lock is intentionally ignored by the mailbox comparison; include the effective lock.
        if (ROOT / name / 'Cargo.lock').is_file():
            shutil.copy2(ROOT / name / 'Cargo.lock', root / name / 'Cargo.lock')
    patch_root = 'firmware/dependencies/rtl8196e-gateway'
    for relative in ('LICENSE', '1-Build-Environment/10-lexra-toolchain/patches/binutils/2.45.1/600-Lexra.patch',
                     '1-Build-Environment/10-lexra-toolchain/patches/gcc/15.2.0/970-Lexra.patch'):
        target = root / patch_root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / patch_root / relative, target)
    for filename, record in json.loads((ROOT / 'config/crosstool-ng/sources.json').read_text()).items():
        source = ROOT / 'build/toolchain-proof/downloads' / filename
        if sha(source) != record['sha256']:
            raise ValueError('Toolchain archive checksum mismatch: ' + filename)
        target = root / 'build/toolchain-proof/downloads' / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    shutil.copytree(sysroot / 'lib/rustlib/src/rust', root / '.release/rust-src')
    rust_licenses = root / '.release/runtime-licenses/rust'
    rust_licenses.mkdir(parents=True)
    for p in (sysroot / 'share/doc/rust').glob('COPYRIGHT-library.html'):
        shutil.copy2(p, rust_licenses / p.name)
    shutil.copytree(ROOT / 'tools/lexra-proof/share/licenses/uClibc-ng', root / '.release/runtime-licenses/uClibc-ng')
    shutil.copy2(ROOT / 'tools/lexra-proof/share/licenses/linux/COPYING', root / '.release/runtime-licenses/Linux-COPYING')
    (root / '.release/toolchain.config').write_text((ROOT / 'build/toolchain-proof/.config').read_text().replace(str(ROOT), '@ROOT@'))
    shutil.copy2(ROOT / 'tools/lexra-proof/mips-lexra-linux-uclibc/sysroot/usr/include/bits/uClibc_config.h', root / '.release/libc-config.h')
    gcc = ROOT / 'tools/lexra-proof/share/licenses/gcc'
    for filename in ('COPYING3', 'COPYING.RUNTIME'):
        shutil.copy2(gcc / filename, root / '.release/runtime-licenses' / filename)
    vendor_env = dict(env, npm_config_cache=str(root / '.release/npm-cache'))
    vendor = run(['cargo', '+' + toolchain, 'vendor', '--versioned-dirs',
                  '--manifest-path', 'micro-thread-matter-daemon/Cargo.toml',
                  '--sync', 'worktrees/ziggurat-multipan/Cargo.toml',
                  '--sync', '.release/rust-src/library/Cargo.toml', *cargo_patches(root), '.release/cargo-vendor'],
                 cwd=root, env=vendor_env, capture=True)
    # Cargo emits an absolute directory when given an absolute path; keep archive relocation safe.
    vendor = vendor.replace(str(root / '.release/cargo-vendor'), '.release/cargo-vendor')
    (root / '.cargo').mkdir(exist_ok=True)
    patches = '\n[patch.crates-io]\n' + '\n'.join(
        name + ' = { path = "build/patched-crates-mailbox/' + name + '-' + version + '" }'
        for name, version in [('getrandom', '0.4.3'), ('nix', '0.26.4'), ('radium', '0.7.0')]) + '\n'
    (root / '.cargo/config.toml').write_text(vendor + patches)
    # Cargo records globally configured but unused patches in lockfiles. Allow
    # that bookkeeping only; a changed dependency graph needs a notice review.
    original_lock = ROOT / 'micro-thread-matter-daemon/Cargo.lock'
    effective_lock = root / 'micro-thread-matter-daemon/Cargo.lock'
    if tomllib.loads(original_lock.read_text())['package'] != tomllib.loads(effective_lock.read_text())['package']:
        raise ValueError('Vendoring changed the Matter dependency graph; review notices before releasing')
    (root / '.release/original-lockfiles').mkdir()
    shutil.copy2(original_lock, root / '.release/original-lockfiles/matter.Cargo.lock')
    notice = root / 'third-party/gateway-inputs.json'
    snapshot = json.loads(notice.read_text())
    snapshot['inputs']['micro-thread-matter-daemon/Cargo.lock'] = sha(effective_lock)
    notice.write_text(json.dumps(snapshot, indent=2, sort_keys=True) + '\n')
    run(['npm', 'ci', '--ignore-scripts', '--no-audit'], cwd=root / 'microhad/webui', env=vendor_env)
    shutil.rmtree(root / 'microhad/webui/node_modules')
    if (root / '.release/npm-cache/_logs').exists(): shutil.rmtree(root / '.release/npm-cache/_logs')


def build(root, env, version, jobs):
    run(['bash', 'ziggurat/scripts/build.sh'], cwd=root, env=env)
    run(['bash', 'micro-thread-matter-daemon/scripts/build.sh', 'mips', 'service'], cwd=root, env=env)
    for enabled in (1, 0):
        run(['make', '-C', 'microhad', '-j' + str(jobs), 'mips', 'rng-seed',
             'ZIGGURAT=' + str(enabled), 'MATTER=' + str(enabled), 'BUILD_REVISION=' + version], cwd=root, env=env)
    run(['bash', 'firmware/scripts/build-maintenance.sh'], cwd=root, env=env)
    run(['make', '-C', 'microhad', 'webui'], cwd=root, env=env)


def binaries(root, destination, metadata):
    destination.mkdir(parents=True)
    (destination / '.release').mkdir()
    mapping = {
        'bin/microhad': 'microhad/build/crosstool-ng/mips-ziggurat-matter/microhad',
        'bin/microhad.matter-manifest': 'microhad/build/crosstool-ng/mips-ziggurat-matter/microhad.matter-manifest',
        'bin/microhad-ezsp': 'microhad/build/crosstool-ng/mips/microhad',
        'bin/mthread-matter': 'build/crosstool-ng/micro-thread-matter-daemon/mthread-matter-mips',
        'bin/ziggurat': 'build/crosstool-ng/ziggurat-hdlc/lexra-crosstool-ng-uclibc/release/ziggurat.stripped',
        'bin/gateway-maintenance': 'build/crosstool-ng/gateway-maintenance',
        'bin/rng-seed': 'microhad/build/crosstool-ng/rng-seed',
    }
    for target, source in mapping.items():
        out = destination / target
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / source, out)
    for name in ['deploy.sh', 'config/toolchains.mk', 'scripts/workspace-env.sh', 'scripts/workspace-ssh.sh',
                 'scripts/release/install.py', 'scripts/release/common.py', 'microhad/native_start.sh',
                 'microhad/ziggurat_start.sh', 'microhad/paired_start.sh', 'microhad/scripts/paired_control.sh']:
        out = destination / name
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / name, out)
    shutil.copytree(root / 'microhad/webui/dist', destination / 'webui')
    import html
    runtime = root / '.release/runtime-licenses'
    text = 'MicroHAD, Ziggurat and the Matter daemon use uClibc-ng under LGPL 2.1.\n'
    text += 'Matching library sources, configurations and rebuild/relink instructions accompany this release.\n\n'
    for p in sorted(runtime.rglob('*')):
        if p.is_file() and p.suffix != '.html':
            text += str(p.relative_to(runtime)) + '\n' + p.read_text() + '\n\n'
    (destination / 'webui/runtime-licenses.html').write_text('<!doctype html><meta charset="utf-8"><title>Runtime licenses</title><h1>Runtime licenses</h1><pre style="white-space:pre-wrap">' + html.escape(text) + '</pre>')
    shutil.copy2(runtime / 'rust/COPYRIGHT-library.html', destination / 'webui/rust-licenses.html')
    notice_page = destination / 'webui/licenses.html'
    notice_page.write_text(notice_page.read_text().replace('<pre>', '<p><a href="/runtime-licenses.html">C/C++ runtime licenses</a> · <a href="/rust-licenses.html">Rust standard-library notices</a></p><pre>', 1))
    shutil.copytree(root / '.release/runtime-licenses', destination / 'licenses/runtime')
    shutil.copy2(root / 'LICENSE', destination / 'licenses/LICENSE')
    shutil.copy2(root / 'THIRD_PARTY.md', destination / 'licenses/THIRD_PARTY.md')
    shutil.copy2(root / 'third-party/gateway-notices.txt', destination / 'licenses/THIRD_PARTY_NOTICES.txt')
    shutil.copy2(root / 'docs/binary-releases.md', destination / 'README.md')
    for mode in ('paired', 'microhad-only'):
        config = json.loads((root / 'microhad/config.example.json').read_text())
        if mode == 'microhad-only': config.pop('ziggurat_socket', None)
        (destination / ('config.' + mode + '.example.json')).write_text(json.dumps(config, indent=2) + '\n')
    script = destination / 'install.sh'
    script.write_text('#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec python3 "$root/scripts/release/install.py" "$@"\n')
    script.chmod(0o755)
    (destination / '.release/binary-manifest.json').write_text(json.dumps(
        dict(metadata, files=inventory(destination)), indent=2, sort_keys=True) + '\n')
    verify_binary(destination)


def check_clean():
    if not git('ls-files', 'scripts/build-release.py'):
        raise ValueError('Commit the release builder before building a release')
    for name in git('diff', '--name-only', 'HEAD').splitlines():
        if not excluded(name): raise ValueError('Commit release input changes first: ' + name)
    rtl = ROOT / 'firmware/dependencies/rtl8196e-gateway'
    if not (rtl / '.git').exists():
        raise ValueError('Initialize the pinned RTL8196E submodule for its compiler patches')
    if git('rev-parse', 'HEAD', cwd=rtl) != git('rev-parse', 'HEAD:firmware/dependencies/rtl8196e-gateway'):
        raise ValueError('Compiler patch submodule does not match its parent pin')
    if git('diff', '--name-only', 'HEAD', '--', 'LICENSE', '1-Build-Environment/10-lexra-toolchain/patches', cwd=rtl):
        raise ValueError('Compiler patches/license must match the pinned committed source')
    for name in DEPENDENCIES:
        if not (ROOT / name / '.git').exists():
            raise ValueError('Initialize required software submodules first: git submodule update --init --recursive ' + ' '.join(DEPENDENCIES))
        if git('status', '--porcelain', '--untracked-files=all', cwd=ROOT / name):
            raise ValueError('Modified dependency: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument('--cargo-home', type=Path, default=Path(os.environ.get('CARGO_HOME', ROOT / 'tools/cargo')))
    parser.add_argument('--rustup-home', type=Path, default=Path(os.environ.get('RUSTUP_HOME', ROOT / 'tools/rustup')))
    parser.add_argument('--rebuild', action='store_true', help='rebuild an extracted source package offline')
    args = parser.parse_args()
    if args.jobs < 1 or platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('Requires a Linux x86_64 host and positive --jobs')
    if args.rebuild:
        metadata = release_source.metadata()
        version = metadata['version']
        epoch = metadata['source_date_epoch']
        release_source.verify()
    else:
        if not args.version or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.version):
            parser.error('--version must be a safe filename component')
        check_clean()
        args.revision = git('rev-parse', 'HEAD')
        version = args.version
        epoch = int(git('show', '-s', '--format=%ct', 'HEAD'))
    output = (args.output or ROOT / 'build/releases' / version).resolve()
    if output.exists(): raise ValueError('Output already exists: ' + str(output))
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.with_name(output.name + '.incomplete')
    if staging.exists(): raise ValueError('Incomplete staging exists; inspect it before retrying: ' + str(staging))
    staging.mkdir()
    env = environment(ROOT, args.jobs, args.cargo_home.resolve(), args.rustup_home.resolve(), epoch)
    toolchain, rust_version, sysroot = rust_setup(ROOT, env, metadata['rust_version'] if args.rebuild else None)
    if args.rebuild:
        root = ROOT
        env.update(MICROHAD_SOURCE_BUNDLE='1', CARGO_NET_OFFLINE='true', npm_config_offline='true')
        run(['python3', 'scripts/build-toolchain.py'], env=env)
    else:
        run(['python3', 'scripts/licensing/notices.py', 'check'], env=env)
        run(['python3', 'scripts/build-toolchain.py'], env=env)
        root = staging / ('microhad-' + version + '-source')
        collect(root, args, env, toolchain, sysroot)
        revisions = pinned_revisions(args.revision)
        metadata = {'schema': 1, 'version': version, 'revision': args.revision,
                    'source_date_epoch': epoch, 'toolchain': 'crosstool-ng', 'link_mode': 'static',
                    'modes': ['paired', 'microhad-only'], 'rust_version': rust_version,
                    'revisions': revisions,
                    'notice_inputs': json.loads((root / 'third-party/gateway-inputs.json').read_text())['inputs'],
                    **toolchain_evidence(ROOT / 'tools/lexra-proof')}
        (root / '.release/metadata.json').write_text(json.dumps(metadata, indent=2, sort_keys=True) + '\n')
        rebuild = root / 'rebuild.sh'
        rebuild.write_text('#!/bin/sh\nset -eu\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec python3 "$root/scripts/build-release.py" --rebuild "$@"\n')
        rebuild.chmod(0o755)
        snapshot_sources(root)
        # Reuse only the validated compiler, not application outputs or dependency caches.
        env = environment(root, args.jobs, args.cargo_home.resolve(), args.rustup_home.resolve(), epoch)
        env.update(CROSSTOOL_NG_ROOT=str(ROOT / 'tools/lexra-proof'), MICROHAD_SOURCE_BUNDLE='1',
                   CARGO_NET_OFFLINE='true', npm_config_offline='true')
        # Archive source inputs before builds create output/cache files.
        archive(root, staging / (root.name + '.tar.xz'), epoch)
    compiler_prefix = Path(env.get('CROSSTOOL_NG_ROOT', root / 'tools/lexra-proof'))
    metadata.update(toolchain_evidence(compiler_prefix))
    rust_source_sysroot(root, sysroot, env)
    build(root, env, metadata['revision'][:12], args.jobs)
    destination = staging / ('microhad-' + version + '-lexra-static')
    binaries(root, destination, metadata)
    archive(destination, staging / (destination.name + '.tar.xz'), epoch)
    archives = sorted(staging.glob('*.tar.xz'))
    manifest = dict(metadata, archives={p.name: sha(p) for p in archives})
    (staging / 'release-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    (staging / 'SHA256SUMS').write_text(''.join(sha(p) + '  ' + p.name + '\n' for p in [*archives, staging / 'release-manifest.json']))
    # The unpacked trees remain in .incomplete for diagnostics until success;
    # final public output contains only archives and manifests.
    if not args.rebuild:
        check_clean()
        if git('rev-parse', 'HEAD') != metadata['revision']:
            raise ValueError('Repository revision changed during the release build; refusing publication')
    publish = staging / 'publish'
    publish.mkdir()
    for p in [*archives, staging / 'release-manifest.json', staging / 'SHA256SUMS']:
        p.rename(publish / p.name)
    publish.rename(output)
    print('Release complete: ' + str(output))
    print('Build workspace retained at ' + str(staging))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit('Release failed: ' + str(error))
