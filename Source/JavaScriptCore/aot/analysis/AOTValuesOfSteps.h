/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDominators.h"
#include "AOTUseLists.h"

namespace JSC { namespace AOT { namespace Typed {

inline bool readsResultOfStep(const Function& function, ValueID id, ValueID step, IteratorResumeResult result)
{
    const Value& value = function.at(id);
    return value.opcode == Opcode::ResultAt && value.resultsOperand() == step && value.index() == static_cast<uint32_t>(result);
}

// `for (const each of list) use(each)`: the step that finds the end answers undefined as its value, and the loop uses the value only where the step has not found the end.
// The block that the loop enters there, if the value of the step goes nowhere else. Where the value is taken out of the two results says nothing: that may stand in front of the branch.
inline BlockID blockThatAloneUsesValueOfStep(const Function& function, const CFG& cfg, const UseLists& uses, const Dominators& dominators, ValueID step)
{
    const Value& operation = function.at(step);
    if (operation.opcode != Opcode::IteratorResume || operation.readsValueWhenDone() || operation.stepIndex())
        return { };
    const Terminator& terminator = function.at(uses.blockOf(step)).terminator;
    if (terminator.kind != TerminatorKind::Branch || !readsResultOfStep(function, terminator.operand(0), step, IteratorResumeResult::Done))
        return { };
    BlockID behind = terminator.notTaken().block;
    if (behind == terminator.taken().block || cfg.incomingEdges(behind).size() != 1)
        return { };
    auto isBehind = [&](const Use& use) {
        return dominators.dominates(behind, use.block);
    };
    bool goesElsewhere = std::ranges::any_of(uses.usesOf(step), [&](const Use& use) {
        return use.kind != Use::Kind::Operand || (readsResultOfStep(function, use.user, step, IteratorResumeResult::Value) && !std::ranges::all_of(uses.usesOf(use.user), isBehind));
    });
    return goesElsewhere ? BlockID() : behind;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
