#include <cstdio>

void runUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();
void runSensorHistoryUnitTests();
void runShellyIntegrationUnitTests();

int main() {
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    runGatewayConfigUnitTests();
    runSensorHistoryUnitTests();
    runShellyIntegrationUnitTests();
    printf("unit tests passed\n");
    return 0;
}
