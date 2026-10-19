#!/usr/bin/env python3
"""Collect verbatim third-party notices. Python 3.11+, no implicit downloads."""
import argparse
import hashlib
import html
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parents[2]
LEGAL = re.compile(r'^(licen[cs]e|copying|copyright|notice)(?:$|[._-])', re.I)
SUBMODULES = {
 'microhad/dependencies/mbedtls': 'Apache-2.0 (selected from Apache-2.0 OR GPL-2.0-or-later)',
 'microhad/dependencies/tinycbor': 'MIT',
 'ziggurat/dependencies/ziggurat': 'Apache-2.0; locally patched',
 'micro-thread-matter-daemon/dependencies/rs-matter': 'Apache-2.0; locally patched',
 'micro-thread-matter-daemon/dependencies/openthread': 'BSD-3-Clause and component-specific licenses; locally patched',
 'firmware/dependencies/gecko-sdk': 'Silicon Labs MSLA and per-file exceptions; redistribution review required',
 'firmware/dependencies/rtl8196e-gateway': 'MIT; SDK-derived files retain upstream terms',
}
LOCKS = ['micro-thread-matter-daemon/Cargo.lock', 'ziggurat/dependencies/ziggurat/Cargo.lock']


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args], text=True).strip()


def legal_files(root, recursive=False):
    return sorted(p for p in (root.rglob('*') if recursive else root.iterdir())
                  if p.is_file() and LEGAL.match(p.name) and '.git' not in p.parts)


def texts(root, recursive=False):
    files = legal_files(root, recursive)
    # Some crates keep texts inside a license/ or licenses/ directory.
    for folder in ('license', 'licenses'):
        if (root / folder).is_dir():
            files += sorted(p for p in (root / folder).rglob('*') if p.is_file())
    if not files:
        raise ValueError(f'No license text for {root.name}; add a reviewed attribution source')
    return [(str(p.relative_to(root)), p.read_text(errors='strict')) for p in sorted(set(files))]


def inputs():
    if os.environ.get('MICROHAD_SOURCE_BUNDLE') == '1':
        metadata = json.loads((ROOT / '.release/metadata.json').read_text())
        result = dict(metadata['notice_inputs'])
        for name in result:
            path = ROOT / name
            if path.is_file():
                result[name] = hashlib.sha256(path.read_bytes()).hexdigest()
            elif name in metadata['revisions']:
                result[name] = metadata['revisions'][name]
        return result
    paths = ['LICENSE', 'third-party/supplements.json', 'scripts/licensing/notices.py',
             'microhad/webui/package-lock.json', 'micro-thread-matter-daemon/Cargo.toml',
             'ziggurat/stock-uclibc-compat/Cargo.toml', *LOCKS]
    paths += git('ls-files', 'patches/*.patch', 'ziggurat/patches/*.patch',
                 'micro-thread-matter-daemon/patches/*.patch', 'firmware/hdlc-multipan/*').splitlines()
    result = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths}
    for p in SUBMODULES:
        result[p] = git('rev-parse', f'HEAD:{p}')
        software_only = os.environ.get('MICROHAD_RELEASE_SOFTWARE_ONLY') == '1'
        if software_only and p.startswith('firmware/dependencies/'):
            continue
        if git('-C', str(ROOT / p), 'rev-parse', 'HEAD') != result[p]:
            raise ValueError(f'{p}: checkout does not match parent repository pin')
    return result


def fetch_crates():
    """Download only checksum-pinned crate archives into ignored build output."""
    from concurrent.futures import ThreadPoolExecutor
    packages = {}
    for lock in LOCKS:
        for p in tomllib.loads((ROOT / lock).read_text())['package']:
            if p.get('source', '').startswith('registry+'):
                if p['source'] != 'registry+https://github.com/rust-lang/crates.io-index':
                    raise ValueError(f'Unreviewed registry: {p["source"]}')
                packages[(p['name'], p['version'])] = p['checksum']
    cache = ROOT / 'build/license-crates'
    cache.mkdir(parents=True, exist_ok=True)
    def fetch(item):
        (name, version), checksum = item
        target = cache / f'{name}-{version}.crate'
        if not target.exists():
            cargo = Path(os.environ.get('CARGO_HOME', Path.home() / '.cargo'))
            matches = sorted((cargo / 'registry/cache').glob(f'*/{target.name}'))
            if matches:
                target.write_bytes(matches[0].read_bytes())
            else:
                subprocess.run(['curl', '--fail', '--silent', '--show-error', '--location', '--retry', '2',
                                f'https://static.crates.io/crates/{name}/{target.name}', '-o', str(target)], check=True)
        if hashlib.sha256(target.read_bytes()).hexdigest() != checksum:
            raise ValueError(f'Crate checksum mismatch: {target.name}')
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(fetch, sorted(packages.items())))
    print(f'Verified {len(packages)} crate archives')


