/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTImpliedBranchMerging.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTSlotRepresentation.h"
#include "B3BasicBlockInlines.h"
#include "AOTFaultTable.h"
#include "AOTPureOperations.h"
#include "AOTStubRegisters.h"
#include "B3ArgumentRegValue.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "B3PatchpointValue.h"
#include "B3Procedure.h"
#include "B3UpsilonValue.h"
#include "B3ValueInlines.h"
#include <limits>
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

static std::optional<LeavingBranch> describeBranchLeavingBy(B3::BasicBlock* block, unsigned leavingSuccessor)
{
    B3::Value* branch = block->last();
    if (branch->opcode() != B3::Branch || block->taken().block() == block->notTaken().block())
        return std::nullopt;

    // The branch is taken where the condition is not zero.
    B3::DominatingCondition condition = B3::stripNegations(branch->child(0));
    bool leavesWhenTaken = !leavingSuccessor;
    bool leavesWhenNonZero = leavesWhenTaken == condition.isNonZero;

    B3::Value* value = condition.value;
    if (isRelation(value->opcode()) && value->child(0)->type().isInt() && value->child(1)->hasInt())
        return LeavingBranch { value->child(0), value->opcode(), value->child(1)->asInt(), leavesWhenNonZero, leavingSuccessor };
    if (!value->type().isInt())
        return std::nullopt;
    return LeavingBranch { value, B3::NotEqual, 0, leavesWhenNonZero, leavingSuccessor };
}

