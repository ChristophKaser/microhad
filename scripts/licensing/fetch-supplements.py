#!/usr/bin/env python3
"""One-time, explicit retrieval of license files omitted from crate archives."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tomllib

ROOT = Path(__file__).resolve().parents[2]
previous = json.loads((ROOT / 'third-party/supplements.json').read_text()) if (ROOT / 'third-party/supplements.json').exists() else {}
records = {}
for archive in sorted((ROOT / 'build/license-crates').glob('*.crate')):
    with tarfile.open(archive) as tf:
        name = archive.name[:-6]
        files = [m for m in tf.getmembers() if m.isfile()]
        if any(re.match(r'^(licen[cs]e|copying|copyright|notice)(?:$|[._-])', Path(m.name).name, re.I) for m in files):
            continue
        package = tomllib.loads(tf.extractfile(name + '/Cargo.toml').read().decode())['package']
        target = ROOT / 'third-party/supplements' / name
        target.mkdir(parents=True, exist_ok=True)
        if name.startswith('qrcodegen-no-heap'):
            text = tf.extractfile(name + '/src/lib.rs').read().decode().split('*/', 1)[0] + '*/\n'
            (target / 'LICENSE').write_text(text)
            records[name] = {'source': 'checksum-pinned crate src/lib.rs license header', 'files': {'LICENSE': hashlib.sha256(text.encode()).hexdigest()}}
            continue
        repository = package['repository'].removesuffix('.git')
        if not repository.startswith('https://github.com/'):
            raise SystemExit('Review repository: ' + repository)
        vcs_member = next((m for m in files if m.name == name + '/.cargo_vcs_info.json'), None)
        if vcs_member:
            vcs = json.loads(tf.extractfile(vcs_member).read())
            revision = vcs['git']['sha1']
        else:
            # Old void crate predates cargo VCS metadata. Pin repository HEAD;
            # this reviewed exception is recorded rather than inferred from a tag.
            revision = previous.get(name, {}).get('source', '').rsplit('/', 1)[-1]
            if not re.fullmatch(r'[0-9a-f]{40}', revision):
                revision = subprocess.check_output(['git', 'ls-remote', repository, 'HEAD'], text=True).split()[0]
        api = 'https://api.github.com/repos/' + repository.split('github.com/')[1] + '/git/trees/' + revision + '?recursive=1'
        tree = json.loads(subprocess.check_output(['curl', '-fsSL', api]))
        candidates = [x['path'] for x in tree['tree'] if x['type'] == 'blob' and
                      (re.match(r'^(licen[cs]e|copying|copyright|notice)(?:$|[._-])', Path(x['path']).name, re.I)
                       or x['path'].startswith('LICENSES/')) and x['path'].count('/') <= 1]
        if not candidates:
            if not any(x in package.get('license', '').split(' OR ') for x in ('MIT', 'Apache-2.0')):
                raise SystemExit('No upstream license: ' + name)
            # The crate explicitly offers Apache-2.0 but upstream ships no text.
            # Preserve its author declaration and supply the standard license.
            selected = 'Apache-2.0' if 'Apache-2.0' in package['license'] else 'MIT'
            standard = ((ROOT / 'third-party/license-texts/Apache-2.0.txt').read_text() if selected == 'Apache-2.0'
                        else (ROOT / 'LICENSE').read_text().replace('Copyright (c) 2026 MicroHAD contributors\n\n', ''))
            text = 'Selected ' + selected + '. Declared crate authors: ' + ', '.join(package.get('authors', [])) + '\n\n' + standard
            (target / 'LICENSE').write_text(text)
            records[name] = {'source': repository + '/tree/' + revision,
                             'review': 'No upstream license file or NOTICE; license explicitly offered in crate metadata; standard text and declared authors supplied',
                             'files': {'LICENSE': hashlib.sha256(text.encode()).hexdigest()}}
            continue
        record = {'source': repository + '/tree/' + revision, 'files': {}}
        for path in sorted(candidates):
            url = repository.replace('github.com', 'raw.githubusercontent.com') + '/' + revision + '/' + path
            data = subprocess.check_output(['curl', '-fsSL', url])
            filename = path.replace('/', '__')
            (target / filename).write_bytes(data)
            record['files'][filename] = hashlib.sha256(data).hexdigest()
        records[name] = record
# The pinned Git dependency also omits a license file. Its Cargo manifests
# explicitly offer Apache-2.0 OR MIT; select Apache-2.0 and retain authors.
revision = '1f67dd91dce8a4c3b2c760ac317780856c1d6805'
source = 'https://github.com/puddly/abstract-bits'
for package, folder in [('abstract-bits', ''), ('abstract-bits-derive', 'abstract-bits-derive/')]:
    url = source.replace('github.com', 'raw.githubusercontent.com') + '/' + revision + '/' + folder + 'Cargo.toml'
    metadata = subprocess.check_output(['curl', '-fsSL', url])
    package_meta = tomllib.loads(metadata.decode())['package']
    if package_meta.get('license') != 'Apache-2.0 OR MIT':
        raise SystemExit('Review changed Git dependency license: ' + package)
    key = package + '@' + revision
    target = ROOT / 'third-party/supplements' / key
    target.mkdir(parents=True, exist_ok=True)
    text = 'Selected Apache-2.0. Declared authors: ' + ', '.join(package_meta.get('authors', [])) + '\n\n' + (ROOT / 'third-party/license-texts/Apache-2.0.txt').read_text()
    (target / 'LICENSE').write_text(text)
    (target / 'Cargo.toml').write_bytes(metadata)
    records[key] = {'source': source + '/tree/' + revision,
                    'review': 'Root Git tree omits license files; standard Apache-2.0 text supplied under declared option',
                    'files': {'LICENSE': hashlib.sha256(text.encode()).hexdigest(), 'Cargo.toml': hashlib.sha256(metadata).hexdigest()}}
(ROOT / 'third-party/supplements.json').write_text(json.dumps(records, indent=2, sort_keys=True) + '\n')
print('Collected', len(records), 'reviewable license supplements')
