#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
int main()
{
    try {
        std::vector<std::string> values;
        values.push_back("gcc15"); values.push_back("stock");
#ifdef PROBE_BOUNDS
        if (values.at(0) != "gcc15") return 1;
#else
        if (values[0] != "gcc15") return 1;
#endif
        throw std::runtime_error(values[1]);
    } catch (const std::exception &error) {
        if (std::string(error.what()) != "stock") return 2;
        std::puts("PASS C++: strings, vectors, allocation, exception handling");
        return 0;
    }
}
