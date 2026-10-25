#include "protobuf_value.h"
#define PB_C99_STATIC_ASSERT
#include "../../third-party/nanopb/pb_encode.h"
#include "../../third-party/nanopb/pb_decode.h"
#include <cstring>
#include <cmath>
#include <set>
#include <sstream>
namespace Proto {
namespace {
struct ProtoField { const char* message; const char* name; unsigned tag; unsigned type; const char* child; bool repeated; bool oneof; };
#include "../../protocol/generated/protobuf_fields.inc"
#include "../../protocol/generated/protobuf_operations.inc"
const unsigned count=sizeof(protoFields)/sizeof(protoFields[0]);
const ProtoField* byName(const std::string& message,const std::string& name) {
    unsigned low=0,high=count;
    while(low<high){unsigned mid=low+(high-low)/2;const ProtoField& field=protoFields[protoByName[mid]];int order=message.compare(field.message);if(!order)order=name.compare(field.name);if(order<0)high=mid;else if(order>0)low=mid+1;else return &field;}
    return 0;
}
const ProtoField* byTag(const std::string& message,unsigned tag) {
    unsigned low=0,high=count;
    while(low<high){unsigned mid=low+(high-low)/2;const ProtoField& field=protoFields[protoByTag[mid]];int order=message.compare(field.message);if(!order)order=tag<field.tag?-1:tag>field.tag?1:0;if(order<0)high=mid;else if(order>0)low=mid+1;else return &field;}
    return 0;
}
bool utf8(const std::string& s) {
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
pb_wire_type_t wire(unsigned type){return type==1?PB_WT_64BIT:(type==9||type==11||type==12)?PB_WT_STRING:PB_WT_VARINT;}
bool writeMessage(pb_ostream_t*,const std::string&,const Value&,unsigned);
bool writeField(pb_ostream_t* stream,const ProtoField& f,const Value& v,unsigned depth) {
    if(!pb_encode_tag(stream,wire(f.type),f.tag))return false;
    switch(f.type) {
    case 1:{double n=v.type==Value::Float?v.f:v.type==Value::UInt?static_cast<double>(v.u):static_cast<double>(v.i);return (v.type==Value::Float||v.type==Value::Int||v.type==Value::UInt)&&std::isfinite(n)&&pb_encode_fixed64(stream,&n);}
    case 4:case 13:return v.type==Value::UInt&&(f.type!=13||v.u<=0xffffffffUL)&&pb_encode_varint(stream,v.u);
    case 18:return (v.type==Value::Int||v.type==Value::UInt)&&(v.type!=Value::UInt||v.u<=(~static_cast<uint64_t>(0)>>1))&&pb_encode_svarint(stream,v.type==Value::Int?v.i:static_cast<int64_t>(v.u));
    case 8:return v.type==Value::Bool&&pb_encode_varint(stream,v.u?1:0);
    case 9:case 12:return v.type==(f.type==9?Value::Text:Value::Binary)&&v.text.size()<=65536&&(f.type!=9||utf8(v.text))&&pb_encode_string(stream,reinterpret_cast<const pb_byte_t*>(v.text.data()),v.text.size());
    case 11:{pb_ostream_t size=PB_OSTREAM_SIZING;if(!writeMessage(&size,f.child,v,depth+1)||size.bytes_written>stream->max_size-stream->bytes_written||!pb_encode_varint(stream,size.bytes_written))return false;if(!stream->callback)return pb_write(stream,0,size.bytes_written);return writeMessage(stream,f.child,v,depth+1);}
    default:return false;
    }
}
bool writeMessage(pb_ostream_t* stream,const std::string& msg,const Value& v,unsigned depth) {
    if(depth>16||v.type!=Value::Map||v.keys.size()!=v.items.size())return false;
    std::set<std::string> seen;bool choice=false;
    for(size_t i=0;i<v.keys.size();++i) {
        const ProtoField* f=byName(msg,v.keys[i]);if(!f||!seen.insert(v.keys[i]).second)return false;
        if(f->oneof){if(choice)return false;choice=true;}
        const Value& item=v.items[i];
        if(f->repeated){if(item.type!=Value::Array||item.items.size()>(msg=="DeviceCache"?4096U:512U))return false;for(size_t j=0;j<item.items.size();++j)if(!writeField(stream,*f,item.items[j],depth))return false;}
        else if(!writeField(stream,*f,item,depth))return false;
    }
    return true;
}
bool readMessage(pb_istream_t* stream,const std::string& msg,Value* out,unsigned depth,unsigned* unknown) {
    if(depth>16)return false;
    out->type=Value::Map;std::set<unsigned> seen;bool choice=false;
    while(stream->bytes_left) {
        pb_wire_type_t wt;uint32_t tag;bool eof=false;
        if(!pb_decode_tag(stream,&wt,&tag,&eof)||!tag||tag>=0x20000000UL)return false;
        const ProtoField* f=byTag(msg,tag);
        if(!f){if(!pb_skip_field(stream,wt))return false;++*unknown;continue;}
        if(f->repeated&&wt==PB_WT_STRING&&(f->type==4||f->type==13||f->type==18||f->type==8)) {
            pb_istream_t sub;if(!pb_make_string_substream(stream,&sub))return false;
            size_t i=0;while(i<out->keys.size()&&out->keys[i]!=f->name)++i;
            if(i==out->keys.size()){Value array;array.type=Value::Array;add(out,f->name,array);}
            while(sub.bytes_left){uint64_t n;if(!pb_decode_varint(&sub,&n)||(f->type==13&&n>0xffffffffUL)||(f->type==8&&n>1)||out->items[i].items.size()>=512)return false;Value x;if(f->type==18){x.type=Value::Int;x.i=static_cast<int64_t>(n>>1)^-static_cast<int64_t>(n&1);}else{x.type=f->type==8?Value::Bool:Value::UInt;x.u=n;}out->items[i].items.push_back(x);}
            if(!pb_close_string_substream(stream,&sub))return false;
            continue;
        }
        if(wt!=wire(f->type)||(!f->repeated&&!seen.insert(tag).second)||(f->oneof&&choice))return false;
        choice=choice||f->oneof;Value v;
        if(wt==PB_WT_VARINT) {
            uint64_t n;if(!pb_decode_varint(stream,&n))return false;
            if(f->type==8){if(n>1)return false;v.type=Value::Bool;v.u=n;}
            else if(f->type==18){v.type=Value::Int;v.i=static_cast<int64_t>(n>>1)^-static_cast<int64_t>(n&1);}
            else{if(f->type==13&&n>0xffffffffUL)return false;v.type=Value::UInt;v.u=n;}
        } else if(wt==PB_WT_64BIT) {v.type=Value::Float;if(!pb_decode_fixed64(stream,&v.f)||!std::isfinite(v.f))return false;}
        else {
            pb_istream_t sub;if(!pb_make_string_substream(stream,&sub))return false;
            bool ok=true;
            if(f->type==11)ok=readMessage(&sub,f->child,&v,depth+1,unknown);
            else {if(sub.bytes_left>65536)return false;v.type=f->type==9?Value::Text:Value::Binary;v.text.resize(sub.bytes_left);if(!v.text.empty())ok=pb_read(&sub,reinterpret_cast<pb_byte_t*>(&v.text[0]),v.text.size());if(f->type==9)ok=ok&&utf8(v.text);}
            if(!ok||!pb_close_string_substream(stream,&sub))return false;
        }
        if(f->repeated) {
            size_t i=0;while(i<out->keys.size()&&out->keys[i]!=f->name)++i;
            if(i==out->keys.size()){Value a;a.type=Value::Array;add(out,f->name,a);}
            if(out->items[i].items.size()>=(msg=="DeviceCache"?4096U:512U))return false;
            out->items[i].items.push_back(v);
        }else add(out,f->name,v);
    }
    return true;
}
std::string text(const Value& v,const char* name) {const Value* p=field(v,name);return p&&p->type==Value::Text?p->text:"";}
}
const char* requestName(unsigned opcode){return opcode<sizeof(protoRequestNames)/sizeof(protoRequestNames[0])?protoRequestNames[opcode]:"";}
void Value::swap(Value& other) {
    std::swap(type,other.type);std::swap(u,other.u);std::swap(i,other.i);std::swap(f,other.f);
    text.swap(other.text);items.swap(other.items);keys.swap(other.keys);
}
const char* ackName(unsigned opcode){return opcode<sizeof(protoAckNames)/sizeof(protoAckNames[0])?protoAckNames[opcode]:"";}
const Value* field(const Value& v,const std::string& key){for(size_t i=0;i<v.keys.size();++i)if(v.keys[i]==key)return &v.items[i];return 0;}
void add(Value* v,const std::string& key,const Value& item){v->type=Value::Map;v->keys.push_back(key);v->items.push_back(item);}
bool encode(const std::string& msg,const Value& v,Bytes* out,size_t limit) {
    pb_ostream_t size=PB_OSTREAM_SIZING;if(!writeMessage(&size,msg,v,0)||size.bytes_written>limit)return false;
    Bytes b(size.bytes_written);pb_ostream_t stream=pb_ostream_from_buffer(b.empty()?0:&b[0],b.size());
    if(!writeMessage(&stream,msg,v,0))return false;
    out->swap(b);return true;
}
bool decode(const std::string& msg,const Bytes& bytes,Value* out,size_t limit,unsigned* unknown) {
    if(bytes.empty()||bytes.size()>limit)return false;
    pb_istream_t stream=pb_istream_from_buffer(&bytes[0],bytes.size());Value v;
    if(!readMessage(&stream,msg,&v,0,unknown))return false;
    out->swap(v);return true;
}
bool normalize(Value* v,const std::string& msg,unsigned kind,unsigned op,const std::string& ev,bool incoming) {
    unsigned low=0,high=count;
    while(low<high){unsigned mid=low+(high-low)/2;if(msg.compare(protoFields[protoByTag[mid]].message)>0)low=mid+1;else high=mid;}
    const ProtoField* first=low<count&&msg==protoFields[protoByTag[low]].message?&protoFields[protoByTag[low]]:0;
    if(!first)return (msg=="MatterStatusRequest"||msg=="ThreadStatusRequest")&&v->type==Value::Map;
    if(first->repeated&&std::strcmp(first->name,"items")==0) {
        if(incoming){Value result;result.type=Value::Array;for(size_t i=0;i<v->keys.size();++i)if(v->keys[i]=="items"){result.swap(v->items[i]);break;}for(size_t i=0;i<result.items.size();++i)if(first->type==11&&!normalize(&result.items[i],first->child,kind,op,ev,true))return false;v->swap(result);return true;}
        if(v->type!=Value::Array)return false;
        Value result;result.swap(*v);for(size_t i=0;i<result.items.size();++i)if(first->type==11&&!normalize(&result.items[i],first->child,kind,op,ev,false))return false;
        Value wrap;wrap.type=Value::Map;wrap.keys.push_back("items");wrap.items.resize(1);wrap.items[0].swap(result);v->swap(wrap);return true;
    }
    if(first->oneof) {
        if(incoming){if(v->type!=Value::Map||v->items.size()!=1)return false;const ProtoField* f=byName(msg,v->keys[0]);if(!f)return false;Value result;result.swap(v->items[0]);if(v->keys[0]=="null"){if(result.type!=Value::Bool||result.u!=1)return false;result=Value();}else if(f->type==11&&!normalize(&result,f->child,kind,op,ev,true))return false;v->swap(result);return true;}
        std::string key;
        if(msg=="MessageBody"){std::ostringstream n;n<<requestName(op);key=kind==0?n.str():kind==2?"event":op==1?"matter":op==2?"thread":"reply";}
        else if(msg=="OperationResult"){std::ostringstream n;n<<ackName(op);key=op<=5?"session":op<=8?n.str():op==9?"thread":field(*v,"subscriptions")?"subscription_health":field(*v,"devices")?"page":"job";}
        else if(msg=="EventPayload")key=ev=="thread"?"thread":ev=="commissioning"?"session":ev=="ble_write"?"ble_write":ev=="matter_report"?"reading":ev=="matter_button"?"button":ev=="matter_inventory"?"inventory":ev=="matter_inventory_nodes"?"inventory_nodes":"";
        else if(v->type==Value::Null)key="null";
        else if(msg=="NullableText")key="text";
        else if(msg=="NullableUint"||msg=="NullableInt")key="integer";
        else key=v->type==Value::Bool?"boolean":v->type==Value::Float?"number":v->type==Value::UInt?(msg=="ReadingValue"?"unsigned":"integer"):v->type==Value::Int?"integer":v->type==Value::Text?"text":v->type==Value::Array?"list":"measurement";
        const ProtoField* f=byName(msg,key);if(!f)return false;Value item;item.swap(*v);if(key=="null"){item.type=Value::Bool;item.u=1;}else if(f->type==11&&!normalize(&item,f->child,kind,op,ev,false))return false;
        Value result;result.type=Value::Map;result.keys.push_back(key);result.items.resize(1);result.items[0].swap(item);v->swap(result);return true;
    }
    if(v->type!=Value::Map)return false;
    std::string event=text(*v,"event");if(event.empty())event=ev;
    size_t kept=0;
    for(size_t i=0;i<v->keys.size();++i){
        const ProtoField* f=byName(msg,v->keys[i]);if(!f)continue;
        if(f->type==11&&!normalize(&v->items[i],f->child,kind,op,event,incoming))return false;
        if(kept!=i){v->keys[kept].swap(v->keys[i]);v->items[kept].swap(v->items[i]);}++kept;
    }
    v->keys.resize(kept);v->items.resize(kept);return true;
}
}
