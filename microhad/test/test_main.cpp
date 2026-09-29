#include <cstdio>

void runUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();
void runSensorHistoryUnitTests();
void runExternalHttpDeviceWorkerUnitTests();

int main() {
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    runGatewayConfigUnitTests();
    runSensorHistoryUnitTests();
    runExternalHttpDeviceWorkerUnitTests();
    printf("unit tests passed\n");
    return 0;
}
