#include "cache_fixture.h"
#include <cstdio>

void runDeviceCacheUnitTests();
void runUnitTests();
void runAutomationUnitTests();
void runMatterWebSocketUnitTests();
void runMatterProtocolUnitTests();
void runMatterClientUnitTests();
void runNetworkBackupUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();
void runCompanionDiscoveryUnitTests();
void runSensorHistoryUnitTests();
void runLogRingUnitTests();
void runSyslogLogReaderUnitTests();
void runExternalHttpDeviceWorkerUnitTests();

#if MICROHAD_ZIGGURAT_ENABLED
void runZigguratUnitTests();
#endif

int main() {
    DeviceCacheIsolation caches;
    runDeviceCacheUnitTests();
    runAutomationUnitTests();
#if MICROHAD_ZIGGURAT_ENABLED
    runZigguratUnitTests();
#endif
    runMatterWebSocketUnitTests();
    runMatterProtocolUnitTests();
    runMatterClientUnitTests();
    runNetworkBackupUnitTests();
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    runGatewayConfigUnitTests();
    runCompanionDiscoveryUnitTests();
    runSensorHistoryUnitTests();
    runLogRingUnitTests();
    runSyslogLogReaderUnitTests();
    runExternalHttpDeviceWorkerUnitTests();
    printf("unit tests passed\n");
    return 0;
}
