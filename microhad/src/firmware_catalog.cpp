#include "firmware_catalog.h"
#include "https_client.h"
#include "json_util.h"
#include <pthread.h>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <vector>
#include <cctype>
#include <cstdlib>
#include "../config/firmware-catalogs.inc"

namespace {
struct Provider { std::string id, label, url, ca; long manufacturer; };
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
std::vector<Provider> providers;
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
    providers.clear(); bundle = caBundle; configurationError.clear();
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
namespace {
const unsigned long catalogLimit = 4UL * 1024 * 1024;
class CatalogResponse : public HttpsCatalogRawSink {
public:
    CatalogResponse(FirmwareCatalogSink* sink, bool* started, bool dcl, std::string* error)
        : sink_(sink), started_(started), dcl_(dcl), error_(error), state_(Headers),
          expected_(-1), received_(0), chunkRemaining_(0), trailerBytes_(0),
          jsonSeen_(false), redirect_(false), synthetic_(false) {}
    bool write(const char* data, unsigned long size) {
        unsigned long at = 0;
        while (at < size) {
            if (state_ == Headers) {
                header_.push_back(data[at++]);
                if (header_.size() > 8192) return fail("Catalog headers too large");
                if (header_.size() >= 4 && header_.compare(header_.size()-4, 4, "\r\n\r\n") == 0) {
                    if (!parseHeaders()) return false;
                    if (redirect_ || synthetic_) return false;
                }
            } else if (state_ == Body) {
                unsigned long n = size - at;
                if (expected_ >= 0 && n > static_cast<unsigned long>(expected_) - received_) return fail("Excess catalog data");
                if (!emit(data + at, n)) return false;
                at += n;
            } else if (state_ == ChunkSize || state_ == Trailers) {
                char c = data[at++];
                if (state_ == Trailers && ++trailerBytes_ > 8192) return fail("Catalog trailers too large");
                line_.push_back(c);
                if (line_.size() > 256) return fail("Invalid catalog chunk line");
                if (line_.size() >= 2 && line_.compare(line_.size()-2, 2, "\r\n") == 0) {
                    line_.resize(line_.size()-2);
                    if (state_ == Trailers) {
                        if (line_.empty()) state_ = Done;
                    } else {
                        std::string::size_type semi = line_.find(';');
                        const std::string number = line_.substr(0, semi);
                        if (number.empty() || number.size() > 8 || number.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return fail("Invalid catalog chunk size");
                        char* end = 0; chunkRemaining_ = std::strtoul(number.c_str(), &end, 16);
                        if (!end || *end || chunkRemaining_ > catalogLimit - received_) return fail("Catalog too large");
                        state_ = chunkRemaining_ ? ChunkData : Trailers;
                    }
                    line_.clear();
                }
            } else if (state_ == ChunkData) {
                const unsigned long n = std::min(size-at, chunkRemaining_);
                if (!emit(data+at, n)) return false;
                at += n; chunkRemaining_ -= n;
                if (!chunkRemaining_) { state_ = ChunkEnd; line_.clear(); }
            } else if (state_ == ChunkEnd) {
                line_.push_back(data[at++]);
                if (line_.size() == 2) {
                    if (line_ != "\r\n") return fail("Invalid catalog chunk terminator");
                    line_.clear(); state_ = ChunkSize;
                }
            } else return fail("Excess catalog data");
        }
        return true;
    }
    bool complete() {
        if (synthetic_) return true;
        if (!jsonSeen_ || (state_ != Done && state_ != Body) || (expected_ >= 0 && received_ != static_cast<unsigned long>(expected_))) return fail("Incomplete catalog response");
        return true;
    }
    bool redirected() const { return redirect_; }
    bool synthetic() const { return synthetic_; }
    const std::string& location() const { return location_; }
private:
    enum State { Headers, Body, ChunkSize, ChunkData, ChunkEnd, Trailers, Done };
    FirmwareCatalogSink* sink_; bool* started_; bool dcl_; std::string* error_;
    State state_; long expected_; unsigned long received_, chunkRemaining_, trailerBytes_;
    bool jsonSeen_, redirect_, synthetic_; std::string header_, line_, location_;
    bool fail(const char* message) { *error_ = message; return false; }
    bool emit(const char* data, unsigned long size) {
        if (size > catalogLimit - received_) return fail("Catalog too large");
        for (unsigned long i=0; i<size && !jsonSeen_; ++i) {
            const unsigned char c = static_cast<unsigned char>(data[i]);
            if (c != ' ' && c != '\r' && c != '\n' && c != '\t') {
                if (c != (dcl_ ? '{' : '[')) return fail("Invalid catalog body");
                jsonSeen_ = true;
            }
        }
        if (size && !*started_) { *started_ = true; if (!sink_->begin()) return fail("Catalog client disconnected"); }
        if (size && !sink_->write(data,size)) return fail("Catalog client disconnected");
        received_ += size; return true;
    }
    bool parseHeaders() {
        std::istringstream lines(header_); std::string protocol, line; int status = 0;
        lines >> protocol >> status; std::getline(lines, line);
        if (protocol != "HTTP/1.1" && protocol != "HTTP/1.0") return fail("Invalid catalog HTTP status");
        bool chunked = false, lengthSeen = false;
        while (std::getline(lines, line)) {
            if (line == "\r" || line.empty()) break;
            const std::string::size_type colon = line.find(':'); if (colon == std::string::npos) return fail("Invalid catalog header");
            std::string name = line.substr(0, colon), value = line.substr(colon+1);
            for (unsigned i=0; i<name.size(); ++i) name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
            while (!value.empty() && (value[0]==' ' || value[0]=='\t')) value.erase(0,1);
            if (!value.empty() && value[value.size()-1]=='\r') value.erase(value.size()-1);
            if (name == "location") location_ = value;
            else if (name == "content-length") {
                if (lengthSeen || value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size()>10) return fail("Invalid catalog length");
                unsigned long length = std::strtoul(value.c_str(),0,10);
                if (length > catalogLimit) return fail("Catalog too large");
                expected_ = static_cast<long>(length); lengthSeen = true;
            } else if (name == "transfer-encoding") {
                if (chunked || value != "chunked") return fail("Unsupported catalog encoding");
                chunked = true;
            } else if (name == "content-encoding" && value != "identity") return fail("Unsupported catalog compression");
        }
        if (status >= 300 && status <= 399 && !location_.empty()) { redirect_ = true; return true; }
        if (dcl_ && status == 404) {
            static const char missing[] = "{\"notFound\":true}";
            *started_ = true;
            if (!sink_->begin() || !sink_->write(missing, sizeof(missing)-1)) return fail("Catalog client disconnected");
            synthetic_ = true; return true;
        }
        if (status != 200) return fail("Catalog returned a non-success HTTP status");
        if (chunked && lengthSeen) return fail("Ambiguous catalog framing");
        state_ = chunked ? ChunkSize : Body;
        return true;
    }
};
}

bool streamFirmwareCatalog(const std::string& id, FirmwareCatalogSink* sink, bool* started, std::string* error) {
    *started = false; error->clear();
    if (pthread_mutex_trylock(&mutex) != 0) { *error = "A catalog request is already in progress; retry shortly"; return false; }
    Unlock unlock; (void)unlock;
    if (!configurationError.empty()) { *error = configurationError; return false; }
    const std::string::size_type slash = id.find('/');
    const std::string providerId = id.substr(0, slash);
    const bool dcl = providerId == "matter-dcl-production" || providerId == "matter-dcl-testnet";
    std::string suffix;
    if (dcl) {
        if (slash == std::string::npos) { *error="Missing Matter catalog identifiers"; return false; }
        suffix = id.substr(slash);
        std::istringstream parts(suffix.substr(1)); std::string part; unsigned count=0;
        while (std::getline(parts, part, '/')) {
            if (part.empty() || part.size()>10 || part.find_first_not_of("0123456789")!=std::string::npos || (part.size()>1 && part[0]=='0')) { *error="Invalid Matter catalog identifier"; return false; }
            unsigned long long n=std::strtoull(part.c_str(),0,10);
            if (n>(count<2?65535ULL:4294967295ULL)) { *error="Invalid Matter catalog identifier"; return false; }
            ++count;
        }
        if ((count!=2 && count!=3) || suffix[suffix.size()-1]=='/') { *error="Invalid Matter catalog path"; return false; }
    } else if (slash != std::string::npos) { *error="Invalid catalog path"; return false; }
    const Provider* provider = 0;
    for (unsigned i=0;i<providers.size();++i) if (providers[i].id == providerId) provider = &providers[i];
    if (!provider) { *error = "Unknown firmware catalog"; return false; }
    const std::string destination = provider->url + suffix;
    std::string url = destination;
    for (unsigned redirect=0; redirect<=3; ++redirect) {
        std::string host, path;
        if (!firmwareCatalogDestination(destination,url) || !splitUrl(url,&host,&path)) { *error="Catalog destination is not configured"; return false; }
        const std::string ca = provider->ca == "ikea" ? ikeaCatalogCa : provider->ca.empty() ? bundle : provider->ca;
        CatalogResponse response(sink,started,dcl,error);
        const bool ok = httpsCatalogStream(host,path,ca,&response,error);
        if (response.synthetic()) return true;
        if (response.redirected()) {
            if (redirect == 3) { *error = "Too many catalog redirects"; return false; }
            url = response.location()[0] == '/' ? "https://" + host + response.location() : response.location();
            continue;
        }
        if (!ok) return false;
        return response.complete();
    }
    *error = "Too many catalog redirects"; return false;
}
