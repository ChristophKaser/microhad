#!/usr/bin/env python3
"""Stop only our collector when its owner's control pipe closes, even under sudo."""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys


def main():
    pid_path = Path(sys.argv[1])
    child = subprocess.Popen(sys.argv[2:], stdin=subprocess.DEVNULL, start_new_session=True)
    pid_path.write_text(json.dumps({'supervisor': os.getpid(), 'collector': child.pid}))
    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    try:
        while child.poll() is None and not stopping:
            readable, _, _ = select.select([sys.stdin.buffer], [], [], .2)
            if readable and not os.read(sys.stdin.fileno(), 4096):
                break
    finally:
        for sig, seconds in ((signal.SIGINT, 8), (signal.SIGTERM, 3), (signal.SIGKILL, 3)):
            if child.poll() is not None:
                break
            try:
                os.killpg(child.pid, sig)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=seconds)
            except subprocess.TimeoutExpired:
                continue
    return child.returncode


if __name__ == '__main__':
    raise SystemExit(main())
