#!/usr/bin/env python3
"""Verify valid and tampered SRP SIG(0) using OpenThread; synthetic radio only."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]


import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, gateway_host, has_credentials, ssh_command


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, default=ROOT / 'micro-thread-matter-daemon/tests/fixtures/srp-ecdsa-validation.json')
    parser.add_argument('--device', nargs='?', const=setting('GATEWAY_IP'), type=gateway_host, help='Explicit isolated test on this gateway; RAM files, no physical radio')
    parser.add_argument('--password-file', type=Path)
    args = parser.parse_args()
    if args.device and not has_credentials(args.password_file):
        parser.error('--device requires configured SSH credentials or --password-file')
    vector = json.loads(args.fixture.read_text())
    key, dns = bytes.fromhex(vector['public_key']), bytes.fromhex(vector['dns'])
    if len(key) != 64 or not 107 <= len(dns) < 2048:
        parser.error('invalid fixture size')
    helper = load('fragment_replay', ROOT / 'micro-thread-matter-daemon/scripts/replay-thread-fragment.py')
    binary = helper.build('mips' if args.device else 'host',
        ROOT / 'micro-thread-matter-daemon/tests/helpers/srp-signature-replay.cpp',
        'srp-signature-replay', ('otPlatCryptoEcdsaVerify',))
    sim = load('ot_sim', ROOT / 'micro-thread-matter-daemon/scripts/test-openthread-device.py')
    radio = sim.ThreadRcp(args.device or '127.0.0.1', tcp_nodelay=True)
    try:
        if args.device:
            output = run_device(args, binary, key, dns, radio)
        else:
            with tempfile.TemporaryDirectory(prefix='ot-sim-srp-signature-') as directory:
                private = Path(directory)
                sock = private / 'radio.sock'
                (private / 'key').write_bytes(key)
                (private / 'dns').write_bytes(dns)
                relay = subprocess.Popen([str(ROOT / 'build/openthread-unix/sim-relay-host'),
                    str(sock), radio.address, str(radio.port), '15'])
                try:
                    for _ in range(100):
                        if sock.exists(): break
                        if relay.poll() is not None: raise RuntimeError('Synthetic relay exited')
                        time.sleep(.02)
                    result = subprocess.run([str(binary), 'spinel+hdlc+unix://' + str(sock) + '?no-reset',
                        str(private / 'key'), str(private / 'dns')], cwd=private,
                        capture_output=True, text=True, timeout=10)
                    output = result.stdout + result.stderr
                    if result.returncode: raise RuntimeError(output)
                finally:
                    relay.terminate()
                    relay.wait()
    finally:
        radio.close()
    print(output, end='')
    assert output.count('verification_sha256=' + vector['sha256']) == 2, 'signed bytes/hash changed'
    assert 'SRP valid=0 tampered=8' in output
    print('PASS valid SRP signature accepted; tampered signature rejected; signed hash matches')


def run_device(args, binary, key, dns, radio):
    ssh = ssh_command(args.device, args.password_file, options=['-T'])

    def remote(command, payload=None):
        result = subprocess.run(ssh + [command], input=payload, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=60)
        if result.returncode: raise RuntimeError(result.stdout.decode(errors='replace'))
        return result.stdout.decode(errors='replace')

    free = int(remote("awk '/^MemFree:/ {print $2}' /proc/meminfo").strip())
    if free < 2048 + binary.stat().st_size // 1024:
        raise RuntimeError('Insufficient free RAM for isolated crypto probe')
    ram = '/tmp/ot-sim-srp-signature-' + uuid.uuid4().hex
    remote('umask 077; mkdir ' + ram)
    try:
        for name, payload in [('probe', binary.read_bytes()),
            ('relay', (ROOT / 'build/openthread-unix/sim-relay-mips').read_bytes()), ('key', key), ('dns', dns)]:
            remote('cat > ' + ram + '/' + name + ' && chmod 700 ' + ram + '/' + name, payload)
        return remote(f'''set -e
cd {ram}
ulimit -c 0
ulimit -v 8192
./relay {ram}/radio.sock {radio.address} {radio.port} 20 &
relay=$!
trap 'kill "$relay" 2>/dev/null || true' EXIT HUP INT TERM
n=0
while [ ! -S {ram}/radio.sock ] && [ "$n" -lt 10 ]; do sleep 1; n=$((n+1)); done
./probe spinel+hdlc+unix://{ram}/radio.sock?no-reset {ram}/key {ram}/dns
''')
    finally:
        remote('rm -rf ' + ram)


if __name__ == '__main__':
    main()
