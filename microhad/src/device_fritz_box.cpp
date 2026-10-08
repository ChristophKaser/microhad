#include "external_http_device.h"
#include "device_fritz_box.h"

#include <cstdlib>
#include <cstring>
#include "mbedtls/md5.h"

bool externalHttpRequest(const std::string& host, const std::string& username, const std::string& password,
                         const std::string& method, const std::string& path, const std::string& headers,
                         const std::string& requestBody, std::string* responseBody, unsigned short defaultPort,
                         bool digestOnly, bool acceptSoapFault);

namespace {
std::string md5Hex(const std::string& value) {
    unsigned char digest[16];
    if (mbedtls_md5_ret(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest) != 0) return "";
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned long i = 0; i < sizeof(digest); ++i) {
        result.push_back(hex[digest[i] >> 4]); result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::string lowerAscii(std::string value) {
    for (unsigned long i = 0; i < value.size(); ++i)
        if (value[i] >= 'A' && value[i] <= 'Z') value[i] = static_cast<char>(value[i] - 'A' + 'a');
    return value;
}
}

namespace FritzBoxProtocol {
std::string xmlValue(const std::string& xml, const std::string& tag) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    std::string::size_type at = xml.find(open);
    if (at == std::string::npos) return "";
    at += open.size();
    std::string::size_type end = xml.find(close, at);
    if (end == std::string::npos) return "";
    return xml.substr(at, end - at);
}

std::string xmlUnescape(const std::string& input) {
    std::string value = input;
    const char* entities[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
    const char* chars[] = {"&", "<", ">", "\"", "'"};
    for (int i = 0; i < 5; ++i) {
        std::string::size_type at = 0;
        while ((at = value.find(entities[i], at)) != std::string::npos) {
            value.replace(at, strlen(entities[i]), chars[i]);
            at += strlen(chars[i]);
        }
    }
    return value;
}

bool soapAction(const std::string& host, const std::string& username, const std::string& password,
                const std::string& service, const std::string& action, const std::string& path,
                const std::string& arguments, std::string* response, std::string* error) {
    const std::string authNs = "http://soap-authentication.org/digest/2001/10/";
    std::string actionXml = "<u:" + action + " xmlns:u=\"" + service + "\">" + arguments + "</u:" + action + ">";
    std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Header><h:InitChallenge xmlns:h=\"" + authNs + "\" s:mustUnderstand=\"1\"><UserID>" + username + "</UserID></h:InitChallenge></s:Header><s:Body>" + actionXml + "</s:Body></s:Envelope>";
    std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action + "\"\r\n";
    if (!externalHttpRequest(host, username, password, "POST", path, headers, body, response, 49000, true, true)) return false;
    std::string nonce = xmlValue(*response, "Nonce");
    std::string realm = xmlValue(*response, "Realm");
    if (nonce.empty() || realm.empty()) {
        std::string code = xmlValue(*response, "errorCode");
        if (!code.empty()) {
            if (error != 0) *error = "FRITZ!Box denied the TR-064 action (error " + code + ")";
            return false;
        }
        return true;
    }
    std::string secret = md5Hex(username + ":" + realm + ":" + password);
    std::string auth = md5Hex(secret + ":" + nonce);
    if (secret.empty() || auth.empty()) return false;
    std::string clientHeader = "<s:Header><h:ClientAuth xmlns:h=\"" + authNs + "\" s:mustUnderstand=\"1\"><Nonce>" + nonce + "</Nonce><Auth>" + auth + "</Auth><UserID>" + username + "</UserID><Realm>" + realm + "</Realm></h:ClientAuth></s:Header>";
    body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">" + clientHeader + "<s:Body>" + actionXml + "</s:Body></s:Envelope>";
    if (!externalHttpRequest(host, username, password, "POST", path, headers, body, response, 49000, true, true)) return false;
    std::string code = xmlValue(*response, "errorCode");
    if (!code.empty()) {
        if (error != 0) *error = "FRITZ!Box denied the TR-064 action (error " + code + ")";
        return false;
    }
    return true;
}

bool parseHostList(const std::string& xml, std::vector<FritzBoxClient>* clients) {
    clients->clear();
    std::string::size_type at = 0;
    while ((at = xml.find("<Item>", at)) != std::string::npos) {
        std::string::size_type end = xml.find("</Item>", at);
        if (end == std::string::npos) return false;
        std::string item = xml.substr(at, end - at);
        at = end + 7;
        std::string interfaceType = xmlValue(item, "InterfaceType");
        if (interfaceType != "802.11" && !interfaceType.empty()) continue;
        FritzBoxClient client;
        client.mac = lowerAscii(xmlValue(item, "MACAddress"));
        client.name = xmlUnescape(xmlValue(item, "X_AVM-DE_FriendlyName"));
        if (client.name.empty()) client.name = xmlUnescape(xmlValue(item, "HostName"));
        std::string active = xmlValue(item, "Active");
        if (client.mac.empty() || (active != "0" && active != "1")) continue;
        client.connected = active == "1";
        clients->push_back(client);
    }
    return true;
}

void mergeSignalList(const std::string& xml, std::vector<FritzBoxClient>* clients) {
    std::string::size_type at = 0;
    while ((at = xml.find("<Item>", at)) != std::string::npos) {
        std::string::size_type end = xml.find("</Item>", at);
        if (end == std::string::npos) break;
        std::string item = xml.substr(at, end - at); at = end + 7;
        std::string mac = lowerAscii(xmlValue(item, "AssociatedDeviceMACAddress"));
        std::string signalText = xmlValue(item, "X_AVM-DE_SignalStrength");
        char* numberEnd = 0;
        long signal = strtol(signalText.c_str(), &numberEnd, 10);
        if (numberEnd == signalText.c_str() || *numberEnd != 0 || signal < 0 || signal > 100) continue;
        for (unsigned long i = 0; i < clients->size(); ++i) if ((*clients)[i].mac == mac) {
            (*clients)[i].hasSignal = true; (*clients)[i].signalPercent = static_cast<int>(signal); break;
        }
    }
}
}
