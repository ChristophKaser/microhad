#include <cstdio>

void runGatewayIntegrationTests();
void runHttpServerIntegrationTests();

int main() {
    runGatewayIntegrationTests();
    runHttpServerIntegrationTests();
    printf("integration tests passed\n");
    return 0;
}
