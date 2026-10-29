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

    bool isEmpty() const { return blocks.isEmpty(); }
    bool contains(BasicBlock* block) const { return blocks.contains(block); }
};

std::expected<HotRegion, GaveUp> findHotRegion(NaturalLoops&, const NaturalLoop&, std::optional<UnswitchedEdge>);
BasicBlock* likelySuccessorOfGuard(NaturalLoops&, const NaturalLoop&, BasicBlock*);
IndexSet<BasicBlock*> findRejoiningBlocks(NaturalLoops&, const HotRegion&, const InvalidationSet&);
Excursion findExcursion(Procedure&, NaturalLoops&, const HotRegion&, const HoistPlan&);

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
