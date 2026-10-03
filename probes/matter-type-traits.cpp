// Compile an actual pinned Matter SDK header, without generated platform headers.
#include <lib/support/TypeTraits.h>

enum class MatterProbeState { Ready = 7 };
static_assert(chip::to_underlying(MatterProbeState::Ready) == 7, "Matter type traits must work");

int main()
{
    return chip::to_underlying(MatterProbeState::Ready) == 7 ? 0 : 1;
}
