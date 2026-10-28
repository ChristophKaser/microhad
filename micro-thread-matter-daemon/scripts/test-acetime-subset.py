#!/usr/bin/env python3
"""Validate the production timezone bridge and its separate half-hour DST fixture.

Default: offline, uses checked-in reference values from pinned IANA 2025b.
--iana-source DIR --write-reference: regenerate those values using system zic
and the checksum-pinned IANA source files, never the host's installed TZDB.
"""
import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts'))
from toolchain_config import settings
SPEC = importlib.util.spec_from_file_location('subset', Path(__file__).with_name('generate-acetime-subset.py'))
subset = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(subset)
VENDOR = ROOT/'third-party/acetimec'
REFERENCE = ROOT/'micro-thread-matter-daemon/tests/fixtures/acetime-reference.json'
SOURCES = ['africa', 'antarctica', 'asia', 'australasia', 'backward', 'etcetera', 'europe', 'northamerica', 'southamerica']
NATIVE = ['acetimec/common.c', 'acetimec/epoch.c', 'acetimec/plain_date.c',
          'acetimec/plain_date_time.c', 'acetimec/plain_time.c', 'acetimec/date_tuple.c',
          'acetimec/transition.c', 'acetimec/zone_processor.c', 'zoneinfo/zone_info_utils.c']
MINIMUM, MAXIMUM = 1735689600, 4102444800


def reference(source):
    with tempfile.TemporaryDirectory(prefix='acetime-reference-') as tmp:
        subprocess.run(['zic','-b','fat','-d',tmp,*[str(source/n) for n in SOURCES]],check=True)
        result = {}
        for name in subset.names()+['Australia/Lord_Howe']:
            with (Path(tmp)/name).open('rb') as file: tz = ZoneInfo.from_file(file)
            def offsets(unix):
                dt = datetime.fromtimestamp(unix,timezone.utc).astimezone(tz)
                total = int(dt.utcoffset().total_seconds())
                return total-int(dt.dst().total_seconds()), total
            times = {MINIMUM, MINIMUM+86400, MAXIMUM-1}
            # Epoch edge cases and future years catch accidental 32-bit time_t.
            for year in [2025,2026,2027,2028,2038,2050,2098,2099]:
                for month in [1,7]: times.add(int(datetime(year,month,1,tzinfo=timezone.utc).timestamp()))
            # Three years of daily checkpoints plus exact transition boundaries.
            start = int(datetime(2026,1,1,tzinfo=timezone.utc).timestamp())
            end = int(datetime(2029,1,1,tzinfo=timezone.utc).timestamp())
            for lo in range(start,end,86400):
                hi = min(lo+86400,end)
                if offsets(lo) != offsets(hi):
                    old = offsets(lo)
                    while hi-lo > 1:
                        mid = (lo+hi)//2
                        if offsets(mid) == old: lo=mid
                        else: hi=mid
                    times.update([hi-1,hi,hi+1])
            result[name] = [[t,*offsets(t)] for t in sorted(times)]
    pin = json.loads((VENDOR/'UPSTREAM.json').read_text())
    return {'tzdb':pin['tzdb'], 'iana_revision':'7e1145bfdb9630c127841dc8ce808a937a300938',
            'iana_sources':{n:hashlib.sha256((source/n).read_bytes()).hexdigest() for n in SOURCES}, 'zones':result}


