/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "GPRInfo.h"

namespace JSC {

class UnlinkedCodeBlock;

namespace AOT {

static constexpr GPRReg instanceGPR = GPRInfo::jitDataRegister;
#if CPU(ARM64)
static constexpr GPRReg cellCageBaseGPR = ARM64Registers::x25;
static constexpr GPRReg numberTagArgumentGPR = GPRInfo::numberTagRegister;
#elif CPU(X86_64)
static constexpr GPRReg cellCageBaseGPR = X86Registers::r14;
static constexpr GPRReg numberTagArgumentGPR = X86Registers::r11;
#endif

constexpr GPRReg argumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#if CPU(ARM64)
static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
static constexpr GPRReg thisGPR = ARM64Registers::x8;
static constexpr GPRReg countGPR = ARM64Registers::x9;
static constexpr GPRReg calleeGPR = ARM64Registers::x10;
static constexpr GPRReg callMarkerGPR = ARM64Registers::lr;
static constexpr bool hasFramelessFunctionStubs = true;
static constexpr GPRReg stubTemporaryGPRs[] = { ARM64Registers::x9, ARM64Registers::x10, ARM64Registers::x11, ARM64Registers::x12, ARM64Registers::x13, ARM64Registers::x14, ARM64Registers::x15 };
static constexpr GPRReg copyEndOffsetGPR = ARM64Registers::x14;
static constexpr GPRReg lazyConstantKeyGPR = ARM64Registers::x9;
static constexpr unsigned numberOfOperationArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg operationArgumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#elif CPU(X86_64)
static constexpr unsigned numberOfArgumentGPRs = 4;
static constexpr GPRReg thisGPR = X86Registers::eax;
static constexpr GPRReg countGPR = X86Registers::r10;
static constexpr GPRReg calleeGPR = X86Registers::r8;
static constexpr GPRReg callMarkerGPR = X86Registers::ebp;
static constexpr bool hasFramelessFunctionStubs = false;
static constexpr GPRReg stubTemporaryGPRs[] = { X86Registers::r10, X86Registers::r8, X86Registers::r9, X86Registers::edi, X86Registers::r12 };
static constexpr GPRReg copyEndOffsetGPR = X86Registers::r8;
static constexpr GPRReg lazyConstantKeyGPR = X86Registers::eax;
static constexpr GPRReg seventhOperationArgumentGPR = X86Registers::r12;
static constexpr GPRReg eighthOperationArgumentGPR = X86Registers::ebx;
static constexpr unsigned numberOfOperationArgumentGPRs = GPRInfo::numberOfArgumentRegisters + 2;
constexpr GPRReg operationArgumentGPR(unsigned index)
{
    if (index < GPRInfo::numberOfArgumentRegisters)
        return GPRInfo::toArgumentRegister(index);
    return index == GPRInfo::numberOfArgumentRegisters ? seventhOperationArgumentGPR : eighthOperationArgumentGPR;
}
#else
static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
static constexpr GPRReg thisGPR = InvalidGPRReg;
static constexpr GPRReg countGPR = InvalidGPRReg;
static constexpr GPRReg calleeGPR = InvalidGPRReg;
static constexpr GPRReg callMarkerGPR = InvalidGPRReg;
static constexpr bool hasFramelessFunctionStubs = false;
static constexpr GPRReg stubTemporaryGPRs[] = { InvalidGPRReg };
static constexpr GPRReg copyEndOffsetGPR = InvalidGPRReg;
static constexpr GPRReg lazyConstantKeyGPR = InvalidGPRReg;
static constexpr unsigned numberOfOperationArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg operationArgumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
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
static constexpr GPRReg stubImmediateGPR = countGPR;
static constexpr GPRReg firstStubOperandGPR = GPRInfo::returnValueGPR;
#if CPU(X86_64)
static constexpr GPRReg pairedNamesGPR = GPRInfo::argumentGPR1;
static constexpr GPRReg secondReceiverGPR = X86Registers::ecx;
static constexpr GPRReg firstOfTwoResultsGPR = X86Registers::edx;
#elif CPU(ARM64)
static constexpr GPRReg pairedNamesGPR = stubImmediateGPR;
static constexpr GPRReg secondReceiverGPR = ARM64Registers::x1;
static constexpr GPRReg firstOfTwoResultsGPR = ARM64Registers::x2;
#else
static constexpr GPRReg pairedNamesGPR = InvalidGPRReg;
static constexpr GPRReg secondReceiverGPR = InvalidGPRReg;
static constexpr GPRReg firstOfTwoResultsGPR = InvalidGPRReg;
#endif
static_assert(stubTemporaryGPRs[0] == stubImmediateGPR);
static constexpr unsigned maxLiteralPropertiesInRegisters = GPRInfo::numberOfArgumentRegisters - 2;
static constexpr unsigned maxArrayElementsInRegisters = 4;
static constexpr unsigned arrayLiteralVectorLength(unsigned count) { return (count ? count : 5) | 1; }
static constexpr unsigned capacityShiftInArrayLiteralCount = 27;
static constexpr unsigned maxFrameSizeWithoutStackCheck = 240;
static constexpr unsigned maxNestedFunctionsWithoutStackCheck = 8;
static constexpr unsigned maxStackUsedWithoutCheck = 4096;
static_assert(maxNestedFunctionsWithoutStackCheck * (maxFrameSizeWithoutStackCheck + 2 * sizeof(void*)) <= maxStackUsedWithoutCheck / 2);

enum class Signature : uint8_t {
    Registers,
    List,
};

struct Convention {
    Signature signature { Signature::Registers };
    uint8_t numberOfParameters { 0 };
    bool usesThis { true };
};
JS_EXPORT_PRIVATE Convention conventionOf(UnlinkedCodeBlock*);

struct EntryWord {
    static constexpr unsigned numberOfAddressBits = 48;
    static constexpr unsigned constructsOrdinaryObjectBit = numberOfAddressBits;
    static constexpr unsigned numberOfParametersShift = constructsOrdinaryObjectBit + 1;
    static constexpr unsigned numberOfParametersBits = 4;
    static constexpr unsigned isListBit = numberOfParametersShift + numberOfParametersBits;
    static constexpr unsigned isNotCallableBit = isListBit + 1;
    static constexpr uint64_t addressMask = (1ULL << numberOfAddressBits) - 1;
    static_assert(numberOfArgumentGPRs < (1u << numberOfParametersBits));

