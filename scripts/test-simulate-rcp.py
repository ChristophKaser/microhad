"""Focused wire-format and simulator error-path checks; no sockets or gateway."""
from collections import Counter
import importlib.util
from pathlib import Path
import unittest
import queue
import struct
from load_rcp import run_load

spec = importlib.util.spec_from_file_location("simulator", Path(__file__).with_name("simulate-rcp.py"))
simulator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(simulator)


class WireTests(unittest.TestCase):
    def test_load_confirm_before_admission_and_backpressure(self):
        class Radio:
            counts = Counter()
            beacon_times_ms = []

            def inject(self, psdu):
                self.counts["rx_injected"] += 1
                self.counts["beacon_replies"] += 1
                self.beacon_times_ms.append(0.1)

        radio = Radio()

        class Client:
            def __init__(self, port):
                self.sequence = 0
                self.frames = queue.Queue()
                self.conn = self

            def websocket_frame(self):
                return self.frames.get(timeout=1)

            def send(self, opcode, payload):
                if opcode == 8:
                    self.frames.put((8, payload))
                    return
                request_id = struct.unpack("<H", payload[1:3])[0] >> 2
                status = 0 if request_id % 2 else 0x20
                if status == 0:
                    radio.counts["tx_frames"] += 1
                    self.frames.put((2, b"\x13" + struct.pack("<H", request_id << 2 | 3) + b"\x00"))
                self.frames.put((2, b"\x31" + struct.pack("<H", request_id << 2 | 1) + bytes([status])))

            def close(self):
                pass

        stages = []
        run_load(Client, 0, radio, [100], 0.06, simulator.timing_summary,
                 lambda _: None, lambda: None, stages)
        result = stages[0]
        self.assertEqual(result["pending_after_drain"], 0)
        self.assertGreater(result["counts"]["accepted"], 0)
        self.assertGreater(result["counts"]["rejected"], 0)
        self.assertEqual(result["counts"]["accepted"], result["counts"]["completed"])

    def test_linuxthreads_cpu_aggregation(self):
        output = ["TOP_PID 100", "TOP_ROW 1 100 50 90% 6000",
                  "TOP_ROW 2 102 101 13% 6000", "TOP_ROW 2 200 50 50% 2000",
                  "TOP_ROW 2 100 50 1% 6000", "TOP_ROW 2 101 100 0% 6000"]
        samples = simulator.top_samples(output)
        self.assertEqual(len(samples), 1)
        self.assertEqual(samples[0]["cpu_percent"], 14)
        self.assertEqual({row["pid"] for row in samples[0]["threads"]}, {100, 101, 102})

    def test_upstream_hdlc_vector(self):
        self.assertEqual(simulator.encode(bytes.fromhex("810202")), bytes.fromhex("7e8102025e807e"))
        self.assertEqual(simulator.decode(bytes.fromhex("8102025e80")), bytes.fromhex("810202"))

    def test_escaped_bytes_and_corruption(self):
        payload = bytes([0x7e, 0x7d, 0x11, 0x13, 0xf8, 0, 255])
        encoded = simulator.encode(payload)
        self.assertEqual(simulator.decode(encoded[1:-1]), payload)
        damaged = bytearray(encoded[1:-1])
        damaged[-1] ^= 1
        with self.assertRaises(ValueError):
            simulator.decode(damaged)
        with self.assertRaises(ValueError):
            simulator.decode(b"\x7d")

    def test_packed_property_boundaries(self):
        for value in (0, 127, 128, 4869, (1 << 21) - 1):
            data = simulator.packed(value)
            self.assertEqual(simulator.unpacked(data, 0), (value, len(data)))
        with self.assertRaises(ValueError):
            simulator.unpacked(b"\x80\x80\x80", 0)

    def test_reset_and_unsupported_property(self):
        rcp = simulator.Rcp.__new__(simulator.Rcp)
        rcp.counts = Counter()
        rcp.properties = {33: b"\x0f"}
        replies = []
        rcp.send = lambda *args: replies.append(args)
        rcp.receive_frame(b"\x80\x01\x02")
        self.assertNotIn(33, rcp.properties)
        self.assertEqual(replies.pop(), (0x80, 0, b"\x72"))
        rcp.receive_frame(b"\x81\x03" + simulator.packed(999) + b"\x01")
        self.assertEqual(replies.pop(), (0x81, 0, b"\x02"))


if __name__ == "__main__":
    unittest.main()
