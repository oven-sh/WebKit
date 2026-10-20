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

    void computeSkippedBlocks(const BitVector* ignoredBlocks);
    void findUnknownObjects(const EscapeAnalysis&, OperandUses);
    void collectActions(FunctionAnalyses&, const EscapeAnalysis&, OperandUses);
    void computeHeads();
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
    Vector<Action> m_actions;
    Vector<uint32_t> m_firstAction;
    Vector<uint64_t> m_head;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
