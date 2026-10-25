// Public HTTP/phone JSON adapter. IPC codecs and worker queues are typed.
#include "matter_json.h"
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

namespace MatterProto {
Proto::Value jsonUInt(uint64_t n){Proto::Value v;v.type=Proto::Value::UInt;v.u=n;return v;}
Proto::Value jsonInt(int64_t n){Proto::Value v;v.type=Proto::Value::Int;v.i=n;return v;}
Proto::Value jsonBool(bool n){Proto::Value v;v.type=Proto::Value::Bool;v.u=n;return v;}
Proto::Value jsonFloat(double n){Proto::Value v;v.type=Proto::Value::Float;v.f=n;return v;}
Proto::Value jsonText(const std::string& n){Proto::Value v;v.type=Proto::Value::Text;v.text=n;return v;}
Proto::Value jsonBytes(const std::vector<unsigned char>& n){Proto::Value v;v.type=Proto::Value::Binary;v.text.assign(n.begin(),n.end());return v;}
bool getUInt(const Proto::Value& v,uint64_t* p){if(v.type!=Proto::Value::UInt)return false;*p=v.u;return true;}
bool getUInt32(const Proto::Value& v,uint32_t* p){if(v.type!=Proto::Value::UInt||v.u>0xffffffffUL)return false;*p=static_cast<uint32_t>(v.u);return true;}
bool getInt(const Proto::Value& v,int64_t* p){if(v.type==Proto::Value::Int){*p=v.i;return true;}if(v.type==Proto::Value::UInt&&v.u<=(~static_cast<uint64_t>(0)>>1)){*p=v.u;return true;}return false;}
bool getFloat(const Proto::Value& v,double* p){if(v.type==Proto::Value::Float)*p=v.f;else if(v.type==Proto::Value::UInt)*p=v.u;else if(v.type==Proto::Value::Int)*p=v.i;else return false;return std::isfinite(*p);}
bool getBool(const Proto::Value& v,bool* p){if(v.type!=Proto::Value::Bool)return false;*p=v.u!=0;return true;}
bool getText(const Proto::Value& v,std::string* p){if(v.type!=Proto::Value::Text)return false;*p=v.text;return true;}
bool getBytes(const Proto::Value& v,std::vector<unsigned char>* p){if(v.type!=Proto::Value::Binary)return false;p->assign(v.text.begin(),v.text.end());return true;}
}
#include "../../protocol/generated/matter_json.inc"
namespace MatterWire {
namespace {
using Proto::Value;
using Proto::field;
std::string textField(const Value& v, const std::string& name) {
    const Value* p=field(v,name); return p && p->type==Value::Text ? p->text : "";
}
const char* child(const std::string& shape, const std::string& key) {
    unsigned int low=0,high=sizeof(wireFields)/sizeof(wireFields[0]);
    while(low<high){
        unsigned int mid=low+(high-low)/2;int order=shape.compare(wireFields[mid].shape);
        if(!order)order=key.compare(wireFields[mid].name);
        if(order<0)high=mid;else if(order>0)low=mid+1;else return wireFields[mid].child;
    }
    return 0;
}
std::string shape(const std::string& context,const Value& v,unsigned int kind,unsigned int op,const std::string& event) {
    std::ostringstream out;
    if(context=="body") {
        if(kind==0){out<<Proto::requestName(op);std::string action=textField(v,op==11?"command":"action");if(op>=9&&op<=11&&!action.empty())out<<':'<<action;return out.str();}
        if(kind==2)return "event";
        return op==1?"matter":op==2?"thread":"reply";
    }
    if(context=="result") {
        if(op<=5)return "session";
        if(op<=8){out<<Proto::ackName(op);return out.str();}
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
// Parse external HTTP/WebSocket JSON; IPC frames carry generated messages.
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
        unsigned int kept=0;
        for(unsigned int n=0;n<v->items.size();++n){
            const char* next=child(ctx,v->keys[n]);
            if(incoming&&!next){++*unknown;continue;}
            if(!convert(&v->items[n],next?next:"",kind,op,event,incoming,unknown))return false;
            if(kept!=n){v->keys[kept].swap(v->keys[n]);v->items[kept].swap(v->items[n]);}++kept;
        }
        v->keys.resize(kept);v->items.resize(kept);
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

}
bool parseJson(unsigned char kind,unsigned char op,const std::string& input,MatterProto::MessageBody* out){
    Value body;if(input.empty())body.type=Value::Map;else if(!JsonReader(input).parse(&body))return false;
    unsigned unknown=0;
    return body.type==Value::Map&&convert(&body,"body",kind,op,"",false,&unknown)&&Proto::normalize(&body,"MessageBody",kind,op,"",false)&&MatterProto::fromValue(body,out);
}
std::string jsonBody(unsigned char kind,unsigned char op,const MatterProto::MessageBody& input){
    Value body=MatterProto::toValue(input);unsigned unknown=0;
    if(!Proto::normalize(&body,"MessageBody",kind,op,"",true)||!convert(&body,"body",kind,op,"",true,&unknown))return "!invalid protobuf body";
    if(kind==0&&op<=2&&body.items.empty())return "";
    std::ostringstream out;json(body,&out);return out.str();
}
}
