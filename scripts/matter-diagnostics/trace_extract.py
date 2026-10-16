"""Lossless offline extraction helpers; no Thread/Matter protocol decoding."""
import collections
import hashlib
import json
import socket
import struct
from pathlib import Path

from spinel_capture import hdlc, packed


class TraceError(ValueError):
    pass


def diagnostic(message, **fields):
    return dict(event='diagnostic', message=message, **fields)


def tcp_packet(packet, link):
    """Return endpoints, sequence, flags and captured payload, or None."""
    if link == 276:
        start, protocol = 20, packet[:2]
    elif link == 113:
        start, protocol = 16, packet[14:16]
    elif link == 1:
        start, protocol = 14, packet[12:14]
        while protocol in (b'\x81\x00', b'\x88\xa8') and len(packet) >= start + 4:
            protocol, start = packet[start + 2:start + 4], start + 4
    elif link == 101:
        start = 0
        protocol = b'\x08\x00' if packet and packet[0] >> 4 == 4 else b'\x86\xdd'
    else:
        raise TraceError(f'unsupported PCAP link type {link}')
    ip = packet[start:]
    if protocol == b'\x08\x00':
        if len(ip) < 20 or ip[9] != 6 or struct.unpack_from('>H', ip, 6)[0] & 0x3fff:
            return None
        header = (ip[0] & 15) * 4
        if header < 20:
            return None
        src, dst = socket.inet_ntop(socket.AF_INET, ip[12:16]), socket.inet_ntop(socket.AF_INET, ip[16:20])
        tcp = ip[header:struct.unpack_from('>H', ip, 2)[0]]
    elif protocol == b'\x86\xdd':
        if len(ip) < 40:
            return None
        src, dst = socket.inet_ntop(socket.AF_INET6, ip[8:24]), socket.inet_ntop(socket.AF_INET6, ip[24:40])
        next_header, offset = ip[6], 40
        while next_header in (0, 43, 60):
            if len(ip) < offset + 2:
                return None
            next_header, offset = ip[offset], offset + (ip[offset + 1] + 1) * 8
        if next_header != 6:  # IP fragments are not reassembled.
            return None
        tcp = ip[offset:40 + struct.unpack_from('>H', ip, 4)[0]]
    else:
        return None
    if len(tcp) < 20 or (tcp[12] >> 4) * 4 < 20:
        return None
    sport, dport, sequence = struct.unpack_from('>HHI', tcp)
    return src, sport, dst, dport, sequence, tcp[13], tcp[(tcp[12] >> 4) * 4:]


def read_pcap(path, gateway, port):
    streams, notes, epochs, syns = {}, [], {}, {}
    packet_count = 0
    with Path(path).open('rb') as source:
        header = source.read(24)
        formats = {b'\xd4\xc3\xb2\xa1': ('<', 1000), b'\xa1\xb2\xc3\xd4': ('>', 1000),
                   b'\x4d\x3c\xb2\xa1': ('<', 1), b'\xa1\xb2\x3c\x4d': ('>', 1)}
        if len(header) != 24 or header[:4] not in formats:
            raise TraceError('expected classic PCAP, not PCAPNG')
        endian, scale = formats[header[:4]]
        link = struct.unpack_from(endian + 'I', header, 20)[0] & 0xffff
        if link not in (1, 101, 113, 276):
            raise TraceError(f'unsupported PCAP link type {link}')
        while True:
            record = source.read(16)
            if not record:
                break
            if len(record) != 16:
                notes.append(diagnostic('truncated PCAP record header', raw_hex=record.hex()))
                break
            sec, fraction, size, original_size = struct.unpack(endian + 'IIII', record)
            if size > 16 * 1024 * 1024:
                raise TraceError('PCAP record exceeds 16 MiB safety limit')
            data = source.read(size)
            packet_count += 1
            if len(data) != size:
                notes.append(diagnostic('truncated PCAP record', packet=packet_count, raw_hex=data.hex()))
                break
            parsed = tcp_packet(data, link)
            if parsed is None:
                continue
            src, sport, dst, dport, seq, flags, payload = parsed
            if src == gateway and sport == port:
                client, direction = (dst, dport), 'rx'
            elif dst == gateway and dport == port:
                client, direction = (src, sport), 'tx'
            else:
                continue
            if size < original_size:
                notes.append(diagnostic('snaplen-truncated target TCP packet', packet=packet_count))
            if flags & 2 and not flags & 16 and syns.get(client) != seq:
                epochs[client] = epochs.get(client, 0) + 1
                syns[client] = seq
            epoch = epochs.get(client, 0)
            connection = f'{client[0]}:{client[1]}->{gateway}:{port}#{epoch}'
            stream = streams.setdefault((connection, direction), dict(connection=connection, direction=direction,
                                         syn_seen=False, segments=[]))
            stream['syn_seen'] |= bool(flags & 2)
            if payload:
                stream['segments'].append(((seq + bool(flags & 2)) & 0xffffffff, payload,
                                           packet_count, sec * 10**9 + fraction * scale))
    return streams.values(), notes, packet_count


