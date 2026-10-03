// Minimal library facilities used by the pinned Matter controller and support code.
#include <optional>
#include <array>
#include <atomic>
#include <memory>
#include <type_traits>

int main()
{
    static_assert(std::is_same_v<unsigned int, unsigned int>, "C++17 type traits required");
    std::array<unsigned int, 1> values{{7}};
    std::optional<unsigned int> result(values[0]);
    std::unique_ptr<unsigned int> owned = std::make_unique<unsigned int>(*result);
    std::atomic<unsigned int> counter(0);
    counter.fetch_add(*owned);
    return counter.load() == 7 ? 0 : 1;
}
