/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(std::ranges::all_of(allMembersOf<Opcode>, [](Opcode opcode) {
    return removedByOf(opcode) != PassID::ExpandIteration || expandedOpcodes.contains(opcode);
}));

// A step of a for..in stays in the function for good: once it has its position there is nothing left to do for it. A run that finds no work computes no analysis but the set of opcodes, which the next pass asks for too.
static OptionSet<ExpansionStep> findStepsWithWork(FunctionPassContext& pass)
{
    OptionSet<ExpansionStep> steps;
    if (!pass.analyses.opcodesOfValues().intersects(expandedOpcodes))
        return steps;

    const Function& function = pass.function;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::ForInStep && value.positionOperand())
                continue;
            steps.add(stepThatExpands(value.opcode));
        }
    }
    return steps;
}

static bool hasOpen(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return block.values.containsIf([&](ValueID id) {
            return function.at(id).opcode == Opcode::IteratorOpen;
        });
    });
}

static Iteration createIteration(const Function& function, const UseLists& useLists, ValueID open)
{
    Iteration iteration;
    iteration.open = open;
    iteration.kind = function.at(open).iteratorKind();
    for (const Use& use : useLists.usesOf(open)) {
        switch (use.kind) {
        case Use::Kind::Operand:
            if (function.at(use.user).opcode == Opcode::IteratorResume) {
                RELEASE_ASSERT(use.operandIndex == OperandIndex::IteratorResume::iterator);
                iteration.resumes.append(use.user);
                continue;
            }
            RELEASE_ASSERT(function.at(use.user).opcode == Opcode::IteratorClose && use.operandIndex == OperandIndex::IteratorClose::iterator);
            iteration.closes.append(use.user);
            continue;
        case Use::Kind::BlockSubject:
            RELEASE_ASSERT(function.at(use.block).role == BlockRole::IterationCleanup);
            iteration.cleanupBlocks.append(use.block);
            continue;
        case Use::Kind::TerminatorOperand:
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    return iteration;
}

// The caller asks until nothing is left, so its last call comes right after the function has changed. That call answers without computing any analysis.
Vector<Iteration> collectIterations(FunctionPassContext& pass)
{
    const Function& function = pass.function;
    if (!hasOpen(function))
        return { };

    const CFG& cfg = pass.analyses.cfg();
    const UseLists& useLists = pass.analyses.useLists();

    Vector<Iteration> iterations;
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            if (function.at(id).opcode == Opcode::IteratorOpen)
                iterations.append(createIteration(function, useLists, id));
        }
    }
    return iterations;
}

PassOutcome expandIteration(FunctionPassContext& pass)
{
    OptionSet<ExpansionStep> steps = findStepsWithWork(pass);
    if (steps.isEmpty())
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = builder.emptyUnreachableBlocks() ? PassOutcome::Changed : PassOutcome::Unchanged;
    if (steps.contains(ExpansionStep::ForInPositions))
        outcome |= threadForInPositions(pass, builder);
    if (steps.contains(ExpansionStep::ArrayIteratorSteps))
        outcome |= expandArrayIteratorSteps(pass, builder);

    Vector<ValueID> opensOfSpreads;
    if (steps.contains(ExpansionStep::Spreads))
        outcome |= expandSpreads(pass, builder, opensOfSpreads);
    if (steps.contains(ExpansionStep::Iterations) || !opensOfSpreads.isEmpty())
        outcome |= expandIterations(pass, builder, opensOfSpreads.span(), ExpandedIterations::All);
    return outcome;
}

// Until an iteration is expanded its steps are unknown code, to the function and, through its effect summary, to every caller.
PassOutcome expandProvenIterations(FunctionPassContext& pass)
{
    if (!findStepsWithWork(pass).contains(ExpansionStep::Iterations))
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = builder.emptyUnreachableBlocks() ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= expandIterations(pass, builder, { }, ExpandedIterations::OnlyProven);
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
