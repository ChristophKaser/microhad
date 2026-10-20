#!/usr/bin/env python3
"""Exercise production history loading, pagination, and range clipping."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='history-webui-') as build:
    subprocess.run([str(root / 'webui/node_modules/.bin/tsc'),
                    str(root / 'webui/src/historyLoader.ts'), '--target', 'ES2020',
                    '--module', 'commonjs', '--strict', '--skipLibCheck',
                    '--outDir', build], check=True, cwd=root)
    subprocess.run(['node', '--test', str(root / 'webui/test/history.test.cjs')],
                   check=True, cwd=root,
                   env=dict(os.environ, HISTORY_WEBUI_MODULE=str(Path(build) / 'historyLoader.js')))
