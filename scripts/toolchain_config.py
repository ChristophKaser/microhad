"""Resolve shared host and gateway compiler settings without requiring RSDK."""
import os
from pathlib import Path
import shlex
import sys

ROOT = Path(__file__).resolve().parents[1]


def settings():
    values = {}
    for line in (ROOT / 'config/toolchains.mk').read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            key, value = line.split('=', 1)
            values[key] = os.environ.get(key, value)
    backend = os.environ.get('TOOLCHAIN', 'rsdk')
    if backend not in ('rsdk', 'crosstool-ng'):
        raise ValueError('TOOLCHAIN must be rsdk or crosstool-ng: ' + backend)
    values['TOOLCHAIN'] = backend
    values['RSDK_ROOT'] = Path(os.environ.get('RSDK_ROOT', ROOT / values['RSDK_RELATIVE'])).resolve()
    values['CROSSTOOL_NG_ROOT'] = Path(os.environ.get('CROSSTOOL_NG_ROOT', ROOT / values['CROSSTOOL_NG_RELATIVE'])).resolve()
    compiler_root = values['RSDK_ROOT' if backend == 'rsdk' else 'CROSSTOOL_NG_ROOT']
    prefix = os.environ.get('TARGET_PREFIX', values['RSDK_PREFIX' if backend == 'rsdk' else 'CROSSTOOL_NG_PREFIX'])
    values.update(TARGET_TOOLCHAIN_ROOT=compiler_root, TARGET_PREFIX=prefix,
                  TARGET_BUILD_SUBDIR='' if backend == 'rsdk' else 'crosstool-ng/',
                  TARGET_SPEC_NAME='lexra-stock-uclibc' if backend == 'rsdk' else 'lexra-crosstool-ng-uclibc')
    for name, executable in [('CC', 'gcc'), ('CXX', 'g++'), ('AR', 'ar'), ('NM', 'nm'),
                             ('STRIP', 'strip'), ('READELF', 'readelf')]:
        key = 'TARGET_' + name
        values[key] = Path(os.environ.get(key, compiler_root / 'bin' / (prefix + executable)))
    return values


def target_path(path):
    """Insert the backend directory immediately below a component's build directory."""
    path = Path(path)
    parts = list(path.parts)
    index = parts.index('build') + 1
    return Path(*parts[:index], settings()['TARGET_BUILD_SUBDIR'], *parts[index:])


if __name__ == '__main__':
    try:
        values = settings()
        if len(sys.argv) == 2 and sys.argv[1] == '--shell':
            for key, value in values.items():
                print('export ' + key + '=' + shlex.quote(str(value)))
        elif len(sys.argv) == 2:
            print(values[sys.argv[1]])
        else:
            raise ValueError('usage: toolchain_config.py SETTING|--shell')
    except ValueError as error:
        raise SystemExit(str(error))
