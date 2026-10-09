#include "matter_protocol.h"
#include <cassert>
#include <string>
#include <cstdlib>
#include "matter_wire_vectors.inc"

void runMatterProtocolUnitTests() {
    using namespace MatterWire;
    for(unsigned int n=0;n<sizeof(wireVectors)/sizeof(wireVectors[0]);++n) {
        const WireVector& v=wireVectors[n];std::string hex=v.hex;Bytes bytes;
        for(unsigned int at=0;at<hex.size();at+=2)bytes.push_back(static_cast<unsigned char>(strtoul(hex.substr(at,2).c_str(),0,16)));
        Decoder decoder;bool valid=true;
        for(unsigned int at=0;at<bytes.size()&&valid;++at)valid=decoder.feed(Bytes(1,bytes[at]));
        assert(valid==v.valid);Frame frame;
        if(valid){assert(decoder.pop(&frame));assert(std::string(frame.payload.begin(),frame.payload.end())==v.json);}
        else{assert(decoder.failed()&&!decoder.pop(&frame)&&!decoder.feed(bytes));}
    }
    Frame frame; frame.opcode=7;frame.id=0x12345678UL;
    std::string body="{\"schema\":1,\"session\":\"ffffffffffffffff\",\"generation\":1,\"sequence\":1,\"data\":\"0080ff\"}";
    frame.payload.assign(body.begin(),body.end());Bytes bytes;
    assert(encode(frame,&bytes));
    assert(bytes[2]==0xa5); // Five envelope fields; only length lives outside CBOR.
    Decoder fragmented;
    for(unsigned int n=0;n<bytes.size();++n)assert(fragmented.feed(Bytes(1,bytes[n])));
    Frame decoded;assert(fragmented.pop(&decoded)&&decoded.id==frame.id&&decoded.payload==frame.payload);
    Bytes together=bytes;together.insert(together.end(),bytes.begin(),bytes.end());
    Decoder joined;assert(joined.feed(together)&&joined.pop(&decoded)&&joined.pop(&decoded)&&!joined.pop(&decoded));
    // Mutate the version value, append trailing CBOR, and feed legacy framing.
    Bytes bad=bytes;bad[11]=3;Decoder version;assert(!version.feed(bad));
    bad=bytes;bad.push_back(0);++bad[0];Decoder trailing;assert(!trailing.feed(bad));
    const unsigned char old[]={0,0,1,0,1,0,1,0,0,0};Decoder legacy;assert(!legacy.feed(Bytes(old,old+10)));
    Decoder oversized;assert(!oversized.feed(Bytes(MaxChunk+1,0)));
    Frame event;event.kind=Frame::Event;event.opcode=12;assert(encode(event,&bytes));
    Decoder queue;for(unsigned int n=0;n<MaxQueued;++n)assert(queue.feed(bytes));assert(!queue.feed(bytes)&&!queue.pop(&decoded));
    frame.payload.assign(MaxPayload,'x');assert(!encode(frame,&bytes)&&bytes.empty());
}
