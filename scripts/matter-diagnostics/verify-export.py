#!/usr/bin/env python3
"""Authenticate saved Matter messages with exported per-session traffic keys."""
import argparse
import collections
import json
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESCCM


def verify(directory: Path, require_complete: bool = True) -> dict:
    keys, sessions, counts, failures = {}, {}, collections.Counter(), []
    for line in (directory / 'keys/matter-session-keys.jsonl').read_text().splitlines():
        row = json.loads(line)
        identity = row['pid'], row['context']
        if row['event'] == 'keys':
            keys[identity] = row
        elif row['event'] == 'session':
            sessions[identity] = row
    plaintext_path = directory / 'capture/matter-decrypted.jsonl'
    with plaintext_path.open('w') as output:
        for index, line in enumerate((directory / 'capture/matter-messages.jsonl').read_text().splitlines(), 1):
            row = json.loads(line)
            identity = row['pid'], row['context']
            key, session = keys[identity], sessions.get(identity, {})
            try:
                decode = bytes.fromhex
                plaintext = AESCCM(decode(key[row['direction'] + '_key']),
                                   tag_length=len(decode(row['tag']))).decrypt(
                    decode(row['nonce']), decode(row['ciphertext'] + row['tag']), decode(row['aad']))
                if row['plaintext'] is not None and plaintext.hex() != row['plaintext']:
                    raise ValueError('plaintext differs from controller trace')
                category = session.get('type', 'unactivated')
                if key['resumed']:
                    category += '-resumed'
                counts[category + '-' + row['direction']] += 1
                output.write(json.dumps({**row, 'type': category, 'plaintext': plaintext.hex()}) + '\n')
            except Exception as error:
                failures.append({'line': index, 'error': type(error).__name__})
    required = ['PASE-tx', 'PASE-rx', 'CASE-tx', 'CASE-rx']
    missing = [name for name in required if not counts[name]] if require_complete else []
    # A resumed session is checked whenever one was observed.
    for identity, key in keys.items():
        if key['resumed'] and sessions.get(identity, {}).get('type') == 'CASE':
            for direction in ('tx', 'rx'):
                if not counts['CASE-resumed-' + direction]:
                    missing.append('CASE-resumed-' + direction)
    result = {'schema': 1, 'keys_exported': len(keys), 'sessions': len(sessions),
              'authenticated_messages': dict(counts), 'authentication_failures': failures,
              'missing': missing, 'complete': not missing and not failures and bool(counts)}
    (directory / 'metadata/decryption-verification.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--partial', action='store_true')
    args = parser.parse_args()
    result = verify(args.directory, not args.partial)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['complete'] else 1)
