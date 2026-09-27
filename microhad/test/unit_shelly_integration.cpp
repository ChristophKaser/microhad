#include "shelly_integration.h"
#include "gateway.h"
#include "sensor_history.h"
#include "json_util.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

void runShellyIntegrationUnitTests() {
    double watts = 0;
    assert(ShellyIntegration::parsePowerStatus("{\"meters\":[{\"power\":3500.04,\"is_valid\":true}]}", &watts));
    assert(watts == 3500.04);
    assert(!ShellyIntegration::parsePowerStatus("{\"meters\":[{\"power\":100,\"is_valid\":false}]}", &watts));
    assert(!ShellyIntegration::parsePowerStatus("{\"meters\":[{\"power\":-1}]}", &watts));
    assert(!ShellyIntegration::parsePowerStatus("{\"meters\":[{\"power\":null}]}", &watts));
    char directory[] = "/tmp/microhad-shelly-unit-XXXXXX";
    assert(mkdtemp(directory) != 0);
    {
        ShellyIntegration shelly(directory);
        std::string id, error;
        assert(!shelly.saveDevice("", "No callback address", "192.0.2.1", "", "uni", "", "",
                                  0, -1, 0, 30, false, true, false, &id, &error));
        assert(shelly.saveDevice("", "Probe-free Uni", "192.0.2.1", "192.168.178.62", "uni", "", "",
                                 0, -1, 0, 30, false, true, false, &id, &error));
        std::string config = shelly.json(true);
        assert(config.find("\"http_auth\":false,\"input_index\":0") != std::string::npos);
        assert(config.find("\"temperature_index\":-1") != std::string::npos);
        assert(config.find("<gateway-ip>") == std::string::npos);
        std::string url;
        assert(jsonStringField(config.substr(1, config.size() - 2), "input_open", &url));
        assert(url.find("http://192.168.178.62/api/shelly/callback/" + id + "/input-on?token=") == 0);
        assert(shelly.callback(id, "input-on", url.substr(url.find("token=") + 6)));

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
        std::string contactAddress;
        for (unsigned long i = 0; i < devices.size(); ++i)
            if (devices[i].component == "contact") contactAddress = devices[i].ieeeAddress;
        assert(!contactAddress.empty());
        assert(gateway.applyShellyEvent("1234567890abcdef", 1, 1, &error));
        assert(gateway.statusJson().find("\"source\":\"shelly\"") != std::string::npos);
        std::string json;
        assert(SensorHistory::readJson(history, contactAddress, 0, &json, &error));
        assert(json.find(",3,1]") != std::string::npos);
        assert(gateway.registerShelly("fedcba0987654321", "My Plug", "My Plug", "plug", -1, &error));
        assert(gateway.applyShellyEvent("fedcba0987654321", 4, 3500.0, &error));
        devices = gateway.devices();
        std::string plugAddress;
        for (unsigned long i = 0; i < devices.size(); ++i)
            if (devices[i].component == "plug") plugAddress = devices[i].ieeeAddress;
        assert(!plugAddress.empty());
        assert(SensorHistory::readJson(history, plugAddress, 0, &json, &error));
        assert(json.find(",4,35000]") != std::string::npos);
        assert(gateway.removeShelly("1234567890abcdef", &error));
        assert(gateway.devices().size() == 1);
        assert(unlink(history.c_str()) == 0);
    }
    std::string controllerState = std::string(directory) + "/devices.json";
    assert(unlink(controllerState.c_str()) == 0);
    std::string filename = std::string(directory) + "/shelly-devices.jsonl";
    assert(unlink(filename.c_str()) == 0);
    assert(rmdir(directory) == 0);
}
