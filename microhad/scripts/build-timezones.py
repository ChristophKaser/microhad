#!/usr/bin/env python3
"""Build the shared offsets-only AceTime engine without modifying upstream."""
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
compiler, archiver = sys.argv[2:4]
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(root / 'micro-thread-matter-daemon/scripts/prepare-acetime.py'), str(out)], check=True)
engine = out / 'acetime-engine/src'
vendor = root / 'third-party/acetimec'
names = ['acetimec/common.c', 'acetimec/epoch.c', 'acetimec/plain_date.c',
         'acetimec/plain_date_time.c', 'acetimec/plain_time.c', 'acetimec/date_tuple.c',
         'acetimec/transition.c', 'acetimec/zone_processor.c', 'zoneinfo/zone_info_utils.c']
files = [engine / name for name in names] + [vendor / 'zones.c', root / 'micro-thread-matter-daemon/timezone/bridge.c', root / 'microhad/src/timezone_names.c']
objects = []
for index, source in enumerate(files):
    obj = out / ('timezone-%d.o' % index)
    subprocess.run([compiler, '-std=gnu99', '-DGATEWAY_ACETIME_OFFSETS_ONLY', '-Os',
                    '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra',
                    '-I' + str(engine), '-I' + str(vendor), '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
subprocess.run([archiver, 'rcs', str(out / 'timezones.a')] + objects, check=True)
