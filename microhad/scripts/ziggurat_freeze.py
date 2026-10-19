#!/usr/bin/env python3
"""Freeze a quiesced Ziggurat stack and save complete private rollback evidence.

Default is offline template validation. --execute requires --quiesced and a
local socket or explicitly supplied SSH bridge; never opens a UART or flashes.
"""
import argparse
import json
from pathlib import Path
import signal
import subprocess

from network_backup import private_write, ssh_args
from ziggurat_cobs import Client
from ziggurat_migration import freeze_and_export, validate


import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, gateway_host, has_credentials


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--template', required=True)
    p.add_argument('--output')
    p.add_argument('--socket', default='/tmp/ziggurat/native.sock')
    p.add_argument('--host', nargs='?', const=setting('GATEWAY_IP'), type=gateway_host)
    p.add_argument('--password-file')
    p.add_argument('--bridge', default='/tuya/ziggurat-stage/gateway-maintenance')
    p.add_argument('--quiesced', action='store_true')
    p.add_argument('--execute', action='store_true')
    args = p.parse_args()
    template = validate(json.loads(Path(args.template).read_text()))
    if not args.execute:
        print('Template validated offline; stack not contacted')
        return
    if not args.quiesced or not args.output:
        p.error('--execute requires --quiesced and a new --output; disable supervisor respawn first')
    destination = Path(args.output)
    if destination.exists() or destination.parent.stat().st_mode & 0o077:
        p.error('output must be new inside a private directory')
    if args.host and not has_credentials(args.password_file):
        p.error('remote access requires configured SSH credentials or --password-file')
    if args.socket != '/tmp/ziggurat/native.sock' and args.host:
        p.error('remote recovery socket must be /tmp/ziggurat/native.sock')
    import shlex
    def expired(*_):
        raise TimeoutError('freeze/export overall deadline')
    signal.signal(signal.SIGALRM, expired)
    signal.alarm(120)
    bridge = client = None
    try:
        if args.host:
            command = shlex.join([args.bridge, 'cobs-bridge', args.socket])
            bridge = subprocess.Popen(ssh_args(args) + [command], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            client = Client(bridge.stdout, bridge.stdin)
        else:
            client = Client.connect(args.socket)
        state = freeze_and_export(client, template)
        private_write(destination, (json.dumps(state, indent=2) + '\n').encode())
        print('Frozen complete snapshot saved privately; no network restart requested')
    finally:
        signal.alarm(0)
        if client:
            client.close()
        if bridge:
            bridge.terminate()
            try:
                bridge.wait(timeout=5)
            except subprocess.TimeoutExpired:
                bridge.kill(); bridge.wait()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TimeoutError, AssertionError, ConnectionError):
        raise SystemExit('Freeze/export failed; state not certified for rollback')
