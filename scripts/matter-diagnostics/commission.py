#!/usr/bin/env python3
"""Capture and commission one Matter-over-Thread accessory through host OTBR."""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import subprocess
import sys
import time

from spinel_capture import inspect as inspect_spinel

ROOT = Path(__file__).resolve().parents[2]


import sys as _workspace_sys
_workspace_sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, gateway_host


def digest(path: Path) -> str:
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


class Attempt:
    def __init__(self, args):
        self.args = args
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
        self.directory = args.output_root.resolve() / stamp
        self.directory.mkdir(parents=True, mode=0o700)
        for name in ('logs', 'capture', 'keys', 'state', 'metadata'):
            (self.directory / name).mkdir(mode=0o700)
        self.collectors = []
        self.foreground = None
        self.last_sample = 0.0
        self.last_sudo_refresh = 0.0
        self.warned_collectors = set()
        self.sudo = [] if os.geteuid() == 0 else ['sudo', '-n', '--']
        self.result = {'started': stamp, 'commission_exit': None, 'read_exit': None,
                       'errors': [], 'warnings': [], 'collectors': {}, 'success': False}

    def save(self, relative, data):
        path = self.directory / relative
        path.write_text(data if isinstance(data, str) else json.dumps(data, indent=2) + '\n')

    def progress(self, message):
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%H:%M:%SZ')
        line = f'[{stamp}] {message}'
        print(line, flush=True)
        with (self.directory / 'logs/runner.log').open('a') as output:
            output.write(line + '\n')

    @staticmethod
    def echo_chip(reader):
        while chunk := reader.read(65536):
            if hasattr(sys.stdout, 'buffer'):
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            else:
                sys.stdout.write(chunk.decode('utf-8', errors='replace'))
                sys.stdout.flush()

    def command(self, command, *, privileged=False, timeout=20, check=True):
        result = subprocess.run((self.sudo if privileged else []) + list(map(str, command)),
                                capture_output=True, text=True, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f'{command[0]} failed ({result.returncode}): {result.stderr.strip()}')
        return result

    def docker(self, *args, **kwargs):
        return self.command(['docker', *args], privileged=True, **kwargs)

    def cli(self, *args):
        text = self.docker('exec', self.args.container, '/opt/ot-ctl', '-I', self.args.thread_interface,
                           *args).stdout.replace('\r', '').strip()
        if not text.endswith('Done'):
            raise RuntimeError(f'OTBR CLI {args}: {text}')
        return text[:-4].strip()

    def snapshot(self, stage):
        snapshot = {}
        for label, command in {'state': ['state'], 'channel': ['channel'], 'ipaddr': ['ipaddr'],
                               'keysequence': ['keysequence', 'counter'], 'srp': ['srp', 'server', 'state'],
                               'br': ['br', 'state'], 'mdns': ['mdns', 'state'],
                               'mac_counters': ['counters', 'mac'], 'children': ['child', 'table']}.items():
            snapshot[label] = self.cli(*command)
        dataset = self.cli('dataset', 'active', '-x')
        bytes.fromhex(dataset)
        self.save(f'keys/thread-dataset-{stage}.hex', dataset + '\n')
        self.save(f'metadata/otbr-{stage}.json', snapshot)
        return dataset, snapshot

    def preflight(self):
        for name in ('sudo', 'docker', 'tcpdump', 'btmon', 'ip', 'bluetoothctl'):
            if name == 'sudo' and not self.sudo:
                continue
            if shutil.which(name) is None:
                raise RuntimeError(f'Missing required command: {name}')
        if self.sudo:
            self.progress('Authenticating sudo for capture tools and Docker')
            subprocess.run(['sudo', '-v'], check=True)
        self.progress('Checking controller build, key-export rehearsal, Bluetooth and OTBR')
        from cryptography.hazmat.primitives.ciphers.aead import AESCCM  # noqa: F401
        controller = self.args.controller.resolve()
        manifest_path = controller.parent / 'controller-build.json'
        manifest = json.loads(manifest_path.read_text())
        if digest(controller) != manifest['binary_sha256'] or manifest.get('security_test_mode') is not False:
            raise RuntimeError('Controller build manifest does not match the diagnostic binary')
        rehearsal = json.loads((controller.parent / 'rehearsal.json').read_text())
        if rehearsal.get('binary_sha256') != manifest['binary_sha256'] or not rehearsal.get('passed'):
            raise RuntimeError('Run the controller fixture rehearsal before commissioning')
        self.save('metadata/controller-build.json', manifest)
        self.save('metadata/rehearsal.json', rehearsal)
        if not self.args.paa_trust_store.is_dir() or not any(self.args.paa_trust_store.glob('*.der')):
            raise RuntimeError('Missing production PAA trust store; provide --paa-trust-store')
        trust = self.directory / 'state/paa-trust-store'
        trust.mkdir(mode=0o700)
        for certificate in self.args.paa_trust_store.glob('*.der'):
            shutil.copyfile(certificate, trust / certificate.name)
        self.save('metadata/invocation.json', {k: str(v) if isinstance(v, Path) else v
                                               for k, v in vars(self.args).items()})
        self.save('metadata/container.json', json.loads(self.docker('inspect', self.args.container).stdout))
        self.save('metadata/host-addresses.json', json.loads(self.command(['ip', '-j', '-6', 'addr']).stdout))
        self.save('metadata/host-routes.json', json.loads(self.command(['ip', '-j', '-6', 'route']).stdout))
        self.save('metadata/ipv6-forwarding.txt', Path('/proc/sys/net/ipv6/conf/all/forwarding').read_text())
        for interface in (self.args.backbone, self.args.thread_interface):
            self.command(['ip', 'link', 'show', 'dev', interface])
        bluetooth = self.command(['bluetoothctl', 'show']).stdout
        self.save('metadata/bluetooth.txt', bluetooth)
        if 'Powered: yes' not in bluetooth:
            raise RuntimeError('Bluetooth adapter is not powered')
        if self.cli('log', 'level') != '5':
            raise RuntimeError('OTBR must already be running the full DEBUG build at log level 5')
        configuration = json.loads(self.docker('inspect', self.args.container).stdout)[0]
        if (configuration['Config'].get('Labels') or {}).get('org.microhad.otbr.full-debug') != '1':
            mounts = configuration['Mounts']
            agent = next(Path(m['Source']) for m in mounts if m['Destination'] == '/opt/otbr-agent')
            cache = (agent.parents[2] / 'CMakeCache.txt').read_text()
            if 'OT_FULL_LOGS:BOOL=ON' not in cache or 'OT_LOG_LEVEL:STRING=DEBG' not in cache:
                raise RuntimeError('OTBR binary is not from the full DEBUG configuration')
            self.save('metadata/otbr-agent-sha256.txt', digest(agent) + '\n')
        self.dataset, status = self.snapshot('before')
        if status['state'] not in ('leader', 'router', 'child'):
            raise RuntimeError('OTBR must be attached before starting the attempt')
        expected = self.args.dataset_file
        if expected and expected.read_text().strip().lower() != self.dataset.lower():
            raise RuntimeError('OTBR active dataset differs from --dataset-file')
        self.progress(f'Preflight passed: OTBR {status["state"]}, channel {status["channel"]}, Bluetooth powered')

    def collector(self, name, command, *, privileged=False, stdout_path=None, required=True):
        self.progress(f'Starting {name} collector; log: logs/{name}.log')
        log = self.directory / f'logs/{name}.log'
        pid_path = self.directory / f'metadata/{name}.pid'
        pid_path.touch(mode=0o600)
        # Pipe closure stops privileged collectors without needing another sudo ticket.
        launcher = ['python3', str(Path(__file__).with_name('collector.py')),
                    str(pid_path), *map(str, command)]
        with log.open('wb') as errors:
            output = stdout_path.open('wb') if stdout_path else None
            try:
                # Keep the controlling terminal for sudo's per-terminal cache.
                # collector.py isolates the actual collector, not this launcher.
                process = subprocess.Popen((self.sudo if privileged else []) + launcher,
                                           stdin=subprocess.PIPE, stdout=output or errors, stderr=errors)
            finally:
                if output:
                    output.close()
        self.collectors.append((name, process, pid_path, privileged, required))

    def ensure_collectors(self):
        if self.sudo and time.monotonic() - self.last_sudo_refresh >= 30:
            self.command(['sudo', '-n', '-v'])
            self.last_sudo_refresh = time.monotonic()
        for name, process, _, _, required in self.collectors:
            if process.poll() is not None:
                if required:
                    raise RuntimeError(f'{name} collector exited early; see logs/{name}.log')
                if name not in self.warned_collectors:
                    self.warned_collectors.add(name)
                    self.progress(f'Warning: {name} logging stopped (exit {process.returncode}); see logs/{name}.log')
        if time.monotonic() - self.last_sample >= 5:
            self.last_sample = time.monotonic()
            try:
                sequence = self.cli('keysequence', 'counter')
                dataset = self.cli('dataset', 'active', '-x')
                with (self.directory / 'keys/thread-key-sequences.jsonl').open('a') as output:
                    output.write(json.dumps({'unix_ns': time.time_ns(), 'sequence': sequence,
                                             'dataset': dataset}) + '\n')
            except (RuntimeError, subprocess.SubprocessError):
                pass # A bounded gap is expected while the agent is restarting.

    def start_collectors(self):
        pcap = self.directory / 'capture/host.pcap'
        ble = self.directory / 'capture/ble.btsnoop'
        pcap.touch(mode=0o600)
        ble.touch(mode=0o600)
        self.collector('tcpdump', ['tcpdump', '-i', 'any', '-p', '-s', '0', '-U', '-Z',
                                  pwd.getpwuid(os.getuid()).pw_name, '-w', str(pcap)], privileged=True)
        self.collector('btmon', ['btmon', '-w', str(ble)], privileged=True)
        self.collector('gateway', self.gateway_command(), required=False)
        deadline = time.monotonic() + 10
        while pcap.stat().st_size < 24 or ble.stat().st_size < 16:
            self.ensure_collectors()
            if time.monotonic() > deadline:
                raise RuntimeError('Collectors did not create their capture headers')
            time.sleep(0.1)
        self.ensure_collectors()
        self.progress('Packet and Bluetooth capture are recording')

    def gateway_command(self):
        return [sys.executable,
                str(ROOT / 'micro-thread-matter-daemon/scripts/capture-thread-logs.py'),
                '--host', self.args.gateway]

    def start_otbr_log(self):
        since = datetime.datetime.strptime(self.result['started'], '%Y%m%dT%H%M%S.%fZ').replace(
            tzinfo=datetime.timezone.utc).isoformat()
        # Starting after restart avoids Docker's log follower exiting on stop;
        # --since retrieves startup messages already emitted during that restart.
        self.collector('otbr', ['docker', 'logs', '--timestamps', '--follow', '--since', since,
                               self.args.container], privileged=True)

    def wait_ready(self):
        self.progress('Waiting for Thread attachment, SRP, border routing and complete Spinel capture (up to 90s)')
        deadline = time.monotonic() + 90
        last_error = 'not ready'
        last_report = time.monotonic()
        while time.monotonic() < deadline:
            self.ensure_collectors()
            try:
                role = self.cli('state')
                border = self.cli('br', 'state')
                srp = self.cli('srp', 'server', 'state')
                last_error = f'Thread={role}, border routing={border}, SRP={srp}'
                if role in ('leader', 'router', 'child') and border == 'running' and srp == 'running':
                    stream = inspect_spinel(self.directory / 'capture/host.pcap', self.args.gateway, self.args.port)
                    self.save('metadata/spinel-before-commission.json', stream)
                    if stream['complete']:
                        self.progress(f'OTBR ready; validated {stream["frames"]} Spinel frames and both TCP handshakes')
                        return
                    last_error = 'Spinel capture not yet complete'
            except (RuntimeError, ValueError, subprocess.SubprocessError) as error:
                last_error = str(error)
            if time.monotonic() - last_report >= 10:
                self.progress('Still waiting for OTBR/capture readiness: ' + last_error)
                last_report = time.monotonic()
            time.sleep(1)
        raise RuntimeError('OTBR/capture readiness failed: ' + last_error)

    def chip(self, name, arguments, timeout):
        self.progress(f'Starting CHIP Tool {name} (timeout {timeout:g}s); streaming output and saving logs/{name}.log')
        build = json.loads((self.directory / 'metadata/controller-build.json').read_text())
        env = {**os.environ, 'MATTER_DIAGNOSTIC_DIR': str(self.directory),
               'MATTER_DIAGNOSTIC_BUILD': build['binary_sha256']}
        command = [str(self.args.controller.resolve()), *arguments, '--storage-directory',
                   str(self.directory / 'state/chip-tool'), '--trace_file',
                   str(self.directory / f'logs/{name}-trace.log'), '--trace_decode', '1',
                   '--trace-to', f'json:{self.directory}/logs/{name}-events.json',
                   '--paa-trust-store-path', str(self.directory / 'state/paa-trust-store')]
        (self.directory / 'state/chip-tool').mkdir(exist_ok=True, mode=0o700)
        self.save(f'metadata/{name}-command.json', command)
        log_path = self.directory / f'logs/{name}.log'
        with log_path.open('wb') as output, log_path.open('rb') as live:
            self.foreground = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                               env=env, start_new_session=True)
            deadline = time.monotonic() + timeout
            while self.foreground.poll() is None:
                self.echo_chip(live)
                self.ensure_collectors()
                if time.monotonic() > deadline:
                    self.progress(f'CHIP Tool {name} timed out; stopping it')
                    self.stop_foreground()
                    self.echo_chip(live)
                    return 124
                time.sleep(0.2)
            code = self.foreground.returncode
            self.echo_chip(live)
            self.foreground = None
            self.progress(f'CHIP Tool {name} exited with status {code}')
            return code

    def stop_foreground(self):
        if self.foreground and self.foreground.poll() is None:
            os.killpg(self.foreground.pid, signal.SIGTERM)
            try:
                self.foreground.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(self.foreground.pid, signal.SIGKILL)
                self.foreground.wait()
        self.foreground = None

    def finish(self):
        self.progress('Saving final OTBR state and stopping capture/log collectors')
        self.stop_foreground()
        try:
            dataset, _ = self.snapshot('after')
            self.result['dataset_unchanged'] = dataset == getattr(self, 'dataset', dataset)
        except Exception as error:
            self.result['warnings'].append('Final snapshot: ' + str(error))
        for name, process, pid_path, privileged, required in reversed(self.collectors):
            early_exit = process.poll() is not None
            try:
                if not early_exit:
                    process.stdin.close()
                    process.wait(timeout=16)
                elif process.stdin:
                    process.stdin.close()
                self.result['collectors'][name] = {'exit': process.returncode, 'exited_early': early_exit}
                self.progress(f'{name} collector stopped (exit {process.returncode})')
                if early_exit and required:
                    self.result['errors'].append(name + ' exited before cleanup')
                elif early_exit:
                    self.result['warnings'].append(name + ' unavailable; see its collector log')
            except Exception as error:
                self.result['errors'].append(f'Could not stop {name}: {error}')
        if self.collectors:
            try:
                stream = inspect_spinel(self.directory / 'capture/host.pcap', self.args.gateway, self.args.port)
                self.save('metadata/spinel-final.json', stream)
                if not stream['complete']:
                    self.result['errors'].append('Final Spinel capture is incomplete')
                stats = (self.directory / 'logs/tcpdump.log').read_text()
                drops = re.search(r'(\d+) packets dropped by kernel', stats)
                if not drops or int(drops[1]):
                    self.result['errors'].append('Packet capture drops or missing final drop statistics')
            except Exception as error:
                self.result['errors'].append('Spinel validation: ' + str(error))
        if (self.directory / 'keys/matter-session-keys.jsonl').exists():
            try:
                self.progress('Verifying exported session keys by authenticating saved Matter messages')
                spec = importlib.util.spec_from_file_location('verify_export', Path(__file__).with_name('verify-export.py'))
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                verification = module.verify(self.directory)
                self.result['keys_verified'] = verification['complete']
                if not verification['complete']:
                    self.result['errors'].append('Incomplete key/decryption verification; see metadata')
            except Exception as error:
                self.result['errors'].append('Key verification: ' + str(error))
        else:
            self.result['keys_verified'] = False
            self.progress('No Matter session keys were generated in this attempt')
        self.result['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        self.result['success'] = (self.result['commission_exit'] == 0 and self.result['read_exit'] == 0 and
                                  self.result.get('keys_verified', False) and not self.result['errors'])
        self.save('result.json', self.result)
        self.progress('Writing checksums and result: ' + ('SUCCESS' if self.result['success'] else 'FAILED'))
        hashes = {str(p.relative_to(self.directory)): digest(p) for p in sorted(self.directory.rglob('*'))
                  if p.is_file() and p.name != 'sha256.json'}
        self.save('metadata/sha256.json', hashes)
        print(f'Trace bundle: {self.directory}', flush=True)

    def run(self):
        try:
            self.progress(f'Trace bundle: {self.directory}')
            self.preflight()
            self.start_collectors()
            self.progress(f'Restarting {self.args.container} with its existing Thread settings')
            self.docker('restart', '--time', '15', self.args.container, timeout=30)
            self.start_otbr_log()
            # The no-reset diagnostic agent intentionally has auto-attach disabled.
            deadline = time.monotonic() + 10
            while True:
                try:
                    self.cli('ifconfig', 'up')
                    self.cli('thread', 'start')
                    break
                except RuntimeError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.2)
            self.wait_ready()
            dataset, _ = self.snapshot('ready')
            if dataset != self.dataset:
                raise RuntimeError('Dataset changed during OTBR restart')
            self.progress('Dataset verified unchanged; starting accessory commissioning')
            self.result['commission_exit'] = self.chip('commission', ['pairing', 'code-thread',
                str(self.args.node_id), 'hex:' + self.dataset, self.args.payload], self.args.timeout)
            if self.result['commission_exit'] == 0:
                self.progress('Commissioning succeeded; reading Basic Information over CASE')
                self.result['read_exit'] = self.chip('read', ['basicinformation', 'read', 'vendor-name',
                    str(self.args.node_id), '0'], 60)
            else:
                self.progress(f'Commissioning failed (exit {self.result["commission_exit"]}); skipping CASE read')
            self.progress(f'Collecting {self.args.tail_seconds:g}s of trailing traffic')
            deadline = time.monotonic() + self.args.tail_seconds
            while time.monotonic() < deadline:
                self.ensure_collectors()
                time.sleep(min(0.2, max(0, deadline - time.monotonic())))
        except KeyboardInterrupt:
            self.result['errors'].append('Interrupted')
            self.progress('Interrupted; cleaning up collectors')
        except Exception as error:
            self.result['errors'].append(str(error))
            self.progress('Error: ' + str(error))
        finally:
            # A second signal must not interrupt reaping collectors and writing results.
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            self.finish()
        return 0 if self.result['success'] else (self.result['commission_exit'] or 1)


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--payload', required=True, help='Accessory MT: QR payload (never stored in Git)')
    result.add_argument('--output-root', type=Path, default=Path('./matter-traces/host-otbr-via-ziggurat'))
    result.add_argument('--controller', type=Path, default=ROOT / 'build/matter-diagnostics/chip-tool')
    result.add_argument('--container', default='microhad-otbr-attach')
    result.add_argument('--gateway', default=setting('GATEWAY_IP'), type=gateway_host)
    result.add_argument('--port', type=int, default=9998)
    result.add_argument('--backbone', default=setting('LAN_INTERFACE'))
    result.add_argument('--thread-interface', default='wpan-otbr-test')
    result.add_argument('--node-id', type=int, default=1)
    result.add_argument('--dataset-file', type=Path)
    result.add_argument('--paa-trust-store', type=Path,
                        default=ROOT / 'build/connectedhomeip/source/credentials/production/paa-root-certs')
    result.add_argument('--timeout', type=float, default=300)
    result.add_argument('--tail-seconds', type=float, default=10)
    return result


if __name__ == '__main__':
    os.umask(0o077)
    args = parser().parse_args()
    if args.timeout <= 0 or args.tail_seconds < 0 or not 0 < args.port <= 65535 or not 0 < args.node_id < 2**64:
        raise SystemExit('Invalid timeout, tail duration, port or node ID')
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    raise SystemExit(Attempt(args).run())
