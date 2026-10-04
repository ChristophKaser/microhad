#include "external_http_device.h"
#include "gateway.h"
#include "sensor_history.h"
#include "json_util.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

void runExternalHttpDeviceWorkerUnitTests() {
    std::vector<FritzBoxClient> fritzClients;
    assert(ExternalHttpDeviceWorker::parseFritzHostList("<List><Item><MACAddress>AA:BB:CC:DD:EE:01</MACAddress><Active>1</Active><HostName>Phone</HostName><InterfaceType>802.11</InterfaceType></Item><Item><MACAddress>aa:bb:cc:dd:ee:02</MACAddress><Active>0</Active><HostName>Tablet</HostName><InterfaceType>802.11</InterfaceType></Item><Item><MACAddress>aa:bb:cc:dd:ee:03</MACAddress><Active>1</Active><HostName>Wired</HostName><InterfaceType>Ethernet</InterfaceType></Item></List>", &fritzClients));
    assert(fritzClients.size() == 2);
    assert(fritzClients[0].mac == "aa:bb:cc:dd:ee:01" && fritzClients[0].connected);
    assert(fritzClients[1].mac == "aa:bb:cc:dd:ee:02" && !fritzClients[1].connected);
    FritzBoxProtocol::mergeSignalList("<List><Item><AssociatedDeviceMACAddress>AA:BB:CC:DD:EE:01</AssociatedDeviceMACAddress><X_AVM-DE_SignalStrength>72</X_AVM-DE_SignalStrength></Item></List>", &fritzClients);
    assert(fritzClients[0].hasSignal && fritzClients[0].signalPercent == 72);
    double watts = 0;
    assert(ExternalHttpDeviceWorker::parsePowerStatus("{\"meters\":[{\"power\":3500.04,\"is_valid\":true}]}", &watts));
    assert(watts == 3500.04);
    assert(!ExternalHttpDeviceWorker::parsePowerStatus("{\"meters\":[{\"power\":100,\"is_valid\":false}]}", &watts));
    assert(!ExternalHttpDeviceWorker::parsePowerStatus("{\"meters\":[{\"power\":-1}]}", &watts));
    assert(!ExternalHttpDeviceWorker::parsePowerStatus("{\"meters\":[{\"power\":null}]}", &watts));
    char directory[] = "/tmp/microhad-shelly-unit-XXXXXX";
    assert(mkdtemp(directory) != 0);
    {
        ExternalHttpDeviceWorker shelly(directory);
        std::string id, error;
        assert(!shelly.saveDevice("", "No callback address", "192.0.2.1", "", "uni", "", "",
                                  0, -1, 0, 30, false, true, false, &id, &error));
        assert(shelly.saveDevice("", "Probe-free Uni", "192.0.2.1", "192.168.178.62", "uni", "", "",
                                 0, -1, 0, 30, false, true, false, &id, &error));
        std::string config = shelly.json(true);
        assert(config.find("\"http_auth\":false,\"input_index\":0") != std::string::npos);
        assert(config.find("\"temperature_index\":-1") != std::string::npos);
        assert(config.find("\"input_indices\":\"0\"") != std::string::npos);
        assert(config.find("<gateway-ip>") == std::string::npos);
        std::string url;
        assert(jsonStringField(config.substr(1, config.size() - 2), "input_open", &url));
        assert(url.find("http://192.168.178.62/api/shelly/callback/" + id + "/input-on?token=") == 0);
        assert(shelly.callback(id, "input-on", url.substr(url.find("token=") + 6)));
        std::vector<int> selectedInputs; selectedInputs.push_back(0); selectedInputs.push_back(1);
        std::vector<int> selectedTemperatures; selectedTemperatures.push_back(0); selectedTemperatures.push_back(2);
        std::vector<int> selectedRelays; selectedRelays.push_back(0); selectedRelays.push_back(1);
        std::string multiId;
        assert(shelly.saveDevice("", "Multi Uni", "192.0.2.2", "192.168.178.62", "uni", "", "",
                                 selectedInputs, selectedTemperatures, selectedRelays, 30, false, true, false,
                                 &multiId, &error));
        config = shelly.json(true);
        assert(config.find("\"input_indices\":\"0,1\"") != std::string::npos);
        assert(config.find("\"temperature_indices\":\"0,2\"") != std::string::npos);
        assert(config.find("\"relay_indices\":\"0,1\"") != std::string::npos);
        assert(config.find("input-on-2") != std::string::npos);

        std::string plugIds[33];
        for (unsigned int i = 0; i < 33; ++i) {
            char plugId[17];
            snprintf(plugId, sizeof(plugId), "%016x", i + 1);
            assert(shelly.saveDevice(plugId, "Plug", "192.0.2.1", "", "plug", "", "",
                                     -1, -1, 0, 30, false, false, false, &plugIds[i], &error));
        }
        for (unsigned int i = 0; i < 32; ++i) assert(shelly.setActor(plugIds[i], true, &error));
        assert(shelly.setActor(plugIds[0], false, &error));
        assert(shelly.setActor(plugIds[0], true, &error));
        assert(!shelly.setActor(plugIds[32], true, &error));
        assert(error == "Shelly actor queue is full");
    }
    {
        const std::string history = std::string(directory) + "/history.bin";
        Gateway gateway("/dev/null", 115200, directory, "", "", "", 0, history);
        std::string error;
        assert(gateway.registerShelly("1234567890abcdef", "My Uni", "My Uni", "uni", -1, &error));
        std::vector<Device> devices = gateway.devices();
        assert(devices.size() == 2);
        assert(gateway.applyShellyEvent("1234567890abcdef", 1, 1, &error));
        assert(gateway.statusJson().find("\"source\":\"shelly\"") != std::string::npos);
        std::string json;
        assert(SensorHistory::readJson(history, "shelly:1234567890abcdef:contact", 0, &json, &error));
        assert(json.find(",3,1,\"\"") != std::string::npos);
        assert(gateway.registerShelly("fedcba0987654321", "My Plug", "My Plug", "plug", -1, &error));
        assert(gateway.applyShellyEvent("fedcba0987654321", 4, 3500.0, &error));
        devices = gateway.devices();
        assert(SensorHistory::readJson(history, "shelly:fedcba0987654321:plug", 0, &json, &error));
        assert(json.find(",4,3500,\"watts\"") != std::string::npos);
        assert(gateway.registerShelly("fb123456789abcde", "Wi-Fi Phone", "Wi-Fi Phone", "fritz", std::vector<int>(), std::vector<int>(), std::vector<int>(), &error));
        assert(gateway.applyFritzEvent("fb123456789abcde", true, true, 72, &error));
        devices = gateway.devices();
        bool foundPresence = false;
        std::string fritzAddress;
        for (unsigned long i = 0; i < devices.size(); ++i) if (devices[i].source == "fritz_box") {
            foundPresence = devices[i].hasOccupancy && devices[i].occupied && devices[i].hasWifiSignal && devices[i].wifiSignalPercent == 72;
            fritzAddress = devices[i].ieeeAddress;
        }
        assert(foundPresence);
        assert(!fritzAddress.empty());
        assert(gateway.applyFritzEvent("fb123456789abcde", false, false, 0, &error));
        assert(SensorHistory::readJson(history, "fritz:fb123456789abcde:presence", 0, &json, &error));
        assert(json.find(",5,1,\"\"") != std::string::npos);
        assert(json.find(",5,0,\"\"") != std::string::npos);
        std::vector<int> inputs; inputs.push_back(0); inputs.push_back(1);
        std::vector<int> temperatures; temperatures.push_back(0); temperatures.push_back(1); temperatures.push_back(2);
        std::vector<int> relays; relays.push_back(0); relays.push_back(1);
        assert(gateway.registerShelly("aaaaaaaaaaaaaaaa", "Multi Uni", "Multi Uni", "uni", inputs, temperatures, relays, &error));
        devices = gateway.devices();
        unsigned int channels = 0;
        std::string secondContact;
        for (unsigned long i = 0; i < devices.size(); ++i) {
            if (devices[i].externalId != "aaaaaaaaaaaaaaaa") continue;
            ++channels;
            if (devices[i].component == "contact:1") secondContact = devices[i].ieeeAddress;
        }
        assert(channels == 7 && !secondContact.empty());
        assert(gateway.applyShellyEvent("aaaaaaaaaaaaaaaa", 1, 1, 1, &error));
        devices = gateway.devices();
        bool secondContactOpen = false;
        for (unsigned long i = 0; i < devices.size(); ++i)
            if (devices[i].ieeeAddress == secondContact) secondContactOpen = devices[i].contactOpen && devices[i].contactKnown;
        assert(secondContactOpen);
        assert(gateway.removeShelly("1234567890abcdef", &error));
        assert(gateway.devices().size() == 9);
        assert(unlink(history.c_str()) == 0);
        assert(unlink((history + ".values").c_str()) == 0);
    }
    std::string controllerState = std::string(directory) + "/devices.json";
    assert(unlink(controllerState.c_str()) == 0);
    std::string filename = std::string(directory) + "/shelly-devices.jsonl";
    assert(unlink(filename.c_str()) == 0);
    assert(rmdir(directory) == 0);
}