    static uint64_t encode(const void* address, Convention convention) { return encode(std::bit_cast<uintptr_t>(address), convention); }
    static uint64_t encode(uint64_t bits, Convention convention)
    {
        RELEASE_ASSERT(bits && !(bits & ~addressMask));
        if (convention.signature == Signature::List)
            return bits | 1ULL << isListBit;
        return bits | static_cast<uint64_t>(convention.numberOfParameters) << numberOfParametersShift;
    }
};

struct ClosureWord {
    static constexpr unsigned numberOfOffsetBits = 32;
#if CPU(ARM64)
    static constexpr unsigned offsetUnit = 4;
#else
    static constexpr unsigned offsetUnit = 1;
#endif
    static constexpr unsigned functionShift = numberOfOffsetBits;
    static constexpr unsigned numberOfFunctionBits = 22;
    static constexpr unsigned hasCompatibleResultsMaskBit = functionShift + numberOfFunctionBits;
    static constexpr unsigned hasValueEntryBit = hasCompatibleResultsMaskBit + 1;
    static constexpr unsigned isDerivedConstructorBit = hasValueEntryBit + 1;
    static constexpr unsigned constructsOrdinaryObjectBit = isDerivedConstructorBit + 1;
    static constexpr unsigned numberOfParametersShift = constructsOrdinaryObjectBit + 1;
    static constexpr unsigned numberOfParametersBits = EntryWord::numberOfParametersBits;
    static constexpr unsigned isListBit = numberOfParametersShift + numberOfParametersBits;
    static constexpr unsigned isNotCallableBit = isListBit + 1;
    static_assert(isNotCallableBit == 63);
    static constexpr uint64_t offsetMask = (1ULL << numberOfOffsetBits) - 1;
    static constexpr uint32_t functionMask = (1u << numberOfFunctionBits) - 1;
    static constexpr uint32_t firstReservedFunction = (functionMask >> 1) + 1 - 16;
    static constexpr uint64_t maxSizeOfCode = (offsetMask + 1) * offsetUnit;

