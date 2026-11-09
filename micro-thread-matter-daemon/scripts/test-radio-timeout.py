#!/usr/bin/env python3
"""Link a clock-controlled radio test against the actual host transport build."""
from pathlib import Path
import shlex
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
target = build / 'src/posix/CMakeFiles/ot-unix-adapter-test.dir'
flags = {}
for line in (target / 'flags.make').read_text().splitlines():
    if line.startswith(('CXX_DEFINES =', 'CXX_INCLUDES =', 'CXX_FLAGS =')):
        key, value = line.split('=', 1)
        flags[key.strip()] = shlex.split(value)
link = shlex.split((target / 'link.txt').read_text())
obj = build / 'radio-timeout.o'
binary = build / 'radio-timeout-test'
subprocess.run([link[0], *flags['CXX_DEFINES'], *flags['CXX_INCLUDES'],
                *flags['CXX_FLAGS'], '-fno-access-control', '-c',
                str(root / 'micro-thread-matter-daemon/tests/radio-timeout.cpp'),
                '-o', str(obj)], check=True)
link[link.index('-o') + 1] = str(binary)
link = [str(obj) if word.endswith('.cpp.o') else word for word in link]
first_archive = next(i for i, word in enumerate(link) if word.endswith('.a'))
link.insert(first_archive, '-Wl,--start-group')
link.append('-Wl,--end-group')
subprocess.run([*link, '-Wl,--wrap=otPlatTimeGet'], cwd=build / 'src/posix', check=True)
subprocess.run([str(binary)], check=True)
