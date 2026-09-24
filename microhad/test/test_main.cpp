#include <cstdio>

void runUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();
void runGatewayConfigUnitTests();

int main() {
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    runGatewayConfigUnitTests();
    printf("unit tests passed\n");
    return 0;
}
