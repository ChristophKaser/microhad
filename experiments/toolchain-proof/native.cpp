#define __STDC_CONSTANT_MACROS
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <stdint.h>
#include <unistd.h>

extern "C" void proof_c(void);
static void check(bool result, const char *name) {
    if (!result) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    std::puts("START native compiler proof");
    std::vector<std::string> values(128, "lexra");
    std::map<std::string, unsigned int> totals;
    for (unsigned int i = 0; i < values.size(); ++i) ++totals[values[i]];
    check(totals["lexra"] == 128, "STL allocation");
    volatile uint64_t numerator = UINT64_C(0x123456789abcdef0);
    volatile uint64_t denominator = 17;
    check(numerator / denominator == UINT64_C(0x112233445566777), "64-bit division");
    volatile double square = 81.0;
    volatile double divisor = 16.0;
    check(std::sqrt(square) == 9.0 && square / divisor == 5.0625, "soft-float arithmetic and libm");
    unsigned char bytes[] = { 0, 0x78, 0x56, 0x34, 0x12, 0 };
    uint32_t word = 0;
    std::memcpy(&word, bytes + 1, sizeof(word));
    const unsigned char *view = reinterpret_cast<const unsigned char *>(&word);
    check(view[0] == 0x78 && view[3] == 0x12, "unaligned memcpy");
    check((static_cast<uint32_t>(bytes[4]) << 24 | static_cast<uint32_t>(bytes[3]) << 16 |
           static_cast<uint32_t>(bytes[2]) << 8 | bytes[1]) == UINT32_C(0x12345678), "byte order");
    std::puts("PASS C++ allocation, STL, arithmetic, byte order, unaligned decoding");
    proof_c();
    alarm(0);
    std::puts("PASS native compiler proof complete");
    return 0;
}
