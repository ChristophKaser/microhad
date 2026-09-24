#include <cstdio>

void runGatewayIntegrationTests();
void runHttpServerIntegrationTests();
void runLifecycleIntegrationTests();

int main() {
    runGatewayIntegrationTests();
    runHttpServerIntegrationTests();
    runLifecycleIntegrationTests();
    printf("integration tests passed\n");
    return 0;
}
