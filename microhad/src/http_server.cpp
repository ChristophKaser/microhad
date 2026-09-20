#include "http_server.h"
#include "gateway.h"
#include "web_push.h"

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MICROHAD_MIPS_TARGET
extern "C" int microhad_tls_read_request(int fd, const char* certificatePath, const char* privateKeyPath,
                                      char* request, unsigned long capacity, void** session);
extern "C" int microhad_tls_write_close(void* session, const char* response, unsigned long length);
#endif

namespace {
#ifdef MICROHAD_MIPS_TARGET
void* activeTlsSession = 0;
#endif

void writeResponse(int fd, int status, const char* contentType, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Bad Request") << "\r\n"
             << "Content-Type: " << contentType << "\r\nCache-Control: no-store\r\nConnection: close\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::string wire = response.str();
    const char* cursor = wire.data();
    unsigned long remaining = wire.size();
#ifdef MICROHAD_MIPS_TARGET
    if (activeTlsSession != 0) {
        microhad_tls_write_close(activeTlsSession, wire.data(), wire.size());
        activeTlsSession = 0;
        return;
    }
#endif
    while (remaining != 0) {
        ssize_t written = write(fd, cursor, remaining);
        if (written <= 0) break;
        cursor += written;
        remaining -= static_cast<unsigned long>(written);
    }
}

std::string decodeComponent(const std::string& value) {
    std::string decoded;
    unsigned long i;
    for (i = 0; i < value.size(); ++i) {
        if (value[i] == '+' ) decoded += ' ';
        else if (value[i] == '%' && i + 2 < value.size()) {
            char hex[3];
            hex[0] = value[i + 1]; hex[1] = value[i + 2]; hex[2] = 0;
            decoded += static_cast<char>(strtoul(hex, 0, 16));
            i += 2;
        } else decoded += value[i];
    }
    return decoded;
}
}

HttpServer::HttpServer(int port, Gateway* gateway, const std::string& certificatePath, const std::string& privateKeyPath) :
    port_(port), listenFd_(-1), gateway_(gateway), webPush_(new WebPushStore(gateway->runtimeDirectory())),
    certificatePath_(certificatePath), privateKeyPath_(privateKeyPath) {
}

HttpServer::~HttpServer() {
    if (listenFd_ >= 0) close(listenFd_);
    delete webPush_;
}

bool HttpServer::start(std::string* error) {
    struct sockaddr_in address;
    int reuse = 1;
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) { *error = strerror(errno); return false; }
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<unsigned short>(port_));
    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 || listen(listenFd_, 8) != 0) {
        *error = strerror(errno); close(listenFd_); listenFd_ = -1; return false;
    }
    fcntl(listenFd_, F_SETFL, O_NONBLOCK);
    return true;
}

void HttpServer::poll() {
    int clientFd = accept(listenFd_, 0, 0);
    if (clientFd >= 0) {
        handleClient(clientFd);
        close(clientFd);
    }
}

