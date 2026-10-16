#!/usr/bin/env python3
"""Verify the custom image can initialize a running synthetic RCP without reset."""
import argparse
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', default='microhad-otbr-debug:local')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        'ot_sim', ROOT / 'micro-thread-matter-daemon/scripts/test-openthread-device.py')
    simulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(simulator)
    radio = simulator.ThreadRcp('127.0.0.1', tcp_nodelay=True)
    try:
        url = ('spinel+hdlc+forkpty:///usr/bin/socat?forkpty-arg=STDIO&'
               f'forkpty-arg=TCP:{radio.address}:{radio.port},nodelay&no-reset&attach-to-running-rcp')
        result = subprocess.run(['docker', 'run', '--rm', '--network', 'host', '--tmpfs', '/var/lib/thread',
                                 '--entrypoint', '/opt/otbr-agent', args.image, '-v', '-s', '-d7',
                                 '--vendor-name', 'Diagnostic', '--model-name', 'Ziggurat-TCP',
                                 '--radio-version', url], capture_output=True, text=True, timeout=15)
        if result.returncode != 0:
            raise RuntimeError(result.stdout + result.stderr)
        if radio.counts['command_1']:
            raise RuntimeError('Unexpected reset command')
        print('PASS custom OTBR image: running RCP initialization, no boot event, no reset')
    finally:
        radio.close()


if __name__ == '__main__':
    main()
