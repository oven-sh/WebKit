/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMemoryLocation.h"

namespace JSC { namespace AOT { namespace Typed {

class ThrowEffect {
public:
    constexpr ThrowEffect(bool mayThrow = true)
        : m_mayThrow(mayThrow)
        , m_mayThrowOtherThanStackOverflow(mayThrow)
        , m_mayThrowByRule(mayThrow)
    {
    }

    static constexpr ThrowEffect onlyForLackOfMemory()
    {
        ThrowEffect result(true);
        result.m_mayThrowByRule = false;
        return result;
    }

    static constexpr ThrowEffect onlyStackOverflow()
    {
        ThrowEffect result = onlyForLackOfMemory();
        result.m_mayThrowOtherThanStackOverflow = false;
        return result;
    }

    constexpr operator bool() const { return m_mayThrow; }
    constexpr bool otherThanStackOverflow() const { return m_mayThrowOtherThanStackOverflow; }
    constexpr bool byRule() const { return m_mayThrowByRule; }
    constexpr bool isDeclaredOnlyForLackOfMemory() const { return m_mayThrowOtherThanStackOverflow && !m_mayThrowByRule; }

    constexpr ThrowEffect& operator|=(ThrowEffect other)
    {
        m_mayThrow |= other.m_mayThrow;
        m_mayThrowOtherThanStackOverflow |= other.m_mayThrowOtherThanStackOverflow;
        m_mayThrowByRule |= other.m_mayThrowByRule;
        return *this;
    }

    constexpr ThrowEffect& operator&=(ThrowEffect other)
    {
        m_mayThrow &= other.m_mayThrow;
        m_mayThrowOtherThanStackOverflow &= other.m_mayThrowOtherThanStackOverflow;
        m_mayThrowByRule &= other.m_mayThrowByRule;
        return *this;
    }

    constexpr bool isSubsetOf(ThrowEffect other) const
    {
        return (!m_mayThrow || other.m_mayThrow)
            && (!m_mayThrowOtherThanStackOverflow || other.m_mayThrowOtherThanStackOverflow)
            && (!m_mayThrowByRule || other.m_mayThrowByRule);
    }

    friend constexpr bool operator==(ThrowEffect, ThrowEffect) = default;

private:
    bool m_mayThrow : 1;
    bool m_mayThrowOtherThanStackOverflow : 1;
    bool m_mayThrowByRule : 1;
};

struct Effects {
    LocationMasks reads { LocationMasks::top() };
    NO_UNIQUE_ADDRESS LocationMasks writes { LocationMasks::top() };
    ThrowEffect mayThrow { true };
    bool mayCollect : 1 { true };
    bool mayCallUnknownCode : 1 { true };
    bool maySuspend : 1 { true };
    bool mayBeTerminated : 1 { true };
    bool writesObjectsMadeHereAlone : 1 { false };

    static constexpr Effects none()
    {
        return { .reads = { }, .writes = { }, .mayThrow = false, .mayCollect = false, .mayCallUnknownCode = false, .maySuspend = false, .mayBeTerminated = false };
    }

    static constexpr Effects unknownCode()
    {
        return { .reads = LocationMasks::top(), .writes = LocationMasks::top(), .mayThrow = true, .mayCollect = true, .mayCallUnknownCode = true, .maySuspend = false, .mayBeTerminated = true };
    }

    static constexpr Effects ofReadByNameThatRunsNoCode()
    {
        return { .reads = LocationMasks::top(), .writes = LocationMasks::top().restrictedTo(unobservableWriteHeaps), .mayThrow = ThrowEffect::onlyForLackOfMemory(), .mayCollect = true, .mayCallUnknownCode = false, .maySuspend = false, .mayBeTerminated = false };
    }

    constexpr bool isPure() const { return reads.isEmpty() && writes.isEmpty() && mayThrow == ThrowEffect(false) && !mayCollect && !mayCallUnknownCode && !maySuspend && !mayBeTerminated; }
    constexpr bool isRemovableWhenUnused() const { return writes.containsOnly(unobservableWriteHeaps) && !mayThrowByRule() && !mayBeTerminated && !mayRunOtherCode(); }
    constexpr bool mayBeRunAgainUnseen() const { return writes.containsOnly(unobservableWriteHeaps) && !mayRunOtherCode(); }
    constexpr bool isUnobservable() const { return isRemovableWhenUnused() && !mayThrow; }
    constexpr bool mayThrowByRule() const { return mayThrow.byRule(); }
    constexpr bool mayThrowOtherThanStackOverflow() const { return mayThrow.otherThanStackOverflow(); }
    constexpr bool alwaysCompletes() const { return !mayBeTerminated && !mayRunOtherCode(); }
    constexpr bool mayRunOtherCode() const { return maySuspend || mayCallUnknownCode; }
    constexpr bool mayExitSideways() const { return mayThrow || mayBeTerminated || mayRunOtherCode(); }

    ALWAYS_INLINE constexpr void merge(const Effects& other)
    {
        if (other.writes.containsAny(perObjectHeaps))
            writesObjectsMadeHereAlone = other.writesObjectsMadeHereAlone && (writesObjectsMadeHereAlone || !writes.containsAny(perObjectHeaps));
        reads.merge(other.reads);
        writes.merge(other.writes);
        mayThrow |= other.mayThrow;
        mayCollect |= other.mayCollect;
        mayCallUnknownCode |= other.mayCallUnknownCode;
        maySuspend |= other.maySuspend;
        mayBeTerminated |= other.mayBeTerminated;
    }

