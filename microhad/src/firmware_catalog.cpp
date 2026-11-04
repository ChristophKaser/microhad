#include "firmware_catalog.h"
#include "https_client.h"
#include "json_util.h"
#include <pthread.h>
#include <ctime>
#include <sstream>
#include <vector>
#include <map>
#include <cctype>
#include <cstdlib>
#include "../config/firmware-catalogs.inc"

namespace {
struct Provider { std::string id, label, url, ca; long manufacturer; };
struct Cached { std::string body; time_t at; Cached():at(0){} };
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::vector<Provider> providers;
std::map<std::string, Cached> cache;
std::string bundle, configurationError;
struct Unlock { ~Unlock(){pthread_mutex_unlock(&mutex);} };
bool splitUrl(const std::string& url, std::string* host, std::string* path) {
    if (url.compare(0, 8, "https://") != 0 || url.size() > 2048 ||
        url.find_first_of("\r\n\t \\#%@") != std::string::npos) return false;
    const std::string::size_type slash = url.find('/', 8);
    if (slash == std::string::npos) return false;
    *host = url.substr(8, slash - 8); *path = url.substr(slash);
    return !host->empty() && host->size() <= 253 && (*host)[0] != '.' &&
        host->find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") == std::string::npos &&
        path->find("..") == std::string::npos && path->find('?') == std::string::npos;
}
}

bool firmwareCatalogDestination(const std::string& configuredUrl, const std::string& candidate) {
    std::string host, path;
    // Each configured catalog URL is an exact destination allowlist entry.
    return configuredUrl == candidate && splitUrl(candidate, &host, &path);
}
void configureFirmwareCatalogs(const std::string& path, const std::string& caBundle) {
    pthread_mutex_lock(&mutex); Unlock unlock; (void)unlock;
    providers.clear(); cache.clear(); bundle = caBundle; configurationError.clear();
    std::string text = defaultFirmwareCatalogs;
    if (!path.empty() && (!readTextFile(path, &text) || text.size() > 32768)) {
        configurationError = "Cannot read firmware catalog configuration"; return;
    }
    std::istringstream lines(text); std::string line;
    while (std::getline(lines, line)) {
        if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
        Provider p; p.manufacturer = -1;
        std::string host, requestPath;
        if (!jsonStringField(line, "id", &p.id) || !jsonStringField(line, "label", &p.label) ||
            !jsonStringField(line, "url", &p.url) || !splitUrl(p.url, &host, &requestPath) ||
            p.id.empty() || p.id.size() > 64 || p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos ||
            p.label.empty() || providers.size() >= 16) { configurationError = "Invalid firmware catalog configuration"; break; }
        for (unsigned i=0;i<providers.size();++i) if (providers[i].id == p.id) configurationError = "Duplicate firmware catalog ID";
        if (!configurationError.empty()) break;
        jsonStringField(line, "ca", &p.ca);
        jsonLongField(line, "manufacturer", &p.manufacturer);
        if (p.manufacturer < -1 || p.manufacturer > 65535) { configurationError = "Invalid catalog manufacturer"; break; }
        providers.push_back(p);
    }
    if (!configurationError.empty()) providers.clear();
}
std::string firmwareCatalogsJson() {
    // Configuration is immutable after startup; catalog I/O must not block this list.
    if (!configurationError.empty()) return "null";
    std::ostringstream json; json << '[';
    for (unsigned i=0;i<providers.size();++i) {
        const Provider& p = providers[i];
        if (i) json << ',';
        json << "{\"id\":\"" << jsonEscape(p.id) << "\",\"label\":\"" << jsonEscape(p.label)
             << "\",\"url\":\"" << jsonEscape(p.url) << "\",\"gateway_only\":" << (!p.ca.empty() ? "true" : "false");
        if (p.manufacturer >= 0) json << ",\"manufacturer\":" << p.manufacturer;
        json << '}';
    }
    json << ']'; return json.str();
}
bool fetchFirmwareCatalog(const std::string& id, std::string* body, std::string* error) {
    if (pthread_mutex_trylock(&mutex) != 0) { *error = "A catalog request is already in progress; retry shortly"; return false; }
    Unlock unlock; (void)unlock;
    if (!configurationError.empty()) { *error = configurationError; return false; }
    const Provider* provider = 0;
    for (unsigned i=0;i<providers.size();++i) if (providers[i].id == id) provider = &providers[i];
    if (!provider) { *error = "Unknown firmware catalog"; return false; }
    if (cache[id].at && time(0) - cache[id].at < 3600) { *body = cache[id].body; return true; }
    std::string url = provider->url;
    for (unsigned redirect=0;redirect<=3;++redirect) {
        std::string host, path, response;
        if (!firmwareCatalogDestination(provider->url, url) || !splitUrl(url, &host, &path)) {
            *error = "Catalog destination is not configured"; return false;
        }
        const std::string ca = provider->ca == "ikea" ? ikeaCatalogCa : provider->ca.empty() ? bundle : provider->ca;
        if (!httpsCatalogGet(host, path, ca, &response, error)) return false;
        std::string::size_type separator = response.find("\r\n\r\n");
        if (separator == std::string::npos || separator > 65536) { *error = "Invalid catalog HTTP response"; return false; }
        std::istringstream statusLine(response); std::string protocol; int status = 0;
        statusLine >> protocol >> status;
        if (protocol != "HTTP/1.1" && protocol != "HTTP/1.0") { *error = "Invalid catalog HTTP status"; return false; }
        std::istringstream headers(response.substr(0, separator)); std::string line, location;
        long contentLength = -1; bool chunked = false;
        while (std::getline(headers, line)) {
            std::string::size_type colon = line.find(':'); if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            for (unsigned i=0;i<name.size();++i) name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value[0]==' ' || value[0]=='\t')) value.erase(0,1);
            if (!value.empty() && value[value.size()-1]=='\r') value.erase(value.size()-1);
            if (name == "location") location = value;
            if (name == "content-length") { char* end=0; contentLength = strtol(value.c_str(), &end, 10); if (!end || *end || contentLength < 0) { *error="Invalid catalog length";return false; } }
            if (name == "transfer-encoding") { if (value != "chunked") { *error="Unsupported catalog encoding";return false; } chunked=true; }
            if (name == "content-encoding" && value != "identity") { *error="Unsupported catalog compression";return false; }
        }
        if (status >= 300 && status <= 399 && !location.empty() && redirect < 3) {
            url = location[0] == '/' ? "https://" + host + location : location;
            continue;
        }
        if (status != 200) { *error = "Catalog returned a non-success HTTP status"; return false; }
        *body = response.substr(separator+4);
        if (body->size() > 4*1024*1024 || (!chunked && contentLength >= 0 && static_cast<unsigned long>(contentLength) != body->size()) ||
            body->find_first_not_of(" \r\n\t") == std::string::npos || (*body)[body->find_first_not_of(" \r\n\t")] != '[') {
            body->clear(); *error = "Invalid or oversized catalog body"; return false;
        }
        // Bound total cache storage to one maximum-sized catalog.
        unsigned long total=body->size();
        for (std::map<std::string,Cached>::const_iterator i=cache.begin();i!=cache.end();++i) if (i->first!=id) total+=i->second.body.size();
        if (total>4*1024*1024) cache.clear();
        cache[id].body=*body;cache[id].at=time(0);return true;
    }
    *error = "Too many catalog redirects"; return false;
}
