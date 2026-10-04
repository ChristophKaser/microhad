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
                        thread = dict(enabled=True, available=True, configured=False, attached=False,
                                      state='detached', linux_ipv6_or_tun=False)
                        if opcode == 9:
                            observation = json.loads(body)
                            assert set(observation) == {'schema', 'action', 'zigbee_channel', 'zigbee_online'}
                            assert observation['schema'] == 1 and observation['action'] == 'observe'
                            self.observations.append(observation)
                            result = dict(schema=1, ok=True, result=thread)
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
                                if opcode == 5:
                                    self.session = dict(session=self.session['session'], state='cancelled',
                                                        deadline_remaining_seconds=0, error='cancelled')
                                result = dict(schema=1, ok=True, result=self.session)
                        else:
                            assert opcode == 2 and not body
                            result = thread
                        payload = json.dumps(result).encode()
                        peer.sendall(struct.pack('<HBBBBI', len(payload), 1, 1, opcode, 0, ident)+payload)
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


def main():
    real = '--real-service' in sys.argv[1:]
    matter = RealService() if real else MatterPeer()
    fixture = NativeTests('test_fragmented_startup_and_join')
    def launch():
        fixture.process = subprocess.Popen([str(ROOT/'build/host-ziggurat-matter/microhad-zigbee-native'),
            '--config', str(fixture.config)], stdout=fixture.log, stderr=fixture.log)
        wait_for(lambda: fixture.status()['online'])
    fixture.launch = launch
    try:
        fixture.setUp()
        expected = bytes.fromhex(fixture.state['network'])[0]
        if not real:
            wait_for(lambda: any(o['zigbee_online'] and o['zigbee_channel'] == expected
                                 for o in matter.observations))
        assert fixture.status()['observed_network_channel'] == expected
        wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running')
        begin = time.monotonic()
        for _ in range(20):
            assert fixture.request('/api/thread/status')['available']
        assert time.monotonic()-begin < 1
        if real:
            assert fixture.request('/api/thread/status')['zigbee_channel'] == expected
            created = matter.runtime.status(matter.path, 9, 901, json.dumps(
                dict(schema=1, action='create', zigbee_channel=expected)).encode())
            assert created['ok'] and created['result']['configured']
            wait_for(lambda: fixture.request('/api/thread/status')['configured'])
            assert matter.radio.channel_writes and all(c == expected for c in matter.radio.channel_writes), (
                expected, matter.radio.channel_writes)
        commissioning_http(fixture)
        # Radio loss must publish offline, rather than renewing the old channel.
        fixture.peer.close()
        if real:
            wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'awaiting_coordinator')
        else:
            wait_for(lambda: any(not o['zigbee_online'] and o['zigbee_channel'] == 0
                                 for o in matter.observations))
        assert matter.error is None
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
        print('PASS persistent heartbeat, queued commissioning HTTP lifecycle, cached reads, radio loss and 503')
    finally:
        if fixture.process is not None:
            fixture.tearDown()
        matter.close()


if __name__ == '__main__':
    main()
