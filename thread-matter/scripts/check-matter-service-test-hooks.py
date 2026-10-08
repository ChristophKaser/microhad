#!/usr/bin/env python3
"""Check that report-fixture hooks only appear in the dedicated test service."""
import argparse
from pathlib import Path
import sys

MARKER = b"MICROHAD_MATTER_TEST_SEED_SENSOR_REPORT_NODE"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("expect", choices=("present", "absent"))
    args = parser.parse_args()
    if not args.binary.is_file():
        parser.error("service binary does not exist: " + str(args.binary))
    found = MARKER in args.binary.read_bytes()
    expected = args.expect == "present"
    if found != expected:
        state = "contains" if found else "does not contain"
        requirement = ("test artifact must contain the fixture hook"
                       if expected else "production artifact must not contain the fixture hook")
        print(f"ERROR: {args.binary} {state} test-hook marker; {requirement}", file=sys.stderr)
        return 1
    print(f"PASS {args.binary}: test-only report hook is {'present' if found else 'absent'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
