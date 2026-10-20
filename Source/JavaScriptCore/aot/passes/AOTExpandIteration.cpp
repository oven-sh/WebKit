/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTExpandIteration.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"

namespace JSC { namespace AOT { namespace Typed {

static bool hasIterationOperation(const Function& function)
{
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            Opcode opcode = function.at(id).opcode;
            if (removedByOf(opcode) == PassID::ExpandIteration || opcode == Opcode::ForInStep)
                return true;
        }
    }
    return false;
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

Vector<Iteration> collectIterations(FunctionPassContext& pass)
{
    const Function& function = pass.function;
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
    if (!hasIterationOperation(pass.function))
        return PassOutcome::Unchanged;

    IRBuilder builder(pass.analyses.context(), pass.function, pass.types, &pass.analyses);
    PassOutcome outcome = builder.emptyUnreachableBlocks() ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= threadForInPositions(pass, builder);
    outcome |= expandArrayIteratorSteps(pass, builder);

    Vector<ValueID> opensOfSpreads;
    outcome |= expandSpreads(pass, builder, opensOfSpreads);

    for (;;) {
        Vector<Iteration> iterations = collectIterations(pass);
        if (iterations.isEmpty())
            break;
        Iteration& last = iterations.last();
        if (opensOfSpreads.contains(last.open))
            last.origin = IterationOrigin::Spread;
        expandOneIteration(pass, builder, last);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
