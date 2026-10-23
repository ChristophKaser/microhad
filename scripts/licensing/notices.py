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
import shlex
import subprocess
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parents[2]
LEGAL = re.compile(r'^(licen[cs]e|copying|copyright|notice)(?:$|[._-])', re.I)
SUBMODULES = {
 'microhad/dependencies/mbedtls': 'Apache-2.0 (selected from Apache-2.0 OR GPL-2.0-or-later)',
 'microhad/dependencies/tinycbor': 'MIT',
 'microhad/dependencies/lua': 'MIT',
 'ziggurat/dependencies/ziggurat': 'Apache-2.0; locally patched',
 'micro-thread-matter-daemon/dependencies/rs-matter': 'Apache-2.0; locally patched',
 'micro-thread-matter-daemon/dependencies/openthread': 'BSD-3-Clause and component-specific licenses; locally patched',
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
             'ziggurat/stock-uclibc-compat/Cargo.toml', 'third-party/authors.json',
             'micro-thread-matter-daemon/scripts/build.sh', 'ziggurat/scripts/build.sh',
             'scripts/rust-env.sh', 'config/toolchains.mk', *LOCKS]
    paths += git('ls-files', 'patches/*.patch', 'ziggurat/patches/*.patch',
                 'micro-thread-matter-daemon/patches/*.patch').splitlines()
    result = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in paths}
    for p in SUBMODULES:
        result[p] = git('rev-parse', f'HEAD:{p}')
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


def production_packages():
    """Resolve only target normal dependencies using the production manifests/patches.

    Host proc macros and build tools are not linked into the distributed binaries.
    The built-in target has the same dependency-relevant cfg as our custom target.
    """
    commands = [
        ['--manifest-path', 'micro-thread-matter-daemon/Cargo.toml', '--features', 'controller-service'],
        ['--manifest-path', 'worktrees/ziggurat-multipan/Cargo.toml', '-p', 'ziggurat-server'],
    ]
    selected = set()
    for index, args in enumerate(commands):
        if index:
            for crate in ('getrandom-0.4.3', 'nix-0.26.4', 'radium-0.7.0'):
                name = crate.rsplit('-', 1)[0]
                args += ['--config', f'patch.crates-io.{name}.path="{ROOT}/build/patched-crates-mailbox/{crate}"']
        command = ['cargo', '+$RUST_TOOLCHAIN', 'tree', '--offline', '--locked',
                   '--target', 'mips-unknown-linux-uclibc', '-e', 'normal,no-proc-macro',
                   '--prefix', 'none', '--format', '{p}', *args]
        shell = '. scripts/rust-env.sh\n' + ' '.join(
            x if x == '+$RUST_TOOLCHAIN' else shlex.quote(x) for x in command)
        output = subprocess.check_output(['bash', '-c', shell], cwd=ROOT, text=True)
        for line in output.splitlines():
            match = re.match(r'^(\S+) v(\S+)', line)
            if match:
                selected.add(match.groups())
    return selected


def authors(package, name):
    overrides = json.loads((ROOT / 'third-party/authors.json').read_text())
    value = overrides.get(name) or package.get('authors') or package.get('author')
    if isinstance(value, dict):
        value = value['name']
    if isinstance(value, str):
        value = [value]
    if not value:
        raise ValueError('Missing reviewed author attribution: ' + name)
    return value


