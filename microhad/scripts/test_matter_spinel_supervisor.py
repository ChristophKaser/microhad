#!/usr/bin/env python3
"""Prove paired supervision recovers Matter after a synthetic Spinel failure.

Pass --native-worker to compose the failure with the actual host gateway and
coordinator worker. Neither mode opens a physical radio.
"""
import importlib.util
import json
import os
from pathlib import Path
import pty
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
RUST_ROOT = ROOT.parent
SERVICE = RUST_ROOT / 'build/micro-thread-matter-daemon/target/release/mthread-matter'
NATIVE_GATEWAY = ROOT / 'build/host-ziggurat-matter/microhad'
RELAY = RUST_ROOT / 'build/openthread-unix/sim-relay-host'
PAIR = ROOT / 'paired_start.sh'
MAINTENANCE = RUST_ROOT / 'build/gateway-maintenance-host'
SPEC = importlib.util.spec_from_file_location(
    'matter_service_test', RUST_ROOT / 'micro-thread-matter-daemon/scripts/test-matter-service.py')
service_test = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(service_test)
NATIVE_SPEC = importlib.util.spec_from_file_location(
    'native_peer_test', ROOT / 'scripts/test_ziggurat_native_peer.py')
native_peer_test = importlib.util.module_from_spec(NATIVE_SPEC)
sys.path.insert(0, str(ROOT / 'scripts'))
NATIVE_SPEC.loader.exec_module(native_peer_test)

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
import json,os,signal,sys,time
from pathlib import Path
if "--print-config" in sys.argv:
 print(json.loads(Path(sys.argv[sys.argv.index("--config")+1]).read_text()).get(sys.argv[sys.argv.index("--print-config")+1],""));sys.exit(0)
with open(os.environ['TEST_EVENTS'],'a') as f:f.write('native start\n')
def stop(*args):
 with open(os.environ['TEST_EVENTS'],'a') as f:f.write('native stop\n')
 raise SystemExit(0)
signal.signal(signal.SIGTERM,stop)
while True:time.sleep(.1)
'''
NATIVE_GATEWAY_WRAPPER = r'''#!/usr/bin/env python3
import os,sys
if "--print-config" in sys.argv:
 os.execv(os.environ["REAL_NATIVE_BIN"],[os.environ["REAL_NATIVE_BIN"]]+sys.argv[1:])