def collect_gateway():
    records = [{'name': 'MicroHAD original code', 'license': 'MIT', 'texts': [('LICENSE', (ROOT / 'LICENSE').read_text())]}]
    for folder, license_name in SUBMODULES.items():
        root = ROOT / folder
        records.append({'name': folder + '@' + inputs()[folder], 'license': license_name,
                        'texts': texts(root, recursive=folder.endswith(('openthread', 'gecko-sdk', 'mbedtls')))})
    packages = {}
    for lock in LOCKS:
        for p in tomllib.loads((ROOT / lock).read_text())['package']:
            if p.get('source', '').startswith('git+'):
                revision = p['source'].rsplit('#', 1)[-1]
                key = p['name'] + '@' + revision
                supplements = json.loads((ROOT / 'third-party/supplements.json').read_text())
                if key not in supplements:
                    raise ValueError('Missing Git dependency attribution: ' + key)
                extra = []
                for filename, digest in sorted(supplements[key]['files'].items()):
                    data = (ROOT / 'third-party/supplements' / key / filename).read_bytes()
                    if hashlib.sha256(data).hexdigest() != digest:
                        raise ValueError('Git supplement checksum mismatch: ' + key)
                    extra.append((filename, data.decode()))
                records.append({'name': key + ' (' + p['source'] + ')',
                                'license': 'Apache-2.0 selected from Apache-2.0 OR MIT', 'texts': extra})
            elif p.get('source', '').startswith('registry+'):
                packages[(p['name'], p['version'])] = p
            elif p.get('source'):
                raise ValueError('Unreviewed Rust source: ' + p['source'])
    for (name, version), p in sorted(packages.items()):
        archive = ROOT / f'build/license-crates/{name}-{version}.crate'
        if not archive.exists():
            raise ValueError(f'Missing {archive.name}; run notices.py fetch-crates explicitly')
        data = archive.read_bytes()
        if hashlib.sha256(data).hexdigest() != p['checksum']:
            raise ValueError(f'Crate checksum mismatch: {name}-{version}')
        with tarfile.open(fileobj=io.BytesIO(data)) as tf:
            members = [m for m in tf.getmembers() if m.isfile()]
            manifest = next(m for m in members if m.name == f'{name}-{version}/Cargo.toml')
            package = tomllib.loads(tf.extractfile(manifest).read().decode())['package']
            license_name = package.get('license') or package.get('license-file')
            if not license_name:
                raise ValueError(f'Missing license metadata: {name}-{version}')
            selected = [m for m in members if LEGAL.match(Path(m.name).name)
                        or any(x.lower() in ('license', 'licenses') for x in Path(m.name).parts[1:-1])
                        or m.name == f'{name}-{version}/' + package.get('license-file', '\0')]
            if not selected:
                supplements = json.loads((ROOT / 'third-party/supplements.json').read_text())
                key = f'{name}-{version}'
                supplement = supplements.get(key)
                if not supplement:
                    raise ValueError(f'Missing license text: {key}')
                extra = []
                for filename, digest in sorted(supplement['files'].items()):
                    data = (ROOT / 'third-party/supplements' / key / filename).read_bytes()
                    if hashlib.sha256(data).hexdigest() != digest:
                        raise ValueError(f'Supplement checksum mismatch: {key}/{filename}')
                    extra.append((filename, data.decode()))
                records.append({'name': key + ' (license supplement: ' + supplement['source'] + ')',
                                'license': license_name, 'texts': extra})
                continue
            records.append({'name': f'{name} {version} (crates.io, sha256 {p["checksum"]})',
                            'license': license_name,
                            'texts': [(m.name.split('/', 1)[1], tf.extractfile(m).read().decode()) for m in sorted(selected, key=lambda m: m.name)]})
    lock = json.loads((ROOT / 'microhad/webui/package-lock.json').read_text())
    for folder, p in sorted(lock['packages'].items()):
        if not folder or p.get('dev'):
            continue
        root = ROOT / 'microhad/webui' / folder
        package = json.loads((root / 'package.json').read_text())
        if package['version'] != p['version']:
            raise ValueError(f'Stale npm install: {folder}')
        records.append({'name': f'{package["name"]} {p["version"]}', 'license': package.get('license', ''), 'texts': texts(root)})
    return records


