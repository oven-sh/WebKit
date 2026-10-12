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

constexpr GPRReg argumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#if CPU(ARM64)
static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
static constexpr GPRReg thisGPR = ARM64Registers::x8;
static constexpr GPRReg countGPR = ARM64Registers::x9;
static constexpr GPRReg calleeGPR = ARM64Registers::x10;
static constexpr GPRReg callMarkerGPR = ARM64Registers::lr;
static constexpr bool hasStubsForFunctionsWithoutFrame = true;
static constexpr GPRReg stubTemporaryGPRs[] = { ARM64Registers::x9, ARM64Registers::x10, ARM64Registers::x11, ARM64Registers::x12, ARM64Registers::x13, ARM64Registers::x14, ARM64Registers::x15 };
static constexpr unsigned numberOfOperationArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg operationArgumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#elif CPU(X86_64)
static constexpr unsigned numberOfArgumentGPRs = 4;
static constexpr GPRReg thisGPR = X86Registers::eax;
static constexpr GPRReg countGPR = X86Registers::r10;
static constexpr GPRReg calleeGPR = X86Registers::r8;
static constexpr GPRReg callMarkerGPR = X86Registers::ebp;
static constexpr bool hasStubsForFunctionsWithoutFrame = false;
static constexpr GPRReg stubTemporaryGPRs[] = { X86Registers::r10, X86Registers::r8, X86Registers::r9, X86Registers::edi, X86Registers::r12 };
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
static constexpr bool hasStubsForFunctionsWithoutFrame = false;
static constexpr GPRReg stubTemporaryGPRs[] = { InvalidGPRReg };
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
static_assert(stubTemporaryGPRs[0] == stubImmediateGPR);
static constexpr unsigned maxLiteralPropertiesInRegisters = GPRInfo::numberOfArgumentRegisters - 2;
static constexpr unsigned maxArrayElementsInRegisters = 4;
// The room an array literal of so many elements is born with: in the stubs, in place, and in the operation behind both. ONE formula: where the operation takes another size class than the
// code it stands behind, the free list that has run out there is never filled again. With the header the butterfly is a multiple of 16 bytes.
static constexpr unsigned vectorLengthOfArrayLiteral(unsigned count) { return (count ? count : 5) | 1; }
// operationAOTTypedNewArray is told the room of an empty literal that is born with another above the count.
static constexpr unsigned capacityShiftInArrayLiteralCount = 27;

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
    // Of a closure of the typed compiler: [[Construct]] makes an ordinary object that inherits from its `prototype`, calls it with that for
    // `this` and the arguments as they are, and yields what it returns if that is an object, else the object. Below the number of
    // parameters: a call does not look at it.
    static constexpr unsigned constructsOrdinaryObjectBit = numberOfAddressBits;
    static constexpr unsigned numberOfParametersShift = constructsOrdinaryObjectBit + 1;
    static constexpr unsigned numberOfParametersBits = 4;
    static constexpr unsigned isListBit = numberOfParametersShift + numberOfParametersBits;
    // [[Call]] throws: a class constructor.
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

// What a closure of the typed compiler has of its function (Typed::Closure::m_entry). It does not depend on where the image is loaded, so
// it is a constant of the build: ProgramData::closureWords() has it by function, read-only.
//   0 to 31   where the code is that takes its arguments as the engine passes them: in units of offsetUnit bytes from the start of the image's code
//   32 to 55  the number of the function, by which the image has its name, its `length`, what it is declared as, how many captures
//   56        isDerivedConstructorBit
//   57 to 63  as in EntryWord. On top: a call tells by one comparison that the closure takes the arguments it has, in registers, and may be called
struct ClosureWord {
    static constexpr unsigned numberOfOffsetBits = 32;
    // What every function starts at a multiple of.
#if CPU(ARM64)
    static constexpr unsigned offsetUnit = 4;
#else
    static constexpr unsigned offsetUnit = 1;
#endif
    static constexpr unsigned functionShift = numberOfOffsetBits;
    static constexpr unsigned numberOfFunctionBits = 24;
    // [[Construct]] makes nothing: it calls it with `undefined` for `this` and new.target ahead of the arguments, and yields what it returns:
    // a class that extends something, which makes the object.
    static constexpr unsigned isDerivedConstructorBit = functionShift + numberOfFunctionBits;
    // [[Construct]] makes an ordinary object that inherits from its `prototype`, calls it with `this` and the arguments as they are, and
    // yields what it returns if that is an object, else the object. Below the number of parameters: a call does not look at it.
    static constexpr unsigned constructsOrdinaryObjectBit = isDerivedConstructorBit + 1;
    static constexpr unsigned numberOfParametersShift = constructsOrdinaryObjectBit + 1;
    static constexpr unsigned numberOfParametersBits = EntryWord::numberOfParametersBits;
    static constexpr unsigned isListBit = numberOfParametersShift + numberOfParametersBits;
    // [[Call]] throws: a class constructor. Or nothing can call it: the resume closure of a coroutine.
    static constexpr unsigned isNotCallableBit = isListBit + 1;
    static_assert(isNotCallableBit == 63);
    static constexpr uint64_t offsetMask = (1ULL << numberOfOffsetBits) - 1;
    static constexpr uint32_t functionMask = (1u << numberOfFunctionBits) - 1;
    // No function of a program has one of these numbers: for closures that the run time has kinds of its own for.
    static constexpr uint32_t firstReservedFunction = functionMask + 1 - 16;
    static constexpr uint64_t maxSizeOfCode = (offsetMask + 1) * offsetUnit;

    // `relative`: an EntryWord whose address is the distance from the start of the code.
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

// What Stub::NewTypedClosure0 and the like are asked for.
//   0 to 23   the number of the function that closures go by: ClosureWord::functionOf()
//   24 to 31  which executable: Typed::FunctionTables::kindOfExecutable()
//   32 to 55  the number of the allocation
//   56 to 63  only for the operation behind the stub: how many captures
struct ClosureRequest {
    static constexpr unsigned executableShift = ClosureWord::numberOfFunctionBits;
    static constexpr unsigned allocationShift = 32;
    static constexpr unsigned countShift = 56;
    static constexpr unsigned isSlimBit = 63;
#if CPU(ARM64)
    static constexpr unsigned maxNumberOfCaptures = 4;
#else
    // The fifth and the sixth argument register are temporaries of the stubs there.
    static constexpr unsigned maxNumberOfCaptures = 2;
#endif
    static uint64_t encode(uint32_t function, unsigned executable, uint32_t allocation)
    {
        RELEASE_ASSERT(function <= ClosureWord::functionMask && executable < 1u << (allocationShift - executableShift) && allocation < 1u << (countShift - allocationShift));
        return function | static_cast<uint64_t>(executable) << executableShift | static_cast<uint64_t>(allocation) << allocationShift;
    }
    static constexpr uint32_t functionOf(uint64_t request) { return request & ClosureWord::functionMask; }
    static constexpr unsigned executableOf(uint64_t request) { return request >> executableShift & 0xff; }
    static constexpr uint32_t allocationOf(uint64_t request) { return request >> allocationShift & ((1u << (countShift - allocationShift)) - 1); }
    static constexpr unsigned countOf(uint64_t request) { return request >> countShift & 0x7f; }
    static constexpr bool isSlim(uint64_t request) { return request >> isSlimBit; }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
