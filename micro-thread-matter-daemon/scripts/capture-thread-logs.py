#!/usr/bin/env python3
"""Continuously archive gateway syslog on the host; never write gateway flash."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='192.168.178.62')
    parser.add_argument('--password-file', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = ['sshpass', '-f', args.password_file, 'ssh', '-T',
               '-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=10',
               '-o', 'ServerAliveCountMax=3', '-o', 'StrictHostKeyChecking=yes',
               '-o', 'HostKeyAlgorithms=+ssh-rsa', '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa',
               '-o', 'PreferredAuthentications=password', '-o', 'PubkeyAuthentication=no',
               f'root@{args.host}', 'exec /tuya/native/logread -f']
    # Raw captures can contain protocol payloads; create the host archive private.
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with os.fdopen(descriptor, 'ab', buffering=0) as output:
        while True:
            stamp = datetime.now(timezone.utc).isoformat()
            output.write(f'capture connection start {stamp}\n'.encode())
            child = subprocess.Popen(command, stdout=output, stderr=output)
            try:
                result = child.wait()
            except KeyboardInterrupt:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
                return
            stamp = datetime.now(timezone.utc).isoformat()
            output.write(f'capture disconnected {stamp} status={result}; reconnecting in 2s\n'.encode())
            time.sleep(2)


if __name__ == '__main__':
    main()