def collect_gateway():
    selected_packages = production_packages()
    records = []
    known_packages = {(p['name'], p['version']) for lock in LOCKS
                      for p in tomllib.loads((ROOT / lock).read_text())['package']}
    if selected_packages - known_packages:
        raise ValueError('Production graph contains packages absent from notice locks: ' +
                         str(sorted(selected_packages - known_packages)))
    revisions = inputs()
    for folder, license_name in SUBMODULES.items():
        root = ROOT / folder
        if root.name == 'lua':
            header = (root / 'lua.h').read_text()
            notice = re.search(r'/\*+\n\* Copyright .*?\n\*+/', header, re.S)
            if not notice:
                raise ValueError('Missing Lua copyright notice')
            license_texts = [('lua.h copyright notice', notice.group())]
        else:
            license_texts = texts(root)
        # Include linked crypto libraries, not upstream tools, examples, or debug firmware.
        bundled = {'mbedtls': ['tf-psa-crypto'],
                   'openthread': ['third_party/mbedtls/repo']}.get(root.name, [])
        for child in bundled:
            license_texts += [(child + '/' + name, text) for name, text in texts(root / child)]
        records.append({'name': root.name, 'revision': revisions[folder], 'license': license_name,
                        'authors': authors({}, folder), 'texts': license_texts})
    packages = {}
    for lock in LOCKS:
        for p in tomllib.loads((ROOT / lock).read_text())['package']:
            if (p['name'], p['version']) not in selected_packages:
                continue
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
                    if LEGAL.match(filename):
                        extra.append((filename, data.decode()))
                records.append({'name': f'{p["name"]} {p["version"]}', 'revision': revision,
                                'license': 'Apache-2.0 selected from Apache-2.0 OR MIT',
                                'authors': authors({}, p['name']), 'texts': extra})
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
                    # Embassy supplements include unrelated RP/CYW43 firmware and documentation.
                    if name.startswith('embassy-') and filename not in ('LICENSE-APACHE', 'LICENSE-MIT', 'NOTICE.md'):
                        continue
                    data = (ROOT / 'third-party/supplements' / key / filename).read_bytes()
                    if hashlib.sha256(data).hexdigest() != digest:
                        raise ValueError(f'Supplement checksum mismatch: {key}/{filename}')
                    extra.append((filename, data.decode()))
                records.append({'name': f'{name} {version}',
                                'license': license_name, 'authors': authors(package, name), 'texts': extra})
                continue
            records.append({'name': f'{name} {version}',
                            'license': license_name, 'authors': authors(package, name),
                            'texts': [(m.name.split('/', 1)[1], tf.extractfile(m).read().decode()) for m in sorted(selected, key=lambda m: m.name)]})
    lock = json.loads((ROOT / 'microhad/webui/package-lock.json').read_text())
    for folder, p in sorted(lock['packages'].items()):
        if not folder or p.get('dev'):
            continue
        root = ROOT / 'microhad/webui' / folder
        package = json.loads((root / 'package.json').read_text())
        if package['version'] != p['version']:
            raise ValueError(f'Stale npm install: {folder}')
        records.append({'name': f'{package["name"]} {p["version"]}', 'license': package.get('license', ''), 'authors': authors(package, package['name']), 'texts': texts(root)})
    if any(r['name'].startswith('ace-builds ') for r in records):
        records.append({'name': 'luaparse 0.3.1 (Ace Lua worker; locally patched)',
                        'license': 'MIT', 'authors': ['Oskar Schöldström'],
                        'texts': texts(ROOT / 'third-party/supplements/luaparse-0.3.1')})
    return sorted(records, key=lambda r: r['name'].casefold())


def render(records):
    # Deduplicate identical full texts without discarding distinct copyright notices.
    sections, references = {}, []
    for record in sorted(records, key=lambda r: r['name']):
        lines = [record['name'], 'Authors: ' + ', '.join(record['authors']), 'License: ' + record['license']]
        for name, text in record['texts']:
            digest = hashlib.sha256(text.encode()).hexdigest()
            sections[digest] = text
            lines.append(f'  {name}: text {digest}')
        references.append('\n'.join(lines))
    return ('MicroHAD licenses and third-party notices\n\n'
            'MicroHAD contains or depends on these third-party software libraries.\n'
            'Thank you to everyone involved in them.\n\n'
            + '\n\n'.join(references) + '\n\n' + '\n\n'.join('TEXT ' + k + '\n' + sections[k] for k in sorted(sections)) + '\n')


