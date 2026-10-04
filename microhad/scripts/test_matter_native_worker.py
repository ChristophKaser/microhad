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
from test_ziggurat_native_peer import NativeTests, ROOT
from test_ziggurat_service import wait_for


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
                            header = self.exact(peer, 10)
                        except socket.timeout:
                            continue
                        except RuntimeError:
                            break
                        size, version, kind, opcode, reserved, ident = struct.unpack('<HBBBBI', header)
                        assert version == 1 and kind == 0 and reserved == 0 and ident != 0 and size <= 2048
                        body = self.exact(peer, size)
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
                            if decoded['action'] == 'snapshot':
                                cursor = decoded.get('cursor', 0)
                                revision = '0000000000000007'
                                if decoded.get('revision', revision) != revision:
                                    result = dict(schema=1, ok=False, error='stale_revision')
                                else:
                                    endpoint = cursor
                                    result = dict(schema=1, ok=True, result=dict(revision=revision,
                                        devices=[dict(node='0000000000000002', state='complete', online=True,
                                            id=f'matter:0000000000000001:0000000000000002:{endpoint:04x}',
                                            endpoint=endpoint, servers=[6, 29, 1026])],
                                        next_cursor=1 if cursor == 0 else None))
                            elif decoded['action'] == 'read':
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
                        payload = json.dumps(result).encode()
                        peer.sendall(struct.pack('<HBBBBI', len(payload), 1, 1, opcode, 0, ident)+payload)
                        if event:
                            payload = json.dumps(event).encode()
                            peer.sendall(struct.pack('<HBBBBI', len(payload), 1, 2, 12, 0, 0)+payload)
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
                            peer.sendall(struct.pack('<HBBBBI', len(payload), 1, 2, 12, 0, 0)+payload)
                            sensor_report = dict(schema=1, event='matter_report', data=dict(
                                node='0000000000000002', endpoint=1, cluster=1026, attribute=0,
                                value=dict(type='measurement', value=dict(
                                    value=21.5, unit='celsius', quality='measured'))))
                            payload = json.dumps(sensor_report).encode()
                            peer.sendall(struct.pack('<HBBBBI', len(payload), 1, 2, 12, 0, 0)+payload)
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
        compilation = ROOT.parent/'ziggurat-compilation'
        self.path = Path('/tmp/microhad-matter.sock')
        if self.path.exists() or self.path.is_symlink():
            raise RuntimeError('Synthetic test requires an unused default Matter socket')
        spec = importlib.util.spec_from_file_location('matter_runtime_test', compilation/'scripts/test-matter-service.py')
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
        self.process = subprocess.Popen([str(compilation/'build/rs-matter/target/release/thread-matter'),
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
        assert stderr == b'Matter controller service started\nMatter controller service stopped\n'
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
    accepted = fixture.request('/api/matter/commissioning/'+action, json.dumps(body).encode())
    def done():
        result = fixture.request('/api/matter/jobs/'+str(accepted['job']))
        assert result['state'] != 'failed'
        return result if result['state'] == 'complete' else None
    wait_for(done)
    return done()['response']


def relay_tests(fixture):
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
    fixture.log.seek(0)
    assert b'20202021' not in fixture.log.read()



def device_snapshot_http(fixture, real):
    def page(body):
        accepted = fixture.request('/api/matter/devices/snapshot', json.dumps(body).encode())
        assert accepted['ok'] and accepted['state'] == 'queued'
        def finished():
            job = fixture.request('/api/matter/jobs/'+str(accepted['job']))
            assert job['state'] != 'failed', job
            return job if job['state'] == 'complete' else None
        wait_for(finished)
        return finished()['response']
    for bad in (dict(schema=1, action='snapshot', cursor=1),
                dict(schema=1, action='snapshot', revision='0000000000000000'),
                dict(schema=1, action='snapshot', cursor=2048),
                dict(schema=1, action='read'),
                dict(schema=1, action='snapshot', secret='discard')):
        try:
            page(bad)
        except urllib.error.HTTPError as error:
            assert error.code == 400 and json.load(error)['error'] == 'malformed'
        else:
            raise AssertionError('Invalid snapshot request accepted')
    first = page(dict(schema=1, action='snapshot'))
    assert first['ok'] and len(first['result']['revision']) == 16
    if real:
        assert first['result']['devices'] == [] and first['result']['next_cursor'] is None
    else:
        assert first['result']['devices'][0]['endpoint'] == 0
        final = page(dict(schema=1, action='snapshot', cursor=first['result']['next_cursor'],
                          revision=first['result']['revision']))
        assert final['result']['devices'][0]['endpoint'] == 1
        assert final['result']['next_cursor'] is None
    changed = '0000000000000001' if first['result']['revision'] != '0000000000000001' else '0000000000000002'
    assert page(dict(schema=1, action='snapshot', revision=changed))['error'] == 'stale_revision'


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
        fixture.process = subprocess.Popen([str(ROOT/'build/host-ziggurat-matter/microhad-zigbee-native'),
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
        device_snapshot_http(fixture, real)
        if not real:
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
                    device['matter_endpoint'] == 1 and device['actor_known'] and device['actor_on'] and
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
        relay_tests(fixture)
        # Radio loss must publish offline, rather than renewing the old channel.
        fixture.peer.close()
        if real:
            wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'awaiting_coordinator')
        else:
            wait_for(lambda: any(not o['zigbee_online'] and o['zigbee_channel'] == 0
                                 for o in matter.observations))
        assert matter.error is None
        assert thread_http(fixture, 'create', dict(schema=1)) == dict(schema=1, ok=False, error='not_ready')
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
