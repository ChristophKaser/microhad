#!/usr/bin/env python3
"""Exercise the real OpenThread/Rust transport against an explicitly synthetic RCP."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ot_sim', ROOT / 'scripts/test-openthread-device.py')
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def main():
    radio = sim.ThreadRcp('127.0.0.1', tcp_nodelay=True)
    relay = None
    try:
        with tempfile.TemporaryDirectory(prefix='ot-sim-matter-') as directory:
            socket = Path(directory) / 'radio.sock'
            relay = subprocess.Popen([str(ROOT / 'build/openthread-unix/sim-relay-host'),
                                      str(socket), radio.address, str(radio.port), '25'])
            deadline = time.monotonic() + 5
            while not socket.exists():
                if relay.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Synthetic relay startup failed')
                time.sleep(0.02)
            binary = ROOT / 'build/rs-matter/target/release/ot-matter-transport-probe'
            result = subprocess.run([str(binary), 'spinel+hdlc+unix://' + str(socket) + '?no-reset'],
                                    cwd=directory, capture_output=True, text=True, timeout=22)
            if result.returncode or 'PASS transport complete' not in result.stdout:
                raise RuntimeError('Transport failed: ' + result.stdout + result.stderr + '\nRCP error: ' + str(radio.error))
            if radio.error:
                raise RuntimeError('Synthetic RCP failed: ' + radio.error)
            report = {'platform': 'development host', 'radio': 'synthetic RCP',
                      'linux_ipv6_or_tun': False, 'real_radio_access': False,
                      'datagram_payload_lengths': [0, 4, 256, 1232],
                      'multicast_membership_api_tested': True,
                      'uart_and_reset_guards_tested': True, 'output': result.stdout.splitlines()}
            path = ROOT / 'build/rs-matter/openthread-transport-validation.json'
            path.write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(report, indent=2))
    finally:
        if relay is not None:
            relay.terminate()
            try:
                relay.wait(timeout=3)
            except subprocess.TimeoutExpired:
                relay.kill()
                relay.wait()
        radio.close()


if __name__ == '__main__':
    main()
