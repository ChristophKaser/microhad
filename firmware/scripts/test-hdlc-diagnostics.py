#!/usr/bin/env python3
"""Host checks for complete, bounded, backpressure-aware error-only dumps."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''
#include "hdlc_rx_error_log.hpp"
#include <assert.h>
#include <string>
#include <vector>
#include <stdio.h>

static std::vector<std::string> lines;
static bool accept(const char *p, unsigned n) {
    assert(n < 128); lines.push_back(std::string(p,n)); return true;
}
static bool reject(const char *, unsigned) { return false; }
template<unsigned N> static void drain(HdlcRxErrorLog<N> &log) {
    for(unsigned i=0;i<N/32+10;i++) log.Pump(accept);
}
int main() {
    HdlcRxErrorLog<512> log;
    log.Pump(accept); assert(lines.empty());
    unsigned char input[512];
    for(unsigned i=0;i<512;i++)input[i]=i;
    log.Record(6,input,512);
    log.Pump(reject); assert(lines.empty());
    log.Record(6,input,10); // Preserve first frame and count this skipped error.
    for(unsigned i=0;i<512;i++)input[i]=0;
    drain(log);
    assert(lines.size()==18);
    assert(lines[0].find("Framing error 6: id=1 len=512 captured=512 residue=")==0);
    std::string hex;
    for(unsigned i=1;i<=16;i++){
        unsigned id,offset,length;int start=0;
        assert(sscanf(lines[i].c_str(),"HDLC_RX_FRAME id=%u offset=%u len=%u hex=%n",&id,&offset,&length,&start)==3);
        assert(id==1 && offset==(i-1)*32 && length==32);
        hex+=lines[i].substr(start,length*2);
    }
    assert(hex.size()==1024);
    for(unsigned i=0;i<512;i++){unsigned value;assert(sscanf(hex.substr(i*2,2).c_str(),"%2x",&value)==1);assert(value==(i&255));}
    assert(lines.back()=="HDLC_RX_END id=1 captured=512 dropped_total=1\n");
    unsigned previous=lines.size();drain(log);assert(lines.size()==previous);
    lines.clear();log.Record(6,input,0);drain(log);
    assert(lines.size()==2 && lines[0].find("id=3 len=0 captured=0 residue=ffff")!=std::string::npos);
    lines.clear();
    unsigned char check[]={ '1','2','3','4','5','6','7','8','9',0x6e,0x90 };
    log.Record(6,check,sizeof(check));drain(log);
    assert(lines[0].find("residue=f0b8")!=std::string::npos);
    lines.clear();HdlcRxErrorLog<4> small;small.Record(3,check,sizeof(check));drain(small);
    assert(lines[0].find("len=11 captured=4")!=std::string::npos);
    puts("PASS full 512-byte recovery, snapshot ownership, CRC residue, backpressure, skipped errors, empty and oversized input");
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='hdlc-diagnostic-test-') as temporary:
        directory = Path(temporary)
        source = directory/'test.cpp'; source.write_text(FIXTURE)
        binary = directory/'test'
        subprocess.run(['g++', '-std=c++11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        '-I'+str(ROOT/'firmware/diagnostics'), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        spec = importlib.util.spec_from_file_location('prepare', ROOT/'firmware/scripts/prepare-hdlc-diagnostics.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        project = directory/'ot-rcp-multipan.project.mak'
        original='$(SDK_PATH)/util/third_party/openthread/src/ncp/ncp_hdlc.cpp'
        project.write_text('source='+original+'\n')
        sdk = ROOT/'firmware/dependencies/gecko-sdk'
        before=(sdk/'util/third_party/openthread/src/ncp/ncp_hdlc.cpp').read_bytes()
        module.prepare(sdk, directory)
        assert project.read_text()=='source=diagnostics/ncp_hdlc.cpp\n'
        patched=(directory/'diagnostics/ncp_hdlc.cpp').read_text()
        assert 'mRxErrorLog.Record(aError, aBuf, aBufLength)' in patched
        assert patched.count('mRxErrorLog.Pump(WriteRxErrorLog)')==3
        assert (sdk/'util/third_party/openthread/src/ncp/ncp_hdlc.cpp').read_bytes()==before
        print('PASS pinned SDK patch applied only to build copies and generated source rule')


if __name__=='__main__': main()
