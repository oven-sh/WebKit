/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "GPRInfo.h"
#include "MacroAssembler.h"
#include "Reg.h"
#include <compare>
#include <optional>

namespace JSC { namespace AOT {

enum class StubFamily : uint16_t {
#define v(Name, ...) Name,
#include "AOTStubFamilies.def"
#undef v
};

constexpr unsigned numberOfStubFamilies = 0
#define v(...) + 1
#include "AOTStubFamilies.def"
#undef v
    ;

enum class CallerFrame : uint8_t { HasFrame, Frameless, StubFrame };

// A stub of such a family takes the frame pointer register at its entry for the frame of the function that calls it, so that function has a frame record.
constexpr bool readsFrameRecordOfCaller(StubFamily family)
{
    switch (family) {
    case StubFamily::ReturnAsyncResult:
    case StubFamily::AwaitBySavingFrame:
    case StubFamily::SuspendGenerator:
    case StubFamily::StepAsyncGenerator:
    case StubFamily::TailCallWithList:
        return true;
    default:
        return false;
    }
}

// A tail call by value gives up the frame that says which closure runs above it. The stubs that make one have a form of their own: StubAssembler::checkStackBeforeEnteringCallee().
enum class CallPosition : uint8_t { Ordinary, Tail };

class StubKey {
public:
    static constexpr unsigned maxNumberOfForms = 64;
    static constexpr unsigned maxCount = 63;

    constexpr StubKey() = default;

    explicit constexpr StubKey(StubFamily family)
        : m_bits(StubKey().withFamily(family).m_bits)
    {
    }

    constexpr bool hasFamily() const
    {
        uint64_t biasedFamily = field(familyShift, familyWidth);
        return biasedFamily && biasedFamily <= numberOfStubFamilies;
    }

    constexpr StubFamily family() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(hasFamily());
        return static_cast<StubFamily>(field(familyShift, familyWidth) - 1);
    }

    constexpr uint8_t form() const { return static_cast<uint8_t>(field(formShift, formWidth)); }
    constexpr CallerFrame callerFrame() const { return static_cast<CallerFrame>(field(callerFrameShift, callerFrameWidth)); }
    constexpr unsigned count() const { return static_cast<unsigned>(field(countShift, countWidth)); }

    constexpr std::optional<GPRReg> operandRegister() const
    {
        uint64_t biasedIndex = field(operandRegisterShift, operandRegisterWidth);
        if (!biasedIndex)
            return std::nullopt;
        return Reg::fromIndex(static_cast<unsigned>(biasedIndex - 1)).gpr();
    }

    constexpr std::optional<uint32_t> immediate() const
    {
        if (!field(hasImmediateShift, hasImmediateWidth))
            return std::nullopt;
        return static_cast<uint32_t>(field(immediateShift, immediateWidth));
    }

    constexpr StubKey withForm(uint8_t form) const { return withField(formShift, formWidth, form); }
    constexpr StubKey withCallerFrame(CallerFrame callerFrame) const { return withField(callerFrameShift, callerFrameWidth, static_cast<uint64_t>(callerFrame)); }
    constexpr StubKey withCount(unsigned count) const { return withField(countShift, countWidth, count); }
    constexpr StubKey withOperandRegister(GPRReg reg) const { return withField(operandRegisterShift, operandRegisterWidth, static_cast<uint64_t>(Reg(reg).index()) + 1); }
    constexpr StubKey withoutOperandRegister() const { return withField(operandRegisterShift, operandRegisterWidth, 0); }
    constexpr StubKey withImmediate(uint32_t immediate) const { return withField(hasImmediateShift, hasImmediateWidth, 1).withField(immediateShift, immediateWidth, immediate); }
    constexpr StubKey withoutImmediate() const { return withField(hasImmediateShift, hasImmediateWidth, 0).withField(immediateShift, immediateWidth, 0); }
    constexpr StubKey withFamily(StubFamily family) const { return withField(familyShift, familyWidth, static_cast<uint64_t>(family) + 1); }

    constexpr uint64_t bits() const { return m_bits; }

    static constexpr StubKey fromBits(uint64_t bits)
    {
        StubKey key;
        key.m_bits = bits;
        return key;
    }

    friend constexpr auto operator<=>(StubKey, StubKey) = default;

private:
    static constexpr unsigned immediateShift = 0;
    static constexpr unsigned immediateWidth = 32;
    static constexpr unsigned hasImmediateShift = immediateShift + immediateWidth;
    static constexpr unsigned hasImmediateWidth = 1;
    static constexpr unsigned operandRegisterShift = hasImmediateShift + hasImmediateWidth;
    static constexpr unsigned operandRegisterWidth = 6;
    static constexpr unsigned callerFrameShift = operandRegisterShift + operandRegisterWidth;
    static constexpr unsigned callerFrameWidth = 2;
    static constexpr unsigned countShift = callerFrameShift + callerFrameWidth;
    static constexpr unsigned countWidth = 6;
    static constexpr unsigned formShift = countShift + countWidth;
    static constexpr unsigned formWidth = 6;
    static constexpr unsigned familyShift = formShift + formWidth;
    static constexpr unsigned familyWidth = 11;

    static_assert(familyShift + familyWidth == 64);
    static_assert(numberOfStubFamilies < (1u << familyWidth) - 1);
    static_assert(maxNumberOfForms == 1u << formWidth);
    static_assert(maxCount == (1u << countWidth) - 1);
    static_assert(MacroAssembler::numberOfRegisters() < (1u << operandRegisterWidth));

    static constexpr uint64_t maskOf(unsigned width) { return (static_cast<uint64_t>(1) << width) - 1; }

    constexpr uint64_t field(unsigned shift, unsigned width) const { return (m_bits >> shift) & maskOf(width); }

    constexpr StubKey withField(unsigned shift, unsigned width, uint64_t value) const
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(value <= maskOf(width));
        return fromBits((m_bits & ~(maskOf(width) << shift)) | value << shift);
    }

    uint64_t m_bits { 0 };
};

static_assert(sizeof(StubKey) == sizeof(uint64_t));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
