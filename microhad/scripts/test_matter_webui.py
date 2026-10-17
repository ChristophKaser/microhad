#!/usr/bin/env python3
"""Compile the production Matter client and exercise it against actual localhost HTTP."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='matter-webui-') as build:
    subprocess.run([str(root / 'webui/node_modules/.bin/tsc'),
                    str(root / 'webui/src/matter.ts'), str(root / 'webui/src/browserMatter.ts'), '--target', 'ES2020',
                    '--module', 'commonjs', '--strict', '--skipLibCheck',
                    '--outDir', build], check=True, cwd=root)
    env = dict(os.environ, MATTER_WEBUI_MODULE=str(Path(build) / 'matter.js'),
               BROWSER_MATTER_WEBUI_MODULE=str(Path(build) / 'browserMatter.js'))
    subprocess.run(['node', '--test', str(root / 'webui/test/matter.test.cjs')],
                   check=True, cwd=root, env=env)
    subprocess.run(['node', '--test', str(root / 'webui/test/browserMatter.test.cjs')],
                   check=True, cwd=root, env=env)