def check_database(lib, data, fixture):
    lib.gateway_tz_find.argtypes=[ctypes.c_char_p,ctypes.c_size_t]
    lib.gateway_tz_find.restype=ctypes.c_int32
    lib.gateway_tz_offsets.argtypes=[ctypes.c_uint32,ctypes.c_int64,ctypes.POINTER(ctypes.c_int32),ctypes.POINTER(ctypes.c_int32)]
    lib.gateway_tz_offsets.restype=ctypes.c_int32
    count=0
    for name, cases in data['zones'].items():
        encoded=name.encode();index=lib.gateway_tz_find(encoded,len(encoded))
        if name == 'Australia/Lord_Howe' and not fixture:
            assert index == -1, 'half-hour fixture leaked into production'
            continue
        assert index >= 0, name
        for unix, standard, total in cases:
            a,b=ctypes.c_int32(),ctypes.c_int32()
            status=lib.gateway_tz_offsets(index,unix,ctypes.byref(a),ctypes.byref(b))
            assert status == 0 and (a.value,b.value)==(standard,total), (name,unix,status,(a.value,b.value),(standard,total))
            count+=1
        for unix in [MINIMUM-1,MAXIMUM,-1,2**63-1]:
            a,b=ctypes.c_int32(42),ctypes.c_int32(43)
            assert lib.gateway_tz_offsets(index,unix,ctypes.byref(a),ctypes.byref(b)) == -1
            assert (a.value,b.value)==(42,43)
    for name in [b'Europe/Dublin',b'Europe/Berlin\0suffix',b'',b'invalid']:
        assert lib.gateway_tz_find(name,len(name)) == -1
    a,b=ctypes.c_int32(),ctypes.c_int32()
    assert lib.gateway_tz_offsets(2**32-1,MINIMUM,ctypes.byref(a),ctypes.byref(b)) == -1
    print(f'PASS {"fixture" if fixture else "production"}: {count} pinned IANA offset checks; shared host workspace {lib.gateway_tz_workspace_size()} bytes')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iana-source',type=Path)
    parser.add_argument('--write-reference',action='store_true')
    args=parser.parse_args()
    if args.write_reference:
        if not args.iana_source: parser.error('--write-reference requires --iana-source')
        actual = subprocess.check_output(['git','-C',str(args.iana_source),'rev-parse','HEAD'],text=True).strip()
        if actual != '7e1145bfdb9630c127841dc8ce808a937a300938': raise ValueError('IANA source revision mismatch')
        data=reference(args.iana_source)
        REFERENCE.write_text(json.dumps(data,indent=2)+'\n')
    data=json.loads(REFERENCE.read_text())
    if args.iana_source:
        regenerated=reference(args.iana_source)
        assert regenerated == data, 'IANA reference values changed'
    ui=(ROOT/'microhad/webui/src/devices.tsx').read_text().split('const timeZones = [',1)[1].split(']',1)[0]
    assert re.findall(r"'([^']+)'",ui) == subset.names(), 'dashboard/whitelist mismatch'
    assert set(data['zones']) == set(subset.names()+['Australia/Lord_Howe'])
    with tempfile.TemporaryDirectory(prefix='acetime-check-') as tmp:
        engine = subset.prepare_engine(Path(tmp)/'engine')/'src'
        assert subset.generate(VENDOR/'upstream', subset.names()) == (VENDOR/'zones.c').read_text()
        assert subset.generate(VENDOR/'upstream', subset.names()+['Australia/Lord_Howe']) == (ROOT/'micro-thread-matter-daemon/tests/fixtures/acetime-zones.c').read_text()
        exports=Path(tmp)/'exports.map'
        exports.write_text('{global: gateway_tz_find; gateway_tz_offsets; gateway_tz_workspace_size; local: *;};')
        for fixture in [False,True]:
            db=ROOT/'micro-thread-matter-daemon/tests/fixtures/acetime-zones.c' if fixture else VENDOR/'zones.c'
            output=Path(tmp)/('fixture.so' if fixture else 'production.so')
            subprocess.run([str(settings()['HOST_CC']),'-std=gnu99','-Os','-DGATEWAY_ACETIME_OFFSETS_ONLY',
                '-fPIC','-ffunction-sections','-fdata-sections','-shared','-Wl,--gc-sections','-Wl,--no-undefined',
                '-Wl,--version-script='+str(exports),'-I'+str(engine),'-I'+str(VENDOR),
                *[str(engine/n) for n in NATIVE],str(db),str(ROOT/'micro-thread-matter-daemon/timezone/bridge.c'),'-o',str(output)],check=True)
            symbols=subprocess.check_output(['nm','--defined-only',str(output)],text=True)
            assert not re.search(r'\batc_(?:print_|buf_|processor_find_by_plain_date_time|processor_create_abbreviation)',symbols)
            check_database(ctypes.CDLL(str(output)),data,fixture)

if __name__=='__main__': main()
