/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMemoryLocation.h"

namespace JSC { namespace AOT { namespace Typed {

class EffectSummaries;
class Program;
struct Function;
struct Terminator;
struct Value;

// Whether an operation may throw, and whether it may throw an error that the language prescribes for its operands.
class ThrowEffect {
public:
    constexpr ThrowEffect(bool mayThrow = true)
        : m_mayThrow(mayThrow)
        , m_mayThrowByRule(mayThrow)
    {
    }

    static constexpr ThrowEffect onlyForLackOfMemory()
    {
        ThrowEffect result(true);
        result.m_mayThrowByRule = false;
        return result;
    }

    constexpr operator bool() const { return m_mayThrow; }
    constexpr bool byRule() const { return m_mayThrowByRule; }

    constexpr ThrowEffect& operator|=(ThrowEffect other)
    {
        m_mayThrow |= other.m_mayThrow;
        m_mayThrowByRule |= other.m_mayThrowByRule;
        return *this;
    }

    constexpr ThrowEffect intersectedWith(ThrowEffect other) const
    {
        ThrowEffect result(m_mayThrow && other.m_mayThrow);
        result.m_mayThrowByRule = m_mayThrowByRule && other.m_mayThrowByRule;
        return result;
    }

    // Whether one thing does no more than another is a question of edges, order and memory. Which kind of error it is decides only whether an unused value goes away.
    constexpr bool isSubsetOf(ThrowEffect other) const { return !m_mayThrow || other.m_mayThrow; }
    constexpr bool saysNoMoreThan(ThrowEffect other) const { return isSubsetOf(other) && (!m_mayThrowByRule || other.m_mayThrowByRule); }

    friend constexpr bool operator==(ThrowEffect, ThrowEffect) = default;

private:
    bool m_mayThrow : 1;
    bool m_mayThrowByRule : 1;
};

struct Effects {
    LocationMasks reads { LocationMasks::top() };
    LocationMasks writes { LocationMasks::top() };
    ThrowEffect mayThrow { true };
    bool mayCollect : 1 { true };
    bool mayCallUnknownCode : 1 { true };
    bool maySuspend : 1 { true };
    bool mayBeTerminated : 1 { true };

    static constexpr Effects none()
    {
        return { .reads = { }, .writes = { }, .mayThrow = false, .mayCollect = false, .mayCallUnknownCode = false, .maySuspend = false, .mayBeTerminated = false };
    }

    static constexpr Effects unknownCode()
    {
        return { .reads = LocationMasks::top(), .writes = LocationMasks::top(), .mayThrow = true, .mayCollect = true, .mayCallUnknownCode = true, .maySuspend = false, .mayBeTerminated = true };
    }

    constexpr bool isPure() const { return *this == none(); }
    // Resolving a rope changes how a string is stored, moving a table how a collection is stored, and neither the result of any operation.
    constexpr bool isRemovableWhenUnused() const { return writes.containsOnly(heapsWhoseWritesNobodySees) && !mayThrowByRule() && !mayBeTerminated && !mayRunOtherCode(); }
    // Nobody can tell where it ran, or how often. What may run out of memory goes away with its unused result, and stays where it stands while it is used.
    constexpr bool isUnobservable() const { return isRemovableWhenUnused() && !mayThrow; }
    constexpr bool mayThrowByRule() const { return mayThrow.byRule(); }
    constexpr bool alwaysCompletes() const { return !mayBeTerminated && !mayRunOtherCode(); }
    constexpr bool mayRunOtherCode() const { return maySuspend || mayCallUnknownCode; }
    constexpr bool mayExitSideways() const { return mayThrow || mayBeTerminated || mayRunOtherCode(); }

    constexpr bool interferesWith(const Effects& other) const
    {
        return orderedWrites().overlaps(other.writes)
            || orderedWrites().overlaps(other.reads)
            || orderedReads().overlaps(other.writes)
            || (mayExitSideways() && other.mayExitSideways())
            || (mayExitSideways() && !other.orderedWrites().isEmpty())
            || (other.mayExitSideways() && !orderedWrites().isEmpty())
            || (mayRunOtherCode() && !other.orderedReads().isEmpty())
            || (other.mayRunOtherCode() && !orderedReads().isEmpty());
    }

