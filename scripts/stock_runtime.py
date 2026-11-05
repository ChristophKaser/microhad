"""Resolve only libraries supplied by the recorded stock gateway image."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'config/stock-runtime.json'


def inventory():
    return json.loads(MANIFEST.read_text())['libraries']


def resolve(values, name):
    entries = inventory()
    if name not in entries:
        raise ValueError('shared library not provided by stock image: ' + name)
    root = values['STOCK_ROOTFS'].resolve()
    path = (root / 'lib' / name).resolve()
    if not path.is_file() or not path.is_relative_to(root):
        raise ValueError('stock runtime library missing; run scripts/bootstrap-stock-runtime.py with --host HOST or --from-root DIR: ' + name)
    if hashlib.sha256(path.read_bytes()).hexdigest() != entries[name]['sha256']:
        raise ValueError('runtime library does not match recorded stock image: ' + name)
    return path


def prepare_link_directory(values):
    """Link against stock ELFs, bypassing the image's incomplete libc.so script."""
    import subprocess
    import tempfile
    import os
    root = values['STOCK_ROOTFS']
    directory = root / ('link-crosstool-ng' if values.get('TOOLCHAIN') == 'crosstool-ng' else 'link')
    directory.mkdir(exist_ok=True)
    links = {}
    for name in inventory():
        if name.startswith('lib') and '.so.' in name:
            links[name.split('.so.')[0] + '.so'] = resolve(values, name)
    # The stock image has no libutil.so; satisfy Rust/OpenThread's -lutil from
    # the SDK archive rather than allowing the compiler to choose its DSO.
    archive = Path(subprocess.check_output(
        [values['TARGET_CC'], '-print-file-name=libutil.a'], text=True).strip()).resolve()
    if not archive.is_file():
        raise ValueError('selected toolchain is missing libutil.a')
    if values.get('TOOLCHAIN') == 'crosstool-ng':
        # uClibc-ng merged libutil into libc.a; extract only these public APIs.
        # Never link the new libc into an executable using the stock loader.
        libc = Path(subprocess.check_output([values['TARGET_CC'], '-print-file-name=libc.a'], text=True).strip()).resolve()
        generated = directory / 'libutil.a'
        with tempfile.TemporaryDirectory(prefix='.libutil-', dir=directory) as temporary:
            staging = Path(temporary)
            objects = []
            for member in ('forkpty.os', 'openpty.os', 'login_tty.os'):
                obj = staging / member
                obj.write_bytes(subprocess.check_output([values['TARGET_AR'], 'p', libc, member]))
                objects.append(obj)
            subprocess.run([values['TARGET_AR'], 'rcs', staging / 'libutil.a', *objects], check=True)
            os.replace(staging / 'libutil.a', generated)
        cxx = Path(subprocess.check_output([values['TARGET_CXX'], '-print-file-name=libstdc++.a'], text=True).strip()).resolve()
        links['libstdc++-compat.a'] = cxx
    else:
        links['libutil.a'] = archive
    for name, target in links.items():
        path = directory / name
        if path.is_symlink() and path.resolve() == target:
            continue
        if path.is_symlink():
            path.unlink()
        elif path.exists():
            raise ValueError('expected generated runtime link symlink: ' + str(path))
        try:
            path.symlink_to(target)
        except FileExistsError:
            if not path.is_symlink() or path.resolve() != target:
                raise
    return directory
