#include <cstdio>

void runUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();
void runSensorHistoryUnitTests();

int main() {
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    runGatewayConfigUnitTests();
    runSensorHistoryUnitTests();
    printf("unit tests passed\n");
    return 0;
}
