#!/usr/bin/env python3
"""Render offline Android notices from Gradle's resolved runtime graph."""
import argparse
import io
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
import zipfile
from notices import ROOT, page, render


def children(element, name):
    return [x for x in element if x.tag.split('}')[-1] == name]


def value(element, name):
    found = children(element, name)
    return (found[0].text or '').strip() if found else ''


def pom_metadata(path, cache, seen=None):
    seen = set() if seen is None else seen
    if path in seen:
        raise ValueError('Cyclic Maven parent: ' + str(path))
    seen.add(path)
    root = ET.parse(path).getroot()
    licenses = []
    for block in children(root, 'licenses'):
        licenses += [(value(x, 'name'), value(x, 'url')) for x in children(block, 'license')]
    authors = []
    for block in children(root, 'developers') + children(root, 'contributors'):
        authors += [value(x, 'name') for x in block if value(x, 'name')]
    if not licenses:
        parents = children(root, 'parent')
        if parents:
            group, name, version = [value(parents[0], key) for key in ('groupId', 'artifactId', 'version')]
            candidates = sorted((cache / group / name / version).glob('*/*.pom'))
            if not candidates:
                raise ValueError(f'Missing parent POM: {group}:{name}:{version}')
            licenses, inherited = pom_metadata(candidates[0], cache, seen)
            authors += inherited
    if not licenses:
        raise ValueError('Missing Maven license: ' + str(path))
    return licenses, sorted(set(authors))


def embedded(data):
    result = []
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for name in sorted(archive.namelist()):
            if name.endswith('/'):
                continue
            if re.match(r'^(licen[cs]e|notice|copyright)(?:$|[._-])', Path(name).name, re.I) or 'third_party_license' in name:
                result.append((name, archive.read(name).decode('utf-8')))
            elif name.endswith('.jar'):
                result += [(name + '/' + sub, text) for sub, text in embedded(archive.read(name))]
    return result


def collect(manifest, cache):
    records = [{'name': 'MicroHAD companion original code', 'license': 'MIT', 'texts': [('LICENSE', (ROOT / 'LICENSE').read_text())]}]
    for dependency in sorted(manifest, key=lambda x: x['coordinate']):
        coordinate = dependency['coordinate']
        pom = dependency.get('pom')
        if not pom:
            group, name, version = coordinate.split(':')
            matches = sorted((cache / group / name / version).glob('*/*.pom'))
            if not matches:
                raise ValueError('Missing resolved POM: ' + coordinate)
            pom = matches[0]
        licenses, authors = pom_metadata(Path(pom), cache)
        texts = embedded(Path(dependency['artifact']).read_bytes())
        if coordinate == 'org.slf4j:slf4j-api:1.7.36':
            texts.append(('https://github.com/qos-ch/slf4j/blob/v_1.7.36/LICENSE.txt',
                          (ROOT / 'third-party/license-texts/slf4j-1.7.36.txt').read_text()))
        for name, url in licenses:
            if 'apache' in (name + url).lower():
                texts.append(('Apache-2.0', (ROOT / 'third-party/license-texts/Apache-2.0.txt').read_text()))
            elif name in ('Android Software Development Kit License', 'ML Kit Terms of Service'):
                texts.append((name, 'This SDK is subject to ' + name + ': ' + url + '\n'
                              'This is not an MIT/Apache license grant. Embedded third-party notices are retained.'))
            elif 'mit' in name.lower():
                if not any('Permission is hereby granted' in text for _, text in texts):
                    raise ValueError('MIT copyright/license text missing for ' + coordinate)
            elif 'bsd' in name.lower():
                if not any('Redistribution and use' in text for _, text in texts):
                    raise ValueError('BSD copyright/license text missing for ' + coordinate)
            else:
                raise ValueError(f'Unreviewed Android license: {coordinate}: {name} {url}')
        if authors:
            texts.append(('Maven declared authors', '\n'.join(authors)))
        records.append({'name': coordinate, 'license': '; '.join(name + ' (' + url + ')' for name, url in licenses), 'texts': texts})
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cache', type=Path, default=Path.home() / '.gradle/caches/modules-2/files-2.1')
    args = parser.parse_args()
    text = render(collect(json.loads(args.manifest.read_text()), args.cache)).replace(
        'Includes a conservative superset of locked Rust dependencies (including host/test/other-platform packages).',
        'Android resolved runtime dependencies for this build variant; test-only dependencies excluded.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(page(text).replace('<a href="/">Back to MicroHAD</a>', ''))
    print('Android notices:', args.output)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, FileNotFoundError) as error:
        raise SystemExit(str(error))
