/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3NaturalLoops.h"
#include "B3Opcode.h"
#include "B3Origin.h"
#include "B3Type.h"
#include <expected>
#include <optional>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class Procedure;
struct HoistPlan;
struct InvalidationSet;

struct GaveUp {
    GaveUpReason reason;
    std::optional<Limit> limit;

    GaveUp(GaveUpReason reason)
        : reason(reason)
    {
        ASSERT(reason != GaveUpReason::None && reason != GaveUpReason::LimitReached);
    }

    static GaveUp atLimit(Limit limit) { return GaveUp(limit); }

private:
    explicit GaveUp(Limit limit)
        : reason(GaveUpReason::LimitReached)
        , limit(limit)
    {
    }
};

struct UnswitchedEdge {
    BasicBlock* block { nullptr };
    BasicBlock* successor { nullptr };
};

struct HotRegion {
    const NaturalLoop* loop { nullptr };
    BasicBlock* preHeader { nullptr };
    BasicBlock* header { nullptr };
    Vector<BasicBlock*, 8> blocks;
    IndexSet<BasicBlock*> blockSet;
    Vector<BasicBlock*, 8> slowPathBlocks;

    bool contains(BasicBlock* block) const { return blockSet.contains(block); }
};

struct Excursion {
    Vector<BasicBlock*, 8> blocks;
    // Blocks that the fast loop reaches without taking a rare edge. They are part of the hot code.
    IndexSet<BasicBlock*> unlikelyBlocks;
    // Blocks of the hot region that an unlikely block jumps back to in the fast loop: from there to the end of the iteration a copy would be the same code.
    // A rare path gets its own copy all the same. It costs cold code only, and the likely path keeps what it knows about the values that meet there.
    IndexSet<BasicBlock*> sharedBlocks;

    bool isEmpty() const { return blocks.isEmpty(); }
    bool contains(BasicBlock* block) const { return blocks.contains(block); }
    bool passesThrough(BasicBlock* block) const { return contains(block) || sharedBlocks.contains(block); }
    bool rejoinsFastLoop(BasicBlock* block, const FrequentedBlock& successor) const { return !successor.isRare() && unlikelyBlocks.contains(block) && sharedBlocks.contains(successor.block()); }
};

std::expected<HotRegion, GaveUp> findHotRegion(NaturalLoops&, const NaturalLoop&, std::optional<UnswitchedEdge>);
BasicBlock* likelySuccessorOfGuard(NaturalLoops&, const NaturalLoop&, BasicBlock*);
IndexSet<BasicBlock*> findRejoiningBlocks(NaturalLoops&, const HotRegion&, const InvalidationSet&);
// isReplacedOnlyInFastLoop is asked about values that are not hoisted.
Excursion findExcursion(Procedure&, NaturalLoops&, const HotRegion&, const HoistPlan&, const ScopedLambda<bool(Value*)>& isReplacedOnlyInFastLoop);

class FastPreHeader {
    WTF_MAKE_NONCOPYABLE(FastPreHeader);
public:
    FastPreHeader(Procedure&, BasicBlock* slowLoopHeader);

    Procedure& procedure() const { return m_proc; }
    BasicBlock* first() const { return m_first; }
    BasicBlock* last() const { return m_last; }
    BasicBlock* slowLoopHeader() const { return m_slowLoopHeader; }

    BasicBlock* startNextBlock();
    Value* appendIntConstant(Origin, Type, int64_t);
    Value* append(Origin, Opcode, Value* left, Value* right);
    Value* appendAddConstant(Origin, Value*, int64_t);
    Value* appendWidened(Origin, Value*, Signedness);
    void appendGuard(Origin, Opcode relation, Value* left, Value* right);

private:
    Procedure& m_proc;
    BasicBlock* m_slowLoopHeader;
    BasicBlock* m_first;
    BasicBlock* m_last;
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
