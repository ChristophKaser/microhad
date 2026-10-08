#include <cstdio>

void runGatewayIntegrationTests();
void runHttpServerIntegrationTests();
void runLifecycleIntegrationTests();
void runOutboundIntegrationTests();

int main() {
    runGatewayIntegrationTests();
    runHttpServerIntegrationTests();
    runLifecycleIntegrationTests();
    runOutboundIntegrationTests();
    printf("integration tests passed\n");
    return 0;
}
