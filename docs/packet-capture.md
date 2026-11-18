# Development packet capture

Ziggurat and the Matter service send packet captures directly to a UDP
listener. The existing `ZIGGURAT_RAW_FRAMES` and `MATTER_RAW_FRAMES` controls
enable their capture streams when `PACKET_CAPTURE_DEST` is set. The destination
is a numeric address and port, such as `127.0.0.1:5515` or `[::1]:5515`.
Standalone services also accept `--capture-dest ADDRESS:PORT`. The three
Spinel warnings for malformed property values and unhandled or unmatched
frames retain the payload in syslog for troubleshooting. Occasional route,
neighbor and join diagnostics also retain their existing syslog detail.

Set `capture_udp_port=5515` in the FTP collector's private configuration to
enable its IPv4 loopback capture listener. The paired supervisor uses that
listener automatically when `PACKET_CAPTURE_DEST` is unset. A separate listener can receive
captures on a remote host. FTP capture files have their own retention count
and use the same rotation limits as the text logs. Completed captures end in
`.pcapng`, and live or interrupted uploads end in `.pcapng.partial`.

This is a development setting. Capture files can include decrypted Zigbee
frames, plaintext device data, and Matter session keys. UDP and plain FTP
transport those records without encryption. Set the destination only for
development environments where sending that data is acceptable.

## Datagram and archive format

All multibyte values are little endian. Each datagram begins with this header.

| Byte offset | Field |
| --- | --- |
| 0 | Four-byte magic `GPC1` |
| 4 | Producer, 1 for Ziggurat and 2 for Matter |
| 5 | Record kind |
| 6 | Two reserved bytes |
| 8 | 64-bit run ID |
| 16 | 64-bit record sequence, starting at 1 |
| 24 | 64-bit Unix timestamp in microseconds |
| 32 | 32-bit total body size, up to 4,096 bytes |
| 36 | 32-bit chunk offset |
| 40 | Eight reserved bytes |
| 48 | Body chunk, at most 1,152 bytes |

Combine chunks by source, producer, run ID, and sequence. Chunks can arrive out
of order. Use the total length and chunk offsets to detect missing chunks.
The periodic report uses kind 0 and sequence 0. Its body contains two 64-bit
counters for dropped producer records and send failures.

| Kind | Body |
| --- | --- |
| 1, 2 | Matter UDP receive/send, with 16-byte IPv6 peer address, 16-bit peer port, signed 32-bit result, and packet bytes |
| 4 | Matter session key description |
| 10 | Radio receive, with one byte each for channel, signed RSSI, and LQI, then an eight-byte radio timestamp, two-byte flags, and PSDU including FCS |
| 11 | Radio send, with one byte for channel followed by PSDU including FCS |
| 12 | Radio transmit result |
| 13, 14 | Spinel receive/send, with one byte each for IID and TID, then four-byte command and payload |
| 18 | Decoded Spinel receive, same metadata and payload layout as kind 13 |
| 15, 16, 17 | Spinel, decoded Zigbee, or firmware radio diagnostic description |

The collector stores every accepted datagram in a non-copyable pcapng custom
block (`0x40000bad`, experimental PEN 0), and additionally stores radio RX
and TX frames in standard IEEE 802.15.4 interfaces. A custom block beginning
with `GPR1` carries the collector's cumulative 32-bit queue-drop count.
Receiver drops count datagrams, while producer drops count records.
Sequence gaps can also be caused by UDP loss and should not be independently
added to the producer and receiver counters.

Use `python3 ftp-log-collector/read_capture.py CAPTURE...` to check the
complete records, missing sequences, incomplete records and reported loss.
Pass adjacent segments together so a rotation boundary does not look like
packet loss. The reader accounts for reordering by inspecting all supplied
segments before counting gaps. A missing initial or final record may leave
no visible sequence gap.

## Read and decrypt captures

Run `tcpdump -nn -r FILE -c 20` to inspect IEEE 802.15.4 radio frames, or
open the archive in Wireshark. Zigbee network-layer decryption requires the
network key configured privately in the analyzer. APS-secured payloads can
also require the matching link key. The decoded Zigbee records in the archive
reflect frames the gateway decoded itself.

Matter UDP packets and traffic keys live in custom blocks. To authenticate and
export the plaintext of captured Matter unicast sessions, run:

```sh
python3 ftp-log-collector/decrypt_matter_capture.py \
  diagnostics/capture-early.pcapng diagnostics/capture-later.pcapng \
  --output diagnostics/matter-plaintext.jsonl
```

This requires the Python `cryptography` package. Pass segments from the
same Matter process back to session establishment. The decoder matches each
packet to a key by run, direction, peer and destination session ID. It uses
AES-CCM with the clear Matter header as authenticated data and the security
flags, message counter and source node ID as the nonce. The output file
contains the authenticated Matter message body as `plaintext_hex`, including
the exchange header and any application payload. It is a new private file.
Omit `--output` for counts only. Missing keys, unsupported headers and failed
authentication are counted separately. Attaching capture after a session was
established can leave that session without its keys in the archive.

## Gateway replay measurements

The isolated release MIPS replay used the real capture sender and FTP
collector. The text baseline approximated the older hex formatting and
syslog fragmentation over UDP through a private collector port. It excluded
the gateway's production syslog daemon and used synthetic Matter-size
payloads with no live radio traffic. The results measure the whole change
from hex text to binary capture and do not isolate any one optimization.

| Matched workload | Hex text baseline | Binary UDP |
| --- | ---: | ---: |
| 30-record burst delivery, both runs | 30/30 | 30/30 |
| 30-record burst producer process CPU | 60–70 ms | below 10 ms resolution |
| 30-record burst producer-path p99 | 30–50 ms | 0–10 ms measured |
| 30-record burst archive | 55,988 bytes | 16,300 bytes |
| Steady 20 records/s delivery, both runs | 120/120 | 120/120 |
| Steady collector CPU | 17–20 ticks | 2–6 ticks |
| Steady archive | 224,002 bytes | 65,564 bytes |

At steady load producer CPU and latency were below the useful resolution of
the gateway timer. The unpaced 300-record stress tests saturated the producer
queue, with 223–236 UDP producer drops per run and 64–77 complete UDP records
archived, versus 80–99 distinct record starts archived by the text baseline.
In the unpaced 3,000-record run with a delayed FTP sink, UDP archived 100
complete records and reported 2,900 producer drops. The text path archived
235 distinct record starts. Those overloaded runs measure different delivered
work and cannot establish an equivalent-throughput speedup.

To reproduce, build the collector and build the `packet-capture-benchmark`
Cargo example with the same release MIPS target and linker flags as the Matter
service. Run `ftp-log-collector/benchmark_gateway_capture.py --host HOST
--collector COLLECTOR --producer EXAMPLE` from the development host. It stages
the two binaries in a private temporary directory on the gateway and runs an
in-memory FTP fixture reachable from the gateway. Use `--matched-only` for the
lighter loads in the table.
