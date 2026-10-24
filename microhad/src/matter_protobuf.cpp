// Protobuf boundary adapter; the worker/public JSON and persistent formats stay separate.
#include "matter_protocol.h"
#include "json_util.h"
#include "protobuf_value.h"
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
using Proto::Value;
using Proto::field;
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
        if(op==10 && field(v,"subscriptions"))return "subscription-health";
        return field(v,"devices")?"page":"job";
    }
    if(context=="event-data") {
        if(event=="commissioning")return "session";
        if(event=="thread")return "thread";
        if(event=="ble_write")return "ble-write";
        if(event=="matter_report")return "reading";
        if(event=="matter_button")return "button";
        if(event=="matter_inventory")return "inventory";
        if(event=="matter_inventory_nodes")return "inventory-nodes";
        return "";
    }
    return context;
}
// JSON is used solely at the existing in-process boundary, never in a protobuf body.
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
void json(const Value& v,std::ostringstream* out){
    switch(v.type){
    case Value::Null:*out<<"null";break;
    case Value::Bool:*out<<(v.u?"true":"false");break;
    case Value::UInt:*out<<v.u;break;
    case Value::Int:*out<<v.i;break;
    case Value::Float:{
        std::ostringstream number;number<<std::setprecision(17)<<v.f;
        std::string text=number.str();
        // Preserve the protobuf numeric type for the existing integer validators.
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
bool encodeProtobuf(const Frame& frame,Bytes* bytes){
    std::string text(frame.payload.begin(),frame.payload.end());Value body;
    if(text.empty())body.type=Value::Map;
    else if(!JsonReader(text).parse(&body))return false;
    if(body.type!=Value::Map)return false;
    unsigned int unknown=0;
    if(!convert(&body,"body",frame.kind,frame.opcode,"",false,&unknown)||!Proto::normalize(&body,"Body",frame.kind,frame.opcode,"",false))return false;
    Value env;env.type=Value::Map;add(&env,"version",3);add(&env,"kind",frame.kind);add(&env,"opcode",frame.opcode);add(&env,"id",frame.id);Proto::add(&env,"body",body);
    Bytes buffer;if(!Proto::encode("Envelope",env,&buffer,MaxPayload))return false;
    bytes->push_back(static_cast<unsigned char>(buffer.size()));bytes->push_back(static_cast<unsigned char>(buffer.size()>>8));bytes->insert(bytes->end(),buffer.begin(),buffer.end());return true;
}
bool decodeProtobuf(const Bytes& bytes,Frame* frame){
    Value env;unsigned unknown=0;
    if(!Proto::decode("Envelope",bytes,&env,MaxPayload,&unknown))return false;
    const Value* version=field(env,"version");const Value* kind=field(env,"kind");const Value* opcode=field(env,"opcode");const Value* id=field(env,"id");const Value* body=field(env,"body");
    if(!version||version->type!=Value::UInt||version->u!=3||!kind||kind->type!=Value::UInt||kind->u>2||!opcode||opcode->type!=Value::UInt||opcode->u<1||opcode->u>12||!id||id->type!=Value::UInt||id->u>0xffffffffUL||!body||body->type!=Value::Map||((kind->u==2)!=(id->u==0))||((kind->u==2)!=(opcode->u==12)))return false;
    frame->kind=static_cast<unsigned char>(kind->u);frame->opcode=static_cast<unsigned char>(opcode->u);frame->id=static_cast<unsigned long>(id->u);
    std::ostringstream request;request<<"request"<<opcode->u;
    const std::string expected=kind->u==0?request.str():kind->u==2?"event":opcode->u==1?"matter":opcode->u==2?"thread":"reply";
    if(!field(*body,expected))return false;
    Value converted=*body;bool valid=Proto::normalize(&converted,"Body",frame->kind,frame->opcode,"",true)&&convert(&converted,"body",frame->kind,frame->opcode,"",true,&unknown);
    if(unknown)syslog(LOG_WARNING,"Matter IPC opcode %u: ignored %u unknown fields",static_cast<unsigned>(frame->opcode),unknown);
    std::ostringstream out;if(!valid)out<<"!invalid protobuf body";else if(!(frame->kind==0&&frame->opcode<=2&&converted.items.empty()))json(converted,&out);
    std::string result=out.str();frame->payload.assign(result.begin(),result.end());return true;
}
}
