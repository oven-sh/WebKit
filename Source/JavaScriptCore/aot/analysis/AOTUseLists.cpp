/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTUseLists.h"

#if ENABLE(AOT)

#include <algorithm>
#include <tuple>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

ValueID usedValueOf(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return function.at(use.user).operand(use.operandIndex);
    case Use::Kind::TerminatorOperand:
        return function.at(use.block).terminator.operand(use.operandIndex);
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return targetOf(function, use.edge()).arguments[use.operandIndex];
    case Use::Kind::BlockSubject:
        return function.at(use.block).subject;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t rankOf(Use::Kind kind)
{
    switch (kind) {
    case Use::Kind::BlockSubject:
        return 0;
    case Use::Kind::HandlerArgument:
        return 1;
    case Use::Kind::Operand:
        return 2;
    case Use::Kind::TerminatorOperand:
        return 3;
    case Use::Kind::EdgeArgument:
        return 4;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Functor>
void UseLists::forEachUse(NOESCAPE const Functor& functor) const
{
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        auto& block = m_function.at(blockID);
        auto visitArguments = [&](Use::Kind kind, const Target& target, uint32_t targetIndex) {
            auto& arguments = target.arguments;
            for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
                if (ValueID argument = arguments[argumentIndex])
                    functor(argument, Use { kind, blockID, { }, argumentIndex, targetIndex });
            }
        };

        if (block.subject)
            functor(block.subject, Use { Use::Kind::BlockSubject, blockID, { }, 0, 0 });
        if (block.handler)
            visitArguments(Use::Kind::HandlerArgument, *block.handler, 0);
        for (ValueID user : block.values) {
            m_function.at(user).forEachOperand([&](ValueID used, uint32_t operandIndex) {
                functor(used, Use { Use::Kind::Operand, blockID, user, operandIndex, 0 });
            });
        }
        block.terminator.forEachOperand([&](ValueID used, uint32_t operandIndex) {
            functor(used, Use { Use::Kind::TerminatorOperand, blockID, { }, operandIndex, 0 });
        });
        auto& targets = block.terminator.targets;
        for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex)
            visitArguments(Use::Kind::EdgeArgument, targets[targetIndex], targetIndex);
    }
}

UseLists::UseLists(const Function& function, const CFG& cfg)
    : m_function(function)
    , m_cfg(cfg)
    , m_locations(function.values.size())
{
    auto setLocation = [&](ValueID value, ValueLocation location) {
        auto& entry = m_locations[value.index()];
        RELEASE_ASSERT(entry.placement == ValuePlacement::Detached);
        entry = location;
    };
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        auto& block = m_function.at(blockID);
        for (uint32_t index = 0; index < block.parameters.size(); ++index)
            setLocation(block.parameters[index], { ValuePlacement::Parameter, blockID, index });
        for (uint32_t index = 0; index < block.values.size(); ++index)
            setLocation(block.values[index], { ValuePlacement::InBlock, blockID, index });
    }

    unsigned count = m_function.values.size();
    m_firstUse.fill(0, count + 1);
    forEachUse([&](ValueID used, const Use&) {
        ++m_firstUse[used.index()];
    });
    uint32_t next = 0;
    for (auto& first : m_firstUse)
        first = std::exchange(next, next + first);
    m_uses.resize(next);
    forEachUse([&](ValueID used, const Use& use) {
        m_uses[m_firstUse[used.index()]++] = use;
    });
    for (unsigned i = count; i; --i)
        m_firstUse[i] = m_firstUse[i - 1];
    m_firstUse[0] = 0;
}

ProgramPoint UseLists::pointOf(const Use& use) const
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return { use.block, locationOf(use.user).index };
    case Use::Kind::TerminatorOperand:
    case Use::Kind::EdgeArgument:
        return pointBeforeTerminatorOf(m_function, use.block);
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return ProgramPoint::atHeadOf(use.block);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProgramPoint UseLists::pointOf(ValueID valueInBlock) const
{
    ValueLocation location = locationOf(valueInBlock);
    RELEASE_ASSERT(location.placement == ValuePlacement::InBlock);
    return { location.block, location.index };
}

ProgramPoint UseLists::pointAfter(ValueID id) const
{
    ValueLocation location = locationOf(id);
    switch (location.placement) {
    case ValuePlacement::Parameter:
        return { location.block, indexAfterCaughtValues(m_function, m_function.at(location.block)) };
    case ValuePlacement::InBlock:
        return { location.block, std::max(location.index + 1, indexAfterCaughtValues(m_function, m_function.at(location.block))) };
    case ValuePlacement::Detached:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool UseLists::isBefore(const Use& a, const Use& b) const
{
    auto keyOf = [&](const Use& use) {
        return std::tuple { m_cfg.reversePostOrderIndex(use.block), pointOf(use).index, rankOf(use.kind), use.targetIndex, use.operandIndex };
    };
    return keyOf(a) < keyOf(b);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
