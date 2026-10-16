#!/usr/bin/env python3
"""Host-only Matter worker/HTTP round trip; synthetic Ziggurat and IPC peers."""
import json
import importlib.util
import sys
import tempfile
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.request
import zlib
from test_ziggurat_native_peer import NativeTests, Peer, ROOT
from test_ziggurat_service import wait_for
sys.path.insert(0, str(ROOT.parent / "scripts"))
import matter_wire


class MatterPeer:
    def __init__(self):
        self.path = Path('/tmp/microhad-matter.sock')
        # Never replace an existing service socket or unrelated filesystem entry.
        if self.path.exists() or self.path.is_symlink():
            raise RuntimeError('Synthetic test requires an unused default Matter socket')
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(str(self.path))
        self.path.chmod(0o600)
        self.inode = self.path.stat().st_ino
        self.listener.listen(4)
        self.listener.settimeout(.1)
        self.stop = threading.Event()
        self.observations = []
        self.session = None
        self.configured = False
        self.mutations = []
        self.generation = 0
        self.next_operation_job = 1
        self.operation_jobs = {}
        self.stall_jobs = False
        self.malformed_next_event = False
        self.malformed_next_reply = False
        self.wrong_next_reply_id = False
        self.stall_next_status = False
        self.malformed_next_header = None
        self.truncate_next_reply = None
        self.saturate_next_event_queue = False
        self.onoff = False
        self.level = 0
        self.report_sent = False
        self.error = None
        self.closed = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def exact(peer, size):
        data = bytearray()
        while len(data) < size:
            chunk = peer.recv(size-len(data))
            if not chunk:
                raise RuntimeError('Incomplete synthetic IPC request')
            data.extend(chunk)
        return data

    @staticmethod
    def send(peer, data):
        try:
            peer.sendall(data)
            return True
        except (BrokenPipeError, ConnectionResetError):
            return False

    def run(self):
        try:
            while not self.stop.is_set():
                try:
                    peer, _ = self.listener.accept()
                except socket.timeout:
                    continue
                with peer:
                    peer.settimeout(.1)
                    while not self.stop.is_set():
                        try:
                            kind, opcode, ident, decoded_body = matter_wire.receive(peer, self.exact)
                        except socket.timeout:
                            continue
                        except (RuntimeError, ConnectionResetError):
                            break
                        assert kind == 0 and ident != 0
                        body = json.dumps(decoded_body).encode() if decoded_body else b''
                        thread = dict(enabled=True, available=True, configured=self.configured, attached=False,
                                      state='attaching' if self.configured else 'detached', linux_ipv6_or_tun=False)
                        event = None
                        if opcode == 9:
                            observation = json.loads(body)
                            action = observation['action']
                            if action == 'observe':
                                assert set(observation) == {'schema', 'action', 'zigbee_channel', 'zigbee_online'}
                                assert observation['schema'] == 1
                                self.observations.append(observation)
                                result = dict(schema=1, ok=True, result=thread)
                            elif action == 'retry':
                                assert set(observation) == {'schema', 'action'} and self.configured
                                assert self.observations and self.observations[-1]['zigbee_online']
                                result = dict(schema=1, ok=True, result=thread)
                            else:
                                assert action in ('create', 'import') and observation['schema'] == 1
                                assert self.observations[-1]['zigbee_online']
                                assert observation['zigbee_channel'] == self.observations[-1]['zigbee_channel']
                                self.mutations.append((action, observation['zigbee_channel']))
                                if action == 'create' and self.configured:
                                    result = dict(schema=1, ok=False, error='busy')
                                elif action == 'create':
                                    self.configured = True
                                    thread.update(configured=True, state='attaching')
                                    result = dict(schema=1, ok=True, result=thread)
                                else:
                                    result = dict(schema=1, ok=False, error='malformed')
                        elif opcode == 10:
                            decoded = json.loads(body)
                            if decoded['action'] == 'read':
                                assert decoded['cluster'] in (6, 8) and decoded['attribute'] == 0
                                job = self.next_operation_job
                                self.next_operation_job += 1
                                self.operation_jobs[f'{job:016x}'] = dict(kind='read', polls=0,
                                    cluster=decoded['cluster'])
                                result = dict(schema=1, ok=True, result=dict(job=f'{job:016x}', state='queued'))
                            else:
                                assert decoded['action'] == 'job'
                                job = self.operation_jobs[decoded['job']]
                                job['polls'] += 1
                                if self.stall_jobs or job['polls'] == 1:
                                    result = dict(schema=1, ok=True, result=dict(state='running', job=decoded['job']))
                                else:
                                    if job['kind'] == 'on': self.onoff = True
                                    elif job['kind'] == 'off': self.onoff = False
                                    elif job['kind'] == 'toggle': self.onoff = not self.onoff
                                    elif job['kind'] == 'move_to_level': self.level = job['level']
                                    cluster = job.get('cluster', 8 if job['kind'] == 'move_to_level' else 6)
                                    value = (dict(type='bool', value=self.onoff) if cluster == 6 else
                                             dict(type='u8', value=self.level))
                                    result = dict(schema=1, ok=True, result=dict(state='complete', job=decoded['job'],
                                        result=dict(node='0000000000000002', endpoint=1, cluster=cluster, attribute=0,
                                            value=value)))
                        elif opcode == 11:
                            decoded = json.loads(body)
                            job = self.next_operation_job
                            self.next_operation_job += 1
                            self.operation_jobs[f'{job:016x}'] = dict(kind=decoded['command'], polls=0,
                                level=decoded.get('level'))
                            result = dict(schema=1, ok=True, result=dict(job=f'{job:016x}', state='queued'))
                        elif opcode == 1:
                            assert not body
                            result = dict(enabled=True, available=True, controller_initialized=True,
                                          fabric_ready=True, state='running')
                        elif opcode in (3, 4, 5):
                            decoded = json.loads(body)
                            if opcode == 3:
                                time.sleep(.2)  # HTTP acceptance must not wait for the service reply.
                                if self.session and self.session['state'] == 'waiting_ble':
                                    result = dict(schema=1, ok=False, error='busy')
                                else:
                                    self.session = dict(session='0000000000000001', state='waiting_ble',
                                                        deadline_remaining_seconds=30)
                                    result = dict(schema=1, ok=True, result=self.session)
                            elif not self.session or decoded['session'] != self.session['session']:
                                result = dict(schema=1, ok=False, error='stale_session')
                            else:
                                if opcode == 5 and self.session['state'] not in ('cancelled', 'failed', 'complete', 'timed_out'):
                                    self.session = dict(session=self.session['session'], state='cancelled',
                                                        deadline_remaining_seconds=0, error='cancelled')
                                result = dict(schema=1, ok=True, result=self.session)
                        elif opcode in (6, 7, 8):
                            decoded = json.loads(body)
                            assert self.session and decoded['session'] == self.session['session']
                            if opcode == 6:
                                self.generation += 1
                                self.session.update(state='pase')
                                result = dict(schema=1, ok=True, result=dict(generation=self.generation))
                                event = dict(schema=1, event='ble_write', data=dict(session=self.session['session'],
                                             generation=self.generation, write=1, data='010203'))
                            elif decoded['generation'] != self.generation:
                                result = dict(schema=1, ok=False, error='stale_generation')
                            elif opcode == 8:
                                result = dict(schema=1, ok=True, result=dict(write=decoded['write']))
                                if not decoded['success']:
                                    result = dict(schema=1, ok=False, error='transport')
                                    self.session.update(state='failed', error='transport', deadline_remaining_seconds=0)
                            else:
                                result = dict(schema=1, ok=True, result=dict(sequence=decoded['sequence']))
                        else:
                            assert opcode == 2 and not body
                            result = thread
                        if opcode in (1, 2) and self.stall_next_status:
                            self.stall_next_status = False
                            time.sleep(11)
                            break
                        if self.malformed_next_header:
                            mode = self.malformed_next_header
                            self.malformed_next_header = None
                            message = matter_wire.frame(opcode, 1, ident, result)
                            if mode == 'version':
                                message = matter_wire.frame(opcode, 1, ident, result, version=1)
                            elif mode == 'oversized':
                                message = struct.pack('<H', 2049)
                            elif mode == 'kind':
                                message = matter_wire.frame(opcode, 0, ident, result)
                            elif mode == 'opcode':
                                message = matter_wire.frame(opcode+1, 1, ident, result)
                            elif mode == 'zero-id':
                                message = matter_wire.frame(opcode, 1, 0, result)
                            else:
                                raise AssertionError('unknown malformed frame mode')
                            self.send(peer, message)
                            break
                        payload = json.dumps(result).encode()
                        if self.truncate_next_reply:
                            mode = self.truncate_next_reply
                            self.truncate_next_reply = None
                            if mode == 'header':
                                self.send(peer, matter_wire.frame(opcode, 1, ident, payload)[:1])
                            elif mode == 'payload':
                                message = matter_wire.frame(opcode, 1, ident, payload)
                                self.send(peer, message[:-1])
                            else:
                                raise AssertionError(f'unknown truncated reply mode: {mode}')
                            peer.shutdown(socket.SHUT_WR)
                            break
                        if self.malformed_next_reply:
                            self.malformed_next_reply = False
                            if not self.send(peer, struct.pack('<H', 1) + b'\xff'):
                                break
                            continue
                        response_id = ident
                        if self.wrong_next_reply_id:
                            self.wrong_next_reply_id = False
                            response_id = ident + 1
                        wire = bytearray(matter_wire.frame(opcode, 1, response_id, payload))
                        if self.malformed_next_event:
                            self.malformed_next_event = False
                            malformed = b'{"schema":1,"event":"matter_report","data":{}}'
                            wire.extend(matter_wire.frame(12, 2, 0, malformed))
                        if self.saturate_next_event_queue:
                            self.saturate_next_event_queue = False
                            overflow_report = dict(schema=1, event='matter_report', data=dict(
                                node='0000000000000002', endpoint=1, cluster=1026, attribute=0,
                                value=dict(type='measurement', value=dict(
                                    value=21.5, unit='celsius', quality='measured'))))
                            overflow_payload = json.dumps(overflow_report).encode()
                            overflow_frame = matter_wire.frame(12, 2, 0, overflow_payload)
                            for _ in range(9):
                                wire.extend(overflow_frame)
                        if opcode == 1:
                            base = dict(node='0000000000000002', state='complete', online=True,
                                id='matter:0000000000000001:0000000000000002',endpoint=None,servers=[],clients=[])
                            records=[dict(kind='identity',device=base,endpoint_count=2)]
                            for endpoint, servers in [(0,[29]),(1,[6,8,29,1026])]:
                                records.append(dict(kind='endpoint',endpoint_count=2,device=dict(base,
                                    id=base['id']+f':{endpoint:04x}',endpoint=endpoint,servers=servers)))
                            records.append(dict(kind='commit',device=base,endpoint_count=2))
                            for record in records:
                                wire.extend(matter_wire.frame(12,2,0,dict(schema=1,event='matter_inventory',data=record)))
                        if not self.send(peer, wire):
                            break
                        if event:
                            payload = json.dumps(event).encode()
                            if not self.send(peer, matter_wire.frame(12, 2, 0, payload)):
                                break
                        if (opcode == 10 and decoded.get('action') == 'job' and not self.report_sent and
                                self.operation_jobs[decoded['job']].get('cluster') == 8 and
                                self.operation_jobs[decoded['job']]['polls'] > 1):
                            # Exercise the unsolicited-report path after endpoint reconciliation and
                            # normal command/read handling have completed.
                            self.report_sent = True
                            report = dict(schema=1, event='matter_report', data=dict(
                                node='0000000000000002', endpoint=1, cluster=6, attribute=0,
                                value=dict(type='bool', value=True)))
                            payload = json.dumps(report).encode()
                            if not self.send(peer, matter_wire.frame(12, 2, 0, payload)):
                                break
                            sensor_report = dict(schema=1, event='matter_report', data=dict(
                                node='0000000000000002', endpoint=1, cluster=1026, attribute=0,
                                value=dict(type='measurement', value=dict(
                                    value=21.5, unit='celsius', quality='measured'))))
                            payload = json.dumps(sensor_report).encode()
                            if not self.send(peer, matter_wire.frame(12, 2, 0, payload)):
                                break
        except Exception as error:
            if not self.stop.is_set():
                self.error = type(error).__name__

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.stop.set()
        self.thread.join(timeout=2)
        self.listener.close()
        if self.path.exists() and self.path.stat().st_ino == self.inode:
            self.path.unlink()


