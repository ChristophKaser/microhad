"""Validate fresh TCP/HDLC/Spinel capture without depending on tshark."""
import collections
import socket
import struct
from pathlib import Path


def packed(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for shift in (0, 7, 14, 21, 28):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError('invalid packed Spinel integer')


def hdlc(data: bytes) -> bytes:
    result, escape = bytearray(), False
    for byte in data:
        if escape:
            result.append(byte ^ 0x20)
            escape = False
        elif byte == 0x7d:
            escape = True
        else:
            result.append(byte)
    crc = 0xffff
    for byte in result:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    if escape or crc != 0xf0b8 or len(result) < 4:
        raise ValueError('invalid HDLC checksum')
    return bytes(result[:-2])


def inspect(path: Path, gateway: str, port: int) -> dict:
    size = path.stat().st_size
    with path.open("rb") as capture_stream:
        data = capture_stream.read(24)
    if len(data) < 24:
        raise ValueError('capture has no PCAP header yet')
    endian = {'d4c3b2a1': '<', '4d3cb2a1': '<', 'a1b2c3d4': '>', 'a1b23c4d': '>'}.get(data[:4].hex())
    if not endian:
        raise ValueError('unsupported PCAP format')
    link = struct.unpack_from(endian + 'I', data, 20)[0]
    if link not in (113, 276):
        raise ValueError('expected Linux cooked capture from tcpdump -i any')
    streams, syn = collections.defaultdict(list), set()
    offset, packets = 24, 0
    with path.open("rb") as capture_stream:
        capture_stream.seek(24)
        while offset + 16 <= size:
            record = capture_stream.read(16)
            length = struct.unpack_from(endian + 'I', record, 8)[0]
            if offset + 16 + length > size:
                break # The writer may still be appending its latest packet.
            packet = capture_stream.read(length)
            offset += 16 + length
            packets += 1
            protocol_offset, ip_offset = (0, 20) if link == 276 else (14, 16)
            if packet[protocol_offset:protocol_offset + 2] != b'\x08\x00':
                continue
            ip = packet[ip_offset:]
            if len(ip) < 20 or ip[9] != 6 or struct.unpack_from('>H', ip, 6)[0] & 0x3fff:
                continue
            tcp = ip[(ip[0] & 15) * 4:struct.unpack_from('>H', ip, 2)[0]]
            if len(tcp) < 20:
                continue
            source, dest, seq = struct.unpack_from('>HHI', tcp)
            src, dst = socket.inet_ntoa(ip[12:16]), socket.inet_ntoa(ip[16:20])
            if not ((src == gateway and source == port) or (dst == gateway and dest == port)):
                continue
            identity = src, source, dst, dest
            if tcp[13] & 2:
                syn.add(identity)
                seq += 1
            payload = tcp[(tcp[12] >> 4) * 4:]
            if payload:
                streams[identity].append((seq, payload))
    midstream = len([identity for identity in streams if identity not in syn])
    # Capture intentionally starts while the previous agent is still connected.
    # Retain those packets but validate the fresh connection created by restart.
    streams = {identity: segments for identity, segments in streams.items() if identity in syn}
    result = {'packets': packets, 'frames': 0, 'raw_tx': 0, 'raw_rx': 0, 'reset_commands': 0,
              'tcp_gaps': 0, 'bad_hdlc': 0, 'directions': len(streams),
              'ignored_midstream_directions': midstream,
              'handshakes': len(syn), 'iid0_only': True, 'pcap_trailing_bytes': size - offset,
              'partial_hdlc': False}
    for identity, segments in streams.items():
        segments.sort()
        expected, assembled = segments[0][0], bytearray()
        for seq, payload in segments:
            if seq > expected:
                result['tcp_gaps'] += 1
                break
            skip = expected - seq
            if skip < len(payload):
                assembled.extend(payload[skip:])
                expected = seq + len(payload)
        if assembled:
            result['partial_hdlc'] |= assembled[0] != 0x7e or assembled[-1] != 0x7e
        for part in assembled.split(b'\x7e')[1:-1]:
            if not part:
                continue
            try:
                frame = hdlc(part)
                result['frames'] += 1
                result['iid0_only'] &= (frame[0] >> 4) & 3 == 0
                command, at = packed(frame, 1)
                result['reset_commands'] += command == 1
                if command in (2, 3, 4, 5, 6, 7, 8):
                    prop, _ = packed(frame, at)
                    result['raw_tx'] += prop == 113 and command == 3 and identity[3] == port
                    result['raw_rx'] += prop == 113 and command == 6 and identity[1] == port
            except (ValueError, IndexError):
                result['bad_hdlc'] += 1
    result['complete'] = (result['directions'] == 2 and result['handshakes'] == 2 and
                          result['raw_tx'] > 0 and result['raw_rx'] > 0 and result['iid0_only'] and
                          not any(result[k] for k in ('tcp_gaps', 'bad_hdlc', 'reset_commands',
                                                      'partial_hdlc', 'pcap_trailing_bytes')))
    return result