static std::optional<LeavingBranch> findLeavingBranch(B3::BasicBlock* block)
{
    if (block->last()->opcode() != B3::Branch)
        return std::nullopt;
    bool takenIsLikely = block->taken().frequency() == B3::FrequencyClass::Normal;
    bool notTakenIsLikely = block->notTaken().frequency() == B3::FrequencyClass::Normal;
    if (takenIsLikely == notTakenIsLikely)
        return std::nullopt;
    return describeBranchLeavingBy(block, takenIsLikely ? 1 : 0);
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

static bool isUnsignedOrder(B3::Opcode opcode)
{
    return opcode == B3::Below || opcode == B3::Above || opcode == B3::BelowEqual || opcode == B3::AboveEqual;
}

// Whatever leaves at the first branch would leave at the second too.
static bool secondLeavesForAllThatFirstLeavesFor(const LeavingBranch& first, const LeavingBranch& second)
{
    std::optional<int64_t> leavingValue = findOnlyLeavingValue(first);
    if (!leavingValue)
        return false;
    if (second.subject == first.subject)
        return leavesFor(second, *leavingValue);

    // The first leaves for the words without a bit of the mask. They lie from zero to the word of all the other bits, and what an unsigned order leaves for has no gap.
    B3::Value* masked = first.subject;
    if (*leavingValue || masked->opcode() != B3::BitAnd || !masked->child(1)->hasInt() || masked->child(0) != second.subject || !isUnsignedOrder(second.relation))
        return false;
    return leavesFor(second, 0) && leavesFor(second, ~masked->child(1)->asInt());
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

static bool askFirstBehindSecond(B3::Procedure& proc, B3::BasicBlock* first, unsigned firstLeavingSuccessor, B3::BasicBlock* second, unsigned secondLeavingSuccessor)
{
    B3::FrequentedBlock firstTarget = first->successor(firstLeavingSuccessor);
    B3::FrequentedBlock secondTarget = second->successor(secondLeavingSuccessor);
    B3::BasicBlock* continuation = first->successorBlock(1 - firstLeavingSuccessor);
    if (firstTarget.block() == second || secondTarget.block() == second || secondTarget.block() == first)
        return false;

    B3::BasicBlock* dispatch = proc.addBlock(std::min(firstTarget.block()->frequency() + secondTarget.block()->frequency(), second->frequency()));
    B3::Value* branch = first->last();
    first->values().removeLast();
    dispatch->append(branch);
    branch->owner = dispatch;
    if (firstLeavingSuccessor)
        dispatch->setSuccessors(secondTarget, firstTarget);
    else
        dispatch->setSuccessors(firstTarget, secondTarget);

    first->appendNew<B3::Value>(proc, B3::Jump, branch->origin());
    first->setSuccessors(B3::FrequentedBlock(continuation));
    second->successor(secondLeavingSuccessor) = B3::FrequentedBlock(dispatch, secondTarget.frequency());
    return true;
}

static bool tryMerge(B3::Procedure& proc, B3::BasicBlock* first)
{
    std::optional<LeavingBranch> firstBranch = findLeavingBranch(first);
    if (!firstBranch)
        return false;

    B3::BasicBlock* second = first->successorBlock(1 - firstBranch->leavingSuccessor);
    if (second == first || second->numPredecessors() != 1)
        return false;
    // The way out of the second may be a likely one: who takes it is asked two tests as before, in the other order. Who takes the other way is asked one, so that has to be a likely one.
    std::optional<LeavingBranch> secondBranch;
    for (unsigned leavingSuccessor = 0; leavingSuccessor < 2 && !secondBranch; ++leavingSuccessor) {
        if (second->numSuccessors() != 2 || second->successor(1 - leavingSuccessor).frequency() != B3::FrequencyClass::Normal)
            continue;
        secondBranch = describeBranchLeavingBy(second, leavingSuccessor);
        if (secondBranch && !secondLeavesForAllThatFirstLeavesFor(*firstBranch, *secondBranch))
            secondBranch = std::nullopt;
    }
    if (!secondBranch || !mayRunForAnyValue(second))
        return false;

    return askFirstBehindSecond(proc, first, firstBranch->leavingSuccessor, second, secondBranch->leavingSuccessor);
}

static bool isComparisonOfNumbers(B3::Value* value)
{
    return (value->opcode() == B3::Equal || value->opcode() == B3::NotEqual) && value->child(0)->type().isFloat();
}

// NaN is the only number that differs from itself.
static B3::Value* findNumberThatLeavesWhereItIsNaN(B3::BasicBlock* block, unsigned leavingSuccessor)
{
    B3::DominatingCondition condition = B3::stripNegations(block->last()->child(0));
    B3::Value* value = condition.value;
    if (!isComparisonOfNumbers(value) || value->child(0) != value->child(1))
        return nullptr;
    bool isTakenForNaN = (value->opcode() == B3::NotEqual) == condition.isNonZero;
    return isTakenForNaN == !leavingSuccessor ? value->child(0) : nullptr;
}

static bool isTruncationOf(B3::Value* value, B3::Value* number)
{
    auto* patchpoint = value->as<B3::PatchpointValue>();
    return patchpoint && patchpoint->pureFunction == Typed::pureFunction(Typed::PureOperation::TruncateToInt64, 0, true) && patchpoint->child(0) == number;
}

static bool isNonNegative(B3::Procedure& proc, B3::Value* value)
{
    return B3::rangeFor(proc, value).min() >= 0;
}

static std::optional<unsigned> findSuccessorTakenForNaN(B3::Procedure& proc, B3::BasicBlock* block, B3::Value* number)
{
    if (block->last()->opcode() != B3::Branch || block->taken().block() == block->notTaken().block())
        return std::nullopt;
    B3::DominatingCondition condition = B3::stripNegations(block->last()->child(0));
    B3::Value* value = condition.value;
    std::optional<bool> holds;
    if (isComparisonOfNumbers(value) && (value->child(0) == number || value->child(1) == number))
        holds = value->opcode() == B3::NotEqual;
    else if (value->opcode() == B3::EqualOrUnordered && (value->child(0) == number || value->child(1) == number))
        holds = true;
    else if (isX86_64() && isUnsignedOrder(value->opcode()) && isTruncationOf(value->child(0), number) && isNonNegative(proc, value->child(1)))
        holds = value->opcode() == B3::Above || value->opcode() == B3::AboveEqual;
    else if (isX86_64() && isUnsignedOrder(value->opcode()) && isTruncationOf(value->child(1), number) && isNonNegative(proc, value->child(0)))
        holds = value->opcode() == B3::Below || value->opcode() == B3::BelowEqual;
    else if (isX86_64() && isRelation(value->opcode()) && value->child(0)->type() == B3::Int64 && value->child(1)->hasInt64()) {
        // x86-64 truncates NaN to the least int64.
        uint64_t truncated = static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
        B3::Value* subject = value->child(0);
        if (subject->opcode() == B3::Add && subject->child(1)->hasInt64()) {
            truncated += static_cast<uint64_t>(subject->child(1)->asInt64());
            subject = subject->child(0);
        }
        if (isTruncationOf(subject, number))
            holds = leavesFor(LeavingBranch { value->child(0), value->opcode(), value->child(1)->asInt64(), true, 0 }, static_cast<int64_t>(truncated));
    }
    if (!holds)
        return std::nullopt;
    return *holds == condition.isNonZero ? 0 : 1;
}

// An element of an array of numbers that is asked whether it is there, and then whether it is a whole number or fits an integer: a hole is NaN, which is neither.
static bool tryMergeTestForNaN(B3::Procedure& proc, B3::BasicBlock* first)
{
    if (first->last()->opcode() != B3::Branch || first->taken().block() == first->notTaken().block())
        return false;
    bool takenIsLikely = first->taken().frequency() == B3::FrequencyClass::Normal;
    if (takenIsLikely == (first->notTaken().frequency() == B3::FrequencyClass::Normal))
        return false;
    unsigned leavingSuccessor = takenIsLikely ? 1 : 0;
    B3::Value* number = findNumberThatLeavesWhereItIsNaN(first, leavingSuccessor);
    if (!number)
        return false;

    constexpr unsigned maxTestsToLookAt = 3;
    B3::BasicBlock* second = first->successorBlock(1 - leavingSuccessor);
    for (unsigned count = 0; count < maxTestsToLookAt; ++count) {
        if (second == first || second->numPredecessors() != 1)
            return false;
        std::optional<unsigned> successorForNaN = findSuccessorTakenForNaN(proc, second, number);
        if (!successorForNaN || !mayRunForAnyValue(second))
            return false;
        if (second->successor(1 - *successorForNaN).frequency() == B3::FrequencyClass::Normal)
            return askFirstBehindSecond(proc, first, leavingSuccessor, second, *successorForNaN);
        second = second->successorBlock(*successorForNaN);
    }
    return false;
}

static std::optional<int64_t> findMaskOfLeavingBits(const LeavingBranch& branch)
{
    bool leavesWhenNonZero = branch.relation == (branch.leavesWhenItHolds ? B3::NotEqual : B3::Equal) && !branch.constant;
    if (!leavesWhenNonZero || branch.subject->opcode() != B3::BitAnd || !branch.subject->child(1)->hasInt())
        return std::nullopt;
    return branch.subject->child(1)->asInt();
}

static bool mayLoadAgainBehind(B3::BasicBlock* block, B3::Value* word)
{
    if (word->owner != block || !B3::isLoad(word->opcode()))
        return false;
    bool isBehindLoad = false;
    for (B3::Value* value : *block) {
        if (isBehindLoad && (value->effects().writes || value->effects().fence))
            return false;
        isBehindLoad |= value == word;
    }
    return isBehindLoad;
}

static bool tryMergeBitTests(B3::Procedure& proc, B3::BasicBlock* first)
{
    std::optional<LeavingBranch> firstBranch = findLeavingBranch(first);
    if (!firstBranch)
        return false;
    std::optional<int64_t> firstMask = findMaskOfLeavingBits(*firstBranch);
    if (!firstMask)
        return false;

    B3::BasicBlock* second = first->successorBlock(1 - firstBranch->leavingSuccessor);
    if (second == first || second->numPredecessors() != 1)
        return false;
    std::optional<LeavingBranch> secondBranch = findLeavingBranch(second);
    if (!secondBranch || !mayRunForAnyValue(second))
        return false;
    std::optional<int64_t> secondMask = findMaskOfLeavingBits(*secondBranch);
    if (!secondMask || secondBranch->subject->child(0) != firstBranch->subject->child(0))
        return false;

    B3::FrequentedBlock firstTarget = first->successor(firstBranch->leavingSuccessor);
    B3::FrequentedBlock secondTarget = second->successor(secondBranch->leavingSuccessor);
    if (firstTarget.block() == second || secondTarget.block() == second || secondTarget.block() == first)
        return false;

    B3::Value* word = firstBranch->subject->child(0);
    B3::Value* secondTerminal = second->last();
    B3::Origin origin = secondTerminal->origin();
    auto appendBits = [&](B3::BasicBlock* block, B3::Value* tested, int64_t mask) {
        return block->appendNew<B3::Value>(proc, B3::BitAnd, origin, tested, block->appendIntConstant(proc, origin, word->type(), mask));
    };

    B3::FrequentedBlock leaving = secondTarget;
    if (firstTarget.block() != secondTarget.block()) {
        B3::BasicBlock* dispatch = proc.addBlock(std::min(firstTarget.block()->frequency() + secondTarget.block()->frequency(), second->frequency()));
        B3::Value* wordInDispatch = word;
        if (mayLoadAgainBehind(first, word)) {
            wordInDispatch = proc.clone(word);
            dispatch->append(wordInDispatch);
        }
        dispatch->appendNew<B3::Value>(proc, B3::Branch, origin, appendBits(dispatch, wordInDispatch, *firstMask));
        dispatch->setSuccessors(firstTarget, secondTarget);
        leaving = B3::FrequentedBlock(dispatch, secondTarget.frequency());
    }
    B3::FrequentedBlock staying = second->successor(1 - secondBranch->leavingSuccessor);

    proc.deleteValue(first->last());
    first->values().removeLast();
    second->values().removeLast();
    for (B3::Value* value : *second)
        first->append(value);
    second->values().clear();
    second->appendNew<B3::Value>(proc, B3::Oops, origin);
    second->clearSuccessors();

    B3::Value* bits = appendBits(first, word, *firstMask | *secondMask);
    secondTerminal->child(0) = secondBranch->leavingSuccessor ? first->appendNew<B3::Value>(proc, B3::Equal, origin, bits, first->appendIntConstant(proc, origin, word->type(), 0)) : bits;
    first->append(secondTerminal);
    if (secondBranch->leavingSuccessor)
        first->setSuccessors(staying, leaving);
    else
        first->setSuccessors(leaving, staying);
    return true;
}

static B3::Value* findSideChosenOnEveryPathTo(B3::BasicBlock* block, B3::Value* select)
{
    constexpr unsigned maxNumberOfBlocksAbove = 8;
    for (unsigned count = 0; count < maxNumberOfBlocksAbove && block->numPredecessors() == 1; ++count) {
        B3::BasicBlock* predecessor = block->predecessor(0);
        B3::Value* terminal = predecessor->last();
        if (terminal->opcode() == B3::Branch && terminal->child(0) == select->child(0) && predecessor->successorBlock(0) != predecessor->successorBlock(1))
            return select->child(predecessor->successorBlock(0) == block ? 1 : 2);
        block = predecessor;
    }
    return nullptr;
}

static bool everyOtherPredecessorSetsPhi(B3::BasicBlock* predecessor, B3::Value* phi)
{
    for (B3::BasicBlock* other : phi->owner->predecessors()) {
        if (other == predecessor)
            continue;
        bool setsPhi = false;
        for (B3::Value* value : *other)
            setsPhi |= value->opcode() == B3::Upsilon && value->as<B3::UpsilonValue>()->phi() == phi;
        if (!setsPhi)
            return false;
    }
    return true;
}

// What a block hands to a Phi of one of its two successors is read on that edge alone.
static B3::Value* findSideChosenOnTheEdgeTo(B3::BasicBlock* block, B3::Value* phi, B3::Value* select)
{
    B3::Value* terminal = block->last();
    if (terminal->opcode() != B3::Branch || terminal->child(0) != select->child(0) || block->successorBlock(0) == block->successorBlock(1))
        return nullptr;
    for (unsigned index = 0; index < 2; ++index) {
        if (block->successorBlock(index) == phi->owner && everyOtherPredecessorSetsPhi(block, phi))
            return select->child(index + 1);
    }
    return nullptr;
}

static void foldSelectsDecidedByBranchesAbove(B3::Procedure& proc)
{
    proc.resetValueOwners();
    for (B3::BasicBlock* block : proc) {
        for (B3::Value* value : *block) {
            if (value->opcode() == B3::Select) {
                if (B3::Value* chosen = findSideChosenOnEveryPathTo(block, value)) {
                    value->replaceWithIdentity(chosen);
                    continue;
                }
            }
            for (B3::Value*& child : value->children()) {
                if (child->opcode() != B3::Select)
                    continue;
                B3::Value* chosen = findSideChosenOnEveryPathTo(block, child);
                if (!chosen && value->opcode() == B3::Upsilon)
                    chosen = findSideChosenOnTheEdgeTo(block, value->as<B3::UpsilonValue>()->phi(), child);
                if (chosen)
                    child = chosen;
            }
        }
    }
}

namespace {

// A compressed reference that was decoded into a boxed value: Select(Above(word, undefinedCell32), pointer, ZExt32(word)). The pointer has no tag bit and is no marker.
class TestsOfDecodedReferences {
public:
    TestsOfDecodedReferences(const Typed::Program& program, const Typed::Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    void testWordsInstead(B3::Procedure& proc) const
    {
        for (B3::BasicBlock* block : proc) {
            for (B3::Value* value : *block) {
                if (value->opcode() != B3::Select)
                    continue;
                if (std::optional<Test> test = findTest(value->child(0))) {
                    value->child(0) = test->isCell;
                    if (!test->holdsForCells)
                        std::swap(value->child(1), value->child(2));
                }
            }
            B3::Value* terminal = block->last();
            if (terminal->opcode() != B3::Branch)
                continue;
            if (std::optional<Test> test = findTest(terminal->child(0))) {
                terminal->child(0) = test->isCell;
                if (!test->holdsForCells) {
                    proc.willReorderSuccessors();
                    std::swap(block->successor(0), block->successor(1));
                    proc.didReorderSuccessors();
                }
            }
        }
    }

    bool findsDecodedReferenceIn(B3::Procedure& proc) const
    {
        for (B3::BasicBlock* block : proc) {
            for (B3::Value* value : *block) {
                if (findWordDecodedInto(value))
                    return true;
            }
        }
        return false;
    }

    // The first pages of the cage of cells are never mapped, and a marker is a small word.
    void readThroughPointers(B3::Procedure& proc) const
    {
        Reads reads;
        for (B3::BasicBlock* block : proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                auto* access = block->at(index)->as<B3::MemoryValue>();
                if (!access || !faultsForMarkers(access))
                    continue;
                B3::Value*& base = access->lastChild();
                if (findWordDecodedInto(base)) {
                    reads.ensure(base, [] { return Vector<Position, 2>(); }).iterator->value.append({ block, index });
                    base = base->child(1);
                } else if (base->opcode() == B3::Phi)
                    reads.ensure(base, [] { return Vector<Position, 2>(); }).iterator->value.append({ block, index });
            }
        }
        if (reads.isEmpty())
            return;

        B3::Dominators& dominators = proc.dominators();
        auto isBehindRead = [&](B3::Value* value, B3::BasicBlock* block, unsigned index) {
            auto found = reads.find(value);
            return found != reads.end() && std::ranges::any_of(found->value, [&](Position read) {
                return read.block == block ? read.index < index : dominators.strictlyDominates(read.block, block);
            });
        };

        // `for (; node !== null; node = node.next) node.value`: the variable is read before anything else is done with it.
        UncheckedKeyHashSet<B3::Value*> variablesUsedUnread;
        for (B3::BasicBlock* block : proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* value = block->at(index);
                auto* access = value->as<B3::MemoryValue>();
                for (B3::Value*& child : value->children()) {
                    if (child->opcode() == B3::Select) {
                        if (isBehindRead(child, block, index))
                            child = child->child(1);
                        continue;
                    }
                    if (child->opcode() != B3::Phi || !reads.contains(child))
                        continue;
                    bool isRead = access && &child == &access->lastChild() && faultsForMarkers(access);
                    if (!isRead && !isBehindRead(child, block, index))
                        variablesUsedUnread.add(child);
                }
            }
        }
        for (B3::BasicBlock* block : proc) {
            for (B3::Value* value : *block) {
                if (value->opcode() != B3::Upsilon || !findWordDecodedInto(value->child(0)))
                    continue;
                B3::Value* phi = value->as<B3::UpsilonValue>()->phi();
                if (reads.contains(phi) && !variablesUsedUnread.contains(phi))
                    value->child(0) = value->child(0)->child(1);
            }
        }
    }

    // No pointer is a marker, so the decoded value is a marker where the word is.
    void askWordsForMarkers(B3::Procedure& proc) const
    {
        B3::InsertionSet insertionSet(proc);
        for (B3::BasicBlock* block : proc) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* value = block->at(index);
                if ((value->opcode() != B3::Equal && value->opcode() != B3::NotEqual) || !value->child(1)->hasInt64())
                    continue;
                B3::Value* word = findWordDecodedInto(value->child(0));
                int64_t constant = value->child(1)->asInt64();
                if (!word || constant < 0 || constant > static_cast<int64_t>(undefinedCell32))
                    continue;
                value->child(0) = word;
                value->child(1) = insertionSet.insert<B3::Const32Value>(index, value->origin(), static_cast<int32_t>(constant));
            }
            insertionSet.execute(block);
        }
    }

private:
    struct Test {
        B3::Value* isCell;
        bool holdsForCells;
    };

    struct Position {
        B3::BasicBlock* block;
        unsigned index;
    };

    using Reads = UncheckedKeyHashMap<B3::Value*, Vector<Position, 2>>;

    static bool isPointerOf(B3::Value* sum, B3::Value* word)
    {
        if (sum->opcode() != B3::Add)
            return false;
        for (unsigned index = 0; index < 2; ++index) {
            auto* base = sum->child(index)->as<B3::ArgumentRegValue>();
            B3::Value* distance = sum->child(1 - index);
            if (base && base->argumentReg() == Reg(cellCageBaseGPR) && distance->opcode() == B3::Shl && distance->child(1)->isInt32(cell32Shift) && distance->child(0)->opcode() == B3::ZExt32 && distance->child(0)->child(0) == word)
                return true;
        }
        return false;
    }

    // Whatever operation has decoded it: a Decode, or a read of a narrow slot whose result is boxed.
    static B3::Value* findWordDecodedInto(B3::Value* value)
    {
        if (value->opcode() != B3::Select)
            return nullptr;
        B3::Value* isCell = value->child(0);
        if (isCell->opcode() != B3::Above || !isCell->child(1)->isInt32(undefinedCell32))
            return nullptr;
        B3::Value* word = isCell->child(0);
        if (value->child(2)->opcode() != B3::ZExt32 || value->child(2)->child(0) != word || !isPointerOf(value->child(1), word))
            return nullptr;
        return word;
    }

    static bool faultsForMarkers(const B3::MemoryValue* access)
    {
        constexpr int64_t largestAccess = 16;
        return access->traps() && access->offset() >= 0 && (static_cast<int64_t>(undefinedCell32) << cell32Shift) + access->offset() + largestAccess <= static_cast<int64_t>(FaultSite::reach);
    }

    static constexpr uint16_t markers = Typed::Primitive::UndefinedOrNull | Typed::Primitive::Absent;

    std::optional<uint16_t> findMarkersOf(B3::Value* value) const
    {
        if (value->opcode() != B3::Select)
            return std::nullopt;
        B3::Value* isCell = value->child(0);
        if (isCell->opcode() != B3::Above || !isCell->child(1)->isInt32(undefinedCell32))
            return std::nullopt;
        if (value->child(2)->opcode() != B3::ZExt32 || value->child(2)->child(0) != isCell->child(0))
            return std::nullopt;
        Typed::ValueID id = Typed::valueOfOrigin(value->origin());
        if (!id || id.index() >= m_function.values.size())
            return std::nullopt;
        const Typed::Value& origin = m_function.at(id);
        if (origin.opcode == Typed::Opcode::Decode)
            return m_program.at(m_function.at(origin.valueOperand()).type).primitives & markers;
        if (origin.representation != Typed::Representation::Cell32)
            return std::nullopt;
        return m_program.at(origin.type).primitives & markers;
    }

    std::optional<Test> findTest(B3::Value* condition) const
    {
        bool isNegated = false;
        while ((condition->opcode() == B3::Equal || condition->opcode() == B3::NotEqual) && condition->child(1)->isInt(0) && condition->child(0)->opcode() != B3::Select) {
            isNegated ^= condition->opcode() == B3::Equal;
            condition = condition->child(0);
        }
        if (condition->numChildren() != 2 || !condition->child(1)->hasInt64())
            return std::nullopt;
        std::optional<uint16_t> possible = findMarkersOf(condition->child(0));
        if (!possible)
            return std::nullopt;
        int64_t constant = condition->child(1)->asInt64();
        B3::Value* isCell = condition->child(0)->child(0);
        switch (condition->opcode()) {
        case B3::BitAnd:
            if (constant != static_cast<int64_t>(JSValue::NotCellMask) || (*possible & Typed::Primitive::Absent))
                return std::nullopt;
            return Test { isCell, isNegated };
        case B3::Equal:
        case B3::NotEqual: {
            bool isOnlyMarker = (constant == JSValue::ValueNull && *possible == Typed::Primitive::Null) || (constant == JSValue::ValueUndefined && *possible == Typed::Primitive::Undefined);
            if (!isOnlyMarker)
                return std::nullopt;
            return Test { isCell, (condition->opcode() == B3::NotEqual) != isNegated };
        }
        default:
            return std::nullopt;
        }
    }

    const Typed::Program& m_program;
    const Typed::Function& m_function;
};

} // anonymous namespace

bool mergeImpliedBranches(B3::Procedure& proc)
{
    foldSelectsDecidedByBranchesAbove(proc);

    bool changed = false;
    unsigned numberOfBlocks = proc.size();
    for (unsigned index = 0; index < numberOfBlocks; ++index) {
        if (B3::BasicBlock* block = proc[index])
            changed |= tryMerge(proc, block) || tryMergeBitTests(proc, block) || tryMergeTestForNaN(proc, block);
    }
    if (changed) {
        proc.resetReachability();
        proc.invalidateCFG();
    }
    return changed;
}

B3::ClientPhase mergeImpliedBranchesAfterOptimization(const Typed::Program& program, const Typed::Function& function)
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "mergeImpliedBranches"_s,
        createSharedTask<bool(B3::Procedure&)>([&program, &function](B3::Procedure& proc) {
            TestsOfDecodedReferences tests(program, function);
            if (tests.findsDecodedReferenceIn(proc)) {
                tests.testWordsInstead(proc);
                tests.askWordsForMarkers(proc);
                tests.readThroughPointers(proc);
            }
            return mergeImpliedBranches(proc);
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
