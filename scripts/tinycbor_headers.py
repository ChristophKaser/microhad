#!/usr/bin/env python3
"""Prepare TinyCBOR's generated headers for our direct, static C99 builds."""
import argparse
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[1] / 'microhad/dependencies/tinycbor'


def prepare(output):
    output = Path(output)
    version = re.search(r'project\(tinycbor LANGUAGES C VERSION (\d+)\.(\d+)\)',
                        (SOURCE / 'CMakeLists.txt').read_text())
    if not version:
        raise ValueError('Cannot determine TinyCBOR CMake project version')
    header = (SOURCE / 'src/tinycbor-version.h.in').read_text()
    for key, value in zip(('MAJOR', 'MINOR'), version.groups()):
        header = header.replace('@PROJECT_VERSION_' + key + '@', value)
    output.mkdir(parents=True, exist_ok=True)
    headers = {
        'tinycbor-version.h': header,
        'tinycbor-export.h': ('/* Static build: no symbol visibility annotations. */\n'
                              '#ifndef MICROHAD_TINYCBOR_EXPORT_H\n'
                              '#define MICROHAD_TINYCBOR_EXPORT_H\n'
                              '#define CBOR_API\n#endif\n'),
    }
    for name, content in headers.items():
        path = output / name
        if not path.exists() or path.read_text() != content:
            path.write_text(content)
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    prepare(parser.parse_args().output)
