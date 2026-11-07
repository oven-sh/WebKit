/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTestSupport.h"
#include <array>
#include <cstring>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

#if CPU(ARM64)

namespace Machine {

static TypedPointer pairsOfLoadsTestWordAt(TestProcedure& test, LValue base, unsigned index)
{
    return test.out().address(test.heaps().argumentBuffer, base, static_cast<ptrdiff_t>(index * sizeof(uint64_t)));
}

static unsigned pairsOfLoadsTestNumberOfLoadPairs(TestProcedure& test)
{
    std::span<const uint8_t> code = test.linkedCode();
    unsigned result = 0;
    for (size_t offset = 0; offset + sizeof(uint32_t) <= code.size(); offset += sizeof(uint32_t)) {
        uint32_t word;
        std::memcpy(&word, code.data() + offset, sizeof(word));
        result += (word & 0xffc00000) == 0xa9400000;
    }
    return result;
}

struct PairsOfLoadsTestSumOfWords {
    unsigned numberOfLoadPairs;
    uint64_t sum;
};

static PairsOfLoadsTestSumOfWords pairsOfLoadsTestSumWords(unsigned firstWord, unsigned secondWord)
{
    TestProcedure test;
    Output& out = test.out();
    LValue buffer = test.argumentGPR(0);
    LValue first = out.load64(pairsOfLoadsTestWordAt(test, buffer, firstWord));
    LValue second = out.load64(pairsOfLoadsTestWordAt(test, buffer, secondWord));
    out.ret(out.sub(out.add(first, first), second));
    compileOrFail(test);

    std::array<uint64_t, 4> words { 3, 50, 700, 9000 };
    return { pairsOfLoadsTestNumberOfLoadPairs(test), test.call<uint64_t>(words.data()) };
}

static void neighboringWordsAreLoadedAsPair()
{
    PairsOfLoadsTestSumOfWords apart = pairsOfLoadsTestSumWords(0, 2);
    PairsOfLoadsTestSumOfWords rising = pairsOfLoadsTestSumWords(0, 1);
    PairsOfLoadsTestSumOfWords falling = pairsOfLoadsTestSumWords(1, 0);
    AOT_TEST_CHECK(rising.numberOfLoadPairs == apart.numberOfLoadPairs + 1 && falling.numberOfLoadPairs == apart.numberOfLoadPairs + 1);
    AOT_TEST_CHECK(apart.sum == static_cast<uint64_t>(6 - 700) && rising.sum == static_cast<uint64_t>(6 - 50) && falling.sum == 97);
}

} // namespace Machine

std::span<const NativeTest> machinePairsOfLoadsTests()
{
    static constexpr NativeTest tests[] = {
        { "machine-pairs-of-loads/neighboring-words-are-loaded-as-a-pair"_s, Machine::neighboringWordsAreLoadedAsPair },
    };
    return tests;
}

#else

std::span<const NativeTest> machinePairsOfLoadsTests()
{
    return { };
}

#endif

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