    constexpr bool merge(const Effects& other)
    {
        bool changed = reads.merge(other.reads);
        changed |= writes.merge(other.writes);
        changed |= !other.mayThrow.saysNoMoreThan(mayThrow)
            || (other.mayCollect && !mayCollect)
            || (other.mayCallUnknownCode && !mayCallUnknownCode)
            || (other.maySuspend && !maySuspend)
            || (other.mayBeTerminated && !mayBeTerminated);
        mayThrow |= other.mayThrow;
        mayCollect |= other.mayCollect;
        mayCallUnknownCode |= other.mayCallUnknownCode;
        maySuspend |= other.maySuspend;
        mayBeTerminated |= other.mayBeTerminated;
        return changed;
    }

    constexpr Effects intersectedWith(const Effects& other) const
    {
        return {
            .reads = reads.intersectedWith(other.reads),
            .writes = writes.intersectedWith(other.writes),
            .mayThrow = mayThrow.intersectedWith(other.mayThrow),
            .mayCollect = mayCollect && other.mayCollect,
            .mayCallUnknownCode = mayCallUnknownCode && other.mayCallUnknownCode,
            .maySuspend = maySuspend && other.maySuspend,
            .mayBeTerminated = mayBeTerminated && other.mayBeTerminated,
        };
    }

    constexpr bool isSubsetOf(const Effects& other) const
    {
        return reads.isSubsetOf(other.reads)
            && writes.isSubsetOf(other.writes)
            && mayThrow.isSubsetOf(other.mayThrow)
            && (!mayCollect || other.mayCollect)
            && (!mayCallUnknownCode || other.mayCallUnknownCode)
            && (!maySuspend || other.maySuspend)
            && (!mayBeTerminated || other.mayBeTerminated);
    }

    constexpr Effects withoutSuspension() const
    {
        Effects result = *this;
        result.maySuspend = false;
        return result;
    }

    void dump(PrintStream&) const;

    friend constexpr bool operator==(const Effects&, const Effects&) = default;

private:
    constexpr LocationMasks orderedReads() const { return reads.restrictedTo(orderedHeaps); }
    constexpr LocationMasks orderedWrites() const { return writes.restrictedTo(orderedHeaps); }
};

struct EffectsOnEdges {
    Effects normal;
    Effects handler;

    static constexpr EffectsOnEdges none() { return { Effects::none(), Effects::none() }; }
    static constexpr EffectsOnEdges onBothEdges(const Effects& effects) { return { effects, effects.mayThrow ? effects : Effects::none() }; }
    static constexpr EffectsOnEdges unknownCode() { return onBothEdges(Effects::unknownCode()); }

    constexpr bool mayThrow() const { return normal.mayThrow; }

    constexpr Effects onAnyEdge() const
    {
        Effects result = normal;
        result.merge(handler);
        return result;
    }

    constexpr bool merge(const EffectsOnEdges& other)
    {
        bool changed = normal.merge(other.normal);
        changed |= handler.merge(other.handler);
        return changed;
    }

    constexpr EffectsOnEdges intersectedWith(const EffectsOnEdges& other) const
    {
        return { normal.intersectedWith(other.normal), handler.intersectedWith(other.handler) };
    }

    constexpr bool isSubsetOf(const EffectsOnEdges& other) const { return normal.isSubsetOf(other.normal) && handler.isSubsetOf(other.handler); }

    void dump(PrintStream&) const;

    friend constexpr bool operator==(const EffectsOnEdges&, const EffectsOnEdges&) = default;
};

class EffectTraits {
public:
    constexpr EffectTraits() = default;

