/* Included once by http_server.cpp. Shared by HTTP/HTTPS workers. */
namespace api_auth {
const char* roles[] = {"guest", "user", "administrator"};
struct Password { std::string salt, verifier; };
struct Challenge { int role, slot; long expires; };
struct State {
    bool loaded, valid;
    Password passwords[3];
    std::string signingKey;
    std::map<std::string, Challenge> challenges;
    long window; unsigned int attempts;
    State(): loaded(false), valid(true), window(0), attempts(0) {}
};
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::map<std::string, State> states;
struct Lock {
    bool held;
    Lock(): held(true) { pthread_mutex_lock(&mutex); }
    void release() { if(held) { held=false; pthread_mutex_unlock(&mutex); } }
    ~Lock() { release(); }
};
std::string hex(const unsigned char* data, size_t length) {
    static const char digits[]="0123456789abcdef"; std::string out;
    for(size_t i=0;i<length;++i) { out+=digits[data[i]>>4]; out+=digits[data[i]&15]; } return out;
}
bool isHex(const std::string& s, size_t length) { return s.size()==length && s.find_first_not_of("0123456789abcdef")==std::string::npos; }
std::string unhex(const std::string& s) {
    std::string out; for(size_t i=0;i+1<s.size();i+=2) { unsigned int b=0; sscanf(s.substr(i,2).c_str(),"%02x",&b); out+=static_cast<char>(b); } return out;
}
std::string randomHex(size_t size) {
    unsigned char bytes[32]; if(size>sizeof(bytes) || microhad_crypto_init()!=0 || psa_generate_random(bytes,size)!=PSA_SUCCESS) return "";
    return hex(bytes,size);
}
std::string digest(const std::string& value) {
    unsigned char out[32]; size_t length=0;
    if(microhad_crypto_init()!=0 || psa_hash_compute(PSA_ALG_SHA_256,reinterpret_cast<const unsigned char*>(value.data()),value.size(),out,sizeof(out),&length)!=PSA_SUCCESS) return "";
    return hex(out,length);
}
std::string mac(const std::string& keyHex, const std::string& message) {
    std::string key=unhex(keyHex); psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes,PSA_KEY_TYPE_HMAC); psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_SIGN_MESSAGE); psa_set_key_algorithm(&attributes,PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id=0; unsigned char out[32]; size_t length=0;
    if(microhad_crypto_init()!=0 || psa_import_key(&attributes,reinterpret_cast<const unsigned char*>(key.data()),key.size(),&id)!=PSA_SUCCESS) return "";
    psa_status_t status=psa_mac_compute(id,PSA_ALG_HMAC(PSA_ALG_SHA_256),reinterpret_cast<const unsigned char*>(message.data()),message.size(),out,sizeof(out),&length);
    psa_destroy_key(id); return status==PSA_SUCCESS ? hex(out,length) : "";
}
bool equal(const std::string& a,const std::string& b) {
    if(a.size()!=b.size() || a.empty()) return false;
    unsigned char difference=0;
    for(size_t i=0;i<a.size();++i) difference|=a[i]^b[i];
    return difference==0;
}
int slot(const State& state,int role) { while(role>=0 && state.passwords[role].verifier.empty()) --role; return role; }
int openRole(const State& state) { int role=-1; while(role<2 && slot(state,role+1)<0) ++role; return role; }
/* Role order and separators are part of the v2 key derivation protocol.
 * Every password change changes the key, without storing an extra secret. */