def page(records, runtime_links=False):
    escape = html.escape
    cards = []
    for record in records:
        licenses = ''.join('<details><summary>' + escape(name) + '</summary><pre>' +
                           escape(text) + '</pre></details>' for name, text in record['texts'])
        cards.append('<article><h2><strong>' + escape(record['name']) + '</strong></h2>'
                     '<p class="authors">' + escape(', '.join(record['authors'])) + '</p>'
                     '<p class="license">' + escape(record['license']) + '</p>' + licenses + '</article>')
    links = ('<nav aria-label="Runtime notices"><a href="/runtime-licenses.html">C/C++ runtime licenses</a>'
             ' · <a href="/rust-licenses.html">Rust standard-library notices</a></nav>') if runtime_links else ''
    return ('''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MicroHAD — Open-source licenses</title><style>
*{box-sizing:border-box}body{margin:0;background:#f3f6f9;color:#172b41;font:16px/1.6 system-ui,sans-serif}
main{max-width:960px;margin:auto;padding:32px 20px 64px}a{color:#315f85}a:hover{text-decoration-thickness:2px}
a:focus-visible,summary:focus-visible{outline:3px solid #3577ab;outline-offset:4px}
header{margin:24px 0 32px}h1{font-size:clamp(1.8rem,5vw,2.6rem);line-height:1.2;letter-spacing:-.03em}
header p,.authors{color:#5f7183}article{background:#fff;border:1px solid #dce5ed;border-radius:16px;padding:24px;margin:16px 0}
h2{font-size:1.15rem;margin:0;overflow-wrap:anywhere}p{margin:8px 0}.license{font-size:.85rem;color:#52718d}
details{border-top:1px solid #e4ebf1;margin-top:12px}summary{cursor:pointer;padding:12px 0;font-size:.9rem;overflow-wrap:anywhere}
pre{white-space:pre-wrap;overflow-wrap:anywhere;font:13px/1.6 ui-monospace,monospace;background:#f3f6f9;padding:16px;border-radius:8px}
@media(max-width:480px){main{padding:20px 12px 40px}article{padding:16px}pre{padding:12px}}
</style></head><body><main><a href="/">← Back to MicroHAD</a><header><h1>Open-source licenses</h1>
<p>MicroHAD contains or depends on these third-party software libraries. Thank you to everyone involved in them.</p>
''' + links + '</header>' + ''.join(cards) + '</main></body></html>')


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
    records = (ROOT / 'third-party/gateway-records.json').read_bytes()
    if hashlib.sha256(records).hexdigest() != snapshot['records_sha256']:
        raise ValueError('Gateway structured notices do not match their recorded checksum')
    if render(json.loads(records)) != text.decode():
        raise ValueError('Gateway notice inventories disagree')
    return text.decode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['fetch-crates', 'refresh', 'check', 'web', 'release'])
    parser.add_argument('--output', type=Path)
    parser.add_argument('--runtime-links', action='store_true')
    args = parser.parse_args()
    if args.command == 'fetch-crates':
        fetch_crates()
    elif args.command == 'refresh':
        records = collect_gateway()
        structured = json.dumps(records, indent=2, ensure_ascii=False) + '\n'
        (ROOT / 'third-party/gateway-records.json').write_text(structured)
        text = render(records)
        (ROOT / 'third-party/gateway-notices.txt').write_text(text)
        (ROOT / 'third-party/gateway-inputs.json').write_text(json.dumps({'inputs': inputs(), 'sha256': hashlib.sha256(text.encode()).hexdigest(), 'records_sha256': hashlib.sha256(structured.encode()).hexdigest()}, indent=2, sort_keys=True) + '\n')
    else:
        text = check_snapshot()
        if args.command == 'web':
            if not args.output:
                parser.error('web requires --output')
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(page(json.loads((ROOT / 'third-party/gateway-records.json').read_text()), args.runtime_links))
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