    constexpr EffectTraits(const Effects& effects)
        : m_bits((effects.mayThrow ? throws : 0)
            | (effects.mayThrowByRule() ? throwsByRule : 0)
            | (effects.mayCollect ? collects : 0)
            | (effects.mayCallUnknownCode ? callsUnknownCode : 0)
            | (effects.maySuspend ? suspends : 0)
            | (effects.mayBeTerminated ? isTerminated : 0)
            | (effects.writes.containsOnly(heapsWhoseWritesNobodySees) ? 0 : writesWhatIsSeen)
            | (effects.reads.isEmpty() && effects.writes.isEmpty() ? 0 : touchesMemory))
    {
    }

    static constexpr EffectTraits all()
    {
        EffectTraits result;
        result.m_bits = 0xff;
        return result;
    }

    constexpr bool isPure() const { return !m_bits; }
    constexpr bool isRemovableWhenUnused() const { return !(m_bits & (writesWhatIsSeen | throwsByRule | isTerminated | suspends | callsUnknownCode)); }
    constexpr bool isUnobservable() const { return isRemovableWhenUnused() && !mayThrow(); }
    constexpr bool mayThrow() const { return m_bits & throws; }
    constexpr bool mayThrowByRule() const { return m_bits & throwsByRule; }
    constexpr bool mayCollect() const { return m_bits & collects; }
    constexpr bool mayCallUnknownCode() const { return m_bits & callsUnknownCode; }
    constexpr bool maySuspend() const { return m_bits & suspends; }
    constexpr bool mayBeTerminated() const { return m_bits & isTerminated; }
    constexpr bool mayRunOtherCode() const { return m_bits & (suspends | callsUnknownCode); }
    constexpr bool alwaysCompletes() const { return !(m_bits & (isTerminated | suspends | callsUnknownCode)); }
    constexpr bool mayExitSideways() const { return m_bits & (throws | isTerminated | suspends | callsUnknownCode); }
    constexpr bool mayWriteWhatIsSeen() const { return m_bits & writesWhatIsSeen; }
    constexpr bool mayTouchMemory() const { return m_bits & touchesMemory; }

    constexpr bool containsAll(EffectTraits other) const { return !(other.m_bits & ~m_bits); }

    constexpr EffectTraits& operator|=(EffectTraits other)
    {
        m_bits |= other.m_bits;
        return *this;
    }

    friend constexpr bool operator==(EffectTraits, EffectTraits) = default;

private:
    static constexpr uint8_t throws = 1 << 0;
    static constexpr uint8_t throwsByRule = 1 << 1;
    static constexpr uint8_t collects = 1 << 2;
    static constexpr uint8_t callsUnknownCode = 1 << 3;
    static constexpr uint8_t suspends = 1 << 4;
    static constexpr uint8_t isTerminated = 1 << 5;
    static constexpr uint8_t writesWhatIsSeen = 1 << 6;
    static constexpr uint8_t touchesMemory = 1 << 7;

    uint8_t m_bits { 0 };
};

struct EffectTraitsOnEdges {
    EffectTraits normal;
    EffectTraits anyEdge;

    constexpr EffectTraitsOnEdges() = default;

    constexpr EffectTraitsOnEdges(const EffectsOnEdges& effects)
        : normal(effects.normal)
        , anyEdge(effects.normal)
    {
        anyEdge |= EffectTraits(effects.handler);
    }
};

struct EffectTraitBounds {
    EffectTraits atLeast;
    EffectTraits atMost { EffectTraits::all() };
};

struct EffectContext {
    const Program& program;
    const EffectSummaries& summaries;
};

EffectsOnEdges effectsOf(const EffectContext&, const Function&, const Value&);
EffectsOnEdges effectsOf(const EffectContext&, const Function&, const Terminator&);
EffectsOnEdges effectsBeyondOperands(const EffectContext&, const Function&, const Value&, const EffectsOnEdges& effectsOfValue);
LocationOverlap overlapOfWrites(const Program&, const Function&, const Value& writer, const Effects& effectsOfWriter, const MemoryLocation&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