std::string deriveSigningKey(const State& state) {
    std::string material="microhad-session-key-v2\n";
    for(int i=0;i<3;++i) material+=std::string(roles[i])+"\n"+state.passwords[i].salt+"\n"+state.passwords[i].verifier+"\n";
    return digest(material);
}
bool tokenShape(const std::string& value) {
    return isHex(value,116) && value.compare(0,2,"02")==0 && value[2]=='0' && value[3]>='0' && value[3]<='2';
}
int signedRole(const std::string& signingKey,const std::string& value) {
    if(!isHex(signingKey,64) || !tokenShape(value)) return -1;
    const std::string payload=value.substr(0,52);
    if(!equal(value.substr(52),mac(signingKey,"microhad-session-token-v2\n"+payload))) return -1;
    uint64_t expires=0;
    for(size_t i=4;i<20;++i) expires=(expires<<4)+static_cast<unsigned int>(value[i]<='9' ? value[i]-'0' : value[i]-'a'+10);
    const time_t now=time(0);
    if(now<0 || (expires!=0 && expires<=static_cast<uint64_t>(now))) return -1;
    return value[3]-'0';
}
std::string issueToken(const State& state,int role,bool persistent) {
    const time_t now=time(0);
    const std::string nonce=randomHex(16);
    if(now<0 || nonce.empty() || !isHex(state.signingKey,64)) return "";
    const uint64_t expires=persistent ? 0 : static_cast<uint64_t>(now)+43200;
    std::ostringstream out;
    out<<"02"<<std::hex<<std::setfill('0')<<std::setw(2)<<role<<std::setw(16)<<expires<<nonce;
    const std::string payload=out.str();
    const std::string signature=mac(state.signingKey,"microhad-session-token-v2\n"+payload);
    return signature.empty() ? "" : payload+signature;
}
void prune(State& state) {
    long now=time(0);
    for(std::map<std::string,Challenge>::iterator i=state.challenges.begin();i!=state.challenges.end();) { if(i->second.expires<=now) state.challenges.erase(i++); else ++i; }
}
bool save(const std::string& path,const State& state) {
    std::ostringstream out;
    for(int i=0;i<3;++i) out<<"{\"role\":"<<i<<",\"salt\":\""<<state.passwords[i].salt<<"\",\"verifier\":\""<<state.passwords[i].verifier<<"\"}\n";
    std::string data=out.str(), temporary=path+".tmp";
    int fd=open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600); if(fd<0) return false;
    bool ok=fchmod(fd,0600)==0; size_t offset=0;
    while(ok && offset<data.size()) { ssize_t n=write(fd,data.data()+offset,data.size()-offset); if(n<=0) ok=false; else offset+=n; }
    if(fsync(fd)!=0) ok=false;
    if(close(fd)!=0) ok=false;
    if(ok) ok=rename(temporary.c_str(),path.c_str())==0;
    if(!ok) unlink(temporary.c_str());
    return ok;
}
void load(const std::string& path,State& state) {
    state.loaded=true; struct stat st;
    if(lstat(path.c_str(),&st)!=0) {
        state.valid=errno==ENOENT;
        if(state.valid) { state.signingKey=deriveSigningKey(state); state.valid=isHex(state.signingKey,64); }
        return;
    }
    if(!S_ISREG(st.st_mode) || (st.st_mode&077)!=0 || st.st_size>65536) { state.valid=false; return; }
    std::ifstream input(path.c_str()); std::string line; int passwords=0;
    while(std::getline(input,line)) {
        long role=-1; std::string token;
        if(!jsonLongField(line,"role",&role) || role<0 || role>2) { state.valid=false; break; }
        if(jsonStringField(line,"token",&token)) {
            // Accept the old file layout, but never authorize legacy sessions.
            long expires=0;
            if(!isHex(token,64) || !jsonLongField(line,"expires",&expires) || expires<0) { state.valid=false; break; }
        } else {
            Password& password=state.passwords[role];
            if(passwords&(1<<role)) { state.valid=false; break; } passwords|=1<<role;
            if(!jsonStringField(line,"salt",&password.salt) || !jsonStringField(line,"verifier",&password.verifier) ||
               (!password.verifier.empty() && (!isHex(password.salt,32) || !isHex(password.verifier,64))) || (password.verifier.empty() && !password.salt.empty())) { state.valid=false; break; }
        }
    }
    if(input.bad() || passwords!=7) state.valid=false;
    if(state.valid) { state.signingKey=deriveSigningKey(state); state.valid=isHex(state.signingKey,64); }
    prune(state);
}
std::string header(const std::string& request,const std::string& name) {
    size_t at=request.find("\r\n"), end=request.find("\r\n\r\n");
    while(at!=std::string::npos && at<end) {
        size_t next=request.find("\r\n",at+2), colon=request.find(':',at+2);
        if(colon<next) { std::string key=request.substr(at+2,colon-at-2); std::transform(key.begin(),key.end(),key.begin(),::tolower);
            if(key==name) { size_t start=request.find_first_not_of(" \t",colon+1); return start<next ? request.substr(start,next-start) : ""; } }
        at=next;
    } return "";
}
std::string token(const std::string& request) {
    std::string auth=header(request,"authorization"); if(auth.compare(0,7,"Bearer ")==0) return auth.substr(7);
    std::string protocols=header(request,"sec-websocket-protocol");
    size_t at=protocols.find("microhad-auth."); if(at!=std::string::npos) { at+=14; size_t end=protocols.find_first_of(", \t",at); return protocols.substr(at,end-at); }
    return "";
}
/* Explicit permissions; configuration/diagnostics never use the read-only default. */
int required(const std::string& method,const std::string& path) {
    if(method=="GET") {
        if(path=="/api/status" || path=="/api/matter/status" || path=="/api/thread/status" || path=="/api/dashboard/config" || path=="/api/scenes/config" || path=="/api/companion/config" || path=="/api/push/vapid-key") return 0;
        if(path.compare(0,13,"/api/devices/")==0) return path.find("/delete")!=std::string::npos ? 2 : 0;
        if(path=="/api/matter/relay") return 2;
        if(path.compare(0,17,"/api/matter/jobs/")==0) return 0;
        if(path.compare(0,5,"/api/")==0) return 2;
    }
    if(method=="POST") {
        if(path=="/api/push/devices/register" || path=="/api/push/subscriptions") return 0;
        if(path=="/api/matter/devices/read") return 0;
        if(path=="/api/matter/devices/command") return 1;
        if(path.compare(0,13,"/api/devices/")==0) {
            size_t slash=path.find_last_of('/'); std::string action=path.substr(slash+1);
            if(action=="light" || action=="thermostat" || action=="identify" || action=="refresh") return 1;
        }
        if((path.compare(0,13,"/api/devices/")==0 || path.compare(0,12,"/api/shelly/")==0) && path.size()>6 && path.substr(path.size()-6)=="/actor") return 1;
        if(path.compare(0,12,"/api/scenes/")==0 && path!="/api/scenes/config") return 1;
        if(path.compare(0,5,"/api/")==0) return 2;
    }
    return 2;
}
/* Returns true when handled (response supplied), false when dispatch is allowed. */
bool gate(const std::string& webui,const std::string& request,int* status,std::string* response) {
    std::istringstream first(request.substr(0,request.find("\r\n"))); std::string method,path; first>>method>>path;
    if(path.compare(0,5,"/api/")!=0) return false;
    // Shelly callbacks retain their independently validated per-device tokens.
    if(method=="GET" && path.compare(0,20,"/api/shelly/callback/")==0) return false;
    std::string origin=header(request,"origin"), host=header(request,"host");
    if(!origin.empty() && origin!="http://"+host && origin!="https://"+host) {
        *status=403; *response="{\"error\":\"cross-origin request denied\"}"; return true;
    }
    path=path.substr(0,path.find('?'));
    const std::string file=webui.substr(0,webui.find_last_of('/'))+"/api-access.json";
    Lock lock; State& state=states[file]; if(!state.loaded) load(file,state);
    *status=503; *response="{\"error\":\"access storage unavailable\"}"; if(!state.valid) return true;
    // Ordinary requests only snapshot shared configuration. Signature checks run
    // outside the auth mutex, with no session lookup or housekeeping scan.
    if(path.compare(0,10,"/api/auth/")!=0) {
        const int anonymousRole=openRole(state);
        const std::string signingKey=state.signingKey;
        lock.release();
        const int role=std::max(anonymousRole,signedRole(signingKey,token(request)));
        const int needed=required(method,path);
        if(role<needed) { *status=role<0?401:403; *response="{\"error\":\""+std::string(roles[needed])+" access required\"}"; return true; }
        return false;
    }
    prune(state);
    const int role=std::max(openRole(state),signedRole(state.signingKey,token(request)));
    size_t bodyAt=request.find("\r\n\r\n"); std::string body=bodyAt==std::string::npos ? "" : request.substr(bodyAt+4);
    *status=200;
    if(method=="GET" && path=="/api/auth/status") {
        std::ostringstream out; out<<"{\"role\":"<<role<<",\"passwords\":[";
        for(int i=0;i<3;++i) { if(i) out<<","; out<<(state.passwords[i].verifier.empty()?"false":"true"); } out<<"]}"; *response=out.str(); return true;
    }
    if(method=="POST" && path.compare(0,10,"/api/auth/")==0 && header(request,"content-type").compare(0,16,"application/json")!=0) {
        *status=400; *response="{\"error\":\"JSON content type required\"}"; return true;
    }
    if(method=="POST" && path=="/api/auth/challenge") {
        std::string requested; jsonStringField(body,"role",&requested); int wanted=-1;
        for(int i=0;i<3;++i) if(requested==roles[i]) wanted=i;
        if(wanted<0) { *status=400; *response="{\"error\":\"invalid role\"}"; return true; }
        long now=time(0); if(now-state.window>=60) { state.window=now; state.attempts=0; }
        if(++state.attempts>60 || state.challenges.size()>=64) { *status=429; *response="{\"error\":\"try again later\"}"; return true; }
        int effective=slot(state,wanted);
        if(effective<0) { *response="{\"open\":true}"; return true; }
        std::string nonce=randomHex(32); if(nonce.empty()) { *status=503; return true; }
        Challenge challenge; challenge.role=wanted; challenge.slot=effective; challenge.expires=now+60; state.challenges[nonce]=challenge;
        *response="{\"challenge\":\""+nonce+"\",\"salt\":\""+state.passwords[effective].salt+"\",\"iterations\":100000}"; return true;
    }
    if(method=="POST" && path=="/api/auth/login") {
        std::string nonce,proof,mode; jsonStringField(body,"challenge",&nonce); jsonStringField(body,"proof",&proof); jsonStringField(body,"mode",&mode);
        std::map<std::string,Challenge>::iterator found=state.challenges.find(nonce);
        *status=401; *response="{\"error\":\"invalid login\"}";
        if(found==state.challenges.end()) return true;
        Challenge challenge=found->second; state.challenges.erase(found);
        if(mode!="persistent" && mode!="browser") return true;
        std::string message="microhad-login-v1\n"+nonce+"\n"+roles[challenge.role]+"\n"+mode;
        if(!isHex(proof,64) || !equal(proof,mac(state.passwords[challenge.slot].verifier,message))) return true;
        const std::string value=issueToken(state,challenge.role,mode=="persistent");
        if(value.empty()) { *status=503; *response="{\"error\":\"cannot sign session\"}"; return true; }
        *status=200; *response="{\"token\":\""+value+"\"}"; return true;
    }
    if(method=="POST" && path=="/api/auth/password") {
        if(role<2) { *status=role<0?401:403; *response="{\"error\":\"administrator access required\"}"; return true; }
        std::string requested,saltValue,verifier; jsonStringField(body,"role",&requested); int target=-1;
        for(int i=0;i<3;++i) if(requested==roles[i]) target=i;
        if(target<0 || !jsonStringField(body,"salt",&saltValue) || !jsonStringField(body,"verifier",&verifier) ||
           (!verifier.empty() && (!isHex(saltValue,32) || !isHex(verifier,64))) || (verifier.empty() && !saltValue.empty())) {
            *status=400; *response="{\"error\":\"invalid password verifier\"}"; return true;
        }
        State next=state; next.passwords[target].salt=saltValue; next.passwords[target].verifier=verifier; next.challenges.clear();
        next.signingKey=deriveSigningKey(next);
        if(!isHex(next.signingKey,64)) { *status=503; *response="{\"error\":\"cannot derive signing key\"}"; return true; }
        if(!save(file,next)) { *status=503; *response="{\"error\":\"cannot persist password\"}"; return true; }
        state=next; *response="{\"ok\":true}"; return true;
    }
    *status=404; *response="{\"error\":\"unknown authentication endpoint\"}"; return true;
}
int currentRole(const std::string& webui,const std::string& request) {
    Lock lock; State& state=states[webui.substr(0,webui.find_last_of('/'))+"/api-access.json"];
    if(!state.valid) return -1;
    const int anonymousRole=openRole(state);
    const std::string signingKey=state.signingKey;
    lock.release();
    return std::max(anonymousRole,signedRole(signingKey,token(request)));
}
} // namespace api_auth
