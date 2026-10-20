/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
#include <wtf/Assertions.h>

namespace JSC { namespace AOT {

struct ClosureWord {
    static constexpr unsigned numberOfOffsetBits = 32;
#if CPU(ARM64)
    static constexpr unsigned offsetUnit = 4;
#else
    static constexpr unsigned offsetUnit = 1;
#endif
    static constexpr unsigned functionShift = numberOfOffsetBits;
    static constexpr unsigned numberOfFunctionBits = 22;
    static constexpr unsigned takesNewTargetBit = 57;
    static constexpr unsigned numberOfParametersShift = 58;
    static constexpr unsigned numberOfParametersBits = 4;
    static constexpr unsigned isNotCallableBit = 63;
    static_assert(functionShift + numberOfFunctionBits <= takesNewTargetBit);
    static_assert(numberOfParametersShift + numberOfParametersBits <= isNotCallableBit);

    static constexpr uint64_t offsetMask = (1ULL << numberOfOffsetBits) - 1;
    static constexpr uint32_t functionMask = (1u << numberOfFunctionBits) - 1;
    static constexpr uint32_t firstReservedFunction = (functionMask >> 1) + 1 - 16;
    static constexpr uint64_t maxSizeOfCode = (offsetMask + 1) * offsetUnit;
    static constexpr unsigned maxNumberOfParameters = (1u << numberOfParametersBits) - 1;

    struct Entry {
        uint64_t distanceFromCode { 0 };
        unsigned numberOfParameters { 0 };
    };

    struct Flags {
        bool isNotCallable { false };
        bool takesNewTarget { false };

        friend constexpr bool operator==(const Flags&, const Flags&) = default;
    };

    static constexpr bool canEncode(Entry entry, uint32_t function)
    {
        return entry.distanceFromCode && !(entry.distanceFromCode % offsetUnit) && entry.distanceFromCode < maxSizeOfCode
            && entry.numberOfParameters <= maxNumberOfParameters && function < firstReservedFunction;
    }

    // Everything but where the code lies. Two functions differ in it, and every closure of one function has the same.
    static constexpr uint32_t encodeUpperHalf(unsigned numberOfParameters, uint32_t function, Flags flags)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(numberOfParameters <= maxNumberOfParameters && function < firstReservedFunction);
        return function
            | static_cast<uint32_t>(flags.takesNewTarget) << (takesNewTargetBit - numberOfOffsetBits)
            | static_cast<uint32_t>(numberOfParameters) << (numberOfParametersShift - numberOfOffsetBits)
            | static_cast<uint32_t>(flags.isNotCallable) << (isNotCallableBit - numberOfOffsetBits);
    }

    static constexpr uint64_t encode(Entry entry, uint32_t function, Flags flags)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(canEncode(entry, function));
        return entry.distanceFromCode / offsetUnit | static_cast<uint64_t>(encodeUpperHalf(entry.numberOfParameters, function, flags)) << numberOfOffsetBits;
    }

    static constexpr uint32_t functionOf(uint64_t word) { return word >> functionShift & functionMask; }
    static constexpr uintptr_t addressOf(uint64_t word, uintptr_t code) { return code + (word & offsetMask) * offsetUnit; }
    static constexpr unsigned numberOfParametersOf(uint64_t word) { return word >> numberOfParametersShift & maxNumberOfParameters; }
    static constexpr bool isCallable(uint64_t word) { return !(word >> isNotCallableBit); }
    static constexpr bool takesNewTarget(uint64_t word) { return word >> takesNewTargetBit & 1; }
};

static_assert(ClosureWord::functionShift == 32 && ClosureWord::firstReservedFunction <= 1u << (ClosureWord::numberOfFunctionBits - 1));
static constexpr uint32_t closureWordUpperHalfBits = ~(1u << (ClosureWord::isNotCallableBit - 32) | 1u << (ClosureWord::numberOfFunctionBits - 1));
static_assert(!canPassShapeTest(closureWordUpperHalfBits));
static_assert(offsetOfSlimClosureWord + static_cast<ptrdiff_t>(sizeof(uint32_t)) == offsetOfShapeWord);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
