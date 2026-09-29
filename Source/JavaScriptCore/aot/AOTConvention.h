/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(FTL_JIT)

#include "GPRInfo.h"

namespace JSC {

class UnlinkedCodeBlock;

namespace AOT {

// How code from the static compiler is called, by other such code and by the stubs.
//
// A frame is the caller's frame pointer and the return address, with the function's own things below them, and that is all. There is
// nothing in it that says whose it is, what it was passed or where it has got to: which function a frame belongs to, and where in
// the function it is, is told from the address it is going to be returned to (see frameAt()).
//
// Three registers hold the same thing in all such code, and in the stubs. A callee saves them in C and in the engine's own convention,
// and this code never writes them: so nobody here saves them, and nobody loads them. Whoever comes in from outside (an adapter) does.
static constexpr GPRReg instanceGPR = GPRInfo::jitDataRegister; // The realm's Instance.
// And GPRInfo::numberTagRegister, GPRInfo::notCellMaskRegister.

// The parameters, not counting `this`.
static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg argumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#if CPU(ARM64)
// `this`; for a constructor, new.target. A caller that knows the function makes no use of it leaves it out.
static constexpr GPRReg thisGPR = ARM64Registers::x8;
// For the stubs that call whatever they are given: how many arguments there are, not counting `this`.
static constexpr GPRReg countGPR = ARM64Registers::x9;
// The object that is called. Likewise left out by who knows better.
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
    // Each parameter in its register. Whoever calls fills in undefined for what it has not got, and keeps what is left over to itself.
    Registers,
    // For a function that can tell what it was really passed, or that has more parameters than there are registers:
    // argumentGPR(0) = how many arguments, not counting `this`; argumentGPR(1) = where the first is. They are the caller's, and stay
    // put until the function returns.
    List,
};

struct Convention {
    Signature signature { Signature::Registers };
    uint8_t numberOfParameters { 0 }; // Not counting `this`.
    bool usesThis { true };
};
// Plain from the bytecode, so the function and whoever calls it agree without asking each other.
JS_EXPORT_PRIVATE Convention conventionOf(UnlinkedCodeBlock*);

// Where a function's code is and, above the address, what it takes to call it without knowing anything else about it.
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

#endif // ENABLE(FTL_JIT)
