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

RUST_ROOT = ROOT.parent
SIM_SPEC = importlib.util.spec_from_file_location(
    'matter_integration_transport', RUST_ROOT / 'micro-thread-matter-daemon/scripts/test-openthread-matter-transport.py')
sim_transport = importlib.util.module_from_spec(SIM_SPEC)
SIM_SPEC.loader.exec_module(sim_transport)


def main():
    test_service_build = subprocess.run(
        ['bash', str(RUST_ROOT / 'micro-thread-matter-daemon/scripts/build-test-service.sh')],
        cwd=RUST_ROOT, check=False)
    if test_service_build.returncode != 0:
        raise RuntimeError('Host test-hook Matter service build failed')
    test_service_binary = RUST_ROOT / 'build/micro-thread-matter-daemon/service-test-target/release/mthread-matter'
    if not test_service_binary.is_file():
        raise RuntimeError('Host test-hook Matter service binary is missing')
    radio = sim_transport.sim.ThreadRcp('127.0.0.1', tcp_nodelay=True)
    relay = probe = daemon = fixture = None
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
            reports_sent = directory / 'native-reports-sent'
            reports_ack = directory / 'native-reports-ack'
            env = os.environ.copy()
            env.update(MICROHAD_MATTER_TEST_STATE_DIR=str(state),
                       MICROHAD_MATTER_TEST_NATIVE_READY_FILE=str(ready),
                       MICROHAD_MATTER_TEST_NATIVE_RELEASE_FILE=str(release),
                       MICROHAD_MATTER_TEST_NATIVE_REPORTS_SENT_FILE=str(reports_sent),
                       MICROHAD_MATTER_TEST_NATIVE_REPORTS_ACK_FILE=str(reports_ack),
                       MICROHAD_MATTER_TEST_IPC_SOCKET='/tmp/microhad-matter.sock')
            probe = subprocess.Popen([
                str(RUST_ROOT / 'build/micro-thread-matter-daemon/target/release/ot-matter-commissioning-probe'),
                'spinel+hdlc+unix://' + str(radio_socket) + '?no-reset', '--ble', '--service-owner',
                '--service-discovery', '--sensor-reports', '--native-ipc'],
                cwd=directory, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            wait_for(lambda: ready.exists() or probe.poll() is not None, timeout=100)
            if not ready.exists():
                raise RuntimeError('Rust Matter producer exited before IPC readiness: ' + probe.stdout.read())

            fixture = NativeTests.__new__(NativeTests)

            def launch():
                fixture.process = subprocess.Popen([
                    str(ROOT / 'build/host-ziggurat-matter/microhad'),
                    '--config', str(fixture.config)], stdout=fixture.log, stderr=fixture.log)
                wait_for(lambda: fixture.status()['online'])

            fixture.launch = launch
            fixture.setUp()
            assert fixture.status()['coordinator_backend'] == 'ziggurat'
            release.touch()
            wait_for(lambda: reports_sent.exists(), timeout=15)
            # Inventory is now pushed independently of the browser. Release
            # reports and verify the ordinary status cache consumes both.
            release.touch()
            wait_for(lambda: reports_sent.exists() or probe.poll() is not None, timeout=30)
            assert reports_sent.exists(), 'CASE reports were not published'

            # The fixture reports Boolean State true: contact made (closed).
            def reported_device():
                for device in fixture.status()['devices']:
                    if (device.get('source') == 'matter' and
                            device.get('matter_node') == '0000000000000002' and
                            device.get('matter_endpoint') == 1):
                        values = device.get('sensor_values', {})
                        if (device.get('has_contact') and device.get('contact_known') and not device.get('contact_open') and
                                values.get('temperature') == {'value': -5.25, 'unit': '°C', 'quality': 'measured'} and
                                values.get('humidity') == {'value': 64, 'unit': '%', 'quality': 'measured'} and
                                values.get('air_quality') == {'value': 'fair', 'unit': '', 'quality': 'measured'} and
                                values.get('co2') == {'value': 800, 'unit': 'ppm', 'quality': 'measured'} and
                                values.get('pm25') == {'value': 25.5, 'unit': 'ug/m3', 'quality': 'measured'}):
                            return device
                return None

            try:
                device = wait_for(reported_device, timeout=15)
            except TimeoutError:
                raise RuntimeError('Matter reports did not populate every advertised sensor: ' +
                                   json.dumps(fixture.status()['devices']))
            assert device['online'], 'live CASE reports must confirm Matter device presence: ' + json.dumps(device)
            values = device['sensor_values']
            external_id = urllib.parse.quote(device['external_id'], safe='')
            history = fixture.request('/api/devices/' + external_id + '/history')
            assert len(history['samples']) >= 5, history
            # Keep the fixture's reported contact state stable until native
            # has consumed the reports and persisted their history.
            reports_ack.touch()
            output, _ = probe.communicate(timeout=15)
            assert probe.returncode == 0, output
            assert 'PASS production manager sensor and contact reports reach IPC from subscribed CASE reports' in output

            # The accessory/commissioner fixture above proves CASE report
            # generation. Now run the actual thread-matter daemon with a
            # test-only Manager queue seed and verify its IPC events traverse
            # the same already-registered native device/history path.
            wait_for(lambda: not Path('/tmp/microhad-matter.sock').exists(), timeout=5)
            relay.wait(timeout=5)
            assert relay.returncode == 0
            radio.close()
            radio = sim_transport.sim.ThreadRcp('127.0.0.1', tcp_nodelay=True)
            daemon_radio_socket = directory / 'daemon-radio.sock'
            relay = subprocess.Popen([str(RUST_ROOT / 'build/openthread-unix/sim-relay-host'),
                                      str(daemon_radio_socket), radio.address, str(radio.port), '105'],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            wait_for(lambda: daemon_radio_socket.exists() or relay.poll() is not None, timeout=5)
            if not daemon_radio_socket.exists():
                raise RuntimeError('Daemon synthetic relay startup failed: ' + relay.stdout.read())
            state_tmp = state / 'tmp'
            state_tmp.mkdir(mode=0o700, exist_ok=True)
            state_tmp.chmod(0o700)
            daemon_env = env.copy()
            daemon_env['MICROHAD_MATTER_TEST_SEED_SENSOR_REPORT_NODE'] = '2'
            daemon = subprocess.Popen([
                str(test_service_binary),
                '--socket', '/tmp/microhad-matter.sock', '--state-dir', str(state),
                '--radio-url', 'spinel+hdlc+unix://' + str(daemon_radio_socket) + '?no-reset'],
                cwd=directory, env=daemon_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                wait_for(lambda: fixture.request('/api/matter/status')['state'] == 'running', timeout=20)
            except TimeoutError:
                return_code = daemon.poll()
                if return_code is not None:
                    daemon_out, daemon_err = daemon.communicate(timeout=2)
                    raise RuntimeError(f'thread-matter exited before reports: rc={return_code}, '
                                       f'stdout={daemon_out!r}, stderr={daemon_err!r}')
                raise RuntimeError('thread-matter did not report running before the report batch')

            # Give the service a Thread dataset and wait for the simulated RCP
            # to attach. Device report polling is intentionally gated on this
            # production lifecycle state.
            thread_status = fixture.request('/api/thread/status')
            if not thread_status['configured']:
                thread_request = urllib.request.Request(
                    'http://127.0.0.1:%d/api/thread/create' % fixture.port,
                    data=json.dumps({'schema': 1}).encode(),
                    headers={'Content-Type': 'application/json'})
                with urllib.request.urlopen(thread_request, timeout=2) as response:
                    assert response.status == 202
                    thread_job = json.load(response)['job']

                def thread_create_complete():
                    result = fixture.request('/api/matter/jobs/' + str(thread_job))
                    assert result['state'] != 'failed', result
                    return result if result['state'] == 'complete' else None

                thread_result = wait_for(thread_create_complete, timeout=10)['response']
                assert thread_result['ok'] and thread_result['result']['configured'], thread_result
            wait_for(lambda: fixture.request('/api/thread/status')['state'] == 'attached', timeout=30)

            def daemon_reports_visible():
                for item in fixture.status()['devices']:
                    if item.get('external_id') != device['external_id']:
                        continue
                    values = item.get('sensor_values', {})
                    if (item.get('contact_known') and not item.get('contact_open') and
                            values.get('temperature') == {'value': -4.25, 'unit': '°C', 'quality': 'measured'} and
                            values.get('humidity') == {'value': 63, 'unit': '%', 'quality': 'measured'} and
                            values.get('air_quality') == {'value': 'poor', 'unit': '', 'quality': 'measured'} and
                            values.get('co2') == {'value': 850, 'unit': 'ppm', 'quality': 'measured'} and
                            values.get('pm25') == {'value': 30.5, 'unit': 'ug/m3', 'quality': 'measured'}):
                        return item
                return None

            try:
                # The service accepts at most one ReportData exchange per
                # radio poll, whose production interval is five seconds.
                daemon_device = wait_for(daemon_reports_visible, timeout=35)
            except TimeoutError:
                thread_status = fixture.request('/api/thread/status')
                service_status = fixture.request('/api/matter/status')
                daemon.terminate()
                daemon_out, daemon_err = daemon.communicate(timeout=5)
                raise RuntimeError('Real thread-matter process reports did not update the common device: ' +
                                   json.dumps({'thread': thread_status, 'service': service_status,
                                               'devices': fixture.status()['devices'],
                                               'stdout': daemon_out.decode(errors='replace'),
                                               'stderr': daemon_err.decode(errors='replace')}))
            assert daemon_device['online'], 'daemon CASE reports must confirm Matter device presence'
            daemon_history = fixture.request('/api/devices/' + external_id + '/history')
            # Analog history is intentionally capped to one sample per metric
            # per minute; air-quality transitions are recorded immediately.
            assert any(sample[2] == 9 and sample[3] == 'poor'
                       for sample in daemon_history['samples']), daemon_history
            assert any(sample[2] == 3 and sample[3] == 1
                       for sample in daemon_history['samples']), daemon_history
            daemon.terminate()
            daemon_out, daemon_err = daemon.communicate(timeout=5)
            assert daemon.returncode == 0 and not daemon_out
            assert not daemon_err, daemon_err
            assert not radio.error, radio.error
            print(json.dumps({'native_device': device, 'history': history,
                              'daemon_device': daemon_device, 'daemon_history': daemon_history,
                              'rust_probe': output.splitlines()}, indent=2))
    finally:
        if fixture is not None:
            fixture.tearDown()
        for process in (probe, daemon, relay):
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
