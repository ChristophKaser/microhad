#!/usr/bin/env python3
"""Check RCP version/capability rejection on the host without radio resets."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ot_sim', ROOT / 'thread-matter/scripts/test-openthread-device.py')
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def check(label, properties, expected):
    radio = sim.ThreadRcp('127.0.0.1', tcp_nodelay=True)
    for props in radio.interfaces.values():
        props.update(properties)
    relay = None
    try:
        with tempfile.TemporaryDirectory(prefix='ot-sim-compatibility-') as directory:
            path = Path(directory) / 'radio.sock'
            relay = subprocess.Popen([
                str(ROOT / 'build/openthread-unix/sim-relay-host'), str(path),
                radio.address, str(radio.port), '10'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 3
            while not path.exists():
                if relay.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Synthetic relay did not start')
                time.sleep(0.02)
            result = subprocess.run([
                str(ROOT / 'build/openthread-unix/host/src/posix/ot-cli'), '-v',
                f'spinel+hdlc+unix://{path}?no-reset'],
                cwd=directory, input='exit\n', text=True, capture_output=True, timeout=8)
            output = result.stdout + result.stderr
            if result.returncode == 0 or expected not in output:
                raise AssertionError(f'{label}: expected explicit incompatibility rejection\n{output}')
            if radio.counts['command_1']:
                raise AssertionError(f'{label}: unexpected radio reset')
            print(f'PASS {label}: explicit rejection without reset')
    finally:
        if relay is not None:
            relay.terminate()
            try:
                relay.wait(timeout=3)
            except subprocess.TimeoutExpired:
                relay.kill()
                relay.wait()
        radio.close()


def main():
    check('old RCP API', {0xb0: sim.sim.packed(3)}, 'older than min required by host')
    check('newer required host API', {0xb1: sim.sim.packed(12)}, 'host is older')
    check('missing radio capabilities', {0x120b: sim.sim.packed(0)}, 'missing required capabilities')


if __name__ == '__main__':
    main()
