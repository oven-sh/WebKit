/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTLimits.h"
#include "AOTNativeTest.h"
#include "AOTStackTrace.h"
#include <bit>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

using Trace = ReturnAddressTrace;
using TraceFlags = OptionSet<Trace::Flag>;

static constexpr uintptr_t codeStart = 0x200000000000ull;
static constexpr uint64_t codeBytesLimit = defaultLimitValue(LimitID::CodeBytes, hostArchitecture());

static constexpr uint64_t codeOffsets[] = { 0, 4, (1u << 30) - 4, 1u << 30, (1ull << 31) + 4, codeBytesLimit - 4 };

static constexpr TraceFlags flagSets[] = {
    { },
    { Trace::Flag::IsConstructCall },
    { Trace::Flag::IsAtFirstReadSite },
    { Trace::Flag::IsConstructCall, Trace::Flag::IsAtFirstReadSite },
};

struct TraceStorage {
    explicit TraceStorage(size_t numberOfReturnAddresses)
        : words(FillWith { }, Trace::allocationSize(numberOfReturnAddresses) / sizeof(uint64_t) + 1, 0)
    {
        trace().numberOfReturnAddresses = numberOfReturnAddresses;
    }

    Trace& trace() { return *reinterpret_cast<Trace*>(words.mutableSpan().data()); }

    Vector<uint64_t> words;
};

static const void* addressAt(uint64_t offset)
{
    return std::bit_cast<const void*>(codeStart + static_cast<uintptr_t>(offset));
}

static void testEveryCodeOffsetRoundTrips()
{
    constexpr size_t numberOfReturnAddresses = std::size(codeOffsets) * std::size(flagSets);
    static_assert(numberOfReturnAddresses > Trace::returnAddressesPerFlagWord);
    TraceStorage storage(numberOfReturnAddresses);
    Trace& trace = storage.trace();

    size_t index = 0;
    for (uint64_t offset : codeOffsets) {
        for (TraceFlags flags : flagSets) {
            trace.returnAddresses()[index] = Trace::encode(addressAt(offset), codeStart);
            trace.addFlagsAt(index, flags);
            ++index;
        }
    }

    index = 0;
    for (uint64_t offset : codeOffsets) {
        for (TraceFlags flags : flagSets) {
            AOT_TEST_CHECK(Trace::decode(trace.returnAddresses()[index], codeStart) == addressAt(offset));
            AOT_TEST_CHECK(trace.flagsAt(index) == flags);
            ++index;
        }
    }
}

static void testDifferentOffsetsHaveDifferentWords()
{
    for (uint64_t first : codeOffsets) {
        for (uint64_t second : codeOffsets)
            AOT_TEST_CHECK((Trace::encode(addressAt(first), codeStart) == Trace::encode(addressAt(second), codeStart)) == (first == second));
    }
}

static void testLimitFitsTheWord()
{
    uint64_t largestOffset = codeBytesLimit - codeOffsetUnit;
    AOT_TEST_CHECK(largestOffset < Trace::encodableCodeSize);
    AOT_TEST_CHECK(largestOffset / codeOffsetUnit <= UINT32_MAX);
    AOT_TEST_CHECK(Trace::decode(Trace::encode(addressAt(largestOffset), codeStart), codeStart) == addressAt(largestOffset));
    AOT_TEST_CHECK(Trace::encodableCodeSize / codeOffsetUnit > UINT32_MAX);
}

static void testFlagsLieBehindTheOffsets()
{
    AOT_TEST_CHECK(Trace::allocationSize(0) == sizeof(Trace));
    AOT_TEST_CHECK(Trace::allocationSize(1) == sizeof(Trace) + 2 * sizeof(uint32_t));
    AOT_TEST_CHECK(Trace::allocationSize(Trace::returnAddressesPerFlagWord) == sizeof(Trace) + (Trace::returnAddressesPerFlagWord + 1) * sizeof(uint32_t));
    AOT_TEST_CHECK(Trace::allocationSize(Trace::returnAddressesPerFlagWord + 1) == sizeof(Trace) + (Trace::returnAddressesPerFlagWord + 3) * sizeof(uint32_t));

    TraceStorage storage(Trace::returnAddressesPerFlagWord + 1);
    Trace& trace = storage.trace();
    for (uint32_t& returnAddress : trace.returnAddresses())
        returnAddress = UINT32_MAX;
    for (size_t index = 0; index < trace.numberOfReturnAddresses; ++index)
        AOT_TEST_CHECK(trace.flagsAt(index).isEmpty());

    trace.addFlagsAt(Trace::returnAddressesPerFlagWord, Trace::Flag::IsAtFirstReadSite);
    for (size_t index = 0; index < trace.numberOfReturnAddresses; ++index) {
        AOT_TEST_CHECK(trace.returnAddresses()[index] == UINT32_MAX);
        AOT_TEST_CHECK(trace.flagsAt(index).contains(Trace::Flag::IsAtFirstReadSite) == (index == Trace::returnAddressesPerFlagWord));
        AOT_TEST_CHECK(!trace.flagsAt(index).contains(Trace::Flag::IsConstructCall));
    }
}

std::span<const NativeTest> stackCaptureLargeImageTests()
{
    static constexpr NativeTest tests[] = {
        { "stack-capture/every-code-offset-round-trips"_s, testEveryCodeOffsetRoundTrips },
        { "stack-capture/different-offsets-have-different-words"_s, testDifferentOffsetsHaveDifferentWords },
        { "stack-capture/limit-fits-the-word"_s, testLimitFitsTheWord },
        { "stack-capture/flags-lie-behind-the-offsets"_s, testFlagsLieBehindTheOffsets },
    };
    return tests;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
