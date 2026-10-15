#!/usr/bin/env python3
"""Replay an authenticated plaintext 6LoWPAN first fragment on a synthetic RCP.

Build the relevant OpenThread transport archives first. --device additionally
uploads an isolated MIPS probe to gateway RAM; it never opens the physical UART.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings


def build(mode):
    base = ROOT / 'build/openthread-unix' / mode
    flags = base / 'src/posix/CMakeFiles/gateway-thread-discovery-fixture.dir/flags.make'
    arguments = []
    for line in flags.read_text().splitlines():
        if line.startswith(('CXX_DEFINES =', 'CXX_INCLUDES =', 'CXX_FLAGS =')):
            arguments.extend(shlex.split(line.split(' = ', 1)[1]))
    config = settings()
    output = ROOT / 'build/matter-retry-diagnostics' / ('fragment-replay-' + mode)
    output.parent.mkdir(parents=True, exist_ok=True)
    obj = output.with_suffix('.o')
    compiler = config['CLANGXX'] if mode == 'mips' else os.environ.get('CXX', 'c++')
    # The test exercises the private production FrameToMessage/HandleFragment
    # entry points. This compiler flag applies only to the separate test object.
    subprocess.run([str(compiler), *arguments, '-DOPENTHREAD_FTD=1', '-fno-access-control',
                    '-c', str(ROOT / 'micro-thread-matter-daemon/tests/helpers/lowpan-fragment-replay.cpp'),
                    '-o', str(obj)], check=True)
    archives = sorted(p for p in base.rglob('*.a') if p.name not in
                      ('libopenthread-cli-ftd.a', 'libgateway-thread-discovery-fixture.a'))
    linker = config['TARGET_CXX'] if mode == 'mips' else compiler
    command = [str(linker), str(obj), '-Wl,--gc-sections', '-Wl,--wrap=otPlatSettingsGet',
               '-Wl,--start-group', *map(str, archives), '-Wl,--end-group',
               '-lpthread', '-lrt', '-lutil', '-o', str(output)]
    if mode == 'mips':
        command.append('-static')
    subprocess.run(command, check=True)
    if mode == 'mips':
        subprocess.run([str(config['LLVM_STRIP']), str(output)], check=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fragment', type=Path, help='plaintext first-fragment bytes, including FRAG1 header')
    parser.add_argument('--device', help='explicitly run isolated MIPS replay on this gateway')
    parser.add_argument('--password-file', type=Path)
    args = parser.parse_args()
    if args.device and args.password_file is None:
        parser.error('--device requires --password-file')
    fragment = args.fragment.resolve()
    data = bytes.fromhex(fragment.read_text()) if fragment.suffix == '.hex' else fragment.read_bytes()
    if not 4 <= len(data) <= 127 or data[0] & 0xf8 != 0xc0:
        parser.error('expected an authenticated plaintext 6LoWPAN first fragment (4..127 bytes)')
    binary = build('mips' if args.device else 'host')
    spec = importlib.util.spec_from_file_location('ot_sim', ROOT / 'micro-thread-matter-daemon/scripts/test-openthread-device.py')
    sim = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sim)
    radio = sim.ThreadRcp(args.device or '127.0.0.1', tcp_nodelay=True)
    try:
        if args.device:
            run_device(args, binary, data, radio)
        else:
            with tempfile.TemporaryDirectory(prefix='ot-sim-fragment-replay-') as directory:
                sock = Path(directory) / 'radio.sock'
                input_file = Path(directory) / 'fragment.bin'
                input_file.write_bytes(data)
                relay = subprocess.Popen([str(ROOT / 'build/openthread-unix/sim-relay-host'),
                                          str(sock), radio.address, str(radio.port), '15'])
                try:
                    for _ in range(100):
                        if sock.exists():
                            break
                        if relay.poll() is not None:
                            raise RuntimeError('Synthetic relay exited')
                        time.sleep(.02)
                    subprocess.run([str(binary), 'spinel+hdlc+unix://' + str(sock) + '?no-reset',
                                    str(input_file)], cwd=directory, check=True, timeout=10)
                finally:
                    relay.terminate()
                    relay.wait()
    finally:
        radio.close()


def run_device(args, binary, data, radio):
    ssh = ['sshpass', '-f', str(args.password_file.resolve()), 'ssh', '-T',
           '-o', 'ConnectTimeout=8', '-o', 'StrictHostKeyChecking=yes',
           '-o', 'HostKeyAlgorithms=+ssh-rsa', '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa',
           '-o', 'PreferredAuthentications=password', '-o', 'PubkeyAuthentication=no',
           'root@' + args.device]

    def remote(command, payload=None):
        result = subprocess.run(ssh + [command], input=payload, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        sys.stdout.write(result.stdout.decode(errors='replace'))
        result.check_returncode()
        return result.stdout

    # Probe and synthetic settings stay in a unique private RAM directory.
    free = int(remote("awk '/^MemFree:/ {print $2}' /proc/meminfo").strip())
    if free < 2048 + binary.stat().st_size // 1024:
        raise RuntimeError('Insufficient free RAM for isolated replay')
    ram = '/tmp/ot-sim-fragment-' + uuid.uuid4().hex
    remote('umask 077; mkdir ' + ram)
    try:
        for name, payload in [('replay', binary.read_bytes()),
                              ('relay', (ROOT / 'build/openthread-unix/sim-relay-mips').read_bytes()),
                              ('frame', data)]:
            remote('cat > ' + ram + '/' + name + ' && chmod 700 ' + ram + '/' + name, payload)
        remote(f'''set -e
cd {ram}
ulimit -c 0
ulimit -v 8192
./relay {ram}/radio.sock {radio.address} {radio.port} 20 &
relay=$!
trap 'kill "$relay" 2>/dev/null || true' EXIT HUP INT TERM
n=0
while [ ! -S {ram}/radio.sock ] && [ "$n" -lt 10 ]; do sleep 1; n=$((n+1)); done
./replay spinel+hdlc+unix://{ram}/radio.sock?no-reset {ram}/frame
''')
    finally:
        remote('rm -rf ' + ram)


if __name__ == '__main__':
    main()
