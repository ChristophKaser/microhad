#!/usr/bin/env python3
"""Prepare the pinned AceTime C engine in a build-owned output directory."""
import importlib.util
from pathlib import Path
import shutil
import sys

spec = importlib.util.spec_from_file_location('subset', Path(__file__).with_name('generate-acetime-subset.py'))
subset = importlib.util.module_from_spec(spec)
spec.loader.exec_module(subset)
# Only this fixed child of the caller's build output is replaced.
destination = Path(sys.argv[1]).resolve() / 'acetime-engine'
if destination.exists():
    shutil.rmtree(destination)
subset.prepare_engine(destination)
