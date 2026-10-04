#!/usr/bin/env python3
"""Exercise the real OpenThread/Rust transport against an explicitly synthetic RCP."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
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
    args = parser.parse_args()
    if args.service_discovery and not args.service_owner:
        parser.error('--service-discovery requires --service-owner')
    if args.service_owner and not args.ble:
        parser.error('--service-owner requires --ble')
    if args.ble and not args.commissioning:
        parser.error('--ble requires --commissioning')
    timeout = 100 if args.commissioning else 22
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
            result = subprocess.run([str(binary), 'spinel+hdlc+unix://' + str(socket) + '?no-reset'] + (['--ble'] if args.ble else []) + (['--service-owner'] if args.service_owner else []) + (['--service-discovery'] if args.service_discovery else []),
                                    cwd=directory, capture_output=True, text=True, timeout=timeout)
            if args.service_discovery and 'PASS synthetic accessory signed SRP registration' not in result.stdout:
                raise RuntimeError('SRP/DNS fixture registration did not complete: ' + result.stdout + result.stderr)
            if args.service_discovery and 'PASS DNS cancelled callback isolation' not in result.stdout:
                raise RuntimeError('DNS cancellation/error isolation did not complete: ' + result.stdout + result.stderr)
            if args.commissioning and 'PASS bounded operational descriptor endpoint and cluster discovery over CASE' not in result.stdout:
                raise RuntimeError('Operational descriptor discovery did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and 'PASS production device scheduler catalog restore and radio detach refresh over CASE' not in result.stdout:
                raise RuntimeError('Device scheduler did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and 'PASS bounded revisioned device snapshot pages from encrypted CASE descriptors' not in result.stdout:
                raise RuntimeError('Device snapshot pagination did not complete: ' + result.stdout + result.stderr)
            if args.service_owner and 'PASS capability-gated OnOff transactions and confirmed readback over CASE' not in result.stdout:
                raise RuntimeError('Operational OnOff transaction did not complete: ' + result.stdout + result.stderr)
            if result.returncode or marker not in result.stdout:
                raise RuntimeError('Transport failed: ' + result.stdout + result.stderr + '\nRCP error: ' + str(radio.error))
            if radio.error:
                raise RuntimeError('Synthetic RCP failed: ' + radio.error)
            report = {'platform': 'development host', 'radio': 'synthetic RCP',
                      'linux_ipv6_or_tun': False, 'real_radio_access': False,
                      'datagram_payload_lengths': [] if args.commissioning else [0, 4, 256, 1232],
                      'encrypted_commissioning_tested': args.commissioning, 'production_owned_commissioner_tested': args.service_owner, 'synthetic_phone_gatt_tested': args.ble,
                      'production_dns_resolver_tested': args.service_discovery,
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


if __name__ == '__main__':
    main()
