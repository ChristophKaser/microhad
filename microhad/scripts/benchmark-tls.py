#!/usr/bin/env python3
"""Synthetic TLS CPU/resumption benchmark. Device access requires --device HOST.

Uses only unique /tmp files on the gateway; never stops or replaces services.
Browser tests require Playwright (optionally PLAYWRIGHT_MODULE and
PLAYWRIGHT_CHROMIUM environment variables). Results include raw server samples.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import queue
import shlex
import socket
import ssl
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings
from workspace_config import gateway_host, ssh_command

CASES = [('ecdsa', 'ECDHE-ECDSA-AES128-GCM-SHA256', 0xC02B),
         ('ecdsa', 'ECDHE-ECDSA-CHACHA20-POLY1305', 0xCCA9),
         ('rsa', 'ECDHE-RSA-AES128-GCM-SHA256', 0xC02F),
         ('rsa', 'ECDHE-RSA-CHACHA20-POLY1305', 0xCCA8)]
SNAPSHOT = """for p in /proc/[0-9]*; do
read comm < "$p/comm" 2>/dev/null || continue
case "$comm" in microhad|gateway-main|ziggurat|mthread-matter)
awk '{print $1, $2, $22}' "$p/stat";; esac
done
awk '$4 == "0A" && ($2 ~ /:0050$/ || $2 ~ /:01BB$/) {print $2, $4, $10}' /proc/net/tcp
"""


def run(command, **kwargs):
    return subprocess.run([str(v) for v in command], check=True, **kwargs)


def summary(samples):
    result = {}
    for phase in sorted({s['phase'] for s in samples}):
        group = [s for s in samples if s['phase'] == phase and s['event'] == 'handshake']
        if not group:
            continue
        good = [s for s in group if s['ok']]
        result[phase] = {'attempts': len(group), 'successes': len(good),
                         'cache_hits': sum(s['cache_hit'] for s in good),
                         'failed_cpu_us': sum(s['cpu_us'] for s in group if not s['ok'])}
        for key in ('cpu_us', 'wall_us'):
            values = sorted(s[key] for s in good)
            if values:
                result[phase][key] = {'median': statistics.median(values), 'mean': statistics.mean(values),
                                     'p95': values[math.ceil(len(values)*.95)-1]}
    records = [s for s in samples if s['event'] == 'records' and
               s['phase'].startswith('records') and s['ok']]
    for block in (1024, 16384):
        group = [s for s in records if s['block'] == block]
        if group:
            result['records_' + str(block)] = {
                'samples': len(group),
                'cpu_us_per_kib': statistics.median(s['cpu_us']/(s['bytes']/1024) for s in group),
                'wall_mib_per_s': statistics.median(s['bytes']/1048576/(max(1,s['wall_us'])/1e6) for s in group)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=gateway_host)
    parser.add_argument('--iterations', type=int, default=20)
    parser.add_argument('--record-iterations', type=int, default=3)
    parser.add_argument('--port', type=int, default=18443)
    parser.add_argument('--skip-browser', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100 or not 1 <= args.record_iterations <= 10 or not 1024 <= args.port <= 65535:
        parser.error('iterations must be 1..100; port must be 1024..65535')
    config = settings()
    mode = 'mips' if args.device else 'host'
    build = ROOT / 'microhad/build' / ((str(config['TARGET_BUILD_SUBDIR']) if args.device else '') + mode + '-ziggurat-matter')
    run(['make', '-C', ROOT/'microhad', 'mbedtls-' + mode], stdout=sys.stderr)
    compiler = str(config['TARGET_CC'] if args.device else config['HOST_CC'])
    remote = '/tmp/microhad-tls-' + uuid.uuid4().hex
    ssh = ssh_command(args.device) if args.device else None
    report = {'synthetic': True, 'mode': mode, 'compiler': compiler,
              'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'mbedtls_revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT/'microhad/dependencies/mbedtls', text=True).strip(),
              'cases': []}
    def save():
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    def snapshot():
        return subprocess.check_output(ssh + [SNAPSHOT], text=True, timeout=20) if ssh else None
    with tempfile.TemporaryDirectory(prefix='microhad-tls-') as tmp:
        directory = Path(tmp)
        binary = directory/'benchmark'
        includes = ['include', 'tf-psa-crypto/include', 'tf-psa-crypto/drivers/builtin/include']
        command = [compiler, '-std=gnu99', '-O2', '-D_DEFAULT_SOURCE',
                   '-DMBEDTLS_USER_CONFIG_FILE="' + str(ROOT/'microhad/config/mbedtls_user_config.h') + '"',
                   '-DTF_PSA_CRYPTO_CONFIG_FILE="' + str(ROOT/'microhad/config/psa_crypto_config.h') + '"']
        command += ['-I' + str(ROOT/'microhad/dependencies/mbedtls'/p) for p in includes]
        command += [ROOT/'microhad/test/benchmark_tls.c', ROOT/'microhad/src/crypto_init.c']
        command += [build/'mbedtls-v4/library'/p for p in ('libmbedtls.a', 'libmbedx509.a', 'libtfpsacrypto.a')]
        command += ['-lpthread', '-lrt', '-o', binary] + (['-static'] if args.device else [])
        run(command)
        report['build_command'] = [str(v) for v in command]
        report['profile_sha256'] = {name: hashlib.sha256((ROOT/'microhad/config'/name).read_bytes()).hexdigest()
                                    for name in ('mbedtls_user_config.h', 'psa_crypto_config.h')}
        report['binary_sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        report['compiler_version'] = subprocess.check_output([compiler, '--version'], text=True).splitlines()[0]
        for key in ('ecdsa', 'rsa'):
            run(['openssl', 'req', '-x509', '-newkey', 'ec' if key == 'ecdsa' else 'rsa:2048',
                 *(['-pkeyopt', 'ec_paramgen_curve:P-256'] if key == 'ecdsa' else []),
                 '-nodes', '-subj', '/CN=localhost', '-days', '1',
                 '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1',
                 '-keyout', directory/(key+'.key'), '-out', directory/(key+'.crt')],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        report['services_before'] = snapshot()
        uploaded = False
        try:
            if ssh:
                run(ssh + ['umask 077; mkdir ' + remote], timeout=20)
                uploaded = True
                for path in directory.iterdir():
                    run(ssh + ['cat > ' + remote + '/' + path.name], input=path.read_bytes(), timeout=30)
                run(ssh + ['chmod 700 ' + remote + '/benchmark'], timeout=20)
            for key, cipher, suite in CASES:
                print('Benchmarking ' + cipher, file=sys.stderr, flush=True)
                case = {'cipher': cipher, 'certificate': key, 'samples': []}
                report['cases'].append(case)
                base = remote if ssh else str(directory)
                server_args = [base+'/benchmark', base+'/'+key+'.crt', base+'/'+key+'.key', str(suite), str(args.port)]
                if ssh:
                    cmd = ' '.join(map(shlex.quote, server_args))
                    command = ssh + ['ulimit -c 0; ulimit -t 180; ulimit -v 24576; ' + cmd +
                                     ' & child=$!; trap \'kill "$child" 2>/dev/null; wait "$child"\' EXIT; '
                                     'trap \'exit 1\' HUP INT TERM; read done']
                else:
                    command = server_args
                events = queue.Queue()
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                def reader():
                    for line in process.stdout:
                        try:
                            events.put(json.loads(line))
                        except ValueError:
                            events.put({'event': 'invalid_output', 'line': line})
                    events.put({'event': 'eof'})
                thread = threading.Thread(target=reader, daemon=True)
                thread.start()
                def collect(phase, until='records'):
                    while True:
                        event = events.get(timeout=30)
                        event['phase'] = phase
                        case['samples'].append(event)
                        if event['event'] in ('eof', 'invalid_output'):
                            raise RuntimeError('dummy server failed: ' + repr(event) + ' exit=' + str(process.poll()))
                        if event['event'] == until:
                            return event
                host = args.device or '127.0.0.1'
                def context():
                    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                    ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
                    ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
                    ctx.set_ciphers(cipher)
                    return ctx
                def request(ctx, phase, session=None, path='/'):
                    with socket.create_connection((host, args.port), timeout=10) as raw:
                        with ctx.wrap_socket(raw, server_hostname='localhost', session=session) as client:
                            reused = client.session_reused
                            negotiated = client.cipher()
                            saved = client.session
                            client.sendall(('GET '+path+' HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n').encode())
                            data = bytearray()
                            while True:
                                part = client.recv(32768)
                                if not part: break
                                data.extend(part)
                    record = collect(phase)
                    hand = next(s for s in reversed(case['samples']) if s['event'] == 'handshake')
                    hand.update(client_reused=reused, client_negotiated=negotiated)
                    if reused != hand['cache_hit'] or not hand['ok'] or not record['ok']:
                        raise RuntimeError('handshake/resumption validation failed')
                    header, body = data.split(b'\r\n\r\n', 1)
                    expected = int(next(line.split(b':', 1)[1] for line in header.split(b'\r\n') if line.startswith(b'Content-Length:')))
                    if len(body) != expected: raise RuntimeError('truncated response')
                    return saved, reused
                try:
                    collect('setup', 'ready')
                    ctx = context()
                    for _ in range(2): request(ctx, 'warmup')
                    for _ in range(args.iterations):
                        _, reused = request(ctx, 'full')
                        if reused: raise RuntimeError('full control unexpectedly resumed')
                    session, _ = request(ctx, 'seed')
                    for _ in range(args.iterations):
                        session, reused = request(ctx, 'resumed', session)
                        if not reused: raise RuntimeError('requested resumption fell back to full')
                    for block in (1024, 16384):
                        for _ in range(args.record_iterations): request(ctx, 'records_'+str(block), session, '/records/'+str(block))
                    if not args.skip_browser:
                        browser = run(['node', ROOT/'microhad/scripts/benchmark-tls-browser.cjs',
                                       f'https://{host}:{args.port}/', '10'], capture_output=True, text=True, timeout=120)
                        case['browser'] = json.loads(browser.stdout)
                        while True:
                            try: event = events.get(timeout=1)
                            except queue.Empty: break
                            event['phase'] = 'browser'; case['samples'].append(event)
                    case['summary'] = summary(case['samples'])
                except ssl.SSLError as error:
                    if any(s.get('phase') == 'full' for s in case['samples']):
                        case['error'] = str(error)
                        raise
                    case['unsupported'] = str(error)
                except Exception as error:
                    case['error'] = str(error)
                    if isinstance(error, subprocess.CalledProcessError):
                        case['stderr'] = error.stderr
                    raise
                finally:
                    if ssh:
                        process.stdin.write('done\n'); process.stdin.flush(); process.stdin.close()
                        process.wait(timeout=15)
                    else:
                        process.terminate(); process.wait(timeout=10)
                    thread.join(timeout=5)
                    save()
                time.sleep(1)
        finally:
            try:
                if uploaded:
                    # Stock BusyBox has rm but does not provide rmdir.
                    run(ssh + ['rm -r ' + remote], timeout=20)
            finally:
                report['services_after'] = snapshot()
                report['services_unchanged'] = report['services_before'] == report['services_after']
                save()
    print(json.dumps({c['cipher']: c.get('summary', c.get('error')) for c in report['cases']}, indent=2))


if __name__ == '__main__':
    main()
