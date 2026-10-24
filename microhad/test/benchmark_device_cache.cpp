// Synthetic cache benchmark. Never opens the gateway's real cache or configuration.
#include "gateway.h"
#include "storage.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <ctime>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

namespace {
double clockSeconds(clockid_t clock) {
    timespec value;
    if (clock_gettime(clock, &value) != 0) std::abort();
    return value.tv_sec + value.tv_nsec / 1e9;
}
}
int main(int argc, char** argv) {
    const unsigned iterations = argc > 1 ? std::strtoul(argv[1], 0, 10) : 100;
    const unsigned count = argc > 2 ? std::strtoul(argv[2], 0, 10) : 26;
    if (!iterations || iterations > 10000 || !count || count > 256) return 2;
    setpriority(PRIO_PROCESS, 0, 19);
    char folder[] = "/tmp/microhad-cache-bench-XXXXXX";
    if (!mkdtemp(folder)) return 2;
    const std::string path = std::string(folder) + "/synthetic.pb";
    std::vector<Device> devices(count);
    for (unsigned i = 0; i < count; ++i) {
        Device& device = devices[i];
        std::ostringstream id; id << "synthetic-" << i;
        device.ieeeAddress = id.str();
        device.name = "Synthetic room temperature sensor";
        device.manufacturerName = "Synthetic vendor"; device.modelId = "Synthetic model";
        device.networkAddress = i + 1;
        device.hasTemperature = device.temperatureKnown = true; device.temperatureCelsius = 21.125;
        device.hasHumidity = device.humidityKnown = true; device.humidityPercent = 55.5;
        device.hasBattery = true; device.batteryPercent = 80; device.lastSeen = 1700000000;
        if (i % 3 == 0) {
            device.source = "matter"; device.matterNode = "0000000000000002";
            device.matterEndpoint = 1; device.matterState = "complete"; device.matterMetadataState = "stale";
            device.matterServers.push_back(6); device.matterServers.push_back(1026);
            device.matterClients.push_back(8); device.matterDeviceTypes.push_back(256);
        } else {
            EndpointDescriptor endpoint; endpoint.endpoint = 1; endpoint.profileId = 260; endpoint.deviceId = 770;
            endpoint.inputClusters.push_back(0); endpoint.inputClusters.push_back(1); endpoint.inputClusters.push_back(1026);
            device.endpoints.push_back(endpoint); device.nodeDescriptor.assign(13, 0x42);
        }
        if (i % 4 == 0) {
            DeviceValueAlert alert; alert.hasHigh = true; alert.high = 28.5; alert.hysteresis = 1.5; alert.unit = "celsius";
            device.valueAlerts["temperature"] = alert;
        }
    }
    std::string error;
    const double cpuStart = clockSeconds(CLOCK_PROCESS_CPUTIME_ID), wallStart = clockSeconds(CLOCK_MONOTONIC);
    bool ok = true;
    for (unsigned i = 0; i < iterations && ok; ++i) ok = saveDeviceCache(path, devices, &error);
    const double cpu = clockSeconds(CLOCK_PROCESS_CPUTIME_ID) - cpuStart;
    const double wall = clockSeconds(CLOCK_MONOTONIC) - wallStart;
    struct stat metadata;
    std::vector<Device> restored;
    ok = ok && stat(path.c_str(), &metadata) == 0 && loadDeviceCache(path, &restored, &error) && restored.size() == count;
    if (ok) {
        std::printf("{\"devices\":%u,\"iterations\":%u,\"bytes\":%lu,\"cpu_ms_per_snapshot\":%.6f,\"wall_ms_per_snapshot\":%.6f,\"clock_ticks\":%ld}\n",
                    count, iterations, static_cast<unsigned long>(metadata.st_size), cpu * 1000 / iterations,
                    wall * 1000 / iterations, sysconf(_SC_CLK_TCK));
    } else std::fprintf(stderr, "Synthetic cache benchmark failed\n");
    unlink(path.c_str()); rmdir(folder);
    return ok ? 0 : 1;
}
