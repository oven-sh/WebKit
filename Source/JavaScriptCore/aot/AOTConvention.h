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

// The calling convention of AOT code, as used between AOT functions and by the stubs.
//
// A frame consists of the caller's frame pointer and the return address, with the function's own data below them, and nothing else.
// It does not record which function it belongs to, what arguments were passed or the current position. The function and the
// position in it are both derived from the address at which execution will resume in the frame (see frameAt()).
//
// Three registers hold the same values in all AOT code and in the stubs. They are callee-saved both in the C calling convention and
// in the engine's own, and AOT code never writes to them, so AOT code neither saves nor reloads them. An adapter sets them up on
// entry from other code.
static constexpr GPRReg instanceGPR = GPRInfo::jitDataRegister; // The realm's Instance.
// And GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister.

// The parameters, not counting `this`.
static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg argumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#if CPU(ARM64)
// `this`; for a constructor, new.target. A caller that knows the function makes no use of it leaves it out.
static constexpr GPRReg thisGPR = ARM64Registers::x8;
// For the stubs that call an unknown callee: the number of arguments, not counting `this`.
static constexpr GPRReg countGPR = ARM64Registers::x9;
// The function object that is called. It is also omitted by a caller that knows that the callee does not use it.
static constexpr GPRReg calleeGPR = ARM64Registers::x10;
#elif CPU(X86_64)
static constexpr GPRReg thisGPR = X86Registers::eax;
static constexpr GPRReg countGPR = X86Registers::r10;
static constexpr GPRReg calleeGPR = X86Registers::r11;
#else
static constexpr GPRReg thisGPR = InvalidGPRReg;
static constexpr GPRReg countGPR = InvalidGPRReg;
static constexpr GPRReg calleeGPR = InvalidGPRReg;
#endif

enum class Signature : uint8_t {
    // Each parameter is in its register. The caller passes undefined for missing arguments and drops extra ones.
    Registers,
    // For a function that can observe its actual arguments, or that has more parameters than there are registers: argumentGPR(0) =
    // the number of arguments, not counting `this`; argumentGPR(1) = the address of the first. The arguments are in the caller's
    // memory, and stay valid until the function returns.
    List,
};

struct Convention {
    Signature signature { Signature::Registers };
    uint8_t numberOfParameters { 0 }; // Not counting `this`.
    bool usesThis { true };
};
// Derived from the bytecode alone, so that a function and its callers agree without any communication.
JS_EXPORT_PRIVATE Convention conventionOf(UnlinkedCodeBlock*);

// The address of a function's code and, in the bits above the address, what is needed to call it without knowing anything else
// about it.
struct EntryWord {
    static constexpr unsigned shiftOfNumberOfParameters = 48;
    static constexpr unsigned bitsOfNumberOfParameters = 4;
    static constexpr unsigned bitOfIsList = 52;
    static constexpr uint64_t addressMask = (1ULL << shiftOfNumberOfParameters) - 1;
    static_assert(numberOfArgumentGPRs < (1u << bitsOfNumberOfParameters));

    static uint64_t encode(const void* address, Convention convention)
    {
        uint64_t bits = std::bit_cast<uintptr_t>(address);
        RELEASE_ASSERT(!(bits & ~addressMask));
        if (convention.signature == Signature::List)
            return bits | 1ULL << bitOfIsList;
        return bits | static_cast<uint64_t>(convention.numberOfParameters) << shiftOfNumberOfParameters;
    }
    static void* address(uint64_t word) { return std::bit_cast<void*>(static_cast<uintptr_t>(word & addressMask)); }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
