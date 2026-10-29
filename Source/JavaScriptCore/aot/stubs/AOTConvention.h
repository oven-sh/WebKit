/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosureWord.h"
#include "AOTStubRegisters.h"
#include "FPRInfo.h"
#include "GPRInfo.h"
#include <wtf/Assertions.h>

namespace JSC { namespace AOT {

constexpr GPRReg argumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#if CPU(ARM64)
static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
static constexpr GPRReg thisGPR = ARM64Registers::x8;
static constexpr GPRReg countGPR = ARM64Registers::x9;
static constexpr GPRReg calleeGPR = ARM64Registers::x10;
static constexpr GPRReg valueEntryGPR = ARM64Registers::x11;
#elif CPU(X86_64)
static constexpr unsigned numberOfArgumentGPRs = 4;
static constexpr GPRReg thisGPR = X86Registers::eax;
static constexpr GPRReg countGPR = X86Registers::r10;
static constexpr GPRReg calleeGPR = X86Registers::r8;
static constexpr GPRReg valueEntryGPR = X86Registers::r12;
#endif

#if CPU(X86_64)
static constexpr unsigned numberOfDirectArgumentGPRs = GPRInfo::numberOfArgumentRegisters + 2;
constexpr GPRReg directArgumentGPR(unsigned index)
{
    if (index < GPRInfo::numberOfArgumentRegisters)
        return GPRInfo::toArgumentRegister(index);
    return index == GPRInfo::numberOfArgumentRegisters ? X86Registers::eax : X86Registers::r10;
}
#else
static constexpr unsigned numberOfDirectArgumentGPRs = numberOfArgumentGPRs;
constexpr GPRReg directArgumentGPR(unsigned index) { return argumentGPR(index); }
#endif
static constexpr unsigned numberOfDirectArgumentFPRs = FPRInfo::numberOfArgumentRegisters;
constexpr FPRReg directArgumentFPR(unsigned index) { return FPRInfo::toArgumentRegister(index); }

static constexpr unsigned sizeOfStackWord = 8;
static constexpr ptrdiff_t offsetOfStackWordsFromFrame = 2 * sizeOfStackWord;
static constexpr unsigned sizeOfSpilledArgumentRegisters = numberOfArgumentGPRs * sizeOfStackWord;
static_assert(!(sizeOfSpilledArgumentRegisters % 16));

constexpr ptrdiff_t offsetOfStackWordFromFrame(unsigned stackIndex)
{
    return offsetOfStackWordsFromFrame + static_cast<ptrdiff_t>(stackIndex) * sizeOfStackWord;
}

static constexpr unsigned maxArrayElementsInRegisters = 4;
static constexpr unsigned arrayLiteralVectorLength(unsigned count) { return (count ? count : 5) | 1; }
// A longer array is allocated by the stub that takes the length as an operand.
static constexpr unsigned maxArrayLengthInStubKey = 16;
static constexpr unsigned capacityShiftInArrayLiteralCount = 27;

static constexpr unsigned maxFrameSizeWithoutStackCheck = 240;
static constexpr unsigned maxUncheckedFrameSize = 256;
static constexpr unsigned maxNestedFunctionsWithoutStackCheck = 8;
static constexpr unsigned maxStackUsedWithoutCheck = 4096;
static_assert(maxNestedFunctionsWithoutStackCheck * (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*)) <= maxStackUsedWithoutCheck / 2);
static_assert(maxFrameSizeWithoutStackCheck <= maxUncheckedFrameSize && maxUncheckedFrameSize + 2 * sizeof(void*) <= maxStackUsedWithoutCheck / 2);
static_assert(sizeOfSpilledArgumentRegisters <= maxUncheckedFrameSize);

static_assert(numberOfArgumentGPRs <= ClosureWord::maxNumberOfParameters);

struct ClosureRequest {
    static constexpr unsigned executableShift = ClosureWord::numberOfFunctionBits + 2;
    static constexpr unsigned allocationShift = 32;
    static constexpr unsigned countShift = 56;
    static constexpr unsigned isSlimBit = 31;
#if CPU(ARM64)
    static constexpr unsigned maxNumberOfCaptures = 4;
#else
    static constexpr unsigned maxNumberOfCaptures = 2;
#endif
    static constexpr unsigned maxSlimClosureCaptures = 4;
    static constexpr bool canEncode(uint32_t function, unsigned executable, uint32_t allocation)
    {
        return function <= ClosureWord::functionMask && executable < 1u << (isSlimBit - executableShift) && allocation < 1u << (countShift - allocationShift);
    }
    static uint64_t encode(uint32_t function, unsigned executable, uint32_t allocation)
    {
        RELEASE_ASSERT(canEncode(function, executable, allocation));
        return function | static_cast<uint64_t>(executable) << executableShift | static_cast<uint64_t>(allocation) << allocationShift;
    }
    static constexpr uint32_t functionOf(uint64_t request) { return request & ClosureWord::functionMask; }
    static constexpr unsigned executableOf(uint64_t request) { return request >> executableShift & ((1u << (isSlimBit - executableShift)) - 1); }
    static constexpr uint32_t allocationOf(uint64_t request) { return request >> allocationShift & ((1u << (countShift - allocationShift)) - 1); }
    static constexpr unsigned countOf(uint64_t request) { return request >> countShift & 0x1f; }
    static constexpr bool isSlim(uint64_t request) { return request >> isSlimBit & 1; }
};

struct ResumeClosureRequest {
    static constexpr unsigned frameWordShift = ClosureRequest::executableShift + 3;
    static constexpr unsigned maxExecutable = (1u << (frameWordShift - ClosureRequest::executableShift)) - 1;
    static constexpr unsigned maxFrameWord = (1u << (ClosureRequest::isSlimBit - frameWordShift)) - 1;
    static constexpr bool canEncode(uint32_t function, unsigned executable, uint32_t allocation, unsigned frameWord)
    {
        return executable <= maxExecutable && frameWord <= maxFrameWord && ClosureRequest::canEncode(function, executable, allocation);
    }
    static uint64_t encode(uint32_t function, unsigned executable, uint32_t allocation, unsigned frameWord)
    {
        RELEASE_ASSERT(canEncode(function, executable, allocation, frameWord));
        return ClosureRequest::encode(function, executable, allocation) | static_cast<uint64_t>(frameWord) << frameWordShift;
    }
    static constexpr unsigned executableOf(uint64_t request) { return request >> ClosureRequest::executableShift & maxExecutable; }
    static constexpr unsigned frameWordOf(uint64_t request) { return request >> frameWordShift & maxFrameWord; }
    static constexpr unsigned stubFrameWord = 2;
    static constexpr unsigned allocationShiftForStub = ClosureRequest::executableShift;
    static constexpr uint32_t maxAllocationForStub = (1u << (32 - allocationShiftForStub)) - 1;
    static constexpr uint32_t encodeForStub(uint32_t function, uint32_t allocation) { return function | allocation << allocationShiftForStub; }
};

static_assert(numberOfArgumentGPRs <= numberOfDirectArgumentGPRs);
static_assert([] {
    for (unsigned index = 0; index < numberOfArgumentGPRs; ++index) {
        GPRReg reg = argumentGPR(index);
        if (reg != directArgumentGPR(index) || !noOverlap(reg, thisGPR, countGPR, calleeGPR, valueEntryGPR, instanceGPR, cellCageBaseGPR))
            return false;
    }
    return true;
}());
static_assert([] {
    for (unsigned index = 0; index < numberOfDirectArgumentGPRs; ++index) {
        if (!noOverlap(directArgumentGPR(index), valueEntryGPR, instanceGPR, cellCageBaseGPR))
            return false;
    }
    return true;
}());
#if CPU(X86_64)
static_assert(valueEntryGPR != MacroAssembler::s_scratchRegister && valueEntryGPR == unsavedCalleeSaveGPRs[0]);
#elif CPU(ARM64)
static_assert(noOverlap(valueEntryGPR, MacroAssembler::dataTempRegister, MacroAssembler::memoryTempRegister));
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
