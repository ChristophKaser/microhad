#include "cache_fixture.h"
#include <cstdio>

void runGatewayIntegrationTests();
void runHttpServerIntegrationTests();
void runLifecycleIntegrationTests();
void runOutboundIntegrationTests();
void runAutomationIntegrationTests();

int main() {
    { DeviceCacheIsolation cache; runAutomationIntegrationTests(); }
    { DeviceCacheIsolation cache; runGatewayIntegrationTests(); }
    { DeviceCacheIsolation cache; runHttpServerIntegrationTests(); }
    { DeviceCacheIsolation cache; runLifecycleIntegrationTests(); }
    { DeviceCacheIsolation cache; runOutboundIntegrationTests(); }
    printf("integration tests passed\n");
    return 0;
}
