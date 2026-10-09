// CBOR boundary adapter; the worker/public JSON and persistent formats stay separate.
#include "matter_protocol.h"
#include "json_util.h"
#include "../dependencies/tinycbor/src/cbor.h"
#include <stdint.h>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <set>
#include <syslog.h>
#include "matter_wire_shapes.inc"

namespace MatterWire {
namespace {
struct Value {
    enum Type { Null, Bool, UInt, Int, Float, Text, Binary, Array, Map } type;
    uint64_t u;
    int64_t i;
    double f;
    std::string text;
    std::vector<Value> items;
    std::vector<std::string> keys;
    Value() : type(Null), u(0), i(0), f(0) {}
};
const Value* field(const Value& v, const std::string& name) {
    for (unsigned int n=0;n<v.keys.size();++n) if(v.keys[n]==name) return &v.items[n];
    return 0;
}
std::string textField(const Value& v, const std::string& name) {
    const Value* p=field(v,name); return p && p->type==Value::Text ? p->text : "";
}
const char* child(const std::string& shape, const std::string& key) {
    for(unsigned int n=0;n<sizeof(wireFields)/sizeof(wireFields[0]);++n)
        if(shape==wireFields[n].shape && key==wireFields[n].name) return wireFields[n].child;
    return 0;
}
std::string shape(const std::string& context,const Value& v,unsigned int kind,unsigned int op,const std::string& event) {
    std::ostringstream out;
    if(context=="body") {
        if(kind==0){out<<"request"<<op;std::string action=textField(v,op==11?"command":"action");if(op>=9&&op<=11&&!action.empty())out<<':'<<action;return out.str();}
        if(kind==2)return "event";
        return op==1?"matter":op==2?"thread":"reply";
    }
    if(context=="result") {
        if(op<=5)return "session";
        if(op<=8){out<<"ack"<<op;return out.str();}
        if(op==9)return "thread";
        return field(v,"devices")?"page":"job";
    }
    if(context=="event-data") {
        if(event=="commissioning")return "session";
        if(event=="thread")return "thread";
        if(event=="ble_write")return "ble-write";
        if(event=="matter_report")return "reading";
        if(event=="matter_button")return "button";
        return "";
    }
    return context;
}
// JSON is used solely at the existing in-process boundary, never in a CBOR body.
class JsonReader {
    const std::string& s;
    unsigned int at;
    void ws(){while(at<s.size() && (s[at]==' '||s[at]=='\r'||s[at]=='\n'||s[at]=='\t'))++at;}
    bool take(char c){ws();if(at>=s.size()||s[at]!=c)return false;++at;return true;}
    bool string(std::string* out){
        ws();unsigned int start=at;
        if(!take('"'))return false;
        while(at<s.size()){
            unsigned char c=s[at++];
            if(c=='"'){
                return jsonStringField("{\"x\":"+s.substr(start,at-start)+"}","x",out);
            }
            if(c<32)return false;
            if(c=='\\'){if(at>=s.size())return false;++at;}
        }
        return false;
    }
    bool value(Value* v,unsigned int depth){
        ws();if(at>=s.size())return false;
        char c=s[at];
        if(c=='{'||c=='['){
            if(depth>=16)return false;
            ++at;v->type=c=='{'?Value::Map:Value::Array;
            ws();char end=c=='{'?'}':']';
            if(at<s.size()&&s[at]==end){++at;return true;}
            std::set<std::string> keys;
            do{
                if(v->items.size()>=(c=='{'?256U:512U))return false;
                if(c=='{'){
                    std::string key;if(!string(&key)||!keys.insert(key).second||!take(':'))return false;
                    v->keys.push_back(key);
                }
                Value item;if(!value(&item,depth+1))return false;v->items.push_back(item);
                ws();if(at<s.size()&&s[at]==end){++at;return true;}
            }while(take(','));
            return false;
        }
        if(c=='"'){v->type=Value::Text;return string(&v->text);}
        unsigned int start=at;
        while(at<s.size()&&s[at]!=','&&s[at]!='}'&&s[at]!=']'&&s[at]!=' '&&s[at]!='\t'&&s[at]!='\r'&&s[at]!='\n')++at;
        std::string token=s.substr(start,at-start);
        if(token=="null")return true;
        if(token=="true"||token=="false"){v->type=Value::Bool;v->u=token=="true";return true;}
        if(token.empty())return false;
        unsigned int pos=token[0]=='-'?1:0;
        if(pos==token.size()||token[pos]<'0'||token[pos]>'9'||(token[pos]=='0'&&pos+1<token.size()&&token[pos+1]>='0'&&token[pos+1]<='9'))return false;
        char* end=0;errno=0;
        if(token.find_first_of(".eE")!=std::string::npos){v->type=Value::Float;v->f=std::strtod(token.c_str(),&end);return end==token.c_str()+token.size()&&std::isfinite(v->f);}
        if(token[0]=='-'){v->type=Value::Int;v->i=strtoll(token.c_str(),&end,10);}
        else{v->type=Value::UInt;v->u=strtoull(token.c_str(),&end,10);}
        return errno!=ERANGE&&end==token.c_str()+token.size();
    }
public:
    explicit JsonReader(const std::string& text):s(text),at(0){}
    bool parse(Value* v){if(!value(v,1))return false;ws();return at==s.size();}
};
std::string hex(uint64_t id){std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<id;return out.str();}
bool canonicalHex(const std::string& s){
    if(s.size()%2)return false;
    for(unsigned int n=0;n<s.size();++n)if(!((s[n]>='0'&&s[n]<='9')||(s[n]>='a'&&s[n]<='f')))return false;
    return true;
}
bool convert(Value* v,const std::string& context,unsigned int kind,unsigned int op,const std::string& ev,bool incoming,unsigned int* unknown){
    if(context=="@id"&&v->type!=Value::Null){
        if(incoming){if(v->type!=Value::UInt)return false;v->text=hex(v->u);v->type=Value::Text;}
        else if(v->type==Value::Text&&v->text.size()==16&&canonicalHex(v->text)){
            v->u=strtoull(v->text.c_str(),0,16);v->type=Value::UInt;
        }
        return true;
    }
    if(context=="@bytes"){
        if(incoming){
            if(v->type!=Value::Binary)return false;
            std::string s;const char* digits="0123456789abcdef";
            for(unsigned int n=0;n<v->text.size();++n){unsigned char b=v->text[n];s+=digits[b>>4];s+=digits[b&15];}
            v->type=Value::Text;v->text=s;
        }else if(v->type==Value::Text&&canonicalHex(v->text)){
            std::string s;
            for(unsigned int n=0;n<v->text.size();n+=2)s+=static_cast<char>(strtoul(v->text.substr(n,2).c_str(),0,16));
            v->text=s;v->type=Value::Binary;
        }
        return true;
    }
    std::string ctx=shape(context,*v,kind,op,ev);
    if(v->type==Value::Array){for(unsigned int n=0;n<v->items.size();++n)if(!convert(&v->items[n],ctx,kind,op,ev,incoming,unknown))return false;}
    if(v->type==Value::Map){
        std::string event=textField(*v,"event");if(event.empty())event=ev;
        Value out;out.type=Value::Map;
        for(unsigned int n=0;n<v->items.size();++n){
            const char* next=child(ctx,v->keys[n]);
            if(incoming&&!next){++*unknown;continue;}
            if(!convert(&v->items[n],next?next:"",kind,op,event,incoming,unknown))return false;
            out.keys.push_back(v->keys[n]);out.items.push_back(v->items[n]);
        }
        *v=out;
    }
    return true;
}
bool encodeValue(CborEncoder* enc,const Value& v){
    CborError err=CborNoError;
    switch(v.type){
    case Value::Null:err=cbor_encode_null(enc);break;
    case Value::Bool:err=cbor_encode_boolean(enc,v.u!=0);break;
    case Value::UInt:err=cbor_encode_uint(enc,v.u);break;
    case Value::Int:err=cbor_encode_int(enc,v.i);break;
    case Value::Float:err=cbor_encode_double(enc,v.f);break;
    case Value::Text:err=cbor_encode_text_string(enc,v.text.data(),v.text.size());break;
    case Value::Binary:err=cbor_encode_byte_string(enc,reinterpret_cast<const uint8_t*>(v.text.data()),v.text.size());break;
    case Value::Map:case Value::Array:{
        CborEncoder container;
        err=v.type==Value::Map?cbor_encoder_create_map(enc,&container,v.items.size()):cbor_encoder_create_array(enc,&container,v.items.size());
        if(err!=CborNoError)return false;
        for(unsigned int n=0;n<v.items.size();++n){
            if(v.type==Value::Map&&cbor_encode_text_string(&container,v.keys[n].data(),v.keys[n].size())!=CborNoError)return false;
            if(!encodeValue(&container,v.items[n]))return false;
        }
        err=cbor_encoder_close_container(enc,&container);break;
    }}
    return err==CborNoError;
}
bool validUtf8(const std::string& s) {
    for(unsigned int at=0;at<s.size();) {
        unsigned char c=s[at++];if(c<128)continue;
        unsigned int count=c>=0xc2&&c<=0xdf?1:c>=0xe0&&c<=0xef?2:c>=0xf0&&c<=0xf4?3:0;
        if(!count||at+count>s.size())return false;
        unsigned int value=c&((1U<<(6-count))-1);
        for(unsigned int n=0;n<count;++n){unsigned char b=s[at++];if((b&0xc0)!=0x80)return false;value=(value<<6)|(b&63);}
        if(value<(count==1?128U:count==2?2048U:65536U)||value>0x10ffffU||(value>=0xd800&&value<=0xdfff))return false;
    }
    return true;
}
bool readValue(CborValue* it,Value* out,unsigned int depth){
    if(cbor_value_is_integer(it)){
        if(cbor_value_is_unsigned_integer(it)){out->type=Value::UInt;if(cbor_value_get_uint64(it,&out->u)!=CborNoError)return false;}
        else{out->type=Value::Int;if(cbor_value_get_int64_checked(it,&out->i)!=CborNoError)return false;}
    }else if(cbor_value_is_boolean(it)){bool b=false;if(cbor_value_get_boolean(it,&b)!=CborNoError)return false;out->type=Value::Bool;out->u=b;}
    else if(cbor_value_is_null(it))out->type=Value::Null;
    else if(cbor_value_is_double(it)||cbor_value_is_float(it)||cbor_value_is_half_float(it)){
        out->type=Value::Float;
        if(cbor_value_is_double(it)){if(cbor_value_get_double(it,&out->f)!=CborNoError)return false;}
        else{float f=0;CborError e=cbor_value_is_float(it)?cbor_value_get_float(it,&f):cbor_value_get_half_float_as_float(it,&f);if(e!=CborNoError)return false;out->f=f;}
        if(!std::isfinite(out->f))return false;
    }else if(cbor_value_is_text_string(it)||cbor_value_is_byte_string(it)){
        size_t size=0;
        if(!cbor_value_is_length_known(it)||cbor_value_calculate_string_length(it,&size)!=CborNoError||size>MaxPayload)return false;
        std::vector<char> buffer(size+1);
        bool text=cbor_value_is_text_string(it);
        CborError e=text?cbor_value_copy_text_string(it,&buffer[0],&size,0):cbor_value_copy_byte_string(it,reinterpret_cast<uint8_t*>(&buffer[0]),&size,0);
        if(e!=CborNoError)return false;
        out->type=text?Value::Text:Value::Binary;out->text.assign(&buffer[0],size);
        if(text&&!validUtf8(out->text))return false;
    }else if(cbor_value_is_map(it)||cbor_value_is_array(it)){
        if(depth>=16||!cbor_value_is_length_known(it))return false;
        size_t count=0;bool map=cbor_value_is_map(it);
        CborError e=map?cbor_value_get_map_length(it,&count):cbor_value_get_array_length(it,&count);
        if(e!=CborNoError||count>(map?256U:512U))return false;
        out->type=map?Value::Map:Value::Array;CborValue sub;
        if(cbor_value_enter_container(it,&sub)!=CborNoError)return false;
        std::set<std::string> keys;
        for(size_t n=0;n<count;++n){
            if(map){Value key;if(!readValue(&sub,&key,depth+1)||key.type!=Value::Text||!keys.insert(key.text).second)return false;out->keys.push_back(key.text);}
            Value item;if(!readValue(&sub,&item,depth+1))return false;out->items.push_back(item);
        }
        return cbor_value_leave_container(it,&sub)==CborNoError;
    }else return false;
    return cbor_value_advance(it)==CborNoError;
}
void json(const Value& v,std::ostringstream* out){
    switch(v.type){
    case Value::Null:*out<<"null";break;
    case Value::Bool:*out<<(v.u?"true":"false");break;
    case Value::UInt:*out<<v.u;break;
    case Value::Int:*out<<v.i;break;
    case Value::Float:{
        std::ostringstream number;number<<std::setprecision(17)<<v.f;
        std::string text=number.str();
        // Preserve the CBOR numeric type for the existing integer validators.
        if(text.find_first_of(".eE")==std::string::npos)text+=".0";
        *out<<text;break;
    }
    case Value::Text:*out<<'"'<<jsonEscape(v.text)<<'"';break;
    case Value::Map:case Value::Array:
        *out<<(v.type==Value::Map?'{':'[');
        for(unsigned int n=0;n<v.items.size();++n){if(n)*out<<',';if(v.type==Value::Map)*out<<'"'<<jsonEscape(v.keys[n])<<"\":";json(v.items[n],out);}
        *out<<(v.type==Value::Map?'}':']');break;
    case Value::Binary:*out<<"null";break;
    }
}
void add(Value* map,const char* name,uint64_t n){Value v;v.type=Value::UInt;v.u=n;map->keys.push_back(name);map->items.push_back(v);}
}
bool encodeCbor(const Frame& frame,Bytes* bytes){
    std::string text(frame.payload.begin(),frame.payload.end());Value body;
    if(text.empty())body.type=Value::Map;
    else if(!JsonReader(text).parse(&body))return false;
    if(body.type!=Value::Map)return false;
    unsigned int unknown=0;
    if(!convert(&body,"body",frame.kind,frame.opcode,"",false,&unknown))return false;
    Value env;env.type=Value::Map;add(&env,"version",2);add(&env,"kind",frame.kind);add(&env,"opcode",frame.opcode);add(&env,"id",frame.id);env.keys.push_back("body");env.items.push_back(body);
    Bytes buffer(MaxPayload);CborEncoder encoder;cbor_encoder_init(&encoder,&buffer[0],buffer.size(),0);
    if(!encodeValue(&encoder,env))return false;
    size_t size=cbor_encoder_get_buffer_size(&encoder,&buffer[0]);
    bytes->push_back(static_cast<unsigned char>(size));bytes->push_back(static_cast<unsigned char>(size>>8));bytes->insert(bytes->end(),buffer.begin(),buffer.begin()+size);return true;
}
bool decodeCbor(const Bytes& bytes,Frame* frame){
    if(bytes.empty()||bytes.size()>MaxPayload)return false;
    CborParser parser;CborValue it;
    if(cbor_parser_init(&bytes[0],bytes.size(),0,&parser,&it)!=CborNoError)return false;
    // Validate UTF-8 before copying strings. Depth/count checks happen in readValue.
    Value env;if(!readValue(&it,&env,0)||cbor_value_get_next_byte(&it)!=&bytes[0]+bytes.size()||env.type!=Value::Map)return false;
    const Value* version=field(env,"version");const Value* kind=field(env,"kind");const Value* opcode=field(env,"opcode");const Value* id=field(env,"id");const Value* body=field(env,"body");
    if(!version||version->type!=Value::UInt||version->u!=2||!kind||kind->type!=Value::UInt||kind->u>2||!opcode||opcode->type!=Value::UInt||opcode->u<1||opcode->u>12||!id||id->type!=Value::UInt||id->u>0xffffffffUL||!body||body->type!=Value::Map||((kind->u==2)!=(id->u==0)))return false;
    frame->kind=static_cast<unsigned char>(kind->u);frame->opcode=static_cast<unsigned char>(opcode->u);frame->id=static_cast<unsigned long>(id->u);
    unsigned int unknown=0;for(unsigned int n=0;n<env.keys.size();++n)if(!child("envelope",env.keys[n]))++unknown;
    Value converted=*body;bool valid=convert(&converted,"body",frame->kind,frame->opcode,"",true,&unknown);
    if(unknown)syslog(LOG_WARNING,"Matter IPC opcode %u: ignored %u unknown fields",static_cast<unsigned int>(frame->opcode),unknown);
    std::ostringstream out;
    if(!valid)out<<"!invalid CBOR body";
    else if(!(frame->kind==0&&frame->opcode<=2&&converted.items.empty()))json(converted,&out);
    std::string result=out.str();frame->payload.assign(result.begin(),result.end());return true;
}
}
