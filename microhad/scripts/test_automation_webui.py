#!/usr/bin/env python3
"""Run script editor workflows against mocked HTTP and browser boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='automation-webui-') as build:
    subprocess.run([str(root / 'webui/node_modules/.bin/tsc'),
                    str(root / 'webui/src/AutomationScripts.tsx'), str(root / 'webui/src/Dashboard.tsx'), '--target', 'ES2020',
                    '--module', 'commonjs', '--jsx', 'react-jsx', '--strict', '--skipLibCheck',
                    '--outDir', build], check=True, cwd=root)
    subprocess.run(['node', '--test', str(root / 'webui/test/automation.test.cjs')], check=True,
                   cwd=root, env=dict(os.environ, AUTOMATION_WEBUI_BUILD=build))
