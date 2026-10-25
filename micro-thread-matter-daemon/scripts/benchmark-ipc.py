#!/usr/bin/env python3
"""Compare host IPC codec CPU using the same synthetic wire fixtures on each revision.

Uses installed pinned Rust dependencies offline. No sockets or gateway access.
"""
import argparse
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', help='Compile the implementation from this Git revision')
    parser.add_argument('--iterations', type=int, default=5000)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error('--iterations must be positive')
    with tempfile.TemporaryDirectory(prefix='matter-ipc-revision-') as temporary:
        source_root = ROOT
        if args.revision:
            source_root = Path(temporary)
            paths = ['micro-thread-matter-daemon/src', 'micro-thread-matter-daemon/build.rs',
                     'micro-thread-matter-daemon/Cargo.toml', 'micro-thread-matter-daemon/Cargo.lock',
                     'protocol/generated/protobuf.rs', 'protocol/matter-wire-vectors.json',
                     'config/thermostat-attributes.def']
            archive = subprocess.check_output(['git', '-C', str(ROOT), 'archive', args.revision, *paths])
            with tarfile.open(fileobj=io.BytesIO(archive)) as bundle:
                bundle.extractall(source_root, filter='data')
            for directory in ['build', 'ziggurat']:
                (source_root/directory).symlink_to(ROOT/directory, target_is_directory=True)
            examples = source_root/'micro-thread-matter-daemon/examples'
            examples.mkdir(exist_ok=True)
            (examples/'benchmark-ipc.rs').write_bytes((ROOT/'micro-thread-matter-daemon/examples/benchmark-ipc.rs').read_bytes())
        command = '''source scripts/rust-env.sh
export CARGO_TARGET_DIR="$PWD/build/micro-thread-matter-daemon/target"
cargo +"$RUST_TOOLCHAIN" run --offline --locked --release --manifest-path "$1" --example benchmark-ipc -- "$2"
'''
        subprocess.run(['bash', '-c', command, 'benchmark-ipc', str(source_root/'micro-thread-matter-daemon/Cargo.toml'), str(args.iterations)], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
