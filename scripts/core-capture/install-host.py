#!/usr/bin/env python3
"""Install and start a persistent user service for gateway core reception."""
import argparse
import os
import pwd
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from workspace_config import setting


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', required=True)
    parser.add_argument('--gateway', default=setting('GATEWAY_IP'))
    parser.add_argument('--port', type=int, default=9999)
    args = parser.parse_args()
    # systemd specifiers and quoting must not reinterpret repository paths.
    def quoted(value):
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%') + '"'
    output = ROOT / 'diagnostics/core-dumps'
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    unit = ROOT / 'build/gateway-core-receiver.service'
    unit.parent.mkdir(exist_ok=True)
    command = [sys.executable, ROOT / 'scripts/core-capture/receiver.py', '--bind', args.bind,
               '--port', str(args.port), '--gateway', args.gateway, '--output', output]
    unit.write_text('[Unit]\nDescription=Gateway TCP core dump receiver\nAfter=network-online.target\n\n'
                    '[Service]\nType=simple\nExecStart=' + ' '.join(map(quoted, command)) + '\n'
                    'Restart=always\nRestartSec=5\nUMask=0077\nNoNewPrivileges=true\n\n'
                    '[Install]\nWantedBy=default.target\n')
    subprocess.run(['systemctl', '--user', 'enable', str(unit)], check=True)
    subprocess.run(['systemctl', '--user', 'daemon-reload'], check=True)
    subprocess.run(['systemctl', '--user', 'restart', unit.name], check=True)
    subprocess.run(['systemctl', '--user', 'is-active', unit.name], check=True)
    subprocess.run(['loginctl', 'enable-linger', pwd.getpwuid(os.getuid()).pw_name], check=True)
    print(output)


if __name__ == '__main__':
    main()
