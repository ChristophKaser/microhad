"""Resolve shared host and gateway compiler settings without requiring RSDK."""
import os
from pathlib import Path
import shlex
import sys

from workspace_config import settings as workspace_settings

ROOT = Path(__file__).resolve().parents[1]


def settings():
    environment = dict(os.environ, **workspace_settings())
    values = {}
    for line in (ROOT / 'config/toolchains.mk').read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            key, value = line.split('=', 1)
            values[key] = environment.get(key, value)
    backend = environment.get('TOOLCHAIN', 'rsdk')
    if backend not in ('rsdk', 'crosstool-ng'):
        raise ValueError('TOOLCHAIN must be rsdk or crosstool-ng: ' + backend)
    link_mode = environment.get('LINK_MODE', 'static')
    if link_mode not in ('static', 'dynamic'):
        raise ValueError('LINK_MODE must be static or dynamic: ' + link_mode)
    if link_mode == 'dynamic' and backend != 'rsdk':
        raise ValueError('LINK_MODE=dynamic requires TOOLCHAIN=rsdk')
    values['LINK_MODE'] = link_mode
    values['TOOLCHAIN'] = backend
    values['RSDK_ROOT'] = Path(environment.get('RSDK_ROOT', ROOT / values['RSDK_RELATIVE'])).resolve()
    values['CROSSTOOL_NG_ROOT'] = Path(environment.get('CROSSTOOL_NG_ROOT', ROOT / values['CROSSTOOL_NG_RELATIVE'])).resolve()
    compiler_root = values['RSDK_ROOT' if backend == 'rsdk' else 'CROSSTOOL_NG_ROOT']
    prefix = environment.get('TARGET_PREFIX', values['RSDK_PREFIX' if backend == 'rsdk' else 'CROSSTOOL_NG_PREFIX'])
    values.update(TARGET_TOOLCHAIN_ROOT=compiler_root, TARGET_PREFIX=prefix,
                  TARGET_BUILD_SUBDIR=('' if backend == 'rsdk' else 'crosstool-ng/') + ('dynamic/' if link_mode == 'dynamic' else ''),
                  TARGET_SPEC_NAME='lexra-stock-uclibc' if backend == 'rsdk' else 'lexra-crosstool-ng-uclibc')
    if link_mode == 'dynamic':
        values['TARGET_SPEC_NAME'] += '-dynamic'
    for name, executable in [('CC', 'gcc'), ('CXX', 'g++'), ('AR', 'ar'), ('NM', 'nm'),
                             ('STRIP', 'strip'), ('READELF', 'readelf')]:
        key = 'TARGET_' + name
        values[key] = Path(environment.get(key, compiler_root / 'bin' / (prefix + executable)))
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
