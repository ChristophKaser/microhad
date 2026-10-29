#!/usr/bin/env python3
"""Receive framed gateway cores over TCP; acknowledge only saved captures."""
import argparse
import json
import os
from pathlib import Path
import re
import socketserver
import struct
import time
import uuid


class Receiver(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 4


class Capture(socketserver.StreamRequestHandler):
    def handle(self):
        if self.server.gateway and self.client_address[0] != self.server.gateway:
            return
        self.request.settimeout(getattr(self.server, 'idle_timeout', 130))  # idle timeout, no total transfer deadline
        identity = f'{time.time_ns()}-{uuid.uuid4().hex}'
        directory = self.server.output / identity
        directory.mkdir(mode=0o700)
        core = directory / 'core.partial'
        metadata = {}
        try:
            header = self.rfile.readline(128)
            match = re.fullmatch(rb'GCORE1 (\d+) (\d+) (\d+)\n', header)
            if not match:
                raise ValueError('invalid header')
            metadata.update(zip(('pid', 'signal', 'epoch'), map(int, match.groups())))
            metadata['peer'] = self.client_address[0]
            def exact(size):
                data = self.rfile.read(size)
                if len(data) != size:
                    raise EOFError('interrupted transfer')
                return data
            def length():
                size, = struct.unpack('!I', exact(4))
                if size > 16384:
                    raise ValueError('oversized frame')
                return size
            (directory / 'process.txt').write_bytes(exact(length()))
            with core.open('xb') as stream:
                while True:
                    size = length()
                    if not size:
                        break
                    stream.write(exact(size))
                status = exact(1)
                if status not in (b'C', b'T'):
                    raise ValueError('invalid completion status')
                if self.rfile.read(1):
                    raise ValueError('trailing bytes')
                stream.flush()
                os.fsync(stream.fileno())
            metadata['complete'] = status == b'C'
            metadata['bytes'] = core.stat().st_size
            self.save_metadata(directory, metadata)
            target = directory / ('core' if metadata['complete'] else 'core.truncated')
            core.rename(target)
            fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            fd = os.open(self.server.output, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
            try:
                self.wfile.write(b'K')
                self.wfile.flush()
            except OSError:
                pass  # Saved successfully; a lost ACK can cause a duplicate retry.
        except (OSError, EOFError, ValueError) as error:
            for name in ('core', 'core.truncated'):
                target = directory / name
                if target.exists():
                    target.rename(core)
            metadata.update(complete=False, error=str(error))
            try:
                self.save_metadata(directory, metadata)
            except OSError:
                pass

    @staticmethod
    def save_metadata(directory, metadata):
        with (directory / 'capture.json').open('w') as stream:
            json.dump(metadata, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', required=True, help='Host LAN IPv4 address')
    parser.add_argument('--port', type=int, default=9999)
    parser.add_argument('--gateway', required=True, help='Allowed gateway IPv4 address')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=True, mode=0o700)
    with Receiver((args.bind, args.port), Capture) as server:
        server.gateway = args.gateway
        server.output = args.output
        server.serve_forever()


if __name__ == '__main__':
    main()
