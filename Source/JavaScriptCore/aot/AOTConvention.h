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

static constexpr unsigned numberOfArgumentGPRs = GPRInfo::numberOfArgumentRegisters;
constexpr GPRReg argumentGPR(unsigned index) { return GPRInfo::toArgumentRegister(index); }
#if CPU(ARM64)
static constexpr GPRReg thisGPR = ARM64Registers::x8;
static constexpr GPRReg countGPR = ARM64Registers::x9;
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
    static constexpr unsigned numberOfParametersShift = 48;
    static constexpr unsigned numberOfParametersBits = 4;
    static constexpr unsigned isListBit = 52;
    static constexpr uint64_t addressMask = (1ULL << numberOfParametersShift) - 1;
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
