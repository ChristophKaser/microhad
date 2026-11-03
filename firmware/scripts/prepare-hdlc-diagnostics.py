#!/usr/bin/env python3
"""Patch build-output copies of the pinned SDK's HDLC implementation."""
import argparse
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def prepare(sdk, build):
    overlay = build / 'diagnostics'
    overlay.mkdir(parents=True, exist_ok=True)
    for name in ('ncp_hdlc.cpp', 'ncp_hdlc.hpp'):
        shutil.copyfile(sdk / 'util/third_party/openthread/src/ncp' / name, overlay / name)
    shutil.copyfile(ROOT / 'firmware/diagnostics/hdlc_rx_error_log.hpp', overlay / 'hdlc_rx_error_log.hpp')
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i',
                    str(ROOT / 'firmware/patches/hdlc-rx-error-frame.patch')], cwd=overlay, check=True)
    project = build / 'ot-rcp-multipan.project.mak'
    text = project.read_text()
    original = '$(SDK_PATH)/util/third_party/openthread/src/ncp/ncp_hdlc.cpp'
    if original not in text:
        raise ValueError('Generated project has no expected SDK HDLC source')
    project.write_text(text.replace(original, 'diagnostics/ncp_hdlc.cpp'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.sdk.resolve(), args.build.resolve())
