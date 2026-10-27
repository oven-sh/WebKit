/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTVerifierInternal.h"

#if ENABLE(AOT)

#include "AOTStronglyConnectedComponents.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool hasLoopMark(const Function& function, BlockID block)
{
    return std::ranges::any_of(function.at(block).values, [&](ValueID id) {
        return function.at(id).opcode == Opcode::LoopHint;
    });
}

static bool hasResult(const Function& function, ValueID definition)
{
    return resultRuleOf(function.at(definition).opcode) != ResultRule::None;
}

static bool useRequiresDefinition(const Function& function, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand:
        return mustBeDefinition(function.at(use.user).operandKind(use.operandIndex));
    case Use::Kind::TerminatorOperand:
        return mustBeDefinition(function.at(use.block).terminator.operandKind(use.operandIndex));
    case Use::Kind::BlockSubject:
        return function.at(use.block).role == BlockRole::IterationCleanup;
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FunctionVerifier::verifyDominance()
{
    for (uint32_t index = 0; index < m_function.values.size(); ++index) {
        verifyUsesOf(ValueID(index));
        if (hasFailed())
            return;
    }
    for (BlockID block : analyses().cfg().reversePostOrder()) {
        verifyHandlerArguments(block, m_function.at(block));
        if (hasFailed())
            return;
    }
    verifyLoopMarks();
}

void FunctionVerifier::verifyUsesOf(ValueID definition)
{
    const UseLists& useLists = analyses().useLists();
    std::span<const Use> uses = useLists.usesOf(definition);
    if (uses.empty())
        return;

    const CFG& cfg = analyses().cfg();
    ValueLocation location = useLists.locationOf(definition);
    auto dominates = [&](ProgramPoint point) {
        if (!cfg.isReachable(point.block))
            return false;
        switch (location.placement) {
        case ValuePlacement::Detached:
            return false;
        case ValuePlacement::Parameter:
            return location.block == point.block || analyses().dominators().dominates(location.block, point.block);
        case ValuePlacement::InBlock:
            if (location.block == point.block)
                return location.index < point.index;
            return analyses().dominators().terminatorDominates(location.block, point.block);
        }
        RELEASE_ASSERT_NOT_REACHED();
    };

    bool isParameter = m_function.at(definition).opcode == Opcode::Parameter;
    bool definitionHasResult = hasResult(m_function, definition);
    for (const Use& use : uses) {
        if (use.kind == Use::Kind::HandlerArgument)
            continue;

        ValueID reportedValue = use.kind == Use::Kind::Operand ? use.user : definition;
        if (!dominates(useLists.pointOf(use))) {
            fail(InternalErrorKind::UseNotDominated, use.block, reportedValue, definition.index());
            return;
        }
        if (!definitionHasResult) {
            fail(InternalErrorKind::OperandTypeMismatch, use.block, reportedValue, definition.index());
            return;
        }
        if (isParameter && useRequiresDefinition(m_function, use)) {
            fail(InternalErrorKind::OperandMustBeDefinition, use.block, reportedValue, definition.index());
            return;
        }
    }
}

void FunctionVerifier::verifyHandlerArguments(BlockID block, const Block& contents)
{
    if (!contents.handler)
        return;

    const Arguments& arguments = contents.handler->arguments;
    for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
        ValueID argument = arguments[argumentIndex];
        if (!analyses().dominates(argument, Use { Use::Kind::HandlerArgument, block, { }, argumentIndex, 0 })) {
            fail(InternalErrorKind::HandlerArgumentNotAvailable, block, argument, argumentIndex);
            return;
        }
        if (!hasResult(m_function, argument)) {
            fail(InternalErrorKind::OperandTypeMismatch, block, argument, argument.index());
            return;
        }
    }
}

void FunctionVerifier::verifyLoopMarks()
{
    const CFG& cfg = analyses().cfg();
    if (!cfg.hasCycle())
        return;
    const BlockCycles& cycles = analyses().blockCycles();
    auto isInCycle = [&](BlockID block) {
        return cycles.isInCycle(block);
    };
    if (std::ranges::none_of(cfg.reversePostOrder(), isInCycle))
        return;

    BitVector markedBlocks(cfg.numNodes());
    for (BlockID block : cfg.reversePostOrder()) {
        if (isInCycle(block) && hasLoopMark(m_function, block))
            markedBlocks.quickSet(block.index());
    }

    StronglyConnectedComponents components = computeStronglyConnectedComponents(cfg.numNodes(), [&](unsigned blockIndex) -> std::span<const BlockID> {
        if (markedBlocks.quickGet(blockIndex))
            return { };
        return cfg.successors(BlockID(blockIndex));
    });
    if (!components.isInCycle.isEmpty())
        fail(InternalErrorKind::CycleWithoutLoopMark, BlockID(components.isInCycle.findBit(0, true)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
