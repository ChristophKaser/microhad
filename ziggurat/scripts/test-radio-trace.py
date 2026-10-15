#!/usr/bin/env python3
"""Exercise radio logging through a host server and synthetic RCP; no device access."""
import argparse
import importlib.util
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("simulator", ROOT / "ziggurat/scripts/simulate-rcp.py")
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)
LOG_LEVEL = 16385


class Radio(sim.Rcp):
    def __init__(self, response):
        self.response = response
        self.levels = []
        super().__init__("127.0.0.1", iid=1)

    def receive_frame(self, data):
        command, offset = sim.unpacked(data, 1)
        if command == 3:
            prop, offset = sim.unpacked(data, offset)
            if prop == LOG_LEVEL:
                self.levels.append(data[offset:])
                prop, value = {
                    "echo": (LOG_LEVEL, data[offset:]),
                    "mismatch": (LOG_LEVEL, b"\x05"),
                    "empty": (LOG_LEVEL, b""),
                    "failure": (0, b"\x02"),
                    "status_ok": (0, b"\x00"),
                }[self.response]
                self.send(data[0], prop, value)
                return
        super().receive_frame(data)


def run(binary, syslog_shim, enabled, response):
    radio = Radio(response)
    with tempfile.TemporaryDirectory(prefix="radio-trace-") as directory:
        log_path = Path(directory) / "server.log"
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            api_port = probe.getsockname()[1]
        with log_path.open("w") as log:
            process = subprocess.Popen([
                str(binary), "--device", f"tcp://{radio.address}:{radio.port}",
                "--spinel-iid", "1", "--thread-socket", str(Path(directory) / "thread.sock"),
                "--listen", f"127.0.0.1:{api_port}", "--log-level", "info",
            ], stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, "LD_PRELOAD": str(syslog_shim), "RUST_LOG": "info", "MATTER_RADIO_TRACE": "on" if enabled else "off"})
            try:
                deadline = time.monotonic() + 5
                while not radio.levels:
                    assert process.poll() is None, log_path.read_text()
                    assert time.monotonic() < deadline, log_path.read_text()
                    time.sleep(0.02)
                assert radio.levels == [b"\x07" if enabled else b"\x06"]
                records = [b"RADIO_TRACE direct", b"[D] Platform------: RADIO_TRACE prefixed",
                           b"Platform------: RADIO_TRACE module_only"]
                # IID 3 is the multi-PAN broadcast notification IID. The trailing
                # bytes model STREAM_LOG metadata following its NUL-terminated text.
                for record in records:
                    radio.send(0xb0, 116, record + b"\x00\x07\x00")
                radio.send(0xb0, 116, b"[D] Platform------: ordinary debug message\x00")
                deadline = time.monotonic() + 3
                while True:
                    output = log_path.read_text()
                    if all(record.decode() in output for record in records):
                        break
                    assert time.monotonic() < deadline, output
                    time.sleep(0.02)
                for record in records:
                    assert output.count(record.decode()) == 1, output
                assert "ordinary debug message" not in output, output
                warning = "Could not set RCP radio trace level"
                if response == "echo":
                    assert warning not in output, output
                    assert f"RCP radio frame trace {'enabled' if enabled else 'disabled'}" in output, output
                else:
                    assert warning in output, output
                assert not radio.error, radio.error
            finally:
                process.terminate()
                process.wait(timeout=5)
                radio.close()
    print(f"PASS trace={'on' if enabled else 'off'}, response={response}, broadcast logging at info")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/host/debug/ziggurat")
    args = parser.parse_args()
    # Capture the production syslog sink without touching the host's /dev/log.
    with tempfile.TemporaryDirectory(prefix="radio-trace-syslog-") as directory:
        source = Path(directory) / "syslog.c"
        shim = Path(directory) / "syslog.so"
        source.write_text("""#include <stdarg.h>
#include <stdio.h>
void syslog(int priority, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\\n', stderr);
    fflush(stderr);
    va_end(args);
}
""")
        subprocess.run(["cc", "-shared", "-fPIC", "-o", str(shim), str(source)], check=True)
        for enabled, response in [(False, "echo"), (True, "echo"), (True, "mismatch"),
                                  (True, "empty"), (True, "failure"), (True, "status_ok")]:
            run(args.binary.resolve(), shim, enabled, response)


if __name__ == "__main__":
    main()
