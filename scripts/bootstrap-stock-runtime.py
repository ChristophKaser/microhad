#!/usr/bin/env python3
"""Populate the host-only stock runtime from the configured gateway or a local extraction."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from stock_runtime import inventory, resolve, MANIFEST
from workspace_config import settings, ssh_command, gateway_host


def verify(root):
    for name in inventory():
        resolve({'STOCK_ROOTFS': root}, name)


def bootstrap(destination, fetch):
    destination = Path(destination).absolute()
    if destination.is_symlink():
        raise ValueError('Destination must not be a symlink')
    if destination.exists() and (destination / 'lib').exists():
        # Reuse only a complete matching installation. Do not repair or replace
        # unknown files; a new --output keeps existing copies and links intact.
        verify(destination)
        return False
    if destination.exists() and not destination.is_dir():
        raise ValueError('Destination exists and is not a directory')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.stock-runtime-', dir=destination.parent) as temporary:
        stage = Path(temporary)
        (stage / 'lib').mkdir()
        for name, entry in inventory().items():
            if not re.fullmatch(r'[A-Za-z0-9+_.-]+', name):
                raise ValueError('Invalid library name in manifest')
            target = stage / 'lib' / name
            fetch(name, entry, target)
            if target.is_symlink() or not target.is_file():
                raise ValueError('Expected a regular library file: ' + name)
            if hashlib.sha256(target.read_bytes()).hexdigest() != entry['sha256']:
                raise ValueError('Stock library hash mismatch: ' + name)
        verify(stage)
        if destination.exists():
            # Publish only the complete lib directory; generated backend link
            # directories are independently rebuilt by the selected toolchain.
            os.rename(stage / 'lib', destination / 'lib')
        else:
            os.rename(stage, destination)
    return True


def local_fetch(root):
    root = Path(root).resolve()
    def fetch(name, entry, target):
        for relative in (entry['file'], 'lib/' + name):
            source = (root / relative).resolve()
            if source.is_relative_to(root) and source.is_file():
                shutil.copyfile(source, target)
                return
        raise ValueError('Library missing from local extraction: ' + name)
    return fetch


def remote_fetch(host, password_file=None):
    def fetch(name, entry, target):
        # Fixed, validated SONAMEs only; cat follows the device's library
        # symlinks. No configuration, userdata, or files outside /lib are read.
        with target.open('xb') as output:
            result = subprocess.run(ssh_command(host, password_file) + ['cat /lib/' + name],
                                    stdin=subprocess.DEVNULL, stdout=output,
                                    stderr=subprocess.PIPE, timeout=30)
        if result.returncode:
            raise ValueError('Could not read stock library over SSH: ' + name)
    return fetch


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--host', type=gateway_host, help='gateway override; otherwise use configured GATEWAY_IP')
    source.add_argument('--from-root', type=Path, help='existing extraction containing lib/')
    source.add_argument('--verify-only', action='store_true', help='check the destination without contacting a gateway')
    parser.add_argument('--output', type=Path, help='destination; default is STOCK_ROOTFS from workspace settings')
    parser.add_argument('--password-file', type=Path, help='SSH credential pathname; default uses workspace settings')
    args = parser.parse_args(argv)
    if not args.host and args.from_root is None and not args.verify_only:
        configured_host = settings().get('GATEWAY_IP', '')
        if not configured_host:
            parser.error('Configure GATEWAY_IP or pass --host, --from-root, or --verify-only')
        try:
            args.host = gateway_host(configured_host)
        except ValueError as error:
            parser.error(str(error))
    if args.password_file and not args.host:
        parser.error('--password-file requires a gateway source')
    os.umask(0o077)
    destination = args.output or Path(settings()['STOCK_ROOTFS'])
    try:
        if args.verify_only:
            verify(destination)
            print('Verified stock runtime:', destination)
        else:
            fetch = remote_fetch(args.host, args.password_file) if args.host else local_fetch(args.from_root)
            installed = bootstrap(destination, fetch)
            print('Installed verified stock runtime:' if installed else 'Reused verified stock runtime:', destination)
        print('Libraries:', len(inventory()), '; manifest:', MANIFEST.name)
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, str(error) + '\nUse a new --output directory to preserve an existing mismatched extraction.\n')


if __name__ == '__main__':
    main()
