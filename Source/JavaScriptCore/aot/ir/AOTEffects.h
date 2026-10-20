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

struct Effects {
    LocationMasks reads { LocationMasks::top() };
    LocationMasks writes { LocationMasks::top() };
    bool mayThrow : 1 { true };
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
    // Resolving a rope changes how a string is stored, and the result of no operation.
    constexpr bool isRemovableWhenUnused() const { return writes.containsOnly(bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage }) && !mayExitSideways(); }
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
        Effects previous = *this;
        reads.merge(other.reads);
        writes.merge(other.writes);
        mayThrow |= other.mayThrow;
        mayCollect |= other.mayCollect;
        mayCallUnknownCode |= other.mayCallUnknownCode;
        maySuspend |= other.maySuspend;
        mayBeTerminated |= other.mayBeTerminated;
        return *this != previous;
    }

    constexpr Effects intersectedWith(const Effects& other) const
    {
        return {
            .reads = reads.intersectedWith(other.reads),
            .writes = writes.intersectedWith(other.writes),
            .mayThrow = mayThrow && other.mayThrow,
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
            && (!mayThrow || other.mayThrow)
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
