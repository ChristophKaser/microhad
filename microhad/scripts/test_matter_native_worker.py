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
                    peer.settimeout(1)
                    header = self.exact(peer, 10)
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
        assert fixture.process.poll() is None
        print('PASS confirmed channel heartbeat, offline on radio loss, cached HTTP, absent-service 503')
    finally:
        if fixture.process is not None:
            fixture.tearDown()
        matter.close()


if __name__ == '__main__':
    main()