    static uint64_t encode(uint64_t relative, uint32_t function)
    {
        uint64_t distance = relative & EntryWord::addressMask;
        RELEASE_ASSERT(distance && !(distance % offsetUnit) && distance < maxSizeOfCode && function < firstReservedFunction);
        uint64_t numberOfParameters = relative >> EntryWord::numberOfParametersShift & ((1u << numberOfParametersBits) - 1);
        return distance / offsetUnit
            | static_cast<uint64_t>(function) << functionShift
            | numberOfParameters << numberOfParametersShift
            | (relative >> EntryWord::isListBit & 1) << isListBit;
    }
    static constexpr uint32_t functionOf(uint64_t word) { return word >> functionShift & functionMask; }
    static constexpr uintptr_t addressOf(uint64_t word, uintptr_t code) { return code + (word & offsetMask) * offsetUnit; }
};

struct ClosureRequest {
    static constexpr unsigned executableShift = ClosureWord::numberOfFunctionBits + 2;
    static constexpr unsigned allocationShift = 32;
    static constexpr unsigned countShift = 56;
    static constexpr unsigned variableIsCell32Bit = 61;
    static constexpr unsigned isCreatedOnceBit = 62;
    static constexpr unsigned isSlimBit = 31;
#if CPU(ARM64)
    static constexpr unsigned maxNumberOfCaptures = 4;
#else
    static constexpr unsigned maxNumberOfCaptures = 2;
#endif
    static constexpr unsigned maxSlimClosureCaptures = 4;
    static uint64_t encode(uint32_t function, unsigned executable, uint32_t allocation)
    {
        RELEASE_ASSERT(function <= ClosureWord::functionMask && executable < 1u << (isSlimBit - executableShift) && allocation < 1u << (countShift - allocationShift));
        return function | static_cast<uint64_t>(executable) << executableShift | static_cast<uint64_t>(allocation) << allocationShift;
    }
    static constexpr uint32_t functionOf(uint64_t request) { return request & ClosureWord::functionMask; }
    static constexpr unsigned executableOf(uint64_t request) { return request >> executableShift & ((1u << (isSlimBit - executableShift)) - 1); }
    static constexpr uint32_t allocationOf(uint64_t request) { return request >> allocationShift & ((1u << (countShift - allocationShift)) - 1); }
    static constexpr unsigned countOf(uint64_t request) { return request >> countShift & 0x1f; }
    static constexpr bool variableIsCell32(uint64_t request) { return request >> variableIsCell32Bit & 1; }
    static constexpr bool isCreatedOnce(uint64_t request) { return request >> isCreatedOnceBit & 1; }
    static constexpr bool isSlim(uint64_t request) { return request >> isSlimBit & 1; }
};

struct ResumeClosureRequest {
    static constexpr unsigned frameWordShift = ClosureRequest::executableShift + 3;
    static constexpr unsigned maxExecutable = (1u << (frameWordShift - ClosureRequest::executableShift)) - 1;
    static constexpr unsigned maxFrameWord = (1u << (ClosureRequest::isSlimBit - frameWordShift)) - 1;
    static uint64_t encode(uint32_t function, unsigned executable, uint32_t allocation, unsigned frameWord)
    {
        RELEASE_ASSERT(executable <= maxExecutable && frameWord <= maxFrameWord);
        return ClosureRequest::encode(function, executable, allocation) | static_cast<uint64_t>(frameWord) << frameWordShift;
    }
    static constexpr unsigned executableOf(uint64_t request) { return request >> ClosureRequest::executableShift & maxExecutable; }
    static constexpr unsigned frameWordOf(uint64_t request) { return request >> frameWordShift & maxFrameWord; }
    static constexpr unsigned stubFrameWord = 2;
    static constexpr unsigned allocationShiftForStub = ClosureRequest::executableShift;
    static constexpr uint32_t maxAllocationForStub = (1u << (32 - allocationShiftForStub)) - 1;
    static constexpr uint32_t encodeForStub(uint32_t function, uint32_t allocation) { return function | allocation << allocationShiftForStub; }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