with open(os.environ['TEST_EVENTS'],'a') as f:f.write('native start\\n')
fd=os.open(os.environ['NATIVE_LOG'],os.O_WRONLY|os.O_CREAT|os.O_APPEND,0o600)
os.dup2(fd,1);os.dup2(fd,2)
os.execv(os.environ['REAL_NATIVE_BIN'],[os.environ['REAL_NATIVE_BIN']]+sys.argv[1:])
'''
ZIG_WITH_PEER = r'''#!/usr/bin/env python3
import json,os,signal,socket,sys,time
from pathlib import Path
fd=os.open(os.environ['ZIG_LOG'],os.O_WRONLY|os.O_CREAT|os.O_APPEND,0o600)
os.dup2(fd,1);os.dup2(fd,2)
sys.path.insert(0,os.environ['NATIVE_TEST_SCRIPTS'])
from test_ziggurat_native_peer import Peer
with open(os.environ['TEST_EVENTS'],'a') as f:f.write('zig start\\n')
path=Path(sys.argv[sys.argv.index('--listen')+1][5:])
probe=socket.socket(socket.AF_UNIX);probe.bind(str(path));probe.listen(1)
client,_=probe.accept();client.sendall(bytes.fromhex('01020302020100'));client.close()
probe.close();path.unlink()
peer=Peer(path,json.loads(Path(os.environ['TEST_ZIG_STATE']).read_text()))
def stop(*args):
 peer.close()
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
        peer.sendall(service_test.matter_wire.frame(opcode, 0, ident, body))
        def exact(stream, size):
            data = bytearray()
            while len(data) < size:
                part = stream.recv(size - len(data))
                if not part:
                    raise ConnectionError('Matter IPC connection closed')
                data.extend(part)
            return data
        kind, operation, response_id, result = service_test.matter_wire.receive(peer, exact)
        assert (kind, operation, response_id) == (1, opcode, ident)
        return result


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
    native_worker = '--native-worker' in sys.argv[1:]
    exhaust_retries = any(flag in sys.argv[1:] for flag in ('--repeat-failures', '--exhaust-retries'))
    binaries = (SERVICE, RELAY, MAINTENANCE, NATIVE_GATEWAY) if native_worker else (SERVICE, RELAY, MAINTENANCE)
    for binary in binaries:
        if not binary.is_file():
            raise RuntimeError('missing required host binary: ' + str(binary))
    if native_worker and (Path('/tmp/microhad-matter.sock').exists() or
                          Path('/tmp/microhad-matter.sock').is_symlink()):
        raise RuntimeError('native-worker mode requires an unused Matter service socket')
    with tempfile.TemporaryDirectory(prefix='ot-sim-matter-supervisor-') as temp:
        root = Path(temp)
        files = (('zig', ZIG_WITH_PEER), ('native', NATIVE_GATEWAY_WRAPPER), ('matter', MATTER)) \
            if native_worker else (('zig', ZIG), ('native', NATIVE), ('matter', MATTER))
        for name, content in files:
            path = root / name
            path.write_text(content)
            path.chmod(0o700)
        # Mock the launcher's syslog startup contract; never replace host /dev/log
        # or forward synthetic test traffic to the gateway's remote collector.
        syslog = root / 'syslog-fixture'
        syslog.write_text("""#!/usr/bin/env python3
import sys
from pathlib import Path
marker=Path(__file__).with_suffix('.started')
if sys.argv[1:]==['syslogd','-C512','-L','-m','0']:
 marker.touch()
elif sys.argv[1:]==['ps']:
 if marker.exists():
  print('999999 root 0 S /bin/busybox syslogd -C512 -L -m 0')
  marker.unlink()
else:
 raise SystemExit(2)
""")
        syslog.chmod(0o700)
        control = root / 'control'
        events = root / 'events'
        events.touch()
        launches = root / 'matter-launches'
        launches.touch()
        api_socket = root / 'zig.sock'
        matter_socket = Path('/tmp/microhad-matter.sock') if native_worker else root / 'matter.sock'
        thread_socket = root / 'thread.sock'
        state = root / 'state'
        state.mkdir(mode=0o700)
        config = root / 'native.json'
        peer_state_path = root / 'ziggurat-state.json'
        native_port = None
        if native_worker:
            peer_state = native_peer_test.native_state(
                native_peer_test.prepared_state(native_peer_test.fixture()))
            peer_state_path.write_text(json.dumps(peer_state))
            peer_state_path.chmod(0o600)
            native_port = native_peer_test.free_port()
            config.write_text(json.dumps({'serial_device': '/dev/forbidden-radio',
                'serial_baud': 115200, 'runtime_dir': str(root),
                'http_port': native_port, 'https_port': native_peer_test.free_port(),
                'ziggurat_socket': str(api_socket)}))
            config.chmod(0o600)
        else:
            config.write_text('{}')
        master, slave = pty.openpty()
        env = dict(os.environ, SYSLOGD_BIN=str(syslog), ZIGGURAT_BIN=str(root/'zig'), NATIVE_BIN=str(root/'native'),
                   NATIVE_CONFIG=str(config), MAINTENANCE_BIN=str(MAINTENANCE),
                   RUNTIME_CONTROL=str(control), ZIGGURAT_SOCKET=str(api_socket),
                   ZIGGURAT_DEVICE=os.ttyname(slave), RESTART_LIMIT='3', RESTART_BACKOFF='4',
                   STARTUP_LIMIT='3', STOP_LIMIT='2', MATTER_BIN=str(root/'matter'),
                   MATTER_STATE_DIR=str(state), MATTER_SOCKET=str(matter_socket),
                   THREAD_SOCKET=str(thread_socket), MATTER_RESTART_LIMIT='2',
                   MATTER_LAUNCHES=str(launches), REAL_MATTER_BIN=str(SERVICE),
                   TEST_EVENTS=str(events))
        if native_worker:
            env.update(REAL_NATIVE_BIN=str(NATIVE_GATEWAY),
                       NATIVE_TEST_SCRIPTS=str(ROOT/'scripts'),
                       TEST_ZIG_STATE=str(peer_state_path),
                       NATIVE_LOG=str(root/'native.log'),
                       ZIG_LOG=str(root/'zig.log'),
                       MATTER_SOCKET='/tmp/microhad-matter.sock')
        os.close(slave)
        output = tempfile.TemporaryFile()
        supervisor = None
        relay = None
        radio = None
        try:
            radio = service_test.ChannelRcp('127.0.0.1', tcp_nodelay=True)
            supervisor = subprocess.Popen(['sh', str(PAIR)], env=env,
                                          stdout=output, stderr=output)
            wait_for(lambda: 'zig start' in events.read_text())
            relay = start_relay(type('RadioPath', (), {'path': thread_socket, 'rcp': radio})())
            try:
                wait_for(lambda: 'native start' in events.read_text())
            except TimeoutError as error:
                output.seek(0)
                supervisor_log = output.read().decode(errors='replace')
                zig_log = (root/'zig.log').read_text() if (root/'zig.log').exists() else ''
                raise RuntimeError('native child did not start; zig=' + zig_log +
                                   ' supervisor=' + supervisor_log) from error
            wait_for(lambda: len(launches.read_text().splitlines()) == 1)
            wait_for(lambda: matter_socket.exists())
            observation = b'{"schema":1,"action":"observe","zigbee_channel":15,"zigbee_online":true}'
            expected_channel = 15
            if native_worker:
                expected_channel = bytes.fromhex(json.loads(peer_state_path.read_text())['network'])[0]
                try:
                    wait_for(lambda: api_state(native_port, '/api/status', 'online'))
                except TimeoutError as error:
                    native_log = (root/'native.log').read_text() if (root/'native.log').exists() else ''
                    raise RuntimeError('native gateway did not become online: ' + native_log) from error
                wait_for(lambda: api_field(native_port, '/api/status', 'observed_network_channel') == expected_channel)
                wait_for(lambda: api_field(native_port, '/api/matter/status', 'state') == 'running')
            else:
                wait_for(lambda: ipc(matter_socket, 1, 1)['state'] == 'awaiting_coordinator')
                assert ipc(matter_socket, 9, 2, observation)['ok']
                wait_for(lambda: ipc(matter_socket, 1, 3)['state'] == 'running')

            credentials = state / 'credentials.bin'
            radio.fail_next_dataset_write = True
            try:
                ipc(matter_socket, 9, 4,
                    json.dumps({'schema': 1, 'action': 'create',
                                'zigbee_channel': expected_channel}).encode())
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
            if native_worker:
                wait_for(lambda: api_field(native_port, '/api/matter/status', 'state') == 'running', timeout=15)
                thread_status = wait_for(lambda: api_object(native_port, '/api/thread/status',
                    lambda value: value if value.get('configured') and
                    value.get('zigbee_channel') == expected_channel else None), timeout=15)
                assert thread_status.get('state') in ('attaching', 'attached'), thread_status
                accepted = http_json(native_port, '/api/thread/create', {'schema': 1})
                assert accepted.get('ok') and accepted.get('state') == 'queued', accepted
                def restored_control():
                    job = http_json(native_port, '/api/matter/jobs/' + str(accepted['job']))
                    if job.get('state') == 'failed':
                        raise AssertionError('Thread create status failed after supervisor recovery')
                    if job.get('state') == 'complete':
                        return job.get('response')
                    return None
                control_result = wait_for(restored_control, timeout=8)
                assert control_result.get('error') == 'busy', control_result
                restored = True
            else:
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
                            restored = result.get('configured') and result.get('dataset_channel') == expected_channel
                    except (ConnectionError, OSError, TimeoutError):
                        time.sleep(0.05)
            assert restored, 'restarted Matter service did not restore the committed Thread dataset'
            assert credentials.read_bytes() == committed_credentials
            assert radio.dataset_write_errors == 0 and radio.dataset_write_count > 0
            assert events.read_text().count('zig start') == 1
            assert events.read_text().count('native start') == 1
            assert 'native stop' not in events.read_text()
            if exhaust_retries:
                matter_pid = int((control/'matter.pid').read_text())
                os.kill(matter_pid, 9)
                # The synthetic relay is single-connection and exits on EOF.
                # Recreate its listener, as real Ziggurat does after a consumer exits.
                relay.wait(timeout=3)
                radio.close()
                radio = service_test.ChannelRcp('127.0.0.1', tcp_nodelay=True)
                thread_socket.unlink(missing_ok=True)
                relay = start_relay(type('RadioPath', (), {'path': thread_socket, 'rcp': radio})())
                wait_for(lambda: len(launches.read_text().splitlines()) >= 3, timeout=15)
                time.sleep(1.1)
                assert len(launches.read_text().splitlines()) >= 3, launches.read_text()
                assert not (control/'matter.failed').exists()
                assert supervisor.poll() is None, 'Repeated Matter failures stopped the paired supervisor'
                assert events.read_text().count('zig start') == 1
                assert events.read_text().count('native start') == 1
                assert 'native stop' not in events.read_text()
                if native_worker:
                    assert api_state(native_port, '/api/status', 'online')
                    wait_for(lambda: api_state(native_port, '/api/matter/status', 'available'), timeout=15)
            mode = 'native worker' if native_worker else 'native stand-in'
            result = 'PASS real Spinel failure restarts Matter, restores Thread dataset through %s, and leaves Zigbee running' % mode
            if exhaust_retries:
                result += '; repeated real-service failure continues recovery without stopping Zigbee/native'
            print(result)
        finally:
            if sys.exc_info()[0] is not None:
                output.seek(0)
                print('SUPERVISOR OUTPUT:', output.read().decode(errors='replace'), file=sys.stderr)
                for evidence in (launches, events, control/'matter.events'):
                    if evidence.exists(): print(str(evidence), evidence.read_text(), file=sys.stderr)
            if supervisor is not None and supervisor.poll() is None:
                supervisor.terminate()
                supervisor.wait(timeout=10)
            if native_worker:
                matter_socket.unlink(missing_ok=True)
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


def http_json(port, path, body=None):
    request = urllib.request.Request('http://127.0.0.1:%d%s' % (port, path),
        data=json.dumps(body).encode() if body is not None else None,
        headers={'Content-Type': 'application/json'} if body is not None else {})
    with urllib.request.urlopen(request, timeout=2) as response:
        return json.load(response)


def api_object(port, path, predicate):
    try:
        value = http_json(port, path)
    except (OSError, TimeoutError, urllib.error.URLError):
        return None
    return predicate(value)


def api_field(port, path, field):
    return api_object(port, path, lambda value: value.get(field))


def api_state(port, path, field):
    return api_object(port, path, lambda value: value.get(field) if value.get(field) else None)


if __name__ == '__main__':
    main()