def render(records):
    # Deduplicate identical full texts without discarding distinct copyright notices.
    sections, references = {}, []
    for record in sorted(records, key=lambda r: r['name']):
        lines = [record['name'], 'License: ' + record['license']]
        for name, text in record['texts']:
            digest = hashlib.sha256(text.encode()).hexdigest()
            sections[digest] = text
            lines.append(f'  {name}: text {digest}')
        references.append('\n'.join(lines))
    return ('MicroHAD licenses and third-party notices\n\n'
            'Includes a conservative superset of locked Rust dependencies (including host/test/other-platform packages).\n'
            'Listing SDK terms is not a grant of redistribution permission. See THIRD_PARTY.md for release restrictions.\n\n'
            + '\n\n'.join(references) + '\n\n' + '\n\n'.join('TEXT ' + k + '\n' + sections[k] for k in sorted(sections)) + '\n')


def page(text):
    return ('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">'
            '<title>MicroHAD — Open-source licenses</title><style>body{max-width:72rem;margin:2rem auto;padding:0 1rem;'
            'font:16px system-ui}pre{white-space:pre-wrap;overflow-wrap:anywhere}</style>'
            '<h1>Open-source licenses</h1><a href="/">Back to MicroHAD</a><pre>' + html.escape(text) + '</pre></html>')


def check_snapshot():
    snapshot = json.loads((ROOT / 'third-party/gateway-inputs.json').read_text())
    if snapshot['inputs'] != inputs():
        raise ValueError('Gateway notices are stale; run fetch-crates, then refresh after reviewing dependency changes')
    for name, record in json.loads((ROOT / 'third-party/supplements.json').read_text()).items():
        for filename, digest in record['files'].items():
            if hashlib.sha256((ROOT / 'third-party/supplements' / name / filename).read_bytes()).hexdigest() != digest:
                raise ValueError(f'Supplement checksum mismatch: {name}/{filename}')
    text = (ROOT / 'third-party/gateway-notices.txt').read_bytes()
    if hashlib.sha256(text).hexdigest() != snapshot['sha256']:
        raise ValueError('Gateway notice text does not match its recorded checksum')
    return text.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['fetch-crates', 'refresh', 'check', 'web', 'release'])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.command == 'fetch-crates':
        fetch_crates()
    elif args.command == 'refresh':
        text = render(collect_gateway())
        (ROOT / 'third-party/gateway-notices.txt').write_text(text)
        (ROOT / 'third-party/gateway-inputs.json').write_text(json.dumps({'inputs': inputs(), 'sha256': hashlib.sha256(text.encode()).hexdigest()}, indent=2, sort_keys=True) + '\n')
    else:
        text = check_snapshot()
        if args.command == 'web':
            if not args.output:
                parser.error('web requires --output')
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(page(text))
        elif args.command == 'release':
            if not args.output:
                parser.error('release requires --output')
            args.output.mkdir(parents=True, exist_ok=True)
            (args.output / 'LICENSE').write_bytes((ROOT / 'LICENSE').read_bytes())
            (args.output / 'THIRD_PARTY.md').write_bytes((ROOT / 'THIRD_PARTY.md').read_bytes())
            (args.output / 'THIRD_PARTY_NOTICES.txt').write_text(text)
            (args.output / 'gateway-inputs.json').write_bytes((ROOT / 'third-party/gateway-inputs.json').read_bytes())
        print('Notices verified')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, FileNotFoundError) as error:
        raise SystemExit(str(error))
