#!/usr/bin/env python3
"""Prove paired supervision recovers a real Matter service after Spinel failure."""
import importlib.util
import json
import os
from pathlib import Path
import pty
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
RUST_ROOT = ROOT.parent / 'ziggurat-compilation'
SERVICE = RUST_ROOT / 'build/rs-matter/target/release/thread-matter'
RELAY = RUST_ROOT / 'build/openthread-unix/sim-relay-host'
PAIR = ROOT / 'paired_start.sh'
MAINTENANCE = RUST_ROOT / 'build/gateway-maintenance-host'
SPEC = importlib.util.spec_from_file_location(
    'matter_service_test', RUST_ROOT / 'scripts/test-matter-service.py')
service_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(service_test)

ZIG = r'''#!/usr/bin/env python3
import os,socket,sys,time
from pathlib import Path
with open(os.environ['TEST_EVENTS'],'a') as f:f.write('zig start\n')
path=sys.argv[sys.argv.index('--listen')+1][5:]
Path(path).unlink(missing_ok=True)
s=socket.socket(socket.AF_UNIX);s.bind(path);os.chmod(path,0o600);s.listen();s.settimeout(.1)
while True:
 try:
  c,_=s.accept();c.sendall(bytes.fromhex('01020302020100'));c.close()
 except socket.timeout:pass
'''
NATIVE = r'''#!/usr/bin/env python3
import os,signal,time
with open(os.environ['TEST_EVENTS'],'a') as f:f.write('native start\n')
def stop(*args):
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write('native stop\n')
 raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
while True:time.sleep(.1)
'''
MATTER = r'''#!/usr/bin/env python3
import os,sys
from pathlib import Path
with open(os.environ['MATTER_LAUNCHES'],'a') as f:f.write('launch\n')
os.execv(os.environ['REAL_MATTER_BIN'],[os.environ['REAL_MATTER_BIN']]+sys.argv[1:])
'''


def wait_for(check, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.03)
    raise TimeoutError('paired Matter recovery deadline')


def ipc(path, opcode, ident, body=b'', timeout=0.8):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(timeout)
        peer.connect(str(path))
        peer.sendall(struct.pack('<HBBBBI', len(body), 1, 0, opcode, 0, ident) + body)
        header = bytearray()
        while len(header) < 10:
            chunk = peer.recv(10-len(header))
            if not chunk:
                raise ConnectionError('Matter service exited before its IPC response')
            header.extend(chunk)
        length, version, kind, operation, reserved, response_id = struct.unpack('<HBBBBI', header)
        assert (version, kind, operation, reserved, response_id) == (1, 1, opcode, 0, ident)
        payload = bytearray()
        while len(payload) < length:
            chunk = peer.recv(length-len(payload))
            if not chunk:
                raise ConnectionError('Matter service closed a partial IPC response')
            payload.extend(chunk)
        return json.loads(payload)


