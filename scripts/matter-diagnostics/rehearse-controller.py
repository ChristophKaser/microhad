#!/usr/bin/env python3
"""Rehearse real commissioning against a disposable, network-isolated Linux accessory."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent.parent


def run_inside(args):
    # This capability exists only in the network-none fixture container.
    for command in (['ip', 'link', 'add', 'fixture0', 'type', 'dummy'],
                    ['ip', 'link', 'set', 'fixture0', 'up'],
                    ['ip', '-6', 'addr', 'add', 'fd00:1234::1/64', 'dev', 'fixture0', 'nodad']):
        subprocess.run(command, check=True)
    os.setgid(args.gid)
    os.setuid(args.uid)
    os.umask(0o077)
    directory = Path('/session')
    for name in ('keys', 'capture', 'metadata', 'state', 'logs'):
        (directory / name).mkdir(exist_ok=True, mode=0o700)
    environment = {**os.environ, 'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent',
                   'MATTER_DIAGNOSTIC_DIR': str(directory),
                   'MATTER_DIAGNOSTIC_BUILD': hashlib.sha256(Path('/build/chip-tool').read_bytes()).hexdigest()}
    # Test accessory exports are separate, so only commissioner keys satisfy validation.
    accessory_env = {key: value for key, value in environment.items() if key != 'MATTER_DIAGNOSTIC_DIR'}
    with (directory / 'logs/accessory.log').open('w') as output:
        accessory = subprocess.Popen(['/build/chip-all-clusters-app', '--KVS', '/session/state/accessory.kvs',
            '--interface-id', 'fixture0', '--discriminator', '3840', '--passcode', '20202021'],
            env=accessory_env, stdout=output, stderr=subprocess.STDOUT)
    def chip(name, arguments):
        with (directory / f'logs/{name}.log').open('w') as output:
            subprocess.run(['/build/chip-tool', *arguments, '--storage-directory', '/session/state/controller',
                            '--trace_decode', '1', '--trace_file', f'/session/logs/{name}-trace.log',
                            '--trace-to', f'json:/session/logs/{name}-events.json'],
                           env=environment, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=90)
    try:
        time.sleep(3)
        if accessory.poll() is not None:
            raise RuntimeError('Accessory fixture exited; see logs/accessory.log')
        (directory / 'state/controller').mkdir(exist_ok=True)
        chip('commission', ['pairing', 'already-discovered', '1', '20202021', 'fd00:1234::1', '5540'])
        chip('case-read', ['basicinformation', 'read', 'vendor-name', '1', '0'])
        # The CLI's standalone shutdown/reinitialization may invalidate cached
        # resumption state. Explicitly evict a live CASE session in one process.
        commands = ('basicinformation read vendor-name 1 0 --storage-directory /session/state/controller\n'
                    'sessionmanagement expire-case-sessions 1 --storage-directory /session/state/controller\n'
                    'basicinformation read vendor-name 1 0 --storage-directory /session/state/controller\nquit\n')
        with (directory / 'logs/resumption-read.log').open('w') as output:
            subprocess.run(['/build/chip-tool', 'interactive', 'start', '--storage-directory',
                            '/session/state/controller', '--trace_decode', '1', '--trace_file',
                            '/session/logs/resumption-read-trace.log'], input=commands, text=True,
                           env=environment, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=90)
        spec = importlib.util.spec_from_file_location('verify_export', '/scripts/verify-export.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        verification = module.verify(directory)
        if not verification['complete']:
            raise RuntimeError('Fixture decryption verification incomplete')
        if not all(verification['authenticated_messages'].get('CASE-resumed-' + direction)
                   for direction in ('tx', 'rx')):
            raise RuntimeError('CASE resumption was not exercised')
        (directory / 'metadata/rehearsal-result.json').write_text(json.dumps(verification, indent=2) + '\n')
    finally:
        accessory.terminate()
        try:
            accessory.wait(timeout=10)
        except subprocess.TimeoutExpired:
            accessory.kill()
            accessory.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inside', action='store_true')
    parser.add_argument('--uid', type=int, default=os.getuid())
    parser.add_argument('--gid', type=int, default=os.getgid())
    parser.add_argument('--image', default='microhad-matter-fixture:diagnostic')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/matter-diagnostics')
    args = parser.parse_args()
    if args.inside:
        run_inside(args)
        return
    directory = args.build_dir.resolve()
    for name in ('chip-tool', 'chip-all-clusters-app', 'controller-build.json'):
        if not (directory / name).exists():
            raise SystemExit('Build with WITH_FIXTURE=1 bash scripts/matter-diagnostics/build-controller.sh first')
    stamp = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
    session = directory / ('rehearsal-' + stamp)
    session.mkdir(mode=0o700)
    command = ['docker', 'run', '--rm', '--network', 'none', '--cap-add', 'NET_ADMIN',
               '--read-only', '--tmpfs', '/tmp:rw,nosuid,nodev',
               '--sysctl', 'net.ipv6.conf.all.disable_ipv6=0',
               '--mount', f'type=bind,src={Path(__file__).parent},dst=/scripts,readonly',
               '--mount', f'type=bind,src={directory},dst=/build,readonly',
               '--mount', f'type=bind,src={session},dst=/session',
               args.image, '--uid', str(args.uid), '--gid', str(args.gid)]
    print('Fixture bundle:', session, flush=True)
    subprocess.run(command, check=True)
    verification = json.loads((session / 'metadata/rehearsal-result.json').read_text())
    result = {'passed': True, 'binary_sha256': hashlib.sha256((directory / 'chip-tool').read_bytes()).hexdigest(),
              'verification': verification, 'bundle': str(session)}
    (directory / 'rehearsal.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: real PASE, CASE and resumed CASE traffic authenticated in both directions')


if __name__ == '__main__':
    main()
