#!/usr/bin/env python3
"""Benchmark native IPC encoding and decoding separately using shared synthetic vectors.

No sockets or gateway access. Run on both revisions with identical arguments;
reports median process CPU microseconds per frame (five samples).
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from toolchain_config import settings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iterations', type=int, default=3000)
    parser.add_argument('--revision', help='Compile native sources from this Git revision for a reproducible baseline')
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error('--iterations must be positive')
    names = ['empty-status', 'max-ble-bytes', 'max-dataset-bytes', 'measurement-event',
             'thermostat-schedule-job', 'repeated-512']
    vectors = {v['name']: v for v in json.loads((ROOT / 'protocol/matter-wire-vectors.json').read_text())}
    source = r'''
#include "matter_protocol.h"
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <string>
namespace MatterWire { bool decodeProtobuf(const Bytes&, Frame*); }
int main(int argc, char** argv) {
    if(argc!=3)return 1;
    MatterWire::Bytes fixture;std::string hex=argv[1];
    for(size_t i=0;i<hex.size();i+=2)fixture.push_back(static_cast<unsigned char>(std::strtoul(hex.substr(i,2).c_str(),0,16)));
    MatterWire::Decoder decoder;MatterWire::Frame frame;
    if(!decoder.feed(fixture)||!decoder.pop(&frame))return 2;
    const int count=std::atoi(argv[2]);MatterWire::Bytes wire;
    if(!MatterWire::encode(frame,&wire))return 2;
    MatterWire::Bytes body(wire.begin()+2,wire.end());
    for(int mode=0;mode<2;++mode){
        std::clock_t start=std::clock();
        for(int i=0;i<count;++i){
            if(mode==0){MatterWire::Bytes out;if(!MatterWire::encode(frame,&out)||out!=wire)return 3;}
            else{MatterWire::Frame out;if(!MatterWire::decodeProtobuf(body,&out)||out.kind!=frame.kind||out.opcode!=frame.opcode||out.id!=frame.id)return 4;}
        }
        std::printf("%.6f\n",1e6*double(std::clock()-start)/CLOCKS_PER_SEC/count);
    }
}
'''
    config = settings()
    with tempfile.TemporaryDirectory(prefix='matter-ipc-benchmark-') as temp:
        temp = Path(temp)
        source_root = ROOT
        if args.revision:
            source_root = temp / 'source'
            for directory in ['microhad/include', 'microhad/src']:
                paths = subprocess.check_output(['git', '-C', str(ROOT), 'ls-tree', '-r', '--name-only', args.revision, '--', directory], text=True).splitlines()
                for path in paths:
                    target = source_root / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(subprocess.check_output(['git', '-C', str(ROOT), 'show', args.revision+':'+path]))
            for directory in ['third-party', 'protocol']:
                (source_root / directory).symlink_to(ROOT / directory, target_is_directory=True)
        driver = temp / 'bench.cpp' 
        driver.write_text(source)
        inc = ROOT / 'third-party/nanopb'
        objects = []
        for name in ['pb_common', 'pb_encode', 'pb_decode']:
            obj = temp / (name + '.o')
            subprocess.run([config['HOST_CC'], '-O2', '-I'+str(inc), '-c', str(inc/(name+'.c')), '-o', str(obj)], check=True)
            objects.append(str(obj))
        executable = temp / 'bench'
        subprocess.run([config['HOST_CXX'], '-std=gnu++03', '-O2', '-Wall', '-Wextra', '-Werror', '-I'+str(source_root/'microhad/include'), '-I'+str(inc),
                        str(driver), *[str(source_root/'microhad/src'/name) for name in ['matter_protocol.cpp', 'matter_protobuf.cpp', 'protobuf_value.cpp', 'json_util.cpp']],
                        *objects, '-o', str(executable)], check=True)
        for name in names:
            v = vectors[name]
            samples = [list(map(float, subprocess.check_output([str(executable), v['wire'], str(args.iterations)], text=True).split())) for _ in range(5)]
            print(json.dumps({'name': name, 'iterations': args.iterations, 'encode_cpu_us': statistics.median(s[0] for s in samples), 'decode_cpu_us': statistics.median(s[1] for s in samples)}), flush=True)


if __name__ == '__main__':
    main()
