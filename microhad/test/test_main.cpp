#include <cstdio>

void runUnitTests();
void runFcmUnitTests();
void runWebPushUnitTests();

int main() {
    runUnitTests();
    runFcmUnitTests();
    runWebPushUnitTests();
    printf("unit tests passed\n");
    return 0;
}
