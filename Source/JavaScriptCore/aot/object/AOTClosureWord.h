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
    // No code is so large that an offset has its top bit set: AOTImageFormat.h. The run time sets the bit in the word of one closure, which has properties by then, when that closure gets another name
    // and its function is named in frames by its closures. Calls by value enter such a closure through the adapter, whose frame holds it. A slim closure with properties has no word of its own: its per-cell bit says the same.
    static constexpr unsigned isRenamedBit = numberOfOffsetBits - 1;
    // The functions that closures are made of are counted from the first of them: ProgramTables::closureFunctions. No other function needs a number that fits here.
    static constexpr unsigned functionShift = numberOfOffsetBits;
    static constexpr unsigned numberOfFunctionBits = 22;
    // Bit i from here on: nobody holds argument i, or what is read through it, any more when a call through this word returns. Whoever calls may pass a closure that lies in his own frame there.
    static constexpr unsigned borrowsArgumentShift = 54;
    static constexpr unsigned numberOfBorrowedArguments = 2;
    // The tail calls of the function come to an end: AOTFrameArrayParameters.h. Whoever calls it in tail position may keep his frame until it returns.
    static constexpr unsigned endsItsTailCallsBit = 56;
    static constexpr unsigned takesNewTargetBit = 57;
    // The field holds maxNumberOfParameters less the number of parameters, so that fewer parameters make a larger word.
    static constexpr unsigned numberOfParametersShift = 58;
    static constexpr unsigned numberOfParametersBits = 4;
    // Set in every closure word. No other cell that is a value has it at this place without the bit above it: AOTCellFormat.cpp.
    static constexpr unsigned isClosureWordBit = 62;
    static constexpr unsigned isNotCallableBit = 63;
    static constexpr unsigned topByteShift = 56;
    static_assert(functionShift + numberOfFunctionBits <= borrowsArgumentShift && borrowsArgumentShift + numberOfBorrowedArguments <= endsItsTailCallsBit && endsItsTailCallsBit < takesNewTargetBit);
    static_assert(numberOfParametersShift + numberOfParametersBits <= isClosureWordBit && isClosureWordBit + 1 == isNotCallableBit && numberOfParametersShift >= topByteShift);

    static constexpr uint64_t offsetMask = (1ULL << numberOfOffsetBits) - 1;
    static constexpr uint32_t functionMask = (1u << numberOfFunctionBits) - 1;
    // The upper half of a slim closure's word lies where other cells have their shape word. It passes no shape test while bits 16 to 23 are not all set (shapeBlockMarker).
    // Bits 16 to 21 belong to the function's number, so no function has a number in which all six are set.
    static constexpr uint32_t firstReservedFunction = (functionMask & shapeBlockMarker) - 16;
    static constexpr uint64_t maxSizeOfCode = (offsetMask + 1) * offsetUnit;
    static constexpr unsigned maxNumberOfParameters = (1u << numberOfParametersBits) - 1;

    struct Entry {
        uint64_t distanceFromCode { 0 };
        unsigned numberOfParameters { 0 };
    };

    struct Flags {
        bool isNotCallable { false };
        bool takesNewTarget { false };
        uint8_t borrowedArguments { 0 };
        bool endsItsTailCalls { false };

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
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(numberOfParameters <= maxNumberOfParameters && function < firstReservedFunction && !(flags.borrowedArguments >> numberOfBorrowedArguments));
        return function
            | static_cast<uint32_t>(flags.borrowedArguments) << (borrowsArgumentShift - numberOfOffsetBits)
            | static_cast<uint32_t>(flags.endsItsTailCalls) << (endsItsTailCallsBit - numberOfOffsetBits)
            | static_cast<uint32_t>(flags.takesNewTarget) << (takesNewTargetBit - numberOfOffsetBits)
            | static_cast<uint32_t>(maxNumberOfParameters - numberOfParameters) << (numberOfParametersShift - numberOfOffsetBits)
            | 1u << (isClosureWordBit - numberOfOffsetBits)
            | static_cast<uint32_t>(flags.isNotCallable) << (isNotCallableBit - numberOfOffsetBits);
    }

    static constexpr uint64_t encode(Entry entry, uint32_t function, Flags flags)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(canEncode(entry, function));
        return entry.distanceFromCode / offsetUnit | static_cast<uint64_t>(encodeUpperHalf(entry.numberOfParameters, function, flags)) << numberOfOffsetBits;
    }

    static constexpr uint32_t closureNumberOf(uint64_t word) { return word >> functionShift & functionMask; }
    static constexpr uintptr_t addressOf(uint64_t word, uintptr_t code) { return code + (word & offsetMask & ~(1ULL << isRenamedBit)) * offsetUnit; }
    static constexpr unsigned numberOfParametersOf(uint64_t word) { return maxNumberOfParameters - (word >> numberOfParametersShift & maxNumberOfParameters); }
    // Read as a signed number, the top byte of a word is at least this where the function may be called and declares no more parameters than the call passes arguments.
    static constexpr int8_t minTopByteOfCalleeTaking(unsigned numberOfArguments)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(numberOfArguments <= maxNumberOfParameters);
        return static_cast<int8_t>(1u << (isClosureWordBit - topByteShift) | (maxNumberOfParameters - numberOfArguments) << (numberOfParametersShift - topByteShift));
    }
    static constexpr int64_t minWordOfCalleeTaking(unsigned numberOfArguments) { return static_cast<int64_t>(minTopByteOfCalleeTaking(numberOfArguments)) << topByteShift; }
    static constexpr bool isCallable(uint64_t word) { return !(word >> isNotCallableBit); }
    static constexpr bool takesNewTarget(uint64_t word) { return word >> takesNewTargetBit & 1; }
    static constexpr uint8_t borrowedArgumentsOf(uint64_t word) { return word >> borrowsArgumentShift & ((1u << numberOfBorrowedArguments) - 1); }
};

