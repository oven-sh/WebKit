#pragma once

#if ENABLE(AOT)

#include <span>
#include <wtf/text/Latin1Character.h>

namespace JSC { namespace AOT {

struct AtomListTable {
    static constexpr unsigned maxWords = 99;
    static constexpr unsigned numberOfHashes = 256;
    static constexpr unsigned numberOfSlots = numberOfHashes + maxWords + 5;
    static constexpr unsigned sizeInElements = numberOfSlots / sizeof(uint64_t);
    static_assert(!(numberOfSlots % sizeof(uint64_t)) && maxWords < 255);

    static constexpr unsigned factorOfFirst = 5;
    static constexpr unsigned factorOfMiddle = 3;
    static constexpr unsigned factorOfLast = 9;
    static unsigned hash(std::span<const Latin1Character> word)
    {
        return (word.front() * factorOfFirst + word[word.size() / 2] * factorOfMiddle + word.back() * factorOfLast + static_cast<unsigned>(word.size())) % numberOfHashes;
    }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