def decode_run(data, origins, stream):
    """Decode contiguous bytes; a boundary fragment cannot become a valid frame."""
    records = []

    def provenance(begin, end):
        sources = [(packet, timestamp) for left, right, packet, timestamp in origins
                   if left < end and right > begin]
        return dict(packets=sorted({p for p, _ in sources}),
                    timestamp_ns=max(t for _, t in sources), first_timestamp_ns=min(t for _, t in sources))

    def emit(begin, end, framed):
        if begin == end:
            return
        fields = dict(connection=stream['connection'], direction=stream['direction'],
                      syn_seen=stream['syn_seen'], **provenance(begin, end))
        raw = data[begin:end]
        if not framed:
            records.append(diagnostic('partial HDLC at capture or TCP-gap boundary', raw_hex=raw.hex(), **fields))
            return
        try:
            frame = hdlc(raw[1:-1])
        except ValueError as error:
            records.append(diagnostic(str(error), hdlc_hex=raw.hex(), **fields))
            return
        fields.update(event='spinel', hdlc_hex=raw.hex(), spinel_hex=frame.hex())
        try:
            if frame[0] & 0xc0 != 0x80:
                raise ValueError('invalid Spinel header flag')
            fields.update(iid=(frame[0] >> 4) & 3, tid=frame[0] & 15)
            command, offset = packed(frame, 1)
            fields['command'] = command
            if command in (2, 3, 4, 5, 6, 7, 8):
                fields['property'], offset = packed(frame, offset)
            fields['payload_hex'] = frame[offset:].hex()
        except (ValueError, IndexError) as error:
            fields['parse_error'] = str(error) or 'truncated Spinel header'
        records.append(fields)

    first = data.find(b'\x7e')
    if first < 0:
        emit(0, len(data), False)
        return records
    emit(0, first, False)
    left = first
    for right in range(first + 1, len(data)):
        if data[right] == 0x7e:
            if right > left + 1:
                emit(left, right + 1, True)
            left = right
    if left + 1 < len(data):
        emit(left, len(data), False)
    return records


