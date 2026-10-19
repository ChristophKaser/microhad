#!/usr/bin/env python3
"""Poll the gateway log API into a private host file or stdout; never write gateway flash."""
import argparse
from collections import Counter
from contextlib import nullcontext
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys
import time
from urllib.error import URLError
from urllib.request import urlopen


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from workspace_config import setting, gateway_host


class LogCursor:
    """Track the API's inclusive, second-resolution timestamp boundary."""
    def __init__(self):
        self.since = 0
        self.boundary = Counter()

    def consume(self, lines):
        if not lines:
            return []
        seen = Counter()
        latest = self.since
        pending = []
        for line in lines:
            timestamp = int(line.split(' ', 1)[0])
            latest = max(latest, timestamp)
            seen[line] += 1
            if timestamp > self.since or seen[line] > self.boundary[line]:
                pending.append(line)
        self.since = latest
        self.boundary = Counter({line: count for line, count in seen.items()
                                 if int(line.split(' ', 1)[0]) == latest})
        return pending


def capture(host, output):
    cursor = LogCursor()
    while True:
        try:
            with urlopen(f'http://{host}/api/logs?since={cursor.since}', timeout=8) as response:
                lines = json.load(response)['lines']
            for line in cursor.consume(lines):
                output.write(line + '\n')
            output.flush()
            time.sleep(1)
        except (URLError, OSError, ValueError, KeyError, TypeError) as error:
            stamp = datetime.now(timezone.utc).isoformat()
            print(f'capture unavailable {stamp}: {error}; retrying in 2s', file=sys.stderr, flush=True)
            time.sleep(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default=setting('GATEWAY_IP'), type=gateway_host)
    parser.add_argument('--output', type=Path, help='Private host archive; defaults to stdout')
    args = parser.parse_args()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # Raw captures can contain protocol payloads; create the host archive private.
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        output = os.fdopen(descriptor, 'a', buffering=1)
    else:
        output = nullcontext(sys.stdout)
    try:
        with output as stream:
            capture(args.host, stream)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