void HttpServer::handleClient(int clientFd) {
    char buffer[2048];
    ssize_t count;
    std::string request;
#ifdef MICROHAD_MIPS_TARGET
    if (!certificatePath_.empty() || !privateKeyPath_.empty()) {
        if (certificatePath_.empty() || privateKeyPath_.empty()) return;
        if (microhad_tls_read_request(clientFd, certificatePath_.c_str(), privateKeyPath_.c_str(), buffer,
                                  sizeof(buffer) - 1, &activeTlsSession) != 0) return;
        count = static_cast<ssize_t>(strlen(buffer));
    } else {
        count = read(clientFd, buffer, sizeof(buffer) - 1);
    }
#else
    count = read(clientFd, buffer, sizeof(buffer) - 1);
#endif
    if (count <= 0) return;
    buffer[count] = 0;
    request.assign(buffer);
    if (request.find("GET /api/status ") == 0) {
        writeResponse(clientFd, 200, "application/json", gateway_->statusJson());
    } else if (request.find("POST /api/join ") == 0) {
        std::string error;
        if (gateway_->setJoining(300, &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"seconds\":300}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("GET /api/push/vapid-key ") == 0) {
        writeResponse(clientFd, 200, "application/json", "{\"public_key\":\"" + webPush_->publicKey() + "\"}");
    } else if (request.find("POST /api/push/subscriptions ") == 0) {
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        std::string error;
        if (bodyAt != std::string::npos && webPush_->addSubscription(request.substr(bodyAt + 4), &error))
            writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("GET /sw.js ") == 0) {
        const char* worker = "self.addEventListener('push',function(e){var d={title:'MicroHAD',body:''};if(e.data){d=e.data.json()}e.waitUntil(self.registration.showNotification(d.title,{body:d.body}))});";
        writeResponse(clientFd, 200, "application/javascript", worker);
    } else if (request.find("POST /api/network/create ") == 0) {
        std::string error;
        if (gateway_->createFreshNetwork(&error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true,\"state\":\"forming\"}");
        else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
    } else if (request.find("POST /api/devices/") == 0) {
        const std::string prefix = "POST /api/devices/";
        std::string::size_type nameAt = request.find("/name ", prefix.size());
        std::string::size_type alertAt = request.find("/temperature-alert ", prefix.size());
        std::string::size_type bodyAt = request.find("\r\n\r\n");
        if (bodyAt == std::string::npos || (nameAt == std::string::npos && alertAt == std::string::npos)) {
            writeResponse(clientFd, 400, "application/json", "{\"error\":\"invalid device rename request\"}");
        } else {
            std::string::size_type routeAt = nameAt == std::string::npos ? alertAt : nameAt;
            std::string ieeeAddress = decodeComponent(request.substr(prefix.size(), routeAt - prefix.size()));
            std::string body = decodeComponent(request.substr(bodyAt + 4));
            std::string error;
            if (alertAt != std::string::npos) {
                std::string::size_type lowAt = body.find("low=");
                std::string::size_type highAt = body.find("high=");
                std::string lowValue;
                std::string highValue;
                if (lowAt != std::string::npos) {
                    std::string::size_type end = body.find('&', lowAt);
                    lowValue = body.substr(lowAt + 4, end == std::string::npos ? std::string::npos : end - lowAt - 4);
                }
                if (highAt != std::string::npos) {
                    std::string::size_type end = body.find('&', highAt);
                    highValue = body.substr(highAt + 5, end == std::string::npos ? std::string::npos : end - highAt - 5);
                }
                bool hasLow = !lowValue.empty();
                bool hasHigh = !highValue.empty();
                double low = hasLow ? strtod(lowValue.c_str(), 0) : 0;
                double high = hasHigh ? strtod(highValue.c_str(), 0) : 0;
                if (gateway_->setTemperatureAlerts(ieeeAddress, hasLow, low, hasHigh, high, &error))
                    writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
                else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
            } else if (gateway_->renameDevice(ieeeAddress, body, &error)) writeResponse(clientFd, 200, "application/json", "{\"ok\":true}");
            else writeResponse(clientFd, 400, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
        }
    } else if (request.find("GET / ") == 0) {
        const char* page = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>MicroHAD</title><style>body{font:16px sans-serif;max-width:48rem;margin:auto;padding:1rem}button{padding:.7rem;margin:.2rem}.device{border-bottom:1px solid #ddd;padding:.6rem}</style><h1>MicroHAD</h1><p><button onclick=\"post('/api/network/create')\">Create fresh network</button><button onclick=\"post('/api/join')\">Allow joining (5 min)</button><button onclick=\"enablePush()\">Enable browser alerts</button></p><p id=state>Loading…</p><div id=devices></div><script>function post(u,b){return fetch(u,{method:'POST',body:b||''}).then(update)}function b64(s){s+=(s.length%4?'='.repeat(4-s.length%4):'');return Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')),function(c){return c.charCodeAt(0)})}function enablePush(){if(!('serviceWorker'in navigator)||!('PushManager'in window)){alert('Push is unavailable in this browser');return}navigator.serviceWorker.register('/sw.js').then(function(r){return fetch('/api/push/vapid-key').then(function(x){return x.json()}).then(function(k){return r.pushManager.subscribe({userVisibleOnly:true,applicationServerKey:b64(k.public_key)})})}).then(function(s){return fetch('/api/push/subscriptions',{method:'POST',body:JSON.stringify(s)})}).then(function(){alert('Browser alerts enabled')}).catch(function(e){alert('Could not enable alerts: '+e)})}function rename(a,n){var v=prompt('Device name',n);if(v)post('/api/devices/'+encodeURIComponent(a)+'/name',v)}function temperatures(a,d){var l=prompt('Low alert °C (blank disables)',d.has_low_temperature_alert?d.low_temperature_alert:'');if(l===null)return;var h=prompt('High alert °C (blank disables)',d.has_high_temperature_alert?d.high_temperature_alert:'');if(h===null)return;post('/api/devices/'+encodeURIComponent(a)+'/temperature-alert','low='+encodeURIComponent(l)+'&high='+encodeURIComponent(h))}function update(){fetch('/api/status').then(function(r){return r.json()}).then(function(v){document.getElementById('state').textContent='Coordinator: '+(v.online?'online':'offline')+' | Network: '+(v.network_formed?'formed':v.network_forming?'forming':'not formed');document.getElementById('devices').innerHTML=v.devices.map(function(d){var s=d.ieee_address+' · '+d.kind;if(d.has_contact)s+=' · '+(d.contact_open?'OPEN':'closed');if(d.has_temperature)s+=' · '+d.temperature_celsius+' °C';if(d.has_battery)s+=' · battery '+d.battery_percent+'%';var b='<button onclick=\"rename(\\\''+d.ieee_address+'\\\',\\\''+d.name.replace(/'/g,'')+'\\\')\">Rename</button>';if(d.has_temperature)b+=' <button onclick=\"temperatures(\\\''+d.ieee_address+'\\\',JSON.parse(this.dataset.device))\" data-device=\''+JSON.stringify(d).replace(/'/g,'&#39;')+'\'>Temperature alerts</button>';return '<div class=device><b>'+d.name+'</b> '+b+'<br>'+s+'</div>'}).join('')})}update();setInterval(update,2000)</script>";
        writeResponse(clientFd, 200, "text/html; charset=utf-8", page);
    } else {
        writeResponse(clientFd, 400, "application/json", "{\"error\":\"unknown endpoint\"}");
    }
}
