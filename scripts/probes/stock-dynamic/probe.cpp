#include <cstdio>
#include <string>
#include <vector>
#include <stdexcept>
#ifdef PROBE_CXX_COMPAT
#include <sstream>
#include <new>
#include <bits/functexcept.h>
#endif
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
#ifdef PROBE_CXX_COMPAT
        std::ostringstream stream;
        stream << 42;
        if (stream.str() != "42") return 3;
        try { values.at(99); return 4; } catch (const std::out_of_range &) { }
        try { std::__throw_bad_array_new_length(); } catch (const std::bad_array_new_length &) { }
#endif
        throw std::runtime_error(values[1]);
    } catch (const std::exception &error) {
        if (std::string(error.what()) != "stock") return 2;
        std::puts("PASS C++: strings, vectors, allocation, exception handling");
        return 0;
    }
}