def extract_spinel(path, gateway='192.168.178.62', port=9998):
    streams, records, count = read_pcap(path, gateway, port)
    for stream in streams:
        segments = stream['segments']
        if not segments:
            continue
        anchor = segments[0][0]
        # Signed serial arithmetic handles a sequence wrap within a <2 GiB span.
        segments = sorted((((seq - anchor + 2**31) % 2**32 - 2**31, data, packet, stamp)
                           for seq, data, packet, stamp in segments), key=lambda s: (s[0], s[2]))
        expected, base = segments[0][0], segments[0][0]
        assembled, origins = bytearray(), []
        for seq, data, packet, stamp in segments:
            if seq > expected:
                records.extend(decode_run(bytes(assembled), origins, stream))
                records.append(diagnostic('TCP sequence gap', connection=stream['connection'],
                                          direction=stream['direction'], missing_bytes=seq - expected,
                                          packets=[packet], timestamp_ns=stamp))
                assembled, origins, base = bytearray(), [], seq
                expected = seq
            overlap = min(expected - seq, len(data))
            if overlap and assembled[seq - base:seq - base + overlap] != data[:overlap]:
                records.append(diagnostic('conflicting TCP overlap; lower-sequence/first-packet bytes retained',
                                          connection=stream['connection'], direction=stream['direction'],
                                          packets=[packet], timestamp_ns=stamp, raw_hex=data.hex()))
            if overlap < len(data):
                left = len(assembled)
                assembled.extend(data[overlap:])
                origins.append((left, len(assembled), packet, stamp))
                expected = seq + len(data)
        records.extend(decode_run(bytes(assembled), origins, stream))
    records.sort(key=lambda row: (row.get('timestamp_ns', 0), row.get('packets', [0])[0]))
    for number, row in enumerate(records, 1):
        row.update(schema=1, record=number)
    summary = dict(pcap_packets=count, diagnostic=0)
    summary.update(collections.Counter(row['event'] for row in records))
    summary['malformed_spinel'] = sum('parse_error' in row for row in records)
    summary['connections'] = [dict(connection=connection,
                                   frames=sum(row['event'] == 'spinel' for row in records
                                              if row.get('connection') == connection),
                                   syn_seen=any(row.get('syn_seen', False) for row in records
                                                if row.get('connection') == connection))
                              for connection in sorted({row['connection'] for row in records
                                                        if 'connection' in row})]
    for direction in ('tx', 'rx'):
        summary['raw_' + direction] = sum(row['event'] == 'spinel' and row.get('property') == 113
                                         and row.get('command') == (3 if direction == 'tx' else 6)
                                         and row['direction'] == direction for row in records)
    return records, summary


class Cursor:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, size):
        if self.at + size > len(self.data):
            raise TraceError(f'truncated radio metadata at byte {self.at}: need {size}')
        value = self.data[self.at:self.at + size]
        self.at += size
        return value

    def unpack(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))

    def blob(self):
        return self.take(self.unpack('H')[0])

    def integer(self):
        try:
            value, end = packed(self.data, self.at)
        except (ValueError, IndexError) as error:
            raise TraceError('truncated/invalid packed integer') from error
        self.at = end
        return value

    def rest(self):
        return self.data[self.at:].hex()


def radio_rx(cursor):
    psdu = cursor.blob()
    rssi, noise, flags = cursor.unpack('bbH')
    phy, vendor = Cursor(cursor.blob()), Cursor(cursor.blob())
    channel, lqi, timestamp = phy.unpack('BBQ')
    meta = dict(rssi_dbm=rssi, noise_dbm=noise, flags=flags, channel=channel, lqi=lqi,
                radio_timestamp_us=timestamp, receive_error=vendor.integer(),
                phy_extra_hex=phy.rest(), vendor_extra_hex=vendor.rest())
    # Optional TRANSMIT_SEC MAC struct: its length prefix distinguishes it
    # from the trailing completion key-index/frame-counter (five raw bytes).
    tail = cursor.data[cursor.at:]
    if len(tail) >= 7 and tail[:2] == b'\x05\x00':
        mac = Cursor(cursor.blob())
        meta['ack_key_index'], meta['ack_frame_counter'] = mac.unpack('BI')
    return psdu, meta


def orphan_completion(data):
    """Recognize an ACK-bearing completion whose request predates capture.

    A bare LAST_STATUS response alone cannot establish that a TX occurred.
    Require the complete radio layout before interpreting an unmatched status.
    """
    cursor = Cursor(data)
    try:
        if cursor.integer() != 0 or any(flag > 1 for flag in cursor.unpack('BB')):
            return False
        radio_rx(cursor)
        return True
    except (ValueError, IndexError):
        return False


