/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEscapeAnalysis.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionAnalyses;

enum class Occurrence : uint8_t { Unknown, MayHaveOccurred, HasNotOccurred };

// The one entry parameter that nobody else has seen when the function begins. To every other object that comes in from outside everything has happened at every point.
std::optional<ValueID> findFreshEntryParameter(const Function&, const CFG&);

struct UseEffectHistoryOptions {
    OperandUses uses;
    const BitVector* ignoredBlocks { nullptr };
};

class UseEffectHistory {
    WTF_MAKE_NONCOPYABLE(UseEffectHistory);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(UseEffectHistory);
public:
    explicit UseEffectHistory(FunctionAnalyses&, const UseEffectHistoryOptions& = { });

    Occurrence occurredBefore(ObjectID, ProgramPoint) const;
    bool gaveUp() const { return m_gaveUp; }

private:
    enum class ActionKind : uint8_t { Event, Reset };

    struct Action {
        uint32_t index;
        ObjectID object;
        ActionKind kind;
    };

public:
    // occurredBefore() for many objects at one point.
    class AtPoint {
    public:
        AtPoint(const UseEffectHistory&, ProgramPoint);

        Occurrence occurrenceOf(ObjectID) const;

    private:
        const UseEffectHistory& m_history;
        bool m_isUnknown { false };
        uint32_t m_index { 0 };
        std::span<const uint64_t> m_head;
        std::span<const Action> m_actions;
    };

private:

    void computeSkippedBlocks(const BitVector* ignoredBlocks);
    void findUnknownObjects(const EscapeAnalysis&, OperandUses);
    void collectActions(FunctionAnalyses&, const EscapeAnalysis&, OperandUses);
    enum class StartsAt : bool { BlocksWhereSomethingHappens, EveryBlock };
    void computeHeads(StartsAt);
    void computeTail(BlockID, std::span<uint64_t> tail) const;
    void computeStateAtHandler(BlockID, std::span<uint64_t> state) const;

    bool isSkipped(BlockID block) const { return m_isSkipped.get(block.index()); }
    std::span<const Action> actionsOf(BlockID) const;
    std::span<const Action> actionsOf(BlockID, ObjectID) const;
    std::span<const uint64_t> headOf(BlockID) const;
    std::span<uint64_t> mutableHeadOf(BlockID);

    const Function& m_function;
    const CFG& m_cfg;
    bool m_gaveUp { false };
    unsigned m_numberOfObjects { 0 };
    unsigned m_wordsPerBlock { 0 };
    BitVector m_isUnknown;
    BitVector m_isSkipped;
    bool m_skipsReachableBlocks { false };
    Vector<uint64_t, 2> m_objectsFromOutside;
    Vector<Action> m_actions;
    Vector<uint32_t> m_firstAction;
    Vector<uint64_t> m_head;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
