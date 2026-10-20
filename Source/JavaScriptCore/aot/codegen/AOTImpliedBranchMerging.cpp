/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImpliedBranchMerging.h"

#if ENABLE(AOT)

#include "B3BasicBlockInlines.h"
#include "B3DominatingConditions.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include <optional>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

namespace {

// The block is left, for a block that is seldom entered, where `subject relation constant` is what leavesWhenItHolds says.
struct LeavingBranch {
    B3::Value* subject;
    B3::Opcode relation;
    int64_t constant;
    bool leavesWhenItHolds;
    unsigned leavingSuccessor;
};

} // anonymous namespace

static bool isRelation(B3::Opcode opcode)
{
    switch (opcode) {
    case B3::Equal:
    case B3::NotEqual:
    case B3::LessThan:
    case B3::GreaterThan:
    case B3::LessEqual:
    case B3::GreaterEqual:
    case B3::Below:
    case B3::Above:
    case B3::BelowEqual:
    case B3::AboveEqual:
        return true;
    default:
        return false;
    }
}

static std::optional<LeavingBranch> findLeavingBranch(B3::BasicBlock* block)
{
    B3::Value* branch = block->last();
    if (branch->opcode() != B3::Branch)
        return std::nullopt;
    bool takenIsLikely = block->taken().frequency() == B3::FrequencyClass::Normal;
    bool notTakenIsLikely = block->notTaken().frequency() == B3::FrequencyClass::Normal;
    if (takenIsLikely == notTakenIsLikely || block->taken().block() == block->notTaken().block())
        return std::nullopt;

    // The branch is taken where the condition is not zero.
    B3::DominatingCondition condition = B3::stripNegations(branch->child(0));
    bool leavesWhenTaken = !takenIsLikely;
    bool leavesWhenNonZero = leavesWhenTaken == condition.isNonZero;
    unsigned leavingSuccessor = leavesWhenTaken ? 0 : 1;

    B3::Value* value = condition.value;
    if (isRelation(value->opcode()) && value->child(0)->type().isInt() && value->child(1)->hasInt())
        return LeavingBranch { value->child(0), value->opcode(), value->child(1)->asInt(), leavesWhenNonZero, leavingSuccessor };
    if (!value->type().isInt())
        return std::nullopt;
    return LeavingBranch { value, B3::NotEqual, 0, leavesWhenNonZero, leavingSuccessor };
}

static std::optional<int64_t> findOnlyLeavingValue(const LeavingBranch& branch)
{
    if (branch.relation != (branch.leavesWhenItHolds ? B3::Equal : B3::NotEqual))
        return std::nullopt;
    return branch.constant;
}

static bool leavesFor(const LeavingBranch& branch, int64_t left)
{
    int64_t right = branch.constant;
    bool is32Bit = branch.subject->type() == B3::Int32;
    uint64_t unsignedLeft = is32Bit ? static_cast<uint32_t>(left) : static_cast<uint64_t>(left);
    uint64_t unsignedRight = is32Bit ? static_cast<uint32_t>(right) : static_cast<uint64_t>(right);
    auto holds = [&] {
        switch (branch.relation) {
        case B3::Equal:
            return left == right;
        case B3::NotEqual:
            return left != right;
        case B3::LessThan:
            return left < right;
        case B3::GreaterThan:
            return left > right;
        case B3::LessEqual:
            return left <= right;
        case B3::GreaterEqual:
            return left >= right;
        case B3::Below:
            return unsignedLeft < unsignedRight;
        case B3::Above:
            return unsignedLeft > unsignedRight;
        case B3::BelowEqual:
            return unsignedLeft <= unsignedRight;
        case B3::AboveEqual:
            return unsignedLeft >= unsignedRight;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    return holds() == branch.leavesWhenItHolds;
}

// What stands in front of the second branch now also runs for the value that used to leave before it.
static bool mayRunForAnyValue(B3::BasicBlock* block)
{
    for (B3::Value* value : *block) {
        if (value == block->last())
            continue;
        B3::Effects effects = value->effects();
        if (value->opcode() == B3::Upsilon || value->opcode() == B3::Phi || effects.mustExecute() || effects.controlDependent || effects.reads || effects.readsPinned || effects.readsLocalState)
            return false;
    }
    return true;
}

static bool tryMerge(B3::Procedure& proc, B3::BasicBlock* first)
{
    std::optional<LeavingBranch> firstBranch = findLeavingBranch(first);
    if (!firstBranch)
        return false;
    std::optional<int64_t> leavingValue = findOnlyLeavingValue(*firstBranch);
    if (!leavingValue)
        return false;

    B3::BasicBlock* second = first->successorBlock(1 - firstBranch->leavingSuccessor);
    if (second == first || second->numPredecessors() != 1)
        return false;
    std::optional<LeavingBranch> secondBranch = findLeavingBranch(second);
    if (!secondBranch || secondBranch->subject != firstBranch->subject || !leavesFor(*secondBranch, *leavingValue) || !mayRunForAnyValue(second))
        return false;

    B3::FrequentedBlock firstTarget = first->successor(firstBranch->leavingSuccessor);
    B3::FrequentedBlock secondTarget = second->successor(secondBranch->leavingSuccessor);
    if (firstTarget.block() == second || secondTarget.block() == second || secondTarget.block() == first)
        return false;

    B3::BasicBlock* dispatch = proc.addBlock(std::min(firstTarget.block()->frequency() + secondTarget.block()->frequency(), second->frequency()));
    B3::Value* branch = first->last();
    first->values().removeLast();
    dispatch->append(branch);
    branch->owner = dispatch;
    if (firstBranch->leavingSuccessor)
        dispatch->setSuccessors(secondTarget, firstTarget);
    else
        dispatch->setSuccessors(firstTarget, secondTarget);

    first->appendNew<B3::Value>(proc, B3::Jump, branch->origin());
    first->setSuccessors(B3::FrequentedBlock(second));
    second->successor(secondBranch->leavingSuccessor) = B3::FrequentedBlock(dispatch, secondTarget.frequency());
    return true;
}

bool mergeImpliedBranches(B3::Procedure& proc)
{
    bool changed = false;
    unsigned numberOfBlocks = proc.size();
    for (unsigned index = 0; index < numberOfBlocks; ++index) {
        if (B3::BasicBlock* block = proc[index])
            changed |= tryMerge(proc, block);
    }
    if (changed) {
        proc.resetReachability();
        proc.invalidateCFG();
    }
    return changed;
}

B3::ClientPhase mergeImpliedBranchesAfterOptimization()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "mergeImpliedBranches"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return mergeImpliedBranches(proc);
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