class RealService:
    def __init__(self):
        compilation = ROOT.parent
        self.path = Path('/tmp/microhad-matter.sock')
        if self.path.exists() or self.path.is_symlink():
            raise RuntimeError('Synthetic test requires an unused default Matter socket')
        spec = importlib.util.spec_from_file_location('matter_runtime_test', compilation/'micro-thread-matter-daemon/scripts/test-matter-service.py')
        runtime = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runtime)
        self.runtime = runtime
        self.tmp = tempfile.TemporaryDirectory(prefix='ot-sim-native-real-service-')
        private = Path(self.tmp.name)
        radio_path = private/'radio.sock'
        self.radio = runtime.ChannelRcp('127.0.0.1', tcp_nodelay=True)
        self.relay = subprocess.Popen([str(compilation/'build/openthread-unix/sim-relay-host'),
            str(radio_path), self.radio.address, str(self.radio.port), '60'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for(lambda: radio_path.exists())
        self.process = subprocess.Popen([str(compilation/'build/micro-thread-matter-daemon/target/release/mthread-matter'),
            '--socket', str(self.path), '--state-dir', str(private/'state'),
            '--radio-url', f'spinel+hdlc+unix://{radio_path}?no-reset'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        wait_for(lambda: self.path.exists())
        self.closed = False
        self.error = None

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.process.terminate()
        stdout, stderr = self.process.communicate(timeout=3)
        assert self.process.returncode == 0 and not stdout
        assert not stderr
        assert not self.radio.error and not self.radio.counts.get('command_1', 0)
        self.relay.wait(timeout=3)
        self.radio.close()
        self.tmp.cleanup()


def commissioning_http(fixture):
    def post(action, body):
        encoded = json.dumps(body).encode() if isinstance(body, dict) else body
        request = urllib.request.Request(f'http://127.0.0.1:{fixture.port}/api/matter/commissioning/{action}',
                                         data=encoded, headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=2) as response:
            assert response.status == 202 and response.reason == 'Accepted'
            result = json.load(response)
            assert result['schema'] == 1 and result['ok'] and result['state'] == 'queued'
            return result['job']

    def finished(job):
        snapshot = fixture.request(f'/api/matter/jobs/{job}')
        assert snapshot['job'] == job
        if snapshot['state'] == 'failed':
            raise AssertionError('Synthetic commissioning HTTP delivery failed')
        return snapshot if snapshot['state'] == 'complete' else None

    def call(action, body):
        job = post(action, body)
        wait_for(lambda: finished(job))
        return finished(job)['response']

    start = dict(schema=1, discriminator=3840, pin=20202021, timeout_seconds=30)
    for invalid in (dict(start, pin=12345678), dict(start, unexpected=True),
                    dict(start, timeout_seconds=29), b'{"schema":1,"schema":1}'):
        try:
            post('start', invalid)
        except urllib.error.HTTPError as error:
            assert error.code == 400 and json.load(error) == dict(ok=False, error='malformed')
        else:
            raise AssertionError('Malformed setup input accepted')
    begin = time.monotonic()
    job = post('start', start)
    assert time.monotonic()-begin < .15
    for _ in range(10):
        assert fixture.request('/api/matter/status')['available']
    wait_for(lambda: finished(job))
    result = finished(job)['response']
    assert result['ok'] and result['result']['state'] == 'waiting_ble'
    session = result['result']['session']
    assert call('start', start) == dict(schema=1, ok=False, error='busy')
    assert call('status', dict(schema=1, session=session))['result']['state'] == 'waiting_ble'
    cancelled = call('cancel', dict(schema=1, session=session))
    assert cancelled['ok'] and cancelled['result']['state'] == 'cancelled'
    assert call('cancel', dict(schema=1, session=session))['result']['state'] == 'cancelled'
    assert call('status', dict(schema=1, session='ffffffffffffffff'))['error'] == 'stale_session'
    for ident in ('0', '01', '4294967296', '1garbage'):
        try:
            fixture.request('/api/matter/jobs/'+ident)
        except urllib.error.HTTPError as error:
            assert error.code == 400
        else:
            raise AssertionError('Invalid job ID accepted')
    try:
        fixture.request('/api/matter/jobs/4294967295')
    except urllib.error.HTTPError as error:
        assert error.code == 404
    else:
        raise AssertionError('Unknown job found')
    fixture.log.seek(0)
    assert b'20202021' not in fixture.log.read()


def thread_http(fixture, action, body):
    request = urllib.request.Request(f'http://127.0.0.1:{fixture.port}/api/thread/{action}',
                                     data=json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(request, timeout=2) as response:
        assert response.status == 202
        accepted = json.load(response)
    def done():
        snapshot = fixture.request('/api/matter/jobs/'+str(accepted['job']))
        assert snapshot['state'] != 'failed'
        return snapshot if snapshot['state'] == 'complete' else None
    wait_for(done)
    result = done()['response']
    assert 'dataset' not in result.get('result', {})
    return result


def synthetic_dataset(matter):
    # Read only this harness's private synthetic state; never export a real gateway dataset.
    assert Path(matter.tmp.name).name.startswith('ot-sim-native-real-service-')
    blob = (Path(matter.tmp.name)/'state/credentials.bin').read_bytes()
    assert blob[:8] == b'TMKV\x01\0\0\0'
    size, crc = struct.unpack_from('<II', blob, 8)
    assert size == len(blob)-16 and zlib.crc32(blob[16:]) == crc
    count, = struct.unpack_from('<H', blob, 16)
    offset = 18
    for _ in range(count):
        key, length = struct.unpack_from('<HH', blob, offset)
        value = blob[offset+4:offset+4+length]
        assert len(value) == length
        offset += length+4
        if key == 0x1001:
            assert value[:8] == b'TMDT\x01\0\0\0'
            length, = struct.unpack_from('<H', value, 9)
            assert len(value) == 11+length
            return value[11:]
    raise AssertionError('Synthetic dataset record absent')


class RelaySocket:
    def __init__(self, fixture, expected=101):
        self.peer = socket.create_connection(('127.0.0.1', fixture.port), timeout=2)
        self.events = []
        self.ident = 0
        self.peer.sendall(b'GET /api/matter/relay HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                          b'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                          b'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n')
        header = bytearray()
        while not header.endswith(b'\r\n\r\n'):
            header.extend(MatterPeer.exact(self.peer, 1))
            assert len(header) <= 4096
        assert header.startswith(f'HTTP/1.1 {expected} '.encode()), header
        if expected == 101:
            assert b'Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=' in header
        else:
            self.peer.close()

    def send(self, opcode, payload):
        if isinstance(payload, dict):
            payload = json.dumps(payload).encode()
        assert len(payload) <= 2048
        header = bytes([0x80 | opcode])
        header += bytes([0x80 | len(payload)]) if len(payload) < 126 else b'\xfe'+struct.pack('>H', len(payload))
        mask = b'abcd'
        encoded = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        self.peer.sendall(header+mask+encoded)

    def receive(self):
        first, second = MatterPeer.exact(self.peer, 2)
        assert first & 0x80 and not first & 0x70 and not second & 0x80
        size = second & 127
        if size == 126:
            size, = struct.unpack('>H', MatterPeer.exact(self.peer, 2))
        assert size <= 2048
        return first & 15, bytes(MatterPeer.exact(self.peer, size))

    def call(self, opcode, body):
        self.ident += 1
        self.send(1, dict(schema=1, id=self.ident, opcode=opcode, body=body))
        while True:
            kind, payload = self.receive()
            assert kind == 1, (kind, payload[:2])
            result = json.loads(payload)
            if 'event' in result:
                self.events.append(result)
                continue
            assert result['id'] == self.ident and result['schema'] == 1
            return result['response']

    def write_event(self):
        while True:
            for event in self.events:
                if event['event'] == 'ble_write':
                    self.events.remove(event)
                    return event['data']
            kind, payload = self.receive()
            assert kind == 1
            self.events.append(json.loads(payload))

    def close(self):
        self.peer.close()


def relay_http_call(fixture, action, body):
    pin = '20202021'
    accepted = fixture.request('/api/matter/commissioning/'+action, json.dumps(body).encode())
    assert pin not in json.dumps(accepted), accepted
    def done():
        result = fixture.request('/api/matter/jobs/'+str(accepted['job']))
        assert result['state'] != 'failed'
        return result if result['state'] == 'complete' else None
    wait_for(done)
    response = done()['response']
    assert pin not in json.dumps(response), response
    return response


def relay_tests(fixture, matter):
    def start():
        result = relay_http_call(fixture, 'start', dict(schema=1, discriminator=3840, pin=20202021, timeout_seconds=30))
        assert result['ok']
        return result['result']['session']
    def cancelled(session):
        result = relay_http_call(fixture, 'status', dict(schema=1, session=session))
        return result['result']['state'] == 'cancelled'
    session = start()
    unbound = RelaySocket(fixture)
    unbound.close()
    assert relay_http_call(fixture, 'status', dict(schema=1, session=session))['result']['state'] == 'waiting_ble'
    relay = RelaySocket(fixture)
    try:
        RelaySocket(fixture, expected=503)  # One phone owner; HTTP workers remain free.
        begin = time.monotonic()
        for _ in range(10):
            assert fixture.request('/api/matter/status')['available']
        assert time.monotonic()-begin < 1
        relay.send(9, b'ping')
        assert relay.receive() == (10, b'ping')
        assert relay.call(4, dict(schema=1, session=session))['result']['state'] == 'waiting_ble'
        generation = relay.call(6, dict(schema=1, session=session, address='010203040506', mtu=247))['result']['generation']
        write = relay.write_event()
        assert write['session'] == session and write['generation'] == generation and write['data']
        confirmed = relay.call(8, dict(schema=1, session=session, generation=generation, write=write['write'], success=True))
        assert confirmed['ok'] and confirmed['result']['write'] == write['write']
        generation = relay.call(6, dict(schema=1, session=session, address='010203040506', mtu=247))['result']['generation']
        write = relay.write_event()
        assert write['generation'] == generation
        indication = relay.call(7, dict(schema=1, session=session, generation=generation+1, sequence=1, data='00'))
        assert indication['error'] == 'stale_generation'
        stale = relay.call(8, dict(schema=1, session=session, generation=generation+1, write=write['write'], success=False))
        assert stale['error'] == 'stale_generation'
        result = relay.call(8, dict(schema=1, session=session, generation=generation, write=write['write'], success=False))
        assert result == dict(schema=1, ok=False, error='transport')
        failed = relay.call(4, dict(schema=1, session=session))
        assert failed['result']['state'] == 'failed' and failed['result']['error'] == 'transport'
    finally:
        relay.close()
    # Disconnect cancels only the bound active session; terminal failure remains terminal.
    wait_for(lambda: relay_http_call(fixture, 'status', dict(schema=1, session=session))['result']['state'] == 'failed')
    session = start()
    relay = RelaySocket(fixture)
    assert relay.call(4, dict(schema=1, session=session))['ok']
    relay.close()
    wait_for(lambda: cancelled(session))
    session = start()
    relay = RelaySocket(fixture)
    try:
        assert relay.call(4, dict(schema=1, session=session))['ok']
        relay.peer.sendall(b'\x81\x01x')  # Missing client mask is a protocol failure.
        kind, payload = relay.receive()
        while kind == 1:
            kind, payload = relay.receive()
        assert kind == 8 and struct.unpack('>H', payload[:2])[0] == 1002
    finally:
        relay.close()
    wait_for(lambda: cancelled(session))
    session = start()
    relay = RelaySocket(fixture)
    try:
        assert relay.call(4, dict(schema=1, session=session))['ok']
        relay.send(1, dict(schema=1, id=1, opcode=4, body=dict(schema=1, session=session)))
        kind, payload = relay.receive()
        while kind == 1:
            kind, payload = relay.receive()
        assert kind == 8 and struct.unpack('>H', payload[:2])[0] == 1008
    finally:
        relay.close()
    wait_for(lambda: cancelled(session))
    if hasattr(matter, 'malformed_next_event'):
        # If the persistent IPC channel breaks with a bound relay session,
        # cancel that exact session after reconnect; never replay BLE writes.
        session = start()
        relay = RelaySocket(fixture)
        assert relay.call(4, dict(schema=1, session=session))['ok']
        matter.malformed_next_event = True
        try:
            def unavailable():
                try:
                    fixture.request('/api/matter/status')
                except urllib.error.HTTPError as error:
                    return error.code == 503
                return False
            wait_for(unavailable, timeout=6)
            wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            wait_for(lambda: cancelled(session))
        finally:
            relay.close()
    fixture.log.seek(0)
    assert b'20202021' not in fixture.log.read()
    assert '20202021' not in json.dumps(fixture.status())
    try:
        logs = fixture.request('/api/logs')
    except urllib.error.HTTPError as error:
        # Host production binaries have no BusyBox syslog shared-memory ring.
        assert error.code == 503
        logs = json.load(error)
        assert logs == {'ok': False, 'error': 'log is unavailable'}
    assert '20202021' not in json.dumps(logs), logs



def device_inventory_http(fixture, real):
    try:
        fixture.request('/api/matter/devices/snapshot', b'{"schema":1,"action":"snapshot"}')
    except urllib.error.HTTPError as error:
        assert error.code in (400, 404)
    else:
        raise AssertionError('Removed listing route still exists')
    if real:
        assert not any(device.get('source') == 'matter' for device in fixture.status()['devices'])
    else:
        def inventory():
            devices=[device for device in fixture.status()['devices'] if device.get('source')=='matter']
            return devices if len(devices)==1 and devices[0].get('endpoint_metadata_state')=='ready' else None
        devices=wait_for(inventory)
        assert devices[0]['id']=='matter:0000000000000001:0000000000000002'
        assert [entry['endpoint'] for entry in devices[0]['endpoint_descriptors']]==[0,1]


def device_operations_http(fixture):
    def operation(path, body):
        accepted = fixture.request(path, json.dumps(body).encode())
        assert accepted['ok'] and accepted['state'] == 'queued'
        def finished():
            job = fixture.request('/api/matter/jobs/'+str(accepted['job']))
            assert job['state'] != 'failed', job
            if job['state'] != 'complete': return None
            assert job['response']['ok'] and job['response']['result']['state'] == 'complete'
            return {'value': job['response']['result']['result']['value']['value']}
        return wait_for(finished)
    identity = dict(node='0000000000000002', endpoint=1)
    assert operation('/api/matter/devices/command', dict(identity, command='on')) == {'value': True}
    assert operation('/api/matter/devices/read', dict(identity, cluster=6, attribute=0)) == {'value': True}
    assert operation('/api/matter/devices/command', dict(identity, command='toggle')) == {'value': False}
    assert operation('/api/matter/devices/command', dict(identity, command='off')) == {'value': False}
    assert operation('/api/matter/devices/read', dict(identity, cluster=8, attribute=0)) == {'value': 0}
    assert operation('/api/matter/devices/command', dict(identity, command='move_to_level',
        level=124, transition_deciseconds=20)) == {'value': 124}
    assert operation('/api/matter/devices/read', dict(identity, cluster=8, attribute=0)) == {'value': 124}


def device_dispatch_http(fixture):
    identity = dict(node='0000000000000002', endpoint=1,
                    command='move_to_level', level=124, transition_deciseconds=20)
    accepted = fixture.request('/api/matter/devices/command', json.dumps(identity).encode())
    assert accepted['ok'] and accepted['state'] == 'queued'

    def finished():
        job = fixture.request('/api/matter/jobs/' + str(accepted['job']))
        return job if job['state'] == 'complete' else None

    job = wait_for(finished)
    assert job['response'] == {'schema': 1, 'ok': False, 'error': 'not_ready'}, job


def device_queue_pressure_http(fixture, matter):
    matter.stall_jobs = True
    body = json.dumps(dict(node='0000000000000002', endpoint=1,
                           command='on')).encode()

    def submit(index):
        try:
            accepted = fixture.request('/api/matter/devices/command', body)
            assert accepted['ok'] and accepted['state'] == 'queued', accepted
            return accepted['job']
        except urllib.error.HTTPError as error:
            response = json.load(error)
            assert error.code == 503 and response == {'ok': False, 'error': 'capacity'}, (index, error.code, response)
            return None

    try:
        results = [submit(index) for index in range(24)]
        accepted = [job for job in results if job is not None]
        rejected = results.count(None)
        assert accepted and rejected, results
        assert len(accepted) <= 16, len(accepted)
    finally:
        matter.stall_jobs = False

    def drained():
        states = []
        for job in accepted:
            status = fixture.request('/api/matter/jobs/' + str(job))
            assert status['state'] != 'failed', status
            states.append(status['state'])
        return states if all(state == 'complete' for state in states) else None

    assert wait_for(drained, timeout=8), 'Accepted Matter jobs did not drain after peer recovery'
    return len(accepted), rejected


def main():
    real = '--real-service' in sys.argv[1:]
    matter = RealService() if real else MatterPeer()
    fixture = NativeTests('test_fragmented_startup_and_join')
    def launch():
        fixture.process = subprocess.Popen([str(ROOT/'build/host-ziggurat-matter/microhad'),
            '--config', str(fixture.config)], stdout=fixture.log, stderr=fixture.log)
        wait_for(lambda: fixture.status()['online'])
    fixture.launch = launch
    queue_counts = (0, 0)
    try:
        fixture.setUp()
        expected = bytes.fromhex(fixture.state['network'])[0]
        if not real:
            wait_for(lambda: any(o['zigbee_online'] and o['zigbee_channel'] == expected
                                 for o in matter.observations))
        assert fixture.status()['observed_network_channel'] == expected
        wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
        if not real:
            # An invalid unsolicited event must clear cached readiness and force
            # the native worker to reconnect before serving Matter status again.
            matter.malformed_next_event = True
            def matter_unavailable():
                try:
                    fixture.request('/api/matter/status')
                except urllib.error.HTTPError as error:
                    return error.code == 503
                return False
            wait_for(matter_unavailable)
            try:
                wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            except TimeoutError:
                fixture.log.seek(0)
                sys.stderr.write(f'Matter peer error: {matter.error}\n')
                sys.stderr.write(fixture.log.read().decode(errors='replace'))
                raise
            # Nine valid events exceed MatterChannel's bounded eight-event
            # queue. The worker must clear readiness and reconnect cleanly.
            matter.saturate_next_event_queue = True
            wait_for(matter_unavailable)
            try:
                wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            except TimeoutError:
                fixture.log.seek(0)
                sys.stderr.write(f'Overflow peer error: {matter.error}\n')
                sys.stderr.write(fixture.log.read().decode(errors='replace'))
                raise
            # A malformed response body must invalidate the persistent channel
            # and cached status, then recover through a fresh IPC connection.
            matter.malformed_next_reply = True
            wait_for(matter_unavailable)
            wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            # A valid response with an unrelated request ID must be discarded
            # as a channel protocol failure, followed by a fresh connection.
            matter.wrong_next_reply_id = True
            wait_for(matter_unavailable)
            wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            # A status peer that accepts IPC but withholds its reply must
            # expire the worker cache and recover over a fresh connection.
            matter.stall_next_status = True
            wait_for(matter_unavailable, timeout=6)
            wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            # Partial response headers and bodies must close the channel and
            # recover without exposing stale cached status.
            for truncated_reply in ('header', 'payload'):
                matter.truncate_next_reply = truncated_reply
                wait_for(matter_unavailable)
                wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
            # A structurally invalid frame header must also clear readiness.
            for malformed_header in ('version', 'oversized',
                                     'kind', 'opcode', 'zero-id'):
                matter.malformed_next_header = malformed_header
                wait_for(matter_unavailable)
                wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
        device_inventory_http(fixture, real)
        if real:
            device_dispatch_http(fixture)
        else:
            try:
                device_operations_http(fixture)
                queue_counts = device_queue_pressure_http(fixture, matter)
            except Exception:
                fixture.log.seek(0)
                sys.stderr.write(f'IPC peer error: {matter.error}\n')
                sys.stderr.write(fixture.log.read().decode(errors='replace'))
                raise
            def report_applied():
                devices = fixture.status()['devices']
                return next((device for device in devices
                    if device['source'] == 'matter' and device['matter_node'] == '0000000000000002' and
                    device['matter_endpoint'] == 1 and device['light_known'] and device['light_on'] and
                    device['sensor_values']['temperature'] == dict(value=21.5, unit='°C', quality='measured')), None)
            assert wait_for(report_applied), 'Unsolicited Matter reports did not update shared device state'
        begin = time.monotonic()
        for _ in range(20):
            assert fixture.request('/api/thread/status')['available']
        assert time.monotonic()-begin < 1
        for action, invalid in (('create', dict(schema=1, zigbee_channel=20)),
                                ('import', dict(schema=1, dataset='zz')),
                                ('import', dict(schema=1, dataset='aa'*255)),
                                ('import', dict(schema=1, dataset='00', zigbee_channel=20))):
            try:
                thread_http(fixture, action, invalid)
            except urllib.error.HTTPError as error:
                assert error.code == 400
            else:
                raise AssertionError('Malformed/overridden Thread input accepted')
        assert thread_http(fixture, 'import', dict(schema=1, dataset='00'))['error'] == 'malformed'
        created = thread_http(fixture, 'create', dict(schema=1))
        assert created['ok'] and created['result']['configured']
        wait_for(lambda: fixture.request('/api/thread/status')['configured'])
        retried = thread_http(fixture, 'retry', dict(schema=1))
        assert retried['ok'] and retried['result']['configured'], retried
        assert thread_http(fixture, 'create', dict(schema=1))['error'] == 'busy'
        if real:
            assert fixture.request('/api/thread/status')['zigbee_channel'] == expected
            dataset = synthetic_dataset(matter)
            credentials = Path(matter.tmp.name)/'state/credentials.bin'
            before = credentials.read_bytes()
            imported = thread_http(fixture, 'import', dict(schema=1, dataset=dataset.hex()))
            assert imported['ok'] and imported['result']['configured']
            assert credentials.read_bytes() == before
            changed = bytearray(dataset)
            offset = 0
            while offset < len(changed):
                tag, length = changed[offset:offset+2]
                if tag == 1:
                    assert length == 2
                    old = bytes(changed[offset+2:offset+4])
                    changed[offset+2:offset+4] = b'\x00\x01' if old != b'\x00\x01' else b'\x00\x02'
                    break
                offset += length+2
            else:
                raise AssertionError('Synthetic dataset missing PAN TLV')
            replacement = thread_http(fixture, 'import', dict(schema=1, dataset=changed.hex()))
            assert replacement['error'] == 'busy' and credentials.read_bytes() == before
            # Mutate only the channel TLV in a valid synthetic dataset.
            mismatched = bytearray(dataset)
            offset = 0
            while offset < len(mismatched):
                tag, length = mismatched[offset:offset+2]
                if tag == 0:
                    assert length == 3
                    mismatched[offset+4] = 11 if expected != 11 else 26
                    break
                offset += length+2
            else:
                raise AssertionError('Synthetic dataset missing channel TLV')
            rejected = thread_http(fixture, 'import', dict(schema=1, dataset=mismatched.hex()))
            assert rejected['error'] == 'malformed' and credentials.read_bytes() == before
            assert matter.radio.channel_writes and all(c == expected for c in matter.radio.channel_writes)
            fixture.log.seek(0)
            assert dataset.hex().encode() not in fixture.log.read()
        else:
            assert matter.mutations and all(c == expected for _, c in matter.mutations)
        commissioning_http(fixture)
        relay_tests(fixture, matter)
        # Coordinator loss and a live channel change must not strand the native
        # worker or permit Thread radio writes on an incompatible channel.
        if real:
            fixture.peer.close()
            wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'awaiting_coordinator')
            assert fixture.process.poll() is None

            def reconnect_coordinator(channel):
                fixture.path.unlink()
                # The native client advances persisted security counters during
                # startup, so a returning coordinator must report its current state.
                coordinator_state = fixture.saved()
                network = bytearray.fromhex(coordinator_state['network'])
                network[0] = channel
                coordinator_state['network'] = network.hex()
                fixture.peer = Peer(fixture.path, coordinator_state)
                fixture.peer.configured = True
                fixture.peer.started = True
                try:
                    wait_for(lambda: fixture.status()['observed_network_channel'] == channel)
                except TimeoutError:
                    fixture.log.seek(0)
                    sys.stderr.write(f'Coordinator peer error: {fixture.peer.error}\n')
                    sys.stderr.write(f'Native status: {fixture.status()}\n')
                    sys.stderr.write(f'Coordinator commands: {[command for command, _ in fixture.peer.commands]}\n')
                    logs = fixture.request('/api/logs').get('lines', [])
                    sys.stderr.write(f'Native logs: {logs[-8:]}\n')
                    sys.stderr.write(fixture.log.read().decode(errors='replace'))
                    raise

            mismatched_channel = 11 if expected != 11 else 26
            reconnect_coordinator(mismatched_channel)
            wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'channel_mismatch')
            assert fixture.process.poll() is None
            assert matter.radio.channel_writes and all(c == expected for c in matter.radio.channel_writes)
            fixture.peer.close()
            wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'awaiting_coordinator')
            reconnect_coordinator(expected)
            try:
                wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'attached', timeout=15)
            except TimeoutError:
                sys.stderr.write(f'Thread recovery status: {fixture.request("/api/thread/status")}\n')
                sys.stderr.write(f'Matter radio writes: {matter.radio.channel_writes}\n')
                raise
            renewal_channel_writes = len(matter.radio.channel_writes)
            time.sleep(6)
            renewed = fixture.request('/api/thread/status')
            assert renewed['state'] == 'attached' and renewed['zigbee_channel'] == expected, renewed
            assert len(matter.radio.channel_writes) == renewal_channel_writes
            assert fixture.process.poll() is None
        else:
            fixture.peer.close()
            wait_for(lambda: any(not o['zigbee_online'] and o['zigbee_channel'] == 0
                                 for o in matter.observations))
        assert matter.error is None
        create_result = thread_http(fixture, 'create', dict(schema=1))
        if real:
            assert create_result == dict(schema=1, ok=False, error='busy')
        else:
            assert create_result == dict(schema=1, ok=False, error='not_ready')
        # Missing service clears the cache while Zigbee/native remains responsive.
        matter.close()
        def unavailable():
            try:
                fixture.request('/api/matter/status')
            except Exception as error:
                return getattr(error, 'code', None) == 503
            return False
        wait_for(unavailable)
        try:
            fixture.request('/api/matter/commissioning/start', json.dumps(dict(
                schema=1, discriminator=3840, pin=20202021, timeout_seconds=30)).encode())
        except urllib.error.HTTPError as error:
            assert error.code == 503 and json.load(error) == dict(ok=False, error='not_ready')
        else:
            raise AssertionError('Absent service accepted a new commissioning job')
        assert fixture.process.poll() is None
        print('PASS persistent heartbeat, queued HTTP/Thread lifecycle, owned WebSocket/BTP callbacks, cancellation, radio loss and 503; ' +
              f'Matter job queue admitted {queue_counts[0]} and rejected {queue_counts[1]} at capacity, then drained')
    finally:
        if fixture.process is not None:
            fixture.tearDown()
        matter.close()


if __name__ == '__main__':
    main()
