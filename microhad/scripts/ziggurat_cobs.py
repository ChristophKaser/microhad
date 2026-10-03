#!/usr/bin/env python3
"""Bounded protocol-v2 COBS client for tests and explicit recovery; no UART access."""
from collections import deque
import select
import socket
import struct
import time


def encode(data):
    result = bytearray([0]); index = 0; code = 1
    for byte in data:
        if byte == 0:
            result[index] = code; index = len(result); result.append(0); code = 1
        else:
            result.append(byte); code += 1
            if code == 255:
                result[index] = code; index = len(result); result.append(0); code = 1
    result[index] = code
    return bytes(result) + b"\x00"


def decode(data):
    result = bytearray(); offset = 0
    if not data or len(data) > 16384:
        raise ValueError("invalid COBS length")
    while offset < len(data):
        code = data[offset]; offset += 1
        if not code or offset + code - 1 > len(data):
            raise ValueError("invalid COBS block")
        block = data[offset:offset + code - 1]
        if 0 in block: raise ValueError("zero in COBS block")
        result.extend(block); offset += code - 1
        if code != 255 and offset < len(data): result.append(0)
    return bytes(result)


class Client:
    def __init__(self, stream, writer=None):
        self.stream = stream; self.writer = writer or stream
        self.buffer = bytearray(); self.frames = deque(); self.events = []
        self.sequence = 0; self.timings = []
        self.hello = self.receive()
        assert self.hello == b"\x00\x03\x00\x02\x00" or self.hello == b"\x00\x03\x00\x02\x01"

    @classmethod
    def connect(cls, path):
        stream = socket.socket(socket.AF_UNIX); stream.connect(str(path))
        return cls(stream)

    def write(self, data):
        if hasattr(self.writer, "sendall"): self.writer.sendall(data)
        else: self.writer.write(data); self.writer.flush()

    def receive(self, timeout=10):
        end = time.monotonic() + timeout
        while not self.frames:
            remaining = end - time.monotonic()
            if remaining <= 0 or not select.select([self.stream], [], [], remaining)[0]:
                raise TimeoutError("COBS response")
            data = self.stream.recv(2048) if hasattr(self.stream, "recv") else self.stream.read1(2048)
            if not data: raise ConnectionError("COBS connection closed")
            self.buffer.extend(data)
            if len(self.buffer)>18432:
                raise ValueError("COBS receive bound")
            while 0 in self.buffer:
                at = self.buffer.index(0); frame = bytes(self.buffer[:at]); del self.buffer[:at+1]
                if frame:
                    if len(self.frames)>=64: raise ValueError("COBS frame queue bound")
                    self.frames.append(decode(frame))
        return self.frames.popleft()

    def request(self, command, payload=b"", status=0):
        self.sequence = self.sequence % 16383 + 1
        header = bytes([command]) + struct.pack("<H", self.sequence << 2)
        start = time.perf_counter_ns(); self.write(encode(header + payload))
        while True:
            frame = self.receive()
            if frame[:3] == bytes([command]) + struct.pack("<H", self.sequence << 2 | 1):
                assert len(frame) >= 4 and frame[3] == status, (command, frame[3] if len(frame)>3 else None)
                self.timings.append((time.perf_counter_ns()-start)/1e6)
                return frame[4:]
            if len(self.events)>=2048: raise ValueError("COBS event bound")
            self.events.append(frame)

    def close(self):
        if hasattr(self.stream, "close"): self.stream.close()
