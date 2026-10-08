"""Paced bidirectional load with a concurrent WebSocket completion reader."""
from collections import Counter
import struct
import threading
import time


def run_load(client_class, port, rcp, rates, seconds, summary, control, sample, stages):
    for index, rate in enumerate(rates):
        client = client_class(port)
        pending = {}
        lock = threading.Lock()
        counts = Counter()
        statuses = Counter()
        admission_times = []
        completion_times = []
        errors = []
        queue_samples = []
        closed = threading.Event()
        baseline_tx = rcp.counts["tx_frames"]
        baseline_rx = rcp.counts["rx_injected"]
        baseline_beacons = rcp.counts["beacon_replies"]
        baseline_beacon_times = len(rcp.beacon_times_ms)

        def receive():
            try:
                while True:
                    opcode, data = client.websocket_frame()
                    if opcode == 8:
                        break
                    if opcode != 2 or len(data) < 4:
                        raise ValueError("Unexpected load-test WebSocket frame")
                    command = data[0]
                    bits = struct.unpack("<H", data[1:3])[0]
                    frame_type, request_id = bits & 3, bits >> 2
                    with lock:
                        if frame_type == 1 and command == 0x31:
                            entry = pending[request_id]
                            elapsed = (time.perf_counter_ns() - entry["sent_ns"]) / 1e6
                            statuses[data[3]] += 1
                            admission_times.append(elapsed)
                            if data[3] == 0:
                                counts["accepted"] += 1
                                entry["admitted"] = True
                                if entry["confirmed"]:
                                    del pending[request_id]
                            else:
                                counts["rejected"] += 1
                                del pending[request_id]
                        elif frame_type == 3 and command == 0x13:
                            entry = pending[request_id]
                            entry["confirmed"] = True
                            completion_times.append((time.perf_counter_ns() - entry["sent_ns"]) / 1e6)
                            if entry["admitted"]:
                                del pending[request_id]
                            counts["completed"] += 1
                            counts["success" if data[3] == 0 else "completion_errors"] += 1
            except Exception as error:
                errors.append(f"{type(error).__name__}: {error}")
            finally:
                closed.set()

        reader = threading.Thread(target=receive, daemon=True)
        reader.start()
        control(f"load-start {index}")
        sample()
        started = time.monotonic()
        next_offer = started
        next_sample = started
        payload = struct.pack("<BHHHBBBBbH", 0, 0xfffc, 0x104, 6, 1, 1, 0, 3, 0, 32) + bytes(range(32))
        try:
            while time.monotonic() - started < seconds and not errors:
                now = time.monotonic()
                if now < next_offer:
                    time.sleep(next_offer - now)
                if time.monotonic() - started >= seconds:
                    break
                # Do not accumulate a catch-up burst if the host falls behind.
                if time.monotonic() - next_offer > 1 / rate:
                    counts["host_missed_slots"] += int((time.monotonic() - next_offer) * rate)
                    next_offer = time.monotonic()
                next_offer += 1 / rate
                with lock:
                    if len(pending) >= 256:
                        counts["host_pending_cap_skips"] += 1
                        send = False
                    else:
                        client.sequence += 1
                        request_id = client.sequence
                        pending[request_id] = {"sent_ns": time.perf_counter_ns(),
                                               "admitted": False, "confirmed": False}
                        counts["submitted"] += 1
                        counts["peak_pending"] = max(counts["peak_pending"], len(pending))
                        send = True
                if send:
                    client.send(2, b"\x31" + struct.pack("<H", request_id << 2) + payload)
                # IEEE 802.15.4 beacon request, broadcast PAN/address, no source.
                rcp.inject(b"\x03\x08" + bytes([counts["rx_sent"] & 255]) + b"\xff\xff\xff\xff\x07")
                counts["rx_sent"] += 1
                counts["peak_pending_beacons"] = max(counts["peak_pending_beacons"],
                    counts["rx_sent"] - (rcp.counts["beacon_replies"] - baseline_beacons))
                if time.monotonic() >= next_sample:
                    with lock:
                        queue_samples.append({"seconds": time.monotonic() - started,
                                              "pending": len(pending),
                                              "pending_beacons": counts["rx_sent"] - (rcp.counts["beacon_replies"] - baseline_beacons)})
                    sample()
                    next_sample += 1
            duration = time.monotonic() - started
            control(f"load-end {index}")
            with lock:
                pending_at_end = len(pending)
            beacons_pending_at_end = counts["rx_sent"] - (rcp.counts["beacon_replies"] - baseline_beacons)
            # Drain work, but do not include drain time in offered throughput/CPU.
            drain_started = time.monotonic()
            while time.monotonic() - drain_started < 4 and not errors:
                with lock:
                    waiting = len(pending)
                replied = rcp.counts["beacon_replies"] - baseline_beacons
                if waiting == 0 and replied >= counts["rx_sent"]:
                    break
                time.sleep(0.02)
            with lock:
                remaining = len(pending)
            beacon_replies = rcp.counts["beacon_replies"] - baseline_beacons
            result = {"target_tx_and_rx_rate_each": rate, "offering_seconds": duration,
                      "counts": dict(counts), "response_status_counts": dict(statuses),
                      "pending_at_offer_end": pending_at_end, "pending_after_drain": remaining,
                      "beacons_pending_at_offer_end": beacons_pending_at_end,
                      "beacons_pending_after_drain": counts["rx_sent"] - beacon_replies,
                      "pending_samples": queue_samples, "drain_seconds": time.monotonic() - drain_started,
                      "admission_latency_ms": summary(admission_times),
                      "completion_latency_ms": summary(completion_times),
                      "beacon_reply_latency_ms": summary(rcp.beacon_times_ms[baseline_beacon_times:]),
                      "rcp_tx_frames": rcp.counts["tx_frames"] - baseline_tx,
                      "rcp_rx_injected": rcp.counts["rx_injected"] - baseline_rx,
                      "beacon_replies": beacon_replies, "errors": errors.copy()}
            stages.append(result)
            print(f"LOAD {rate}/s each: {counts['submitted']} submitted, {counts['accepted']} accepted, "
                  f"{counts['completed']} confirmed, {counts['rejected']} rejected; "
                  f"RX {counts['rx_sent']} → {beacon_replies} beacons; "
                  f"pending peak {counts['peak_pending']}, drained {remaining}", flush=True)
        finally:
            try:
                client.send(8, struct.pack("!H", 1000))
                closed.wait(2)
            finally:
                client.conn.close()
                reader.join(2)
        if errors or remaining or counts["completion_errors"] or beacon_replies < counts["rx_sent"]:
            raise RuntimeError(f"Load stage failed: {result}")
    return stages
