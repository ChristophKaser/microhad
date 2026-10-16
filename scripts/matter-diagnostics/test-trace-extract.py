#!/usr/bin/env python3
"""Offline regression cases for stream framing and raw radio extraction."""
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

from trace_extract import extract_spinel, extract_thread, TraceError


GATEWAY = '192.168.178.62'
HOST = '192.168.178.51'


def hdlc(frame):
    crc = 0xffff
    for byte in frame:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x8408 if crc & 1 else 0)
    frame += struct.pack('<H', crc ^ 0xffff)
    escaped = bytearray(b'\x7e')
    for byte in frame:
        if byte in (0x7e, 0x7d):
            escaped.extend((0x7d, byte ^ 0x20))
        else:
            escaped.append(byte)
    return bytes(escaped) + b'\x7e'


def tcp(payload=b'', seq=100, rx=False, flags=0x18):
    src, dst = (GATEWAY, HOST) if rx else (HOST, GATEWAY)
    ports = (9998, 45678) if rx else (45678, 9998)
    header = struct.pack('>HHIIBBHHH', *ports, seq & 0xffffffff, 0, 0x50, flags, 1024, 0, 0)
    ip = bytearray(20)
    ip[0], ip[9] = 0x45, 6
    struct.pack_into('>H', ip, 2, 40 + len(payload))
    ip[12:16], ip[16:20] = socket.inet_aton(src), socket.inet_aton(dst)
    return bytes(ip) + header + payload


def pcap(path, packets, link=276, endian='<', nano=False):
    magic = 0xa1b23c4d if nano else 0xa1b2c3d4
    header = struct.pack(endian + 'IHHIIII', magic, 2, 4, 0, 0, 65535, link)
    with path.open('wb') as output:
        output.write(header)
        for index, packet in enumerate(packets):
            if link == 276:
                packet = b'\x08\x00' + bytes(18) + packet
            elif link == 113:
                packet = bytes(14) + b'\x08\x00' + packet
            elif link == 1:
                packet = bytes(12) + b'\x08\x00' + packet
            output.write(struct.pack(endian + 'IIII', 1, index + 1, len(packet), len(packet)))
            output.write(packet)


def blob(data):
    return struct.pack('<H', len(data)) + data


def radio(psdu=b'\x02\x00\x42\x00\x00', mac=True):
    result = blob(psdu) + struct.pack('<bbH', -44, -100, 2)
    result += blob(struct.pack('<BBQ', 11, 240, 123456)) + blob(b'\x00')
    if mac:
        result += blob(struct.pack('<BI', 1, 1234))
    return result


def row(number, command, prop, data, direction='tx', tid=1):
    return dict(event='spinel', schema=1, record=number, connection='fixture#1', direction=direction,
                timestamp_ns=number * 1000, first_timestamp_ns=number * 1000, packets=[number],
                command=command, property=prop, tid=tid, iid=0, payload_hex=data.hex())


def transmit(number=1, tid=1):
    data = blob(b'\x61\x88\x42\x00\x00') + struct.pack('<BBBBBBBIIBb', 11, 4, 3, 1, 0, 0, 0, 12, 34, 11, -10)
    return row(number, 3, 113, data, tid=tid)


class StreamTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'host.pcap'

    def run_capture(self, packets, **kwargs):
        pcap(self.path, packets, **kwargs)
        return extract_spinel(self.path)

    def test_out_of_order_retransmission_escape_provenance(self):
        frame = hdlc(b'\x81\x03\x71\x7e\x7d')
        rows, summary = self.run_capture([tcp(seq=99, flags=2), tcp(frame[4:], 104),
                                          tcp(frame[:4], 100), tcp(frame[:4], 100)])
        self.assertEqual(summary['spinel'], 1)
        self.assertEqual(summary['diagnostic'], 0)
        self.assertEqual(rows[0]['spinel_hex'], '8103717e7d')
        self.assertEqual(rows[0]['packets'], [2, 3])
        self.assertTrue(rows[0]['syn_seen'])
        self.assertEqual(rows[0]['timestamp_ns'], 1000003000)

    def test_midstream_and_boundary_fragments(self):
        frame = hdlc(b'\x80\x06\x71')
        rows, counts = self.run_capture([tcp(b'partial' + frame + b'\x81')])
        self.assertEqual(counts['spinel'], 1)
        self.assertEqual(counts['diagnostic'], 2)
        self.assertFalse(next(r for r in rows if r['event'] == 'spinel')['syn_seen'])

    def test_gap_recovers_later_frames(self):
        frame = hdlc(b'\x81\x03\x71')
        rows, counts = self.run_capture([tcp(frame[:3]), tcp(frame, 130)])
        self.assertEqual(counts['spinel'], 1)
        self.assertEqual(sum(r['message'] == 'TCP sequence gap' for r in rows if r['event'] == 'diagnostic'), 1)

    def test_conflicting_overlap(self):
        frame = hdlc(b'\x81\x03\x71')
        rows, counts = self.run_capture([tcp(frame), tcp(b'WRONG', 101)])
        self.assertEqual(counts['spinel'], 1)
        self.assertIn('conflicting TCP overlap', rows[1]['message'])

    def test_sequence_wrap(self):
        frame = hdlc(b'\x81\x03\x71')
        _, counts = self.run_capture([tcp(frame[:3], 0xfffffffd), tcp(frame[3:], 0)])
        self.assertEqual(counts['spinel'], 1)
        self.assertEqual(counts['diagnostic'], 0)

    def test_bad_crc_and_malformed_spinel_preserved(self):
        frame = hdlc(b'\x81\x03\x71')
        rows, counts = self.run_capture([tcp(frame[:-2] + bytes([frame[-2] ^ 1]) + frame[-1:] + hdlc(b'\x81\x83'))])
        self.assertEqual(counts['diagnostic'], 1)
        self.assertIn('hdlc_hex', rows[0])
        self.assertIn('parse_error', rows[1])
        self.assertEqual(rows[1]['spinel_hex'], '8183')

    def test_new_connection_same_tuple(self):
        frame = hdlc(b'\x81\x03\x71')
        rows, _ = self.run_capture([tcp(frame), tcp(seq=499, flags=2), tcp(frame, 500)])
        self.assertEqual(len({r['connection'] for r in rows}), 2)

    def test_links_byte_orders_and_timestamp_units(self):
        for link in (1, 101, 113, 276):
            for endian in ('<', '>'):
                for nano in (False, True):
                    with self.subTest(link=link, endian=endian, nano=nano):
                        rows, counts = self.run_capture([tcp(hdlc(b'\x81\x03\x71'))],
                                                        link=link, endian=endian, nano=nano)
                        self.assertEqual(counts['spinel'], 1)
                        self.assertEqual(rows[0]['timestamp_ns'], 10**9 + (1 if nano else 1000))

    def test_truncated_pcap_and_unsupported_format(self):
        self.run_capture([])
        with self.path.open('ab') as output:
            output.write(b'cut')
        rows, _ = extract_spinel(self.path)
        self.assertIn('truncated PCAP', rows[0]['message'])
        self.path.write_bytes(b'not pcap')
        with self.assertRaises(TraceError):
            extract_spinel(self.path)

    def test_truncated_packet_record(self):
        self.run_capture([tcp(hdlc(b'\x81\x03\x71'))])
        self.path.write_bytes(self.path.read_bytes()[:-2])
        rows, counts = extract_spinel(self.path)
        self.assertEqual(counts['diagnostic'], 1)
        self.assertEqual(rows[0]['message'], 'truncated PCAP record')

    def test_ipv6_and_vlan(self):
        payload = hdlc(b'\x81\x03\x71')
        tcp_header_payload = tcp(payload)[20:]
        source, gateway = 'fd00::1', 'fd00::2'
        ipv6 = struct.pack('>IHBB16s16s', 6 << 28, len(tcp_header_payload), 6, 64,
                           socket.inet_pton(socket.AF_INET6, source), socket.inet_pton(socket.AF_INET6, gateway))
        pcap(self.path, [ipv6 + tcp_header_payload], link=101)
        rows, counts = extract_spinel(self.path, gateway=gateway)
        self.assertEqual(counts['spinel'], 1)
        packet = bytes(12) + b'\x81\x00\x00\x01\x08\x00' + tcp(payload)
        with self.path.open('wb') as output:
            output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
            output.write(struct.pack('<IIII', 1, 0, len(packet), len(packet)))
            output.write(packet)
        _, counts = extract_spinel(self.path)
        self.assertEqual(counts['spinel'], 1)

    def test_cli_outputs_no_overwrite(self):
        self.run_capture([tcp(hdlc(b'\x81\x03\x71'))])
        script = Path(__file__).with_name('extract-spinel.py')
        first = subprocess.run([sys.executable, str(script), str(self.path)], capture_output=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertTrue(self.path.with_name('spinel.trace.summary.json').exists())
        before = self.path.with_name('spinel.trace').read_bytes()
        second = subprocess.run([sys.executable, str(script), str(self.path)], capture_output=True)
        self.assertNotEqual(second.returncode, 0)
        self.assertEqual(self.path.with_name('spinel.trace').read_bytes(), before)


class RadioTests(unittest.TestCase):
    def test_tx_rx_completion_ack(self):
        records = [transmit(), row(2, 6, 113, radio(), 'rx', 0),
                   row(3, 6, 0, b'\x00\x00\x01' + radio() + struct.pack('<BI', 2, 5678), 'rx')]
        rows, summary = extract_thread(records)
        self.assertEqual((summary['tx'], summary['rx'], summary['ack'], summary['tx_done']), (1, 1, 1, 1))
        self.assertEqual(summary['diagnostic'], 0)
        self.assertEqual(rows[0]['radio']['tx_power_dbm'], -10)
        self.assertEqual(rows[1]['radio']['radio_timestamp_us'], 123456)
        self.assertEqual(rows[1]['radio']['ack_frame_counter'], 1234)
        done = next(r for r in rows if r['event'] == 'tx_done')
        self.assertEqual(done['extra_hex'], struct.pack('<BI', 2, 5678).hex())
        self.assertEqual(next(r for r in rows if r.get('kind') == 'ack')['tx_spinel_record'], 1)

    def test_no_transmit_security_capability(self):
        rows, summary = extract_thread([row(1, 6, 113, radio(mac=False), 'rx', 0)])
        self.assertEqual(summary['rx'], 1)
        self.assertNotIn('ack_key_index', rows[0]['radio'])
        self.assertEqual(summary['diagnostic'], 0)

    def test_ack_completion_without_captured_request(self):
        rows, summary = extract_thread([row(1, 6, 0, b'\x00\x00\x01' + radio(), 'rx')])
        self.assertEqual(summary['ack'], 1)
        self.assertIsNone(next(r for r in rows if r.get('kind') == 'ack')['tx_spinel_record'])
        self.assertEqual(summary['diagnostic'], 0)

    def test_empty_ack_and_failed_tx(self):
        rows, summary = extract_thread([transmit(), row(2, 6, 0, b'\x00\x00\x00' + radio(b''), 'rx'),
                                       transmit(3), row(4, 6, 0, b'\x05\x00\x00', 'rx')])
        self.assertEqual(summary['tx_done'], 2)
        self.assertNotIn('ack', summary)
        self.assertEqual(summary['pending_tx_without_completion'], 0)

    def test_truncated_metadata_preserves_psdu(self):
        for command, direction in ((3, 'tx'), (6, 'rx')):
            rows, summary = extract_thread([row(1, command, 113, blob(b'\x01\x02'), direction)])
            self.assertEqual(summary['thread'], 1)
            self.assertEqual(summary['diagnostic'], 1)
            self.assertEqual(rows[0]['psdu_hex'], '0102')

    def test_tid_reuse_does_not_associate_unrelated_status(self):
        rows, summary = extract_thread([transmit(), row(2, 2, 5, b''), row(3, 6, 0, b'\x00', 'rx')])
        self.assertNotIn('tx_done', summary)
        self.assertEqual(summary['unmatched_status'], 1)
        self.assertEqual(summary['tx_without_completion'], 1)

    def test_connections_and_iids_separate_transactions(self):
        other = transmit(2)
        other['iid'] = 1
        rows, summary = extract_thread([transmit(), other, row(3, 6, 0, b'\x05\x00\x00', 'rx')])
        self.assertEqual(summary['pending_tx_without_completion'], 1)
        self.assertEqual(summary['tx_done'], 1)
        self.assertEqual(next(r for r in rows if r['event'] == 'tx_done')['tx_spinel_record'], 1)


if __name__ == '__main__':
    unittest.main()