    constexpr void intersect(const Effects& other)
    {
        reads.intersect(other.reads);
        writes.intersect(other.writes);
        mayThrow &= other.mayThrow;
        mayCollect &= other.mayCollect;
        mayCallUnknownCode &= other.mayCallUnknownCode;
        maySuspend &= other.maySuspend;
        mayBeTerminated &= other.mayBeTerminated;
        writesObjectsMadeHereAlone &= other.writesObjectsMadeHereAlone;
    }

    constexpr Effects intersectedWith(const Effects& other) const
    {
        Effects result {
            .reads = reads.intersectedWith(other.reads),
            .writes = writes.intersectedWith(other.writes),
            .mayThrow = mayThrow,
            .mayCollect = mayCollect && other.mayCollect,
            .mayCallUnknownCode = mayCallUnknownCode && other.mayCallUnknownCode,
            .maySuspend = maySuspend && other.maySuspend,
            .mayBeTerminated = mayBeTerminated && other.mayBeTerminated,
            .writesObjectsMadeHereAlone = writesObjectsMadeHereAlone && other.writesObjectsMadeHereAlone
        };
        result.mayThrow &= other.mayThrow;
        return result;
    }

    constexpr bool isSubsetOf(const Effects& other) const
    {
        return reads.isSubsetOf(other.reads)
            && writes.isSubsetOf(other.writes)
            && mayThrow.isSubsetOf(other.mayThrow)
            && (!mayCollect || other.mayCollect)
            && (!mayCallUnknownCode || other.mayCallUnknownCode)
            && (!maySuspend || other.maySuspend)
            && (!mayBeTerminated || other.mayBeTerminated)
            && (!other.writesObjectsMadeHereAlone || writesObjectsMadeHereAlone || !writes.containsAny(perObjectHeaps));
    }

    friend constexpr bool operator==(const Effects&, const Effects&) = default;
};

#if CPU(X86_64) && OS(LINUX)
static_assert(sizeof(Effects) == 128);
#endif

class EffectTraits {
public:
    static constexpr unsigned numberOfBits = 9;

    constexpr EffectTraits() = default;

    ALWAYS_INLINE constexpr EffectTraits(const Effects& effects)
        : m_bits((effects.mayThrow ? throws : 0)
            | (effects.mayThrowByRule() ? throwsByRule : 0)
            | (effects.mayCollect ? collects : 0)
            | (effects.mayCallUnknownCode ? callsUnknownCode : 0)
            | (effects.maySuspend ? suspends : 0)
            | (effects.mayBeTerminated ? isTerminated : 0)
            | (effects.writes.containsOnly(unobservableWriteHeaps) ? 0 : writesObservableState)
            | (effects.reads.isEmpty() && effects.writes.isEmpty() ? 0 : touchesMemory)
            | (effects.writes.containsOnly(perObjectHeaps | unobservableWriteHeaps) ? 0 : writesOutsideObjects))
    {
    }

    static constexpr EffectTraits fromBits(uint16_t bits)
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(!(bits >> numberOfBits));
        EffectTraits traits;
        traits.m_bits = bits;
        return traits;
    }

    constexpr uint16_t bits() const { return m_bits; }

    constexpr bool isPure() const { return !m_bits; }
    constexpr bool isRemovableWhenUnused() const { return !(m_bits & (writesObservableState | throwsByRule | isTerminated | suspends | callsUnknownCode)); }
    constexpr bool isUnobservable() const { return isRemovableWhenUnused() && !mayThrow(); }
    constexpr EffectTraits withoutThrowByRule() const { return fromBits(m_bits & ~throwsByRule); }
    constexpr bool mayThrow() const { return m_bits & throws; }
    constexpr bool mayThrowByRule() const { return m_bits & throwsByRule; }
    constexpr bool mayCollect() const { return m_bits & collects; }
    constexpr bool mayCallUnknownCode() const { return m_bits & callsUnknownCode; }
    constexpr bool maySuspend() const { return m_bits & suspends; }
    constexpr bool mayBeTerminated() const { return m_bits & isTerminated; }
    constexpr bool mayRunOtherCode() const { return m_bits & (suspends | callsUnknownCode); }
    constexpr bool alwaysCompletes() const { return !(m_bits & (isTerminated | suspends | callsUnknownCode)); }
    constexpr bool mayExitSideways() const { return m_bits & (throws | isTerminated | suspends | callsUnknownCode); }
    constexpr bool mayWriteObservableState() const { return m_bits & writesObservableState; }
    constexpr bool mayTouchMemory() const { return m_bits & touchesMemory; }
    constexpr bool mayWriteOutsideObjects() const { return m_bits & writesOutsideObjects; }

    constexpr EffectTraits& operator|=(EffectTraits other)
    {
        m_bits |= other.m_bits;
        return *this;
    }

    friend constexpr bool operator==(EffectTraits, EffectTraits) = default;

private:
    static constexpr uint16_t throws = 1 << 0;
    static constexpr uint16_t throwsByRule = 1 << 1;
    static constexpr uint16_t collects = 1 << 2;
    static constexpr uint16_t callsUnknownCode = 1 << 3;
    static constexpr uint16_t suspends = 1 << 4;
    static constexpr uint16_t isTerminated = 1 << 5;
    static constexpr uint16_t writesObservableState = 1 << 6;
    static constexpr uint16_t touchesMemory = 1 << 7;
    static constexpr uint16_t writesOutsideObjects = 1 << 8;
    static_assert(writesOutsideObjects >> (numberOfBits - 1) == 1);

    uint16_t m_bits { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