static_assert(ClosureWord::functionShift == 32 && ClosureWord::firstReservedFunction <= (ClosureWord::functionMask & shapeBlockMarker));
// With every other bit set that a callable closure's word can have.
static constexpr uint32_t upperHalfOfClosureWordWithAllFlags(uint32_t function) { return function | (~ClosureWord::functionMask & ~(1u << (ClosureWord::isNotCallableBit - 32))); }
static_assert(!canPassShapeTest(upperHalfOfClosureWordWithAllFlags(ClosureWord::firstReservedFunction - 1)) && !canPassShapeTest(upperHalfOfClosureWordWithAllFlags((ClosureWord::functionMask & shapeBlockMarker) - 1)));
static_assert(canPassShapeTest(upperHalfOfClosureWordWithAllFlags(ClosureWord::functionMask & shapeBlockMarker)));
static_assert(closureWordMarker == 1u << (ClosureWord::isClosureWordBit - 32) && compiledObjectShapeBit == 1u << (ClosureWord::isNotCallableBit - 32));
static_assert(ClosureWord::numberOfParametersOf(ClosureWord::encode({ 16, 3 }, 7, { })) == 3 && mayBeUpperHalfOfSlimClosureWord(ClosureWord::encodeUpperHalf(3, 7, { })));
static_assert(static_cast<int64_t>(ClosureWord::encode({ 16, 3 }, 7, { })) >= ClosureWord::minWordOfCalleeTaking(3) && static_cast<int64_t>(ClosureWord::encode({ 16, 3 }, 7, { .takesNewTarget = true })) < ClosureWord::minWordOfCalleeTaking(2));
static_assert(static_cast<int64_t>(ClosureWord::encode({ 16, 0 }, 7, { .isNotCallable = true })) < ClosureWord::minWordOfCalleeTaking(ClosureWord::maxNumberOfParameters));
static_assert(static_cast<int64_t>(ClosureWord::encode({ 16, 3 }, 7, { .takesNewTarget = true, .borrowedArguments = 3, .endsItsTailCalls = true })) < ClosureWord::minWordOfCalleeTaking(2) && ClosureWord::borrowedArgumentsOf(ClosureWord::encode({ 16, 3 }, 7, { .borrowedArguments = 2, .endsItsTailCalls = true })) == 2);
static_assert(offsetOfSlimClosureWord + static_cast<ptrdiff_t>(sizeof(uint32_t)) == offsetOfShapeWord);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