def extract_thread(rows):
    output, pending = [], {}
    for row in rows:
        if row['event'] != 'spinel' or 'parse_error' in row:
            continue
        key = row['connection'], row['iid'], row['tid']
        base = {field: row[field] for field in ('connection', 'direction', 'timestamp_ns', 'first_timestamp_ns', 'packets')}
        base.update(schema=1, spinel_record=row['record'], iid=row['iid'], tid=row['tid'])
        prop, command = row.get('property'), row['command']
        previous = pending.get(key)
        # Any later host transaction reusing this TID supersedes a pending TX.
        if row['direction'] == 'tx' and row['tid']:
            pending.pop(key, None)
        cursor = Cursor(bytes.fromhex(row['payload_hex']))
        try:
            if prop == 113 and command == 3 and row['direction'] == 'tx':
                item = dict(base, event='thread', kind='tx', psdu_hex=cursor.blob().hex())
                output.append(item)  # Preserve PSDU even when metadata is incomplete.
                if row['tid']:
                    pending[key] = item
                names = ('channel', 'max_csma_backoffs', 'max_retries', 'csma_ca_enabled',
                         'header_updated', 'is_retransmission', 'security_processed', 'tx_delay',
                         'tx_delay_base_time', 'rx_channel_after_tx', 'tx_power_dbm')
                item['radio'] = dict(zip(names, cursor.unpack('BBBBBBBIIBb')))
                item['extra_hex'] = cursor.rest()
            elif prop == 113 and command == 6 and row['direction'] == 'rx':
                # Read PSDU separately so truncated metadata never discards it.
                psdu = Cursor(cursor.data).blob()
                item = dict(base, event='thread', kind='rx', psdu_hex=psdu.hex())
                output.append(item)
                _, item['radio'] = radio_rx(cursor)
                item['extra_hex'] = cursor.rest()
            elif (prop == 0 and command == 6 and row['direction'] == 'rx' and row['tid']
                  and (previous or orphan_completion(cursor.data))):
                pending.pop(key, None)
                tx_record = previous['spinel_record'] if previous else None
                status = cursor.integer()
                frame_pending, header_updated = cursor.unpack('BB')
                completion = dict(base, event='tx_done', tx_spinel_record=tx_record,
                                  status=status, frame_pending=bool(frame_pending), header_updated=bool(header_updated))
                output.append(completion)
                if status == 0 and cursor.at < len(cursor.data):
                    psdu = Cursor(cursor.data[cursor.at:]).blob()
                    if psdu:
                        item = dict(base, event='thread', kind='ack', psdu_hex=psdu.hex(),
                                    tx_spinel_record=tx_record)
                        output.append(item)
                    psdu, meta = radio_rx(cursor)
                    if psdu:
                        item['radio'] = meta
                    else:
                        completion['empty_ack_radio'] = meta
                completion['extra_hex'] = cursor.rest()
            elif prop == 0 and command == 6 and row['direction'] == 'rx' and row['tid']:
                # Could be a completion whose request predates capture. Preserve
                # it explicitly; do not misclassify ordinary LAST_STATUS as TX.
                output.append(dict(base, event='unmatched_status', payload_hex=row['payload_hex']))
        except (ValueError, IndexError) as error:
            output.append(diagnostic(str(error), payload_hex=row['payload_hex'], **base))
    for number, row in enumerate(output, 1):
        row['record'] = number
    summary = dict(diagnostic=0)
    summary.update(collections.Counter(row['event'] for row in output))
    summary.update(collections.Counter(row['kind'] for row in output if row['event'] == 'thread'))
    summary['pending_tx_without_completion'] = len(pending)
    completed = {row['tx_spinel_record'] for row in output if row['event'] == 'tx_done'}
    summary['tx_without_completion'] = sum(row['spinel_record'] not in completed for row in output
                                           if row.get('kind') == 'tx')
    return output, summary


def save_trace(path, rows):
    with Path(path).open('x', encoding='utf-8') as destination:
        for row in rows:
            destination.write(json.dumps(row, sort_keys=True) + '\n')


def checksum(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()
