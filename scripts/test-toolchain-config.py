#!/usr/bin/env python3
"""Check shared selector agreement and cache isolation without compiling applications."""
import os
import importlib.util
from unittest.mock import patch
from pathlib import Path
import subprocess
import tempfile
from toolchain_config import ROOT


def run(args, env):
    return subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, check=True).stdout


def main():
    base = {k: v for k, v in os.environ.items() if not k.startswith('TARGET_') and k not in ('TOOLCHAIN', 'RSDK_ROOT', 'CROSSTOOL_NG_ROOT', 'LINK_MODE')}
    base['WORKSPACE_CONFIG'] = str(ROOT / 'workspace.local.env.example')
    with tempfile.TemporaryDirectory() as temporary:
        for backend, prefix in [('rsdk', 'mips-linux-'), ('crosstool-ng', 'mips-lexra-linux-uclibc-')]:
            env = dict(base, TOOLCHAIN=backend, RSDK_ROOT=temporary + '/missing-rsdk', CROSSTOOL_NG_ROOT=temporary + '/candidate')
            for key in ('TARGET_CC', 'TARGET_CXX', 'TARGET_AR', 'TARGET_NM', 'TARGET_STRIP', 'TARGET_READELF', 'TARGET_PREFIX', 'TARGET_BUILD_SUBDIR'):
                python = run(['python3', 'scripts/toolchain_config.py', key], env).strip()
                shell = run(['bash', '-c', 'source scripts/toolchains.sh; printf "%s" "$' + key + '"'], env).strip()
                recipe = 'include microhad/Makefile\n.PHONY: print-setting\nprint-setting:\n\t@echo $(' + key + ')\n'
                make = subprocess.run(['make', '--no-print-directory', '-f', '-', 'print-setting'], input=recipe,
                                      env=env, cwd=ROOT, text=True, capture_output=True, check=True).stdout.strip()
                assert python == shell == make, (key, python, shell, make)
            assert run(['python3', 'scripts/toolchain_config.py', 'TARGET_PREFIX'], env).strip() == prefix
            env['TARGET_CC'] = temporary + '/custom-gcc'
            assert run(['python3', 'scripts/toolchain_config.py', 'TARGET_CC'], env).strip() == env['TARGET_CC']
            assert run(['bash', '-c', 'source scripts/toolchains.sh; echo "$TARGET_CC"'], env).strip() == env['TARGET_CC']
        env = dict(base, TOOLCHAIN='rsdk', LINK_MODE='dynamic')
        for key in ('LINK_MODE', 'TARGET_BUILD_SUBDIR'):
            python = run(['python3', 'scripts/toolchain_config.py', key], env).strip()
            shell = run(['bash', '-c', 'source scripts/toolchains.sh; printf "%s" "$' + key + '"'], env).strip()
            recipe = 'include microhad/Makefile\n.PHONY: print-setting\nprint-setting:\n\t@echo $(' + key + ')\n'
            make = subprocess.run(['make', '--no-print-directory', '-f', '-', 'print-setting'], input=recipe,
                                  env=env, cwd=ROOT, text=True, capture_output=True, check=True).stdout.strip()
            assert python == shell == make, (key, python, shell, make)
        assert run(['python3', 'scripts/toolchain_config.py', 'TARGET_BUILD_SUBDIR'], env).strip() == 'dynamic/'
        assert run(['python3', 'scripts/toolchain_config.py', 'TARGET_SPEC_NAME'], env).strip() == 'lexra-stock-uclibc-dynamic'
        for backend, mode in [('rsdk', ''), ('rsdk', 'unknown'), ('rsdk', 'static dynamic'), ('crosstool-ng', 'dynamic')]:
            env = dict(base, TOOLCHAIN=backend, LINK_MODE=mode)
            for args in (['python3', 'scripts/toolchain_config.py', 'TARGET_CC'],
                         ['bash', '-c', 'source scripts/toolchains.sh'],
                         ['make', '-C', 'microhad', '-n', 'mips']):
                result = subprocess.run(args, env=env, cwd=ROOT, capture_output=True, text=True)
                assert result.returncode != 0 and 'LINK_MODE' in result.stderr, (args, result.stderr)
        for invalid in ('', 'unknown', 'rsdk crosstool-ng', '%'):
            env = dict(base, TOOLCHAIN=invalid)
            for args in (['python3', 'scripts/toolchain_config.py', 'TARGET_CC'],
                         ['bash', '-c', 'source scripts/toolchains.sh'],
                         ['make', '-C', 'microhad', '-n', 'mips']):
                result = subprocess.run(args, env=env, cwd=ROOT, capture_output=True, text=True)
                assert result.returncode != 0 and 'TOOLCHAIN' in result.stderr, (invalid, args, result.stderr)
        # Command-line Make settings override environment settings.
        result = run(['make', '-C', 'microhad', '-n', 'TOOLCHAIN=crosstool-ng', 'TARGET_CC=/custom/gcc', 'mips'], dict(base, TOOLCHAIN='rsdk'))
        assert '/custom/gcc' in result and 'build/crosstool-ng/mips-ziggurat-matter' in result
    # A target cache must survive identical settings, and lose its objects when
    # an installation changes. Host artifacts outside that directory survive.
    spec = importlib.util.spec_from_file_location('cache_guard', ROOT / 'scripts/toolchain-build.py')
    guard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guard)
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
        base_path = Path(temporary)
        tool = base_path / 'compiler'
        tool.write_text('compiler identity')
        archive = base_path / 'runtime.a'
        archive.write_text('library identity')
        values = dict(TOOLCHAIN='rsdk', LINK_MODE='static', TARGET_TOOLCHAIN_ROOT=base_path, TARGET_PREFIX='fake-')
        for name in ('CC', 'CXX', 'AR', 'NM', 'STRIP', 'READELF'):
            values['TARGET_' + name] = tool
        def output(args, **kwargs):
            if args[1] == '--version':
                return 'fake compiler version\n'
            if args[1] == '-print-sysroot':
                return str(base_path) + '\n'
            return str(archive) + '\n'
        directory = base_path / 'target'
        host = base_path / 'host-fixture'
        host.write_text('preserve')
        with patch.object(guard, 'settings', return_value=values), patch.object(guard.subprocess, 'check_output', side_effect=output):
            guard.ensure(directory)
            sentinel = directory / 'old-object.o'
            sentinel.write_text('cached object')
            stamp = (directory / '.toolchain').stat().st_mtime_ns
            guard.ensure(directory)
            assert sentinel.exists() and (directory / '.toolchain').stat().st_mtime_ns == stamp
            archive.write_text('changed library identity')
            guard.ensure(directory)
            assert not sentinel.exists() and host.read_text() == 'preserve'
            try:
                guard.ensure(ROOT / 'build')
            except ValueError:
                pass
            else:
                raise AssertionError('guard accepted the entire shared build directory')
    print('PASS shared Make/shell/Python selection, overrides, missing RSDK, invalid values, isolated outputs')


if __name__ == '__main__':
    main()
