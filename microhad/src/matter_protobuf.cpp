// Typed protobuf framing. Public JSON adapters are deliberately separate.
#include "matter_protocol.h"
#define PB_C99_STATIC_ASSERT
#include "../../third-party/nanopb/pb_encode.h"
#include "../../third-party/nanopb/pb_decode.h"
#include <cmath>
#include <syslog.h>
namespace MatterProto {
bool validUtf8(const std::string& s) {
    for(size_t at=0;at<s.size();) {
        unsigned char c=s[at++];if(c<128)continue;
        unsigned n=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!n||at+n>s.size())return false;
        unsigned value=c&((1U<<(6-n))-1);
        for(unsigned i=0;i<n;++i){unsigned char b=s[at++];if((b&192)!=128)return false;value=(value<<6)|(b&63);}
        if(value<(n==1?128U:n==2?2048U:65536U)||value>0x10ffffU||(value>=0xd800&&value<=0xdfff))return false;
    }
    return true;
}
}
#include "../../protocol/generated/matter_codec.inc"
namespace MatterWire {
bool bodyMatches(unsigned kind,unsigned op,const MatterProto::MessageBody& body){
    bool expected=false;
    if(kind==0){switch(op){
    case 1:expected=body.matter_status_request.present;break;
    case 2:expected=body.thread_status_request.present;break;
    case 3:expected=body.commissioning_start_request.present;break;
    case 4:expected=body.commissioning_status_request.present;break;
    case 5:expected=body.commissioning_cancel_request.present;break;
    case 6:expected=body.ble_attach_request.present;break;
    case 7:expected=body.ble_indication_request.present;break;
    case 8:expected=body.ble_write_complete_request.present;break;
    case 9:expected=body.thread_control_request.present;break;
    case 10:expected=body.device_control_request.present;break;
    case 11:expected=body.device_command_request.present;break;

    default:break;}}
    else expected=kind==2?body.event.present:op==1?body.matter.present:op==2?body.thread.present:body.reply.present;
    return expected;
}
bool encodeProtobuf(const Frame& frame,Bytes* bytes){
    if(!bodyMatches(frame.kind,frame.opcode,frame.payload))return false;
    MatterProto::IpcEnvelope env;env.version=3;env.kind=frame.kind;env.opcode=frame.opcode;env.id=frame.id;env.body=frame.payload;
    pb_ostream_t sizing=PB_OSTREAM_SIZING;
    if(!MatterProto::write(&sizing,env,0)||!sizing.bytes_written||sizing.bytes_written>MaxPayload)return false;
    bytes->resize(sizing.bytes_written+2);(*bytes)[0]=sizing.bytes_written&255;(*bytes)[1]=sizing.bytes_written>>8;
    pb_ostream_t stream=pb_ostream_from_buffer(&(*bytes)[2],sizing.bytes_written);
    return MatterProto::write(&stream,env,0);
}
bool decodeProtobuf(const Bytes& bytes,Frame* frame){
    if(bytes.empty()||bytes.size()>MaxPayload)return false;
    pb_istream_t stream=pb_istream_from_buffer(&bytes[0],bytes.size());MatterProto::IpcEnvelope env;unsigned unknown=0;
    if(!MatterProto::read(&stream,&env,0,&unknown)||!env.version.present||env.version.value!=3||!env.kind.present||env.kind.value>2||!env.opcode.present||env.opcode.value<1||env.opcode.value>12||!env.id.present||!env.body.present)return false;
    const unsigned kind=env.kind.value,op=env.opcode.value,id=env.id.value;
    if((kind==2)!=(id==0)||(kind==2)!=(op==12))return false;
    const MatterProto::MessageBody& body=env.body.value;
    if(!bodyMatches(kind,op,body))return false;
    if(unknown)syslog(LOG_WARNING,"Matter IPC opcode %u: ignored %u unknown fields",op,unknown);
    frame->kind=kind;frame->opcode=op;frame->id=id;frame->payload=body;return true;
}
}
