#!/usr/bin/env python3
"""Run native IPC framing vectors on host and optionally the factory gateway; no radio or state access."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'scripts'))
from toolchain_config import settings

TOOLCHAIN = settings()
RSDK = TOOLCHAIN['RSDK_ROOT']
from workspace_config import setting, gateway_host, has_credentials, ssh_command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', nargs='?', const=setting('GATEWAY_IP'), type=gateway_host)
    parser.add_argument('--password-file', type=Path)
    args = parser.parse_args()
    if args.host and not has_credentials(args.password_file):
        parser.error('--host requires configured SSH credentials or --password-file')
    report = {'host': False, 'factory_mips': False, 'real_radio_access': False}
    with tempfile.TemporaryDirectory(prefix='matter-ipc-codec-') as directory:
        directory = Path(directory)
        protobuf_include = ROOT.parent / 'third-party/nanopb'
        driver = directory / 'driver.cpp'
        status_vector = json.loads((ROOT.parent/'protocol/matter-wire-vectors.json').read_text())[0]
        status_bytes = ','.join(str(v) for v in bytes.fromhex(status_vector['wire']))
        driver.write_text('#include <cstdio>\n#include <fstream>\n#include <string>\n#include <cassert>\n#include "matter_protocol.h"\nvoid runMatterProtocolUnitTests();\n'
                          'void checkStatusEncoding() { MatterWire::Frame frame; frame.opcode=1; frame.id=1; '
                          'MatterWire::Bytes bytes; assert(MatterWire::encode(frame, &bytes)); '
                          'const unsigned char expected[]={' + status_bytes + '}; '
                          'assert(bytes==MatterWire::Bytes(expected, expected+sizeof(expected))); }\n'
                          'int main() { checkStatusEncoding(); runMatterProtocolUnitTests(); puts("PASS Matter IPC codec"); '
                          'std::ifstream status("/proc/self/status"); std::string line; '
                          'while (std::getline(status, line)) if (line.compare(0, 6, "VmHWM:")==0) puts(line.c_str()); return 0; }\n')
        sources = [str(driver), str(ROOT / 'src/matter_protocol.cpp'), str(ROOT / 'src/matter_protobuf.cpp'), str(ROOT / 'src/protobuf_value.cpp'), str(ROOT / 'src/json_util.cpp'), str(ROOT / 'test/unit_matter_protocol.cpp')]
        protobuf_names = ['pb_common', 'pb_encode', 'pb_decode']
        def protobuf_objects(compiler, suffix):
            objects=[]
            for name in protobuf_names:
                output=directory/(name+suffix+'.o')
                subprocess.run([compiler, '-std=gnu99', '-O2', '-DPB_C99_STATIC_ASSERT', '-I'+str(protobuf_include), '-c', str(protobuf_include/(name+'.c')), '-o', str(output)], check=True)
                objects.append(str(output))
            return objects
        host_objects = protobuf_objects(TOOLCHAIN['HOST_CC'], '-host')
        host = directory / 'host'
        subprocess.run(['g++', '-std=gnu++03', '-Wall', '-Wextra', '-Werror', '-pedantic', '-O2',
                        '-I' + str(ROOT / 'include'), '-I'+str(protobuf_include), *sources, *host_objects, '-o', str(host)], check=True)
        subprocess.run([str(host)], check=True)
        report['host'] = True
        if args.host:
            target_objects = protobuf_objects(str(TOOLCHAIN['TARGET_CC']), '-mips')
            target = directory / 'mips'
            subprocess.run([str(TOOLCHAIN['TARGET_CXX']), '-Wall', '-Wextra', '-Werror', '-pedantic',
                            '-Os', '-msoft-float', '-static', '-I' + str(ROOT / 'include'), '-I'+str(protobuf_include), *sources, *target_objects,
                            '-o', str(target)], check=True)
            subprocess.run([str(TOOLCHAIN['TARGET_STRIP']), str(target)], check=True)
            ssh = ssh_command(args.host, args.password_file)
            stage = '/tmp/matter-ipc-' + uuid.uuid4().hex
            def remote(command, payload=None):
                return subprocess.run(ssh + [command], input=payload, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, check=True, timeout=15).stdout.decode()
            try:
                payload = target.read_bytes()
                remote('umask 077; (set -C; cat > ' + stage +
                       ') && chmod 700 ' + stage, payload)
                command = f'''ulimit -c 0; ulimit -v 4096
{stage} &
child=$!
(sleep 8; kill -9 "$child" 2>/dev/null) &
watchdog=$!
trap 'kill "$child" "$watchdog" 2>/dev/null' EXIT HUP INT TERM
wait "$child"
exit $?
'''
                output = remote('sh -c ' + shlex.quote(command))
                if 'PASS Matter IPC codec' not in output:
                    raise RuntimeError('Factory codec probe did not pass')
                report.update(factory_mips=True, binary_bytes=len(payload))
            finally:
                remote('rm -f ' + stage)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
