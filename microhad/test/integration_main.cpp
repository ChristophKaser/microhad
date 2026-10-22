#include <cstdio>

void runGatewayIntegrationTests();
void runHttpServerIntegrationTests();
void runLifecycleIntegrationTests();
void runOutboundIntegrationTests();
void runAutomationIntegrationTests();

int main() {
    runAutomationIntegrationTests();
    runGatewayIntegrationTests();
    runHttpServerIntegrationTests();
    runLifecycleIntegrationTests();
    runOutboundIntegrationTests();
    printf("integration tests passed\n");
    return 0;
}
