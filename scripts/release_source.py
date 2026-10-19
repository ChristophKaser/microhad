"""Verified source-package support shared by the normal build helpers."""
import hashlib
import json
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def enabled():
    return os.environ.get('MICROHAD_SOURCE_BUNDLE') == '1'


def metadata():
    return json.loads((ROOT / '.release/metadata.json').read_text())


def verify(prefix=None):
    """Verify exported files before using an archive in place of Git worktrees."""
    manifest = json.loads((ROOT / '.release/source-manifest.json').read_text())
    matched = 0
    for name, record in manifest['files'].items():
        if prefix and not (name == prefix or name.startswith(prefix.rstrip('/') + '/')):
            continue
        path = ROOT / name
        if Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('Unsafe source manifest path: ' + name)
        if not path.resolve().is_relative_to(ROOT.resolve()):
            raise ValueError('External source path: ' + name)
        if record.get('symlink') is not None:
            if not path.is_symlink() or os.readlink(path) != record['symlink'] or not path.resolve().is_relative_to(ROOT):
                raise ValueError('Source symlink mismatch: ' + name)
        elif not path.is_file() or path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
            raise ValueError('Source checksum mismatch: ' + name)
        elif bool(path.stat().st_mode & 0o111) != record['executable']:
            raise ValueError('Source executable mode mismatch: ' + name)
        matched += 1
    if not matched:
        raise ValueError('Source package has no verified files for: ' + str(prefix))
    return manifest


if __name__ == '__main__':
    import sys
    try:
        verify(sys.argv[1] if len(sys.argv) > 1 else None)
        print('Source bundle verified')
    except (ValueError, FileNotFoundError) as error:
        raise SystemExit(str(error))
