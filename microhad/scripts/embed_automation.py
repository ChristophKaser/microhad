#!/usr/bin/env python3
"""Embed trusted Lua scheduler in an ignored build header."""
import json
import pathlib
import sys
source = pathlib.Path(sys.argv[1]).read_text()
output = pathlib.Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(json.dumps(line + '\n', ensure_ascii=True) for line in source.splitlines()) + '\n')
