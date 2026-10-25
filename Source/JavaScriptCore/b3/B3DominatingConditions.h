/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3BasicBlock.h"
#include "B3IntRange.h"
#include "B3Limits.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include <wtf/IndexMap.h>
#include <wtf/IterationStatus.h>
#include <wtf/Noncopyable.h>
#include <wtf/TriState.h>
#include <wtf/Vector.h>

namespace JSC { namespace B3 {

struct DominatingCondition {
    Value* value { nullptr };
    bool isNonZero { false };
};

template<typename Resolve>
std::optional<DominatingCondition> stripNegations(Value* condition, const Resolve& resolve)
{
    bool isNonZero = true;
    Value* value = condition;
    for (;;) {
        value = resolve(value);
        if (!value)
            return std::nullopt;
        if ((value->opcode() == Equal || value->opcode() == NotEqual) && value->child(1)->isInt(0)) {
            isNonZero ^= value->opcode() == Equal;
            value = value->child(0);
            continue;
        }
        if (value->opcode() == BitXor && value->child(1)->isInt(1) && value->child(0)->returnsBool()) {
            isNonZero ^= true;
            value = value->child(0);
            continue;
        }
        return DominatingCondition { value, isNonZero };
    }
}

inline DominatingCondition stripNegations(Value* condition)
{
    return *stripNegations(condition, [](Value* value) { return value; });
}

JS_EXPORT_PRIVATE std::optional<DominatingCondition> conditionAtHead(BasicBlock*);

class DominatingConditionStack {
    WTF_MAKE_NONCOPYABLE(DominatingConditionStack);
public:
    DominatingConditionStack() = default;

    JS_EXPORT_PRIVATE void reset(unsigned numberOfValues);
    void pushLayer() { m_layerStarts.append(m_added.size()); }
    JS_EXPORT_PRIVATE void popLayer();
    JS_EXPORT_PRIVATE bool add(Value*, bool isNonZero);

    TriState isNonZero(Value* value) const
    {
        unsigned index = value->index();
        unsigned knowledge = index < m_knowledge.size() ? m_knowledge[index] : 0;
        if (knowledge & IsZero)
            return TriState::False;
        if (knowledge & IsNonZero)
            return TriState::True;
        return TriState::Indeterminate;
    }

private:
    enum Knowledge : uint8_t {
        IsZero = 1,
        IsNonZero = 2,
    };

    Vector<uint8_t, 1024> m_knowledge;
    Vector<unsigned, 64> m_added;
    Vector<unsigned, 64> m_layerStarts;
};

class DominatingConditions {
    WTF_MAKE_NONCOPYABLE(DominatingConditions);
public:
    using Chain = std::optional<unsigned>;

    JS_EXPORT_PRIVATE explicit DominatingConditions(Procedure&);

    Chain chainAt(BasicBlock* block) const { return m_chainAtBlock[block]; }
    JS_EXPORT_PRIVATE Chain extend(Chain, DominatingCondition);

    template<typename Functor>
    void forEach(Chain chain, const Functor& functor) const
    {
        uint64_t maxQueries = m_proc.limit(Limit::MaxConditionQueriesPerBlock);
        uint64_t queryCount = 0;
        while (chain) {
            if (queryCount++ == maxQueries) {
                m_proc.didReachLimit(Limit::MaxConditionQueriesPerBlock, m_nodes[*chain].condition.value->origin());
                return;
            }
            const Node& node = m_nodes[*chain];
            chain = node.outer;
            if (functor(node.condition) == IterationStatus::Done)
                return;
        }
    }

private:
    struct Node {
        DominatingCondition condition;
        Chain outer;
    };

    Procedure& m_proc;
    Vector<Node> m_nodes;
    IndexMap<BasicBlock*, Chain> m_chainAtBlock;
};

JS_EXPORT_PRIVATE bool isComparisonWithConstant(const Value*);
JS_EXPORT_PRIVATE IntRange rangeImpliedBy(const Value* comparison, bool holds);
JS_EXPORT_PRIVATE TriState evaluateImpliedComparison(const Value* comparison, const Value* known, bool knownHolds);
JS_EXPORT_PRIVATE bool isRelationImpliedAt(Procedure&, BasicBlock*, Opcode relation, Value* left, Value* right);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
