/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT)

#include "B3Limits.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "CPU.h"

namespace JSC { namespace B3 {

template<typename Functor>
class CompareChainMatcher {
public:
    CompareChainMatcher(const Procedure& proc, const Functor& canBeInternal)
        : m_proc(proc)
        , m_canBeInternal(canBeInternal)
    {
    }

    bool matches(const Value* condition)
    {
        if (condition->opcode() != BitAnd && condition->opcode() != BitOr)
            return false;
        return classify(condition) == NodeKind::LogicOp && m_logicOpCount <= Limits::maxCompareChainSize;
    }

private:
    enum class NodeKind : uint8_t {
        None,
        Comparison,
        LogicOp,
    };

    static bool isNegation(const Value* value)
    {
        return value->opcode() == Equal && value->child(1)->isInt(0);
    }

    NodeKind classify(const Value* value)
    {
        switch (value->opcode()) {
        case BitAnd:
        case BitOr:
            return classifyLogicOp(value);
        case Equal:
            if (isNegation(value))
                return classifyNegation(value);
            [[fallthrough]];
        case NotEqual:
        case LessThan:
        case GreaterThan:
        case LessEqual:
        case GreaterEqual:
            return value->child(0)->type().isScalar() ? NodeKind::Comparison : NodeKind::None;
        case Above:
        case Below:
        case AboveEqual:
        case BelowEqual:
            return value->child(0)->type().isInt() ? NodeKind::Comparison : NodeKind::None;
        default:
            return NodeKind::None;
        }
    }

    NodeKind classifyOperand(Value* operand)
    {
        return m_canBeInternal(operand) ? classify(operand) : NodeKind::None;
    }

    NodeKind classifyLogicOp(const Value* value)
    {
        if (value->type() != Int32)
            return NodeKind::None;
        if (m_searchSize == m_proc.limit(Limit::MaxCompareChainSearchSize)) {
            m_proc.didReachLimit(Limit::MaxCompareChainSearchSize, value->origin());
            m_didHitSearchLimit = true;
            return NodeKind::None;
        }
        ++m_searchSize;
        ++m_logicOpCount;
        NodeKind left = classifyOperand(value->child(0));
        if (left == NodeKind::None)
            return NodeKind::None;
        NodeKind right = classifyOperand(value->child(1));
        if (right == NodeKind::None || (left == NodeKind::LogicOp && right == NodeKind::LogicOp))
            return NodeKind::None;
        return NodeKind::LogicOp;
    }

    NodeKind classifyNegation(const Value* negation)
    {
        Value* operand = negation->child(0);
        while (isNegation(operand) && m_canBeInternal(operand))
            operand = operand->child(0);

        unsigned logicOpCountBefore = m_logicOpCount;
        NodeKind kind = classifyOperand(operand);
        if (kind != NodeKind::None)
            return kind;
        if (m_didHitSearchLimit)
            return NodeKind::None;
        m_logicOpCount = logicOpCountBefore;
        return NodeKind::Comparison;
    }

    const Procedure& m_proc;
    const Functor& m_canBeInternal;
    unsigned m_logicOpCount { 0 };
    uint64_t m_searchSize { 0 };
    bool m_didHitSearchLimit { false };
};

template<typename Functor>
inline bool isSelectedAsCompareChain(const Procedure& proc, const Value* condition, const Functor& canBeInternal)
{
    if constexpr (!isARM64())
        return false;
    return CompareChainMatcher<Functor>(proc, canBeInternal).matches(condition);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
