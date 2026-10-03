#!/usr/bin/env python3
"""Bounded TH01 reporting diagnostics; no UART access, firmware or network reset.

observe/trace are read-only. readback sends Read Reporting Configuration only.
alert uses the gateway's existing generic test push and opens its join window.
Keep output private: it contains device identifiers and application payloads.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import urllib.request

from network_backup import ssh_args
from ziggurat_cobs import Client


def http(args, path, post=False):
    request = urllib.request.Request('http://' + args.host + path,
                                     data=b'' if post else None)
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.load(response)


def aps(frame):
    if len(frame) < 20 or frame[0] != 0x10:
        return None
    value = frame[3:]
    length = int.from_bytes(value[15:17], 'little')
    if len(value) != 17 + length:
        return None
    data = value[17:]
    offset = 5 if data and data[0] & 4 else 3
    return {'timestamp': int(time.time()), 'source': int.from_bytes(value[:2], 'little'),
            'profile': int.from_bytes(value[7:9], 'little'),
            'cluster': int.from_bytes(value[9:11], 'little'),
            'source_endpoint': value[11], 'data': data.hex(),
            'command': data[offset - 1] if len(data) >= offset else None,
            'transaction': data[offset - 2] if len(data) >= offset else None}


class Bridge:
    def __init__(self, args):
        self.process = subprocess.Popen(ssh_args(args) + [args.bridge +
            ' cobs-bridge /tmp/ziggurat/native.sock'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            self.client = Client(self.process.stdout, self.process.stdin)
        except Exception:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            raise

    def close(self):
        self.client.close()
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


def observe(args, emit):
    start = args.since if args.since is not None else int(time.time())
    end = start + args.duration
    seen = set()
    reports = samples = offline = 0
    while True:
        now = int(time.time())
        try:
            status = http(args, '/api/status')
            logs = http(args, '/api/logs')['lines']
            device = next(d for d in status['devices'] if d['ieee_address'] == args.ieee)
            healthy = bool(status.get('online') and status.get('network_formed'))
            offline += int(not healthy)
            for line in logs:
                if line in seen:
                    continue
                seen.add(line)
                timestamp = int(line.split(' ', 1)[0])
                if timestamp < start or args.ieee not in line:
                    continue
                emit({'kind': 'log', 'line': line})
                if 'command=0xa(Report Attributes)' in line:
                    reports += 1
                    print(line, flush=True)
            emit({'kind': 'sample', 'timestamp': now, 'healthy': healthy,
                  'last_seen': device['last_seen'],
                  'temperature_celsius': device['temperature_celsius'],
                  'humidity_percent': device['humidity_percent'], 'reports': reports})
        except (OSError, ValueError, KeyError, StopIteration):
            offline += 1
            emit({'kind': 'sample_error', 'timestamp': now})
        samples += 1
        if now >= end:
            break
        time.sleep(min(20, end - now))
    result = {'kind': 'summary', 'start': start, 'end': now,
              'reports': reports, 'samples': samples, 'offline_samples': offline}
    emit(result)
    print(json.dumps(result), flush=True)


def wire_ieee(args):
    return bytes.fromhex(args.ieee[2:])[::-1]


def scan_node(client, args):
    before = len(client.events)
    reply = client.request(0x23)
    rows = [frame[3:] for frame in client.events[before:]
            if frame[0] == 0x23 and frame[1] & 3 == 2]
    if len(reply) != 2 or int.from_bytes(reply, 'little') != len(rows):
        raise ValueError('incomplete address scan')
    for row in rows:
        if len(row) == 10 and row[:8] == wire_ieee(args):
            return int.from_bytes(row[8:10], 'little')
    raise ValueError('device absent from live address cache')


def trace(args, emit):
    end = time.monotonic() + args.duration
    count = 0
    # The gateway bridge intentionally expires at 120 seconds. Rotate at 110.
    while time.monotonic() < end and count < 12000:
        bridge = Bridge(args)
        try:
            node = scan_node(bridge.client, args)
            rotate = min(end, time.monotonic() + 110)
            while time.monotonic() < rotate and count < 12000:
                try:
                    frame = (bridge.client.events.pop(0) if bridge.client.events
                             else bridge.client.receive(min(5, rotate - time.monotonic())))
                except TimeoutError:
                    continue
                row = aps(frame)
                if row is None:
                    continue
                if (row['profile'] == 0 and row['cluster'] == 0x13 and
                        wire_ieee(args) in bytes.fromhex(row['data'])):
                    node = row['source']
                if row['source'] != node:
                    continue
                emit(row)
                count += 1
                print(json.dumps(row), flush=True)
        finally:
            bridge.close()
    emit({'kind': 'summary', 'frames': count})


def readback(args, emit):
    bridge = Bridge(args)
    received = set()
    try:
        node = scan_node(bridge.client, args)
        for index, (cluster, attribute) in enumerate([(0x402, 0), (0x405, 0), (1, 0x21)]):
            asdu = bytes([0x10, 0xe1 + index, 8, 0]) + struct.pack('<H', attribute)
            payload = (bytes([11]) + struct.pack('<H', node) + wire_ieee(args) +
                       struct.pack('<HHBBBBbBH', 0x104, cluster, 1, 1, 0, 30, 0, 0, len(asdu)) + asdu)
            bridge.client.request(0x30, payload)
        end = time.monotonic() + min(args.duration, 35)
        while time.monotonic() < end and len(received) < 3:
            try:
                frame = (bridge.client.events.pop(0) if bridge.client.events else
                         bridge.client.receive(min(5, end - time.monotonic())))
            except TimeoutError:
                continue
            row = aps(frame)
            if (row is not None and row['source'] == node and row['profile'] == 0x104 and
                    row['command'] == 9 and row['transaction'] in [0xe1, 0xe2, 0xe3]):
                emit(row)
                print(json.dumps(row), flush=True)
                received.add(row['transaction'])
        emit({'kind': 'summary', 'readbacks': len(received)})
        print('Reporting readbacks received:', len(received), flush=True)
    finally:
        bridge.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['observe', 'trace', 'readback', 'alert'], default='observe')
    parser.add_argument('--host', required=True)
    parser.add_argument('--password-file')
    parser.add_argument('--bridge', default='/tuya/native/gateway-maintenance')
    parser.add_argument('--ieee', default='0x00124b002a52140a')
    parser.add_argument('--duration', type=int, default=900)
    parser.add_argument('--since', type=int)
    parser.add_argument('--output', required=True)
    parser.add_argument('--execute', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.duration <= 900:
        parser.error('duration must be 1..900 seconds')
    try:
        if not args.ieee.startswith('0x') or len(args.ieee) != 18 or len(wire_ieee(args)) != 8:
            raise ValueError()
    except ValueError:
        parser.error('IEEE must be 0x followed by 16 hexadecimal digits')
    if args.mode in ['trace', 'readback'] and not args.password_file:
        parser.error('SSH modes require --password-file')
    if args.mode in ['readback', 'alert'] and not args.execute:
        parser.error('readback/alert require explicit --execute')
    if args.bridge != '/tuya/native/gateway-maintenance':
        parser.error('only the production maintenance bridge is allowed')
    destination = Path(args.output)
    if destination.parent.stat().st_mode & 0o077:
        parser.error('output parent must be private (mode 0700)')
    with os.fdopen(os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'w') as output:
        def emit(value):
            output.write(json.dumps(value) + '\n')
            output.flush()
        if args.mode == 'alert':
            status = http(args, '/api/status')
            if not status.get('online') or not status.get('network_formed'):
                raise ValueError('gateway is not ready')
            result = {'join': http(args, '/api/join', True),
                      'notification': http(args, '/api/notifications/test', True)}
            emit(result)
            print(json.dumps(result), flush=True)
        else:
            {'observe': observe, 'trace': trace, 'readback': readback}[args.mode](args, emit)


if __name__ == '__main__':
    main()