def start_relay(radio):
    process = subprocess.Popen([str(RELAY), str(radio.path), radio.rcp.address,
                                str(radio.rcp.port), '45'],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    wait_for(lambda: radio.path.exists() or process.poll() is not None, timeout=5)
    if not radio.path.exists():
        detail = process.stdout.read() if process.poll() is not None else 'still running without socket'
        raise RuntimeError('synthetic RCP relay failed to bind: ' + detail)
    return process


def main():
    for binary in (SERVICE, RELAY, MAINTENANCE):
        if not binary.is_file():
            raise RuntimeError('missing required host binary: ' + str(binary))
    with tempfile.TemporaryDirectory(prefix='ot-sim-matter-supervisor-') as temp:
        root = Path(temp)
        for name, content in (('zig', ZIG), ('native', NATIVE), ('matter', MATTER)):
            path = root / name
            path.write_text(content)
            path.chmod(0o700)
        control = root / 'control'
        events = root / 'events'
        events.touch()
        launches = root / 'matter-launches'
        launches.touch()
        api_socket = root / 'zig.sock'
        matter_socket = root / 'matter.sock'
        thread_socket = root / 'thread.sock'
        state = root / 'state'
        state.mkdir(mode=0o700)
        config = root / 'native.json'
        config.write_text('{}')
        master, slave = pty.openpty()
        env = dict(os.environ, ZIGGURAT_BIN=str(root/'zig'), NATIVE_BIN=str(root/'native'),
                   NATIVE_CONFIG=str(config), MAINTENANCE_BIN=str(MAINTENANCE),
                   RUNTIME_CONTROL=str(control), ZIGGURAT_SOCKET=str(api_socket),
                   ZIGGURAT_DEVICE=os.ttyname(slave), RESTART_LIMIT='3', RESTART_BACKOFF='4',
                   STARTUP_LIMIT='3', STOP_LIMIT='2', MATTER_BIN=str(root/'matter'),
                   MATTER_STATE_DIR=str(state), MATTER_SOCKET=str(matter_socket),
                   THREAD_SOCKET=str(thread_socket), MATTER_RESTART_LIMIT='2',
                   MATTER_LAUNCHES=str(launches), REAL_MATTER_BIN=str(SERVICE),
                   TEST_EVENTS=str(events))
        os.close(slave)
        output = tempfile.TemporaryFile()
        supervisor = None
        relay = None
        radio = None
        try:
            radio = service_test.ChannelRcp('127.0.0.1', tcp_nodelay=True)
            relay = start_relay(type('RadioPath', (), {'path': thread_socket, 'rcp': radio})())
            supervisor = subprocess.Popen(['sh', str(PAIR)], env=env,
                                          stdout=output, stderr=output)
            wait_for(lambda: 'native start' in events.read_text())
            wait_for(lambda: len(launches.read_text().splitlines()) == 1)
            wait_for(lambda: matter_socket.exists())
            wait_for(lambda: ipc(matter_socket, 1, 1)['state'] == 'awaiting_coordinator')
            observation = b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}'
            assert ipc(matter_socket, 9, 2, observation)['ok']
            wait_for(lambda: ipc(matter_socket, 1, 3)['state'] == 'running')

            credentials = state / 'credentials.bin'
            radio.fail_next_dataset_write = True
            try:
                ipc(matter_socket, 9, 4,
                    b'{"schema":1,"action":"create","zigbee_channel":15}')
            except (ConnectionError, OSError, TimeoutError):
                pass
            else:
                raise AssertionError('Spinel dataset rejection unexpectedly returned an IPC response')
            wait_for(lambda: radio.dataset_write_errors == 1)
            wait_for(lambda: credentials.is_file())
            committed_credentials = credentials.read_bytes()
            wait_for(lambda: len(launches.read_text().splitlines()) == 1 and
                     not ipc_alive(matter_socket), timeout=6)

            relay.terminate()
            relay.wait(timeout=3)
            radio.close()
            radio = service_test.ChannelRcp('127.0.0.1', tcp_nodelay=True)
            thread_socket.unlink(missing_ok=True)
            relay = start_relay(type('RadioPath', (), {'path': thread_socket, 'rcp': radio})())

            wait_for(lambda: len(launches.read_text().splitlines()) == 2, timeout=10)
            restored = False
            next_id = 10
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not restored:
                try:
                    assert ipc(matter_socket, 9, next_id, observation)['ok']
                    next_id += 1
                    status = ipc(matter_socket, 1, next_id)
                    next_id += 1
                    if status.get('state') == 'running':
                        result = ipc(matter_socket, 2, next_id)
                        next_id += 1
                        restored = result.get('configured') and result.get('dataset_channel') == 15
                except (ConnectionError, OSError, TimeoutError):
                    time.sleep(0.05)
            assert restored, 'restarted Matter service did not restore the committed Thread dataset'
            assert credentials.read_bytes() == committed_credentials
            assert radio.dataset_write_errors == 0 and radio.dataset_write_count > 0
            assert events.read_text().count('zig start') == 1
            assert events.read_text().count('native start') == 1
            assert 'native stop' not in events.read_text()
            print('PASS real Spinel failure restarts Matter, restores Thread dataset, and leaves native/Zigbee running')
        finally:
            if supervisor is not None and supervisor.poll() is None:
                supervisor.terminate()
                supervisor.wait(timeout=10)
            if relay is not None and relay.poll() is None:
                relay.terminate()
                relay.wait(timeout=3)
            if radio is not None:
                radio.close()
            output.close()
            os.close(master)


def ipc_alive(path):
    try:
        ipc(path, 1, 99, timeout=0.1)
        return True
    except (ConnectionError, OSError, TimeoutError):
        return False


if __name__ == '__main__':
    main()
