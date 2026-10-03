#include <cstdio>

void runUnitTests();
void runNetworkBackupUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();
void runSensorHistoryUnitTests();
void runLogRingUnitTests();
void runExternalHttpDeviceWorkerUnitTests();

#if MICROHAD_ZIGGURAT_ENABLED
void runZigguratUnitTests();
#endif

int main() {
#if MICROHAD_ZIGGURAT_ENABLED
    runZigguratUnitTests();
#endif
    runNetworkBackupUnitTests();
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    runGatewayConfigUnitTests();
    runSensorHistoryUnitTests();
    runLogRingUnitTests();
    runExternalHttpDeviceWorkerUnitTests();
    printf("unit tests passed\n");
    return 0;
}
