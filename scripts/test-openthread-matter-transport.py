#!/usr/bin/env python3
"""Exercise the real OpenThread/Rust transport against an explicitly synthetic RCP."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ot_sim', ROOT / 'scripts/test-openthread-device.py')
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--commissioning', action='store_true', help='Run encrypted Matter fixture')
    parser.add_argument('--ble', action='store_true', help='Commission over synthetic phone GATT/BTP')
    parser.add_argument('--service-owner', action='store_true', help='Exercise the production owned commissioner with synthetic BLE')
    parser.add_argument('--service-discovery', action='store_true', help='Register accessory through SRP and use production DNS resolver')
    parser.add_argument('--sensor-reports', action='store_true', help='Run notifier-backed multi-cluster sensor report fixture')
    parser.add_argument('--icd', action='store_true', help='Exercise LIT RegisterClient, authenticated Check-In and wake-window dispatch')
    parser.add_argument('--native-bridge', action='store_true', help='Deliver live CASE sensor reports through a native gateway process and verify shared device state')
    args = parser.parse_args()
    if args.service_discovery and not args.service_owner:
        parser.error('--service-discovery requires --service-owner')
    if args.sensor_reports and not args.service_owner:
        parser.error('--sensor-reports requires --service-owner')
    if args.icd and not args.service_owner:
        parser.error('--icd requires --service-owner')
    if args.native_bridge and not (args.service_owner and args.service_discovery and args.sensor_reports):
        parser.error('--native-bridge requires --service-owner --service-discovery --sensor-reports')
    if args.service_owner and not args.ble:
        parser.error('--service-owner requires --ble')
    if args.ble and not args.commissioning:
        parser.error('--ble requires --commissioning')
    timeout = 160 if args.icd else 100 if args.commissioning else 22
    name = 'ot-matter-commissioning-probe' if args.commissioning else 'ot-matter-transport-probe'
    marker = 'PASS commissioning complete' if args.commissioning else 'PASS transport complete'
    radio = sim.ThreadRcp('127.0.0.1', tcp_nodelay=True)
    relay = None
    try:
        with tempfile.TemporaryDirectory(prefix='ot-sim-matter-') as directory:
            socket = Path(directory) / 'radio.sock'
            relay = subprocess.Popen([str(ROOT / 'build/openthread-unix/sim-relay-host'),
                                      str(socket), radio.address, str(radio.port), str(timeout + 5)])
            deadline = time.monotonic() + 5
            while not socket.exists():
                if relay.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Synthetic relay startup failed')
                time.sleep(0.02)
            binary = ROOT / 'build/rs-matter/target/release' / name
            command = [str(binary), 'spinel+hdlc+unix://' + str(socket) + '?no-reset']
            command.extend(flag for enabled, flag in (
                (args.ble, '--ble'), (args.service_owner, '--service-owner'),
                (args.service_discovery, '--service-discovery'),
                (args.sensor_reports, '--sensor-reports'), (args.icd, '--icd')) if enabled)
            if args.native_bridge:
                result, native_status = run_native_bridge(command, directory, timeout)
                print('PASS live CASE sensor reports reached the native gateway shared device model')
            else:
                try:
                    result = subprocess.run(command, cwd=directory, capture_output=True, text=True, timeout=timeout)
                except subprocess.TimeoutExpired as error:
                    partial_out = error.stdout.decode(errors='replace') if isinstance(error.stdout, bytes) else (error.stdout or '')
                    partial_err = error.stderr.decode(errors='replace') if isinstance(error.stderr, bytes) else (error.stderr or '')
                    raise RuntimeError('Commissioning fixture timed out; partial output:\n' + partial_out + partial_err) from error
                native_status = None
            if result.returncode or marker not in result.stdout:
                raise RuntimeError('Transport failed: ' + result.stdout + result.stderr + '\nRCP error: ' + str(radio.error))
            if args.service_discovery and 'PASS synthetic accessory SRP registration' not in result.stdout:
                raise RuntimeError('SRP/DNS fixture registration did not complete: ' + result.stdout + result.stderr)
            if args.service_discovery and 'PASS DNS cancelled callback isolation' not in result.stdout:
                raise RuntimeError('DNS cancellation/error isolation did not complete: ' + result.stdout + result.stderr)
            if args.commissioning and 'PASS bounded operational descriptor endpoint and cluster discovery over CASE' not in result.stdout:
                raise RuntimeError('Operational descriptor discovery did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and not args.icd and 'PASS production device scheduler catalog restore and radio detach refresh over CASE' not in result.stdout:
                raise RuntimeError('Device scheduler did not complete: ' + result.stdout + result.stderr)
            if args.icd and 'PASS production device manager online snapshot with ICD Management cluster' not in result.stdout:
                raise RuntimeError('Production manager did not retain the ICD accessory snapshot: ' + result.stdout + result.stderr)
            if args.service_owner and not args.icd and 'PASS bounded revisioned device snapshot pages from encrypted CASE descriptors' not in result.stdout:
                raise RuntimeError('Device snapshot pagination did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and not args.icd and 'PASS capability-gated OnOff transactions and confirmed readback over CASE' not in result.stdout:
                raise RuntimeError('Operational OnOff transaction did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and not args.icd and 'PASS production manager subscription and peer-correlated OnOff reports' not in result.stdout:
                raise RuntimeError('Production subscription/report path did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and not args.icd and 'PASS obsolete subscription rejected with INVALID_SUBSCRIPTION over CASE' not in result.stdout:
                raise RuntimeError('Obsolete subscription rejection did not complete: ' + result.stdout + result.stderr)
            if args.sensor_reports and 'PASS recovery-required catalog node reconciled after live CASE descriptor discovery' not in result.stdout:
                raise RuntimeError('Interrupted node CASE recovery did not complete: ' + result.stdout + result.stderr)
            if args.sensor_reports and 'PASS production manager sensor and contact reports reach IPC from subscribed CASE reports' not in result.stdout:
                raise RuntimeError('Production sensor report-to-IPC path did not complete: ' + result.stdout + result.stderr)
            if args.icd and 'PASS queued CASE read completed during the Check-In active window' not in result.stdout:
                raise RuntimeError('ICD Check-In wake-window dispatch did not complete: ' + result.stdout + result.stderr)
            if args.icd and 'PASS production ICD registration and authenticated Check-In counter acceptance' not in result.stdout:
                raise RuntimeError('ICD registration or authenticated Check-In was not accepted: ' + result.stdout + result.stderr)
            if radio.error:
                raise RuntimeError('Synthetic RCP failed: ' + radio.error)
            report = {'platform': 'development host', 'radio': 'synthetic RCP',
                      'linux_ipv6_or_tun': False, 'real_radio_access': False,
                      'datagram_payload_lengths': [] if args.commissioning else [0, 4, 256, 1232],
                      'encrypted_commissioning_tested': args.commissioning, 'production_owned_commissioner_tested': args.service_owner, 'synthetic_phone_gatt_tested': args.ble,
                      'production_dns_resolver_tested': args.service_discovery,
                      'production_sensor_reports_tested': args.sensor_reports,
                      'production_icd_checkin_and_wake_dispatch_tested': args.icd,
                      'native_gateway_case_reports_tested': args.native_bridge,
                      'native_gateway_sensor_values': native_status,
                      'signed_srp_registration_tested': args.service_discovery,
                      'operational_descriptor_discovery_tested': args.commissioning,
                      'fixture_address_injection': args.commissioning and not args.service_discovery,
                      'real_ble_access': False,
                      'multicast_membership_api_tested': not args.commissioning,
                      'uart_and_reset_guards_tested': True, 'output': result.stdout.splitlines()}
            path = ROOT / 'build/rs-matter/openthread-transport-validation.json'
            path.write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(report, indent=2))
    finally:
        if relay is not None:
            relay.terminate()
            try:
                relay.wait(timeout=3)
            except subprocess.TimeoutExpired:
                relay.kill()
                relay.wait()
        radio.close()


def run_native_bridge(command, working_directory, timeout):
    """Run the production native host bridge beside the live CASE accessory fixture."""
    native_root = ROOT / 'microhad'
    native_binary = native_root / 'build/host-ziggurat-matter/microhad'
    if not native_binary.is_file():
        raise RuntimeError('Build the MATTER host gateway first: ' + str(native_binary))
    sys.path.insert(0, str(native_root / 'scripts'))
    from test_ziggurat_native_peer import NativeTests
    from test_ziggurat_service import wait_for

    matter_socket = Path('/tmp/microhad-matter.sock')
    if matter_socket.exists() or matter_socket.is_symlink():
        raise RuntimeError('Native CASE report fixture requires an unused /tmp/microhad-matter.sock')

    class MatterNativeFixture(NativeTests):
        def launch(self):
            self.process = subprocess.Popen(
                [str(native_binary), '--config', str(self.config)],
                stdout=self.log,
                stderr=self.log,
            )
            wait_for(lambda: self.status()['online'])

    native = MatterNativeFixture()
    probe = None
    fixture_socket_bound = False
    try:
        native.setUp()
        ready = Path(working_directory) / 'native-ipc-ready'
        release = Path(working_directory) / 'native-ipc-release'
        environment = os.environ.copy()
        environment.update({
            'MICROHAD_MATTER_TEST_STATE_DIR': str(Path(working_directory) / 'state'),
            'MICROHAD_MATTER_TEST_IPC_SOCKET': str(matter_socket),
            'MICROHAD_MATTER_TEST_NATIVE_READY_FILE': str(ready),
            'MICROHAD_MATTER_TEST_NATIVE_RELEASE_FILE': str(release),
        })
        probe = subprocess.Popen(
            command + ['--native-ipc'], cwd=working_directory, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        deadline = time.monotonic() + timeout
        while not ready.exists():
            if probe.poll() is not None:
                output, _ = probe.communicate()
                raise RuntimeError('CASE fixture exited before native IPC readiness:\n' + output)
            if time.monotonic() >= deadline:
                raise TimeoutError('Native IPC CASE fixture did not reach its ready gate')
            time.sleep(0.05)
        fixture_socket_bound = matter_socket.exists()
        if not fixture_socket_bound:
            raise RuntimeError('CASE fixture announced readiness without binding its IPC socket')

        def matter_connected():
            try:
                return native.request('/api/matter/status').get('state') == 'running'
            except Exception:
                return False
        wait_for(matter_connected, timeout=10)

        snapshot = native.request(
            '/api/matter/devices/snapshot',
            json.dumps({'schema': 1, 'action': 'snapshot'}).encode(),
        )
        if not snapshot.get('ok') or not isinstance(snapshot.get('job'), int):
            raise RuntimeError('Native gateway rejected the Matter device snapshot: ' + json.dumps(snapshot))
        snapshot_job = snapshot['job']
        snapshot_result = wait_for(
            lambda: (job if (job := native.request('/api/matter/jobs/' + str(snapshot_job))).get('state') == 'complete' else None),
            timeout=15,
        )
        snapshot_page = snapshot_result.get('response', {}).get('result', {})
        while snapshot_page.get('next_cursor') is not None:
            snapshot_request = {
                'schema': 1,
                'action': 'snapshot',
                'cursor': snapshot_page['next_cursor'],
                'revision': snapshot_page['revision'],
            }
            accepted = native.request(
                '/api/matter/devices/snapshot', json.dumps(snapshot_request).encode())
            if not accepted.get('ok') or not isinstance(accepted.get('job'), int):
                raise RuntimeError('Native gateway rejected a Matter snapshot page: ' + json.dumps(accepted))
            snapshot_result = wait_for(
                lambda: (job if (job := native.request('/api/matter/jobs/' + str(accepted['job'])))
                         .get('state') == 'complete' else None),
                timeout=15,
            )
            snapshot_page = snapshot_result.get('response', {}).get('result', {})

        def endpoint_registered():
            try:
                status = native.status()
                return any(device.get('source') == 'matter' and
                           device.get('matter_node') == '0000000000000002' and
                           device.get('matter_endpoint') == 1 for device in status.get('devices', []))
            except Exception:
                return False
        try:
            wait_for(endpoint_registered, timeout=10)
        except TimeoutError as error:
            try:
                status = native.status()
            except Exception as status_error:
                status = {'error': str(status_error)}
            try:
                matter_status = native.request('/api/matter/status')
            except Exception as matter_error:
                matter_status = {'error': str(matter_error)}
            native.log.seek(0)
            log_tail = native.log.read().decode(errors='replace')[-4000:]
            raise RuntimeError(
                'Native gateway did not reconcile the CASE endpoint; status=' +
                json.dumps(status) + '; matter=' + json.dumps(matter_status) +
                '; snapshot=' + json.dumps(snapshot_result) +
                '; native log=' + log_tail
            ) from error
        release.write_text('continue')

        expected = {
            'temperature': (-5.25, '°C'),
            'humidity': (64.0, '%'),
            'air_quality': ('fair', ''),
            'co2': (800.0, 'ppm'),
            'pm25': (25.5, 'ug/m3'),
        }
        observed = {}
        def reports_applied():
            nonlocal observed
            for device in native.status().get('devices', []):
                if (device.get('source') == 'matter' and
                        device.get('matter_node') == '0000000000000002' and
                        device.get('matter_endpoint') == 1):
                    observed = device.get('sensor_values', {})
                    break
            return all(observed.get(key, {}).get('value') == value and
                       observed.get(key, {}).get('unit') == unit and
                       observed.get(key, {}).get('quality') == 'measured'
                       for key, (value, unit) in expected.items())
        wait_for(reports_applied, timeout=15)

        output, _ = probe.communicate(timeout=max(1, deadline - time.monotonic()))
        if probe.returncode != 0:
            raise RuntimeError('CASE report fixture failed:\n' + output)
        if 'PASS production manager sensor and contact reports reach IPC from subscribed CASE reports' not in output:
            raise RuntimeError('Production manager did not publish live CASE reports:\n' + output)
        return subprocess.CompletedProcess(command, 0, stdout=output, stderr=''), observed
    finally:
        if probe is not None and probe.poll() is None:
            probe.terminate()
            try:
                probe.wait(timeout=3)
            except subprocess.TimeoutExpired:
                probe.kill()
                probe.wait()
        try:
            native.tearDown()
        finally:
            if fixture_socket_bound and matter_socket.exists():
                matter_socket.unlink()


if __name__ == '__main__':
    main()
