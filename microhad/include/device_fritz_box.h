#ifndef MICROHAD_ZIGBEE_DEVICE_FRITZ_BOX_H
#define MICROHAD_ZIGBEE_DEVICE_FRITZ_BOX_H

#include <string>
#include <vector>

struct FritzBoxClient { std::string id, mac, name; bool connected; bool hasSignal; int signalPercent; FritzBoxClient(); };
struct FritzBoxDevice { std::string id, name, host, username, password; unsigned int pollSeconds; long nextPoll; std::vector<FritzBoxClient> clients; FritzBoxDevice(); };

namespace FritzBoxProtocol {
bool soapAction(const std::string& host, const std::string& username, const std::string& password,
                const std::string& service, const std::string& action, const std::string& path,
                const std::string& arguments, std::string* response, std::string* error = 0);
std::string xmlValue(const std::string& xml, const std::string& tag);
std::string xmlUnescape(const std::string& value);
bool parseHostList(const std::string& xml, std::vector<FritzBoxClient>* clients);
void mergeSignalList(const std::string& xml, std::vector<FritzBoxClient>* clients);
}

#endif
