#!/usr/bin/env python3
"""Pinned, host-only simulated controller tests; never deploys or uses a real radio."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = '1570356ecb938a9d19be032b93ca3bb487069e75'
SOURCE = ROOT / 'dependencies/rs-matter'
WORKTREE = ROOT / 'build/rs-matter/controller-source'
PATCH = ROOT / 'patches/rs-matter-commissioning-probe.patch'


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def main():
    run(['bash', str(ROOT / 'scripts/fetch-rs-matter.sh')])
    if not WORKTREE.exists():
        run(['git', '-C', str(SOURCE), 'worktree', 'add', '--detach', str(WORKTREE), REVISION])
    run(['python3', str(ROOT / 'scripts/apply-mailbox-series.py'), str(WORKTREE), REVISION,
         str(PATCH), str(ROOT / 'patches/rs-matter-probe-commissioning-region.patch')])
    if subprocess.check_output(['git', '-C', str(WORKTREE), 'ls-files', '--others', '--exclude-standard']):
        raise RuntimeError('Unexpected untracked controller source files')
    shutil.copyfile(ROOT / 'rs-matter-probe/controller-tests.lock', WORKTREE / 'Cargo.lock')
    env = dict(os.environ)
    env.update(CARGO_HOME=str(ROOT / 'tools/cargo'), RUSTUP_HOME=str(ROOT / 'tools/rustup'),
               CARGO_TARGET_DIR=str(ROOT / 'build/rs-matter/controller-target'), RUST_LOG='warn',
               CARGO_INCREMENTAL='0')
    env.pop('RUSTFLAGS', None)
    env['PATH'] = str(ROOT / 'tools/cargo/bin') + os.pathsep + env['PATH']
    cargo = ['cargo', '+nightly-2026-10-02', 'test', '--locked', '--release', '--manifest-path',
             str(WORKTREE / 'Cargo.toml'), '-p', 'rs-matter', '--no-default-features',
             '--features', 'os rustcrypto']
    suites = [
        ('commissioning', ['--test', 'commissioning']),
        ('pase', ['--test', 'pase']),
        ('case', ['--test', 'case']),
        ('attestation_components', ['--lib', 'attest::']),
    ]
    report = {'source_revision': REVISION, 'rust_toolchain': 'nightly-2026-10-02',
              'platform': 'development host', 'device_attestation_verified': False,
              'discovery': 'stub resolver; no mDNS network discovery',
              'real_thread_or_ble': False, 'thread_dataset_provisioning_tested': False,
              'manufacturer_attestation_policy': 'skip verification, selected by gateway owner',
              'secure_default_rejection_tested': True, 'sensor_values_hundredths_celsius': [2315, -525, None],
              'suites': []}
    report_path = ROOT / 'build/rs-matter/controller-validation.json'
    for name, selection in suites:
        log_path = ROOT / ('build/rs-matter/controller-' + name + '.log')
        with log_path.open('w') as log:
            result = subprocess.run(cargo + selection + ['--', '--test-threads=1'], env=env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=600)
        output = log_path.read_text()
        summary = re.search(r'test result: (\w+)\. (\d+) passed; (\d+) failed;', output)
        entry = {'name': name, 'exit_code': result.returncode,
                 'passed': int(summary[2]) if summary else 0,
                 'failed': int(summary[3]) if summary else None}
        timing = re.search(r'finished in ([0-9.]+)s', output)
        if timing:
            entry['elapsed_seconds'] = float(timing[1])
        report['suites'].append(entry)
        if name == 'commissioning' and result.returncode == 0 and summary and entry['passed'] >= 5:
            report['thread_dataset_provisioning_tested'] = True
            report['network_failure_responses_tested'] = True
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(entry), flush=True)
        if result.returncode or not summary or not entry['passed'] or entry['failed']:
            raise RuntimeError('Controller test failed; inspect ' + str(log_path))
    print('Host-only provisioning passed with owner-selected attestation bypass; OpenThread/BLE integration remains required.', flush=True)


if __name__ == '__main__':
    main()
