#include <cstdio>

void runUnitTests();
void runNetworkBackupUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();
void runSensorHistoryUnitTests();
void runLogRingUnitTests();
void runExternalHttpDeviceWorkerUnitTests();

int main() {
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
