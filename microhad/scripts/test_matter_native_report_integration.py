#!/usr/bin/env python3
"""Run the real Rust Matter report producer into the native gateway over IPC."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.parse

from test_ziggurat_native_peer import NativeTests, ROOT
from test_ziggurat_service import wait_for

RUST_ROOT = ROOT.parent / 'ziggurat-compilation'
SIM_SPEC = importlib.util.spec_from_file_location(
    'matter_integration_transport', RUST_ROOT / 'scripts/test-openthread-matter-transport.py')
sim_transport = importlib.util.module_from_spec(SIM_SPEC)
SIM_SPEC.loader.exec_module(sim_transport)


def main():
    radio = sim_transport.sim.ThreadRcp('127.0.0.1', tcp_nodelay=True)
    relay = probe = fixture = None
    try:
        with tempfile.TemporaryDirectory(prefix='ot-sim-matter-native-') as temp:
            directory = Path(temp)
            radio_socket = directory / 'radio.sock'
            relay = subprocess.Popen([str(RUST_ROOT / 'build/openthread-unix/sim-relay-host'),
                                      str(radio_socket), radio.address, str(radio.port), '105'],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                wait_for(lambda: radio_socket.exists() or relay.poll() is not None, timeout=5)
            except TimeoutError:
                raise RuntimeError('Synthetic relay startup timed out')
            if not radio_socket.exists():
                raise RuntimeError('Synthetic relay exited: ' + relay.stdout.read())
            state = directory / 'controller-state'
            ready = directory / 'native-ready'
            release = directory / 'native-release'
            env = os.environ.copy()
            env.update(MICROHAD_MATTER_TEST_STATE_DIR=str(state),
                       MICROHAD_MATTER_TEST_NATIVE_READY_FILE=str(ready),
                       MICROHAD_MATTER_TEST_NATIVE_RELEASE_FILE=str(release),
                       MICROHAD_MATTER_TEST_IPC_SOCKET='/tmp/microhad-matter.sock')
            probe = subprocess.Popen([
                str(RUST_ROOT / 'build/rs-matter/target/release/ot-matter-commissioning-probe'),
                'spinel+hdlc+unix://' + str(radio_socket) + '?no-reset', '--ble', '--service-owner',
                '--service-discovery', '--sensor-reports', '--native-ipc'],
                cwd=directory, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            wait_for(lambda: ready.exists() or probe.poll() is not None, timeout=100)
            if not ready.exists():
                raise RuntimeError('Rust Matter producer exited before IPC readiness: ' + probe.stdout.read())

            fixture = NativeTests.__new__(NativeTests)

            def launch():
                fixture.process = subprocess.Popen([
                    str(ROOT / 'build/host-ziggurat-matter/microhad-zigbee-native'),
                    '--config', str(fixture.config)], stdout=fixture.log, stderr=fixture.log)
                wait_for(lambda: fixture.status()['online'])

            fixture.launch = launch
            fixture.setUp()
            assert fixture.status()['coordinator_backend'] == 'ziggurat'
            accepted = fixture.request('/api/matter/devices/snapshot',
                                       json.dumps({'schema': 1, 'action': 'snapshot'}).encode())
            assert accepted['ok'] and accepted['state'] == 'queued', accepted

            def snapshot_complete():
                job = fixture.request('/api/matter/jobs/' + str(accepted['job']))
                assert job['state'] != 'failed', job
                return job if job['state'] == 'complete' else None

            snapshot = wait_for(snapshot_complete)['response']
            assert snapshot['ok'], snapshot
            while snapshot['result']['next_cursor'] is not None and not any(
                    item['endpoint'] == 1 for item in snapshot['result']['devices']):
                accepted = fixture.request('/api/matter/devices/snapshot', json.dumps({
                    'schema': 1, 'action': 'snapshot',
                    'cursor': snapshot['result']['next_cursor'],
                    'revision': snapshot['result']['revision']}).encode())
                assert accepted['ok'] and accepted['state'] == 'queued', accepted
                job_id = accepted['job']

                def next_snapshot_complete():
                    job = fixture.request('/api/matter/jobs/' + str(job_id))
                    assert job['state'] != 'failed', job
                    return job if job['state'] == 'complete' else None

                snapshot = wait_for(next_snapshot_complete)['response']
                assert snapshot['ok'], snapshot
            assert any(item['endpoint'] == 1 for item in snapshot['result']['devices']), snapshot
            release.touch()

            def reported_device():
                for device in fixture.status()['devices']:
                    if (device.get('source') == 'matter' and
                            device.get('matter_node') == '0000000000000002' and
                            device.get('matter_endpoint') == 1):
                        values = device.get('sensor_values', {})
                        if (values.get('temperature') == {'value': -5.25, 'unit': '°C', 'quality': 'measured'} and
                                values.get('humidity') == {'value': 64, 'unit': '%', 'quality': 'measured'} and
                                values.get('air_quality') == {'value': 'moderate', 'unit': '', 'quality': 'measured'} and
                                values.get('co2') == {'value': 800, 'unit': 'ppm', 'quality': 'measured'} and
                                values.get('pm25') == {'value': 25.5, 'unit': 'ug/m3', 'quality': 'measured'}):
                            return device
                return None

            try:
                device = wait_for(reported_device, timeout=15)
            except TimeoutError:
                raise RuntimeError('Matter reports did not populate every advertised sensor: ' +
                                   json.dumps(fixture.status()['devices']))
            values = device['sensor_values']
            external_id = urllib.parse.quote(device['external_id'], safe='')
            history = fixture.request('/api/devices/' + external_id + '/history')
            assert len(history['samples']) >= 5, history
            output, _ = probe.communicate(timeout=15)
            assert probe.returncode == 0, output
            assert 'PASS production manager sensor reports reach IPC from subscribed CASE reports' in output
            assert not radio.error, radio.error
            print(json.dumps({'native_device': device, 'history': history,
                              'rust_probe': output.splitlines()}, indent=2))
    finally:
        if fixture is not None:
            fixture.tearDown()
        for process in (probe, relay):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        radio.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise
