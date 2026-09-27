#include "shelly_integration.h"
#include "json_util.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

void runShellyIntegrationUnitTests() {
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
        assert(config.find("\"has_temperature\":false") != std::string::npos);
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
    std::string filename = std::string(directory) + "/shelly-devices.jsonl";
    assert(unlink(filename.c_str()) == 0);
    assert(rmdir(directory) == 0);
}
