"""Read the shared toolchain settings for Python build and verification helpers."""

import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def settings():
    values = {}
    for line in (ROOT / 'config/toolchains.mk').read_text().splitlines():
        line = line.strip()
        if line and not line.startswith('#'):
            key, value = line.split('=', 1)
            values[key] = value
    rsdk_root = Path(os.environ.get('RSDK_ROOT', ROOT / values['RSDK_RELATIVE'])).resolve()
    prefix = values['TARGET_PREFIX']
    values.update({
        'RSDK_ROOT': rsdk_root,
        'TARGET_CC': Path(os.environ.get('TARGET_CC', rsdk_root / 'bin' / (prefix + 'gcc'))),
        'TARGET_CXX': Path(os.environ.get('TARGET_CXX', rsdk_root / 'bin' / (prefix + 'g++'))),
        'TARGET_AR': Path(os.environ.get('TARGET_AR', rsdk_root / 'bin' / (prefix + 'ar'))),
        'TARGET_STRIP': Path(os.environ.get('TARGET_STRIP', rsdk_root / 'bin' / (prefix + 'strip'))),
        'TARGET_READELF': Path(os.environ.get('TARGET_READELF', rsdk_root / 'bin' / (prefix + 'readelf'))),
    })
    return values


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: toolchain_config.py SETTING')
    value = settings()[sys.argv[1]]
    print(value)
