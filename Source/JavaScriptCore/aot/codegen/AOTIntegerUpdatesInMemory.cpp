/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIntegerUpdatesInMemory.h"

#if ENABLE(AOT)

#include "AOTCodeGenerator.h"
#include "AOTHeaps.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3FixSSA.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "B3MovedLoadCheck.h"
#include "B3NaturalLoops.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include "B3VariableValue.h"
#include "IndexingHeader.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

namespace {

enum class UpdateKind : uint8_t { CompareThenAdd, AddThenBranchOnOverflow, AddToLowWordThenBranchOnCarry };

struct Update {
    UpdateKind kind;
    B3::MemoryValue* store;
    B3::Value* truncated;
    B3::Value* sum;
    B3::Value* extended;
    B3::MemoryValue* load;
    B3::Value* comparison;
    B3::Value* biasedSum;
    B3::Value* addend;
    int32_t constantAddend;
    int32_t oldValueBound;
    std::optional<int32_t> minStoredValue;
    bool addsToLowWord;
    Vector<B3::BasicBlock*, 4> path;
    struct UserOnExit {
        B3::Value* user;
        B3::BasicBlock* exit;
    };
    Vector<UserOnExit, 2> sumUsersOnExits;
};

struct StoredRange {
    std::optional<int64_t> min;
    std::optional<int64_t> max;
};

// The function that the procedure was made from, where there is one.
struct Source {
    const Typed::Program* program { nullptr };
    const Typed::Function* function { nullptr };
};

} // anonymous namespace

static constexpr int64_t minInt32 = std::numeric_limits<int32_t>::min();
static constexpr int64_t maxInt32 = std::numeric_limits<int32_t>::max();

static bool fitsInt32(int64_t value)
{
    return value >= minInt32 && value <= maxInt32;
}

static bool isSignedOrder(B3::Opcode opcode)
{
    return opcode == B3::LessThan || opcode == B3::LessEqual || opcode == B3::GreaterThan || opcode == B3::GreaterEqual;
}

static bool comparesUnsignedOrder(B3::Opcode opcode)
{
    return opcode == B3::Below || opcode == B3::BelowEqual || opcode == B3::Above || opcode == B3::AboveEqual;
}

static bool hasNoWritesBetween(B3::BasicBlock* block, B3::Value* after, B3::Value* before)
{
    bool isInRange = !after;
    for (B3::Value* value : *block) {
        if (value == before)
            return true;
        if (isInRange) {
            B3::Effects effects = value->effects();
            if (effects.writes || effects.fence || effects.exitsSideways)
                return false;
        }
        isInRange |= value == after;
    }
    return isInRange && !before;
}

static bool isLocationReadBeforeStore(B3::MemoryValue* store)
{
    for (B3::Value* value : *store->owner) {
        if (value == store)
            return false;
        if (value->effects().reads.overlaps(store->range()))
            return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isWide(const Update& update)
{
    return update.load->type() == B3::Int64;
}

// Sums of a few of them do not overflow.
static bool isUsableBound(const Update& update, int64_t bound)
{
    constexpr int64_t limit = std::numeric_limits<int64_t>::max() / 4;
    return isWide(update) ? bound > -limit && bound < limit : fitsInt32(bound >> 2);
}

static std::optional<StoredRange> computeStoredRange(const Update& update, B3::BasicBlock* test, B3::DominatingCondition condition)
{
    B3::Value* comparison = condition.value;
    B3::Opcode opcode = comparison->opcode();
    if (!isSignedOrder(opcode) && !comparesUnsignedOrder(opcode))
        return std::nullopt;
    if (test->successorBlock(0) == test->successorBlock(1) || !comparison->child(1)->hasInt64())
        return std::nullopt;
    int64_t bound = comparison->child(1)->asInt64();
    if (!isUsableBound(update, bound))
        return std::nullopt;

    bool storesOnTrue = (test->successorBlock(0) == update.path[0]) == condition.isNonZero;
    bool isUpperBound = opcode == B3::LessThan || opcode == B3::LessEqual || opcode == B3::Below || opcode == B3::BelowEqual;
    bool isInclusive = opcode == B3::LessEqual || opcode == B3::GreaterEqual || opcode == B3::BelowEqual || opcode == B3::AboveEqual;
    bool boundsFromAbove = isUpperBound == storesOnTrue;
    int64_t adjustment = isInclusive == storesOnTrue ? 0 : 1;
    int64_t limit = boundsFromAbove ? bound - adjustment : bound + adjustment;

    if (isSignedOrder(opcode)) {
        if (comparison->child(0) != update.sum)
            return std::nullopt;
        return boundsFromAbove ? StoredRange { std::nullopt, limit } : StoredRange { limit, std::nullopt };
    }
    if (!boundsFromAbove || limit < 0)
        return std::nullopt;
    B3::Value* biased = comparison->child(0);
    if (biased->opcode() != B3::Add || biased->child(0) != update.sum || !biased->child(1)->hasInt64() || !isUsableBound(update, biased->child(1)->asInt64()))
        return std::nullopt;
    int64_t bias = biased->child(1)->asInt64();
    return StoredRange { -bias, limit - bias };
}

static bool tryUseOverflowFlag(B3::Procedure& proc, Update& update, const StoredRange& storedRange)
{
    B3::IntRange oldValueRange = B3::rangeFor(proc, update.load);
    B3::IntRange addendRange = B3::rangeFor(proc, update.addend);
    if (!fitsInt32(addendRange.min()) || !fitsInt32(addendRange.max()))
        return false;
    bool subtracts = update.sum->opcode() == B3::Sub;
    int64_t minSum = oldValueRange.min() + (subtracts ? -addendRange.max() : addendRange.min());
    int64_t maxSum = oldValueRange.max() + (subtracts ? -addendRange.min() : addendRange.max());

    bool cannotOverflow = maxSum <= maxInt32;
    if (storedRange.max ? *storedRange.max != maxInt32 && (*storedRange.max < maxSum || !cannotOverflow) : !cannotOverflow)
        return false;
    bool cannotUnderflow = minSum >= minInt32;
    if ((!storedRange.min || *storedRange.min < minInt32) && !cannotUnderflow)
        return false;
    if (storedRange.min && *storedRange.min > maxInt32)
        return false;
    std::optional<int32_t> minStoredValue;
    if (storedRange.min && *storedRange.min > minInt32 && minSum < *storedRange.min)
        minStoredValue = static_cast<int32_t>(*storedRange.min);
    if (minStoredValue && update.addend->hasInt64())
        return false;
    if (isLocationReadBeforeStore(update.store))
        return false;
    update.kind = UpdateKind::AddThenBranchOnOverflow;
    update.minStoredValue = minStoredValue;
    return true;
}

// A load may run ahead of the guard of the read that it was made for. Where the sum is stored the read has run, and nothing was stored between the two.
static B3::IntRange rangeOfIntegerRead(B3::Procedure& proc, const Source& source, B3::Value* load)
{
    Typed::ValueID id = Typed::valueOfOrigin(load->origin());
    if (!source.function || !id || id.index() >= source.function->values.size())
        return B3::rangeFor(proc, load);
    const Typed::Value& read = source.function->at(id);
    const Typed::Type& type = source.program->at(read.type);
    if (read.opcode != Typed::Opcode::GetSlot || read.representation != Typed::Representation::Int64 || !type.isInteger())
        return B3::rangeFor(proc, load);
    return B3::IntRange(type.integers.minimum, type.integers.maximum);
}

// Where the low word neither carries nor borrows, the sum has the high word of the old value.
static bool tryUseCarryFlag(B3::Procedure& proc, const Source& source, Update& update, const StoredRange& storedRange)
{
    constexpr int64_t lowWord = std::numeric_limits<uint32_t>::max();
    B3::IntRange oldValueRange = rangeOfIntegerRead(proc, source, update.load);
    if (!isUsableBound(update, oldValueRange.min()) || !isUsableBound(update, oldValueRange.max()))
        return false;
    bool adds = update.sum->opcode() == B3::Add;
    B3::IntRange amountRange = B3::rangeFor(proc, update.addend);
    if (update.constantAddend < 0) {
        adds = false;
        amountRange = B3::IntRange(-static_cast<int64_t>(update.constantAddend), -static_cast<int64_t>(update.constantAddend));
    }
    if (amountRange.min() < 0 || amountRange.max() > lowWord)
        return false;
    int64_t minSum = adds ? oldValueRange.min() + amountRange.min() : oldValueRange.min() & ~lowWord;
    int64_t maxSum = adds ? oldValueRange.max() | lowWord : oldValueRange.max() - amountRange.min();
    if ((storedRange.min && minSum < *storedRange.min) || (storedRange.max && maxSum > *storedRange.max))
        return false;
    if (isLocationReadBeforeStore(update.store))
        return false;
    update.kind = UpdateKind::AddToLowWordThenBranchOnCarry;
    update.addsToLowWord = adds;
    return true;
}

static std::optional<Update> matchUpdate(B3::Procedure& proc, const Source& source, B3::Value* value)
{
    AOT_THRESHOLD(unsigned, maxPathLength, 4);
    auto* store = value->as<B3::MemoryValue>();
    if (!store || store->opcode() != B3::Store)
        return std::nullopt;
    bool isWide = store->child(0)->type() == B3::Int64;
    if (!isWide && store->child(0)->opcode() != B3::Trunc)
        return std::nullopt;
    B3::Value* truncated = store->child(0);
    B3::Value* sum = isWide ? truncated : truncated->child(0);
    if (sum->opcode() != B3::Add && sum->opcode() != B3::Sub)
        return std::nullopt;

    auto matchExtendedLoad = [&](B3::Value* extended) -> B3::MemoryValue* {
        if (!isWide && extended->opcode() != B3::SExt32)
            return nullptr;
        auto* load = (isWide ? extended : extended->child(0))->as<B3::MemoryValue>();
        if (!load || load->opcode() != B3::Load || load->lastChild() != store->lastChild() || load->offset() != store->offset() || load->traps() || store->traps())
            return nullptr;
        return load;
    };
    unsigned oldValueIndex = 0;
    B3::MemoryValue* load = matchExtendedLoad(sum->child(0));
    if (!load && sum->opcode() == B3::Add) {
        oldValueIndex = 1;
        load = matchExtendedLoad(sum->child(1));
    }
    if (!load)
        return std::nullopt;
    B3::Value* extended = sum->child(oldValueIndex);
    B3::Value* addend = sum->child(1 - oldValueIndex);
    if (addend->hasInt64() && (sum->opcode() == B3::Sub || !addend->asInt64() || !fitsInt32(addend->asInt64())))
        return std::nullopt;
    if (isWide && addend->isInt64(minInt32))
        return std::nullopt;

    Update update { UpdateKind::CompareThenAdd, store, truncated, sum, extended, load, nullptr, nullptr, addend, addend->hasInt64() ? static_cast<int32_t>(addend->asInt64()) : 0, 0, std::nullopt, false, { }, { } };
    for (B3::BasicBlock* block = store->owner; ; block = block->predecessor(0)) {
        update.path.append(block);
        if (block == load->owner)
            break;
        if (block->numPredecessors() != 1 || update.path.size() == maxPathLength)
            return std::nullopt;
    }
    if (update.path.size() < 2)
        return std::nullopt;
    for (B3::BasicBlock* block : update.path) {
        if (!hasNoWritesBetween(block, block == load->owner ? load : nullptr, block == store->owner ? store : nullptr))
            return std::nullopt;
    }

    B3::BasicBlock* test = update.path[1];
    if (test->last()->opcode() != B3::Branch)
        return std::nullopt;
    B3::DominatingCondition condition = B3::stripNegations(test->last()->child(0));
    B3::Value* comparison = condition.value;
    if (comparison->owner != test)
        return std::nullopt;
    update.comparison = comparison;

    if (std::optional<StoredRange> storedRange = computeStoredRange(update, test, condition)) {
        update.biasedSum = comparison->child(0) == sum ? nullptr : comparison->child(0);
        if (isWide ? tryUseCarryFlag(proc, source, update, *storedRange) : tryUseOverflowFlag(proc, update, *storedRange))
            return update;
        update.biasedSum = nullptr;
    }
    if (isWide)
        return std::nullopt;

    if (!addend->hasInt64() || !isSignedOrder(comparison->opcode()) || comparison->child(0) != sum || !comparison->child(1)->hasInt64())
        return std::nullopt;
    int64_t bound = comparison->child(1)->asInt64();
    if (!fitsInt32(bound >> 2) || !fitsInt32(bound - update.constantAddend))
        return std::nullopt;
    update.oldValueBound = static_cast<int32_t>(bound - update.constantAddend);
    return update;
}

static B3::BasicBlock* findExitDominating(B3::Procedure& proc, const Update& update, B3::Value* user)
{
    for (B3::BasicBlock* block : update.path.span().subspan(1)) {
        for (B3::BasicBlock* successor : block->successorBlocks()) {
            if (!update.path.contains(successor) && successor->numPredecessors() == 1 && proc.dominators().dominates(successor, user->owner))
                return successor;
        }
    }
    return nullptr;
}

static unsigned indexOf(B3::BasicBlock* block, B3::Value* value)
{
    size_t index = block->values().find(value);
    RELEASE_ASSERT(index != notFound);
    return static_cast<unsigned>(index);
}

static void rematerializeSumOnExits(B3::Procedure& proc, const Update& update)
{
    B3::InsertionSet insertionSet(proc);
    HashMap<B3::BasicBlock*, B3::Value*> sumForExit;
    for (auto [user, exit] : update.sumUsersOnExits) {
        B3::Value* sum = sumForExit.ensure(exit, [&] {
            unsigned index = 0;
            while (exit->at(index)->opcode() == B3::Phi)
                ++index;
            B3::Value* oldValue = proc.clone(update.load);
            insertionSet.insertValue(index, oldValue);
            B3::Value* extended = oldValue;
            if (update.extended != update.load) {
                extended = proc.clone(update.extended);
                extended->child(0) = oldValue;
                insertionSet.insertValue(index, extended);
            }
            B3::Value* addend = update.addend;
            if (addend->opcode() == B3::SExt32 || addend->opcode() == B3::ZExt32) {
                addend = proc.clone(addend);
                insertionSet.insertValue(index, addend);
            }
            B3::Value* clonedSum = proc.clone(update.sum);
            for (B3::Value*& child : clonedSum->children())
                child = child == update.extended ? extended : addend;
            insertionSet.insertValue(index, clonedSum);
            insertionSet.execute(exit);
            return clonedSum;
        }).iterator->value;
        for (B3::Value*& child : user->children()) {
            if (child == update.sum)
                child = sum;
        }
    }
}

static void emitCompareThenAdd(B3::Procedure& proc, const Update& update)
{
    B3::InsertionSet insertionSet(proc);
    auto insertLoadAt = [&](unsigned index) {
        B3::Value* clonedSum = proc.clone(update.load);
        insertionSet.insertValue(index, clonedSum);
        return clonedSum;
    };

    B3::BasicBlock* test = update.comparison->owner;
    unsigned comparisonIndex = indexOf(test, update.comparison);
    update.comparison->child(0) = insertLoadAt(comparisonIndex);
    update.comparison->child(1) = insertionSet.insert<B3::Const32Value>(comparisonIndex, update.comparison->origin(), update.oldValueBound);
    insertionSet.execute(test);

    B3::BasicBlock* storeBlock = update.store->owner;
    unsigned storeIndex = indexOf(storeBlock, update.store);
    B3::Value* oldValue = insertLoadAt(storeIndex);
    B3::Value* addend = insertionSet.insert<B3::Const32Value>(storeIndex, update.store->origin(), update.constantAddend);
    update.store->child(0) = insertionSet.insert<B3::Value>(storeIndex, B3::Add, update.store->origin(), oldValue, addend);
    insertionSet.execute(storeBlock);
}

static void emitAddThenBranchOnOverflow(B3::Procedure& proc, const Update& update)
{
    B3::BasicBlock* test = update.path[1];
    unsigned fastPathIndex = test->successorBlock(0) == update.path[0] ? 0 : 1;
    B3::FrequentedBlock fastPath = test->successor(fastPathIndex);
    B3::FrequentedBlock slowPath = test->successor(1 - fastPathIndex);
    B3::Origin origin = update.store->origin();
    bool subtracts = update.sum->opcode() == B3::Sub;

    auto appendLoad = [&](B3::BasicBlock* block) {
        B3::Value* load = proc.clone(update.load);
        block->append(load);
        return load;
    };
    auto appendStore = [&](B3::BasicBlock* block, B3::Value* value) {
        B3::Value* store = proc.clone(update.store);
        store->child(0) = value;
        block->append(store);
    };

    B3::BasicBlock* undoBlock = proc.addBlock(slowPath.block()->frequency());
    B3::FrequentedBlock undoEdge(undoBlock, slowPath.frequency());

    test->removeLast(proc);
    B3::Value* addend = nullptr;
    B3::Value* extendedAddend = update.addend;
    if (!update.addend->hasInt64()) {
        bool isExtended = update.addend->opcode() == B3::SExt32 || update.addend->opcode() == B3::ZExt32;
        addend = isExtended ? update.addend->child(0) : test->appendNew<B3::Value>(proc, B3::Trunc, origin, update.addend);
        extendedAddend = test->appendNew<B3::Value>(proc, B3::SExt32, origin, addend);
    }
    B3::Value* exactSum = test->appendNew<B3::Value>(proc, update.sum->opcode(), origin, test->appendNew<B3::Value>(proc, B3::SExt32, origin, appendLoad(test)), extendedAddend);
    B3::Value* wrappedSum = test->appendNew<B3::Value>(proc, B3::Trunc, origin, exactSum);
    appendStore(test, wrappedSum);
    B3::Value* overflowed = test->appendNew<B3::Value>(proc, B3::NotEqual, origin, test->appendNew<B3::Value>(proc, B3::SExt32, origin, wrappedSum), exactSum);
    test->appendNew<B3::Value>(proc, B3::Branch, origin, overflowed);
    undoBlock->addPredecessor(test);

    if (update.minStoredValue) {
        B3::BasicBlock* minCheck = proc.addBlock(fastPath.block()->frequency());
        B3::Value* isBelowMin = minCheck->appendNew<B3::Value>(proc, B3::LessThan, origin, appendLoad(minCheck), minCheck->appendNew<B3::Const32Value>(proc, origin, *update.minStoredValue));
        minCheck->appendNew<B3::Value>(proc, B3::Branch, origin, isBelowMin);
        minCheck->setSuccessors(undoEdge, fastPath);
        minCheck->addPredecessor(test);
        undoBlock->addPredecessor(minCheck);
        fastPath.block()->replacePredecessor(test, minCheck);
        test->setSuccessors(undoEdge, B3::FrequentedBlock(minCheck, fastPath.frequency()));
    } else
        test->setSuccessors(undoEdge, fastPath);

    B3::Value* amount = addend ? addend : undoBlock->appendNew<B3::Const32Value>(proc, origin, update.constantAddend);
    appendStore(undoBlock, undoBlock->appendNew<B3::Value>(proc, subtracts ? B3::Add : B3::Sub, origin, appendLoad(undoBlock), amount));
    undoBlock->appendNew<B3::Value>(proc, B3::Jump, origin);
    undoBlock->setSuccessors(B3::FrequentedBlock(slowPath.block()));
    slowPath.block()->replacePredecessor(test, undoBlock);

    update.store->replaceWithNopIgnoringType();
    proc.invalidateCFG();
}

static void emitAddToLowWordThenBranchOnCarry(B3::Procedure& proc, const Update& update)
{
    B3::BasicBlock* test = update.path[1];
    unsigned fastPathIndex = test->successorBlock(0) == update.path[0] ? 0 : 1;
    B3::FrequentedBlock fastPath = test->successor(fastPathIndex);
    B3::FrequentedBlock slowPath = test->successor(1 - fastPathIndex);
    B3::Origin origin = update.store->origin();

    auto appendLoadOfLowWord = [&](B3::BasicBlock* block) {
        return block->appendNew<B3::MemoryValue>(proc, B3::Load, B3::Int32, origin, update.load->lastChild(), update.load->offset(), update.load->range(), update.load->fenceRange());
    };
    auto appendStoreOfLowWord = [&](B3::BasicBlock* block, B3::Value* value) {
        block->appendNew<B3::MemoryValue>(proc, B3::Store, origin, value, update.store->lastChild(), update.store->offset(), update.store->range(), update.store->fenceRange());
    };
    auto appendClone = [&](B3::BasicBlock* block, B3::Value* value) {
        B3::Value* clone = proc.clone(value);
        block->append(clone);
        return clone;
    };

    B3::BasicBlock* exact = proc.addBlock(slowPath.block()->frequency());
    B3::BasicBlock* exactStore = proc.addBlock(slowPath.block()->frequency());

    bool isTakenWhereComparisonHolds = B3::stripNegations(test->last()->child(0)).isNonZero;
    test->removeLast(proc);
    B3::Value* amount = nullptr;
    if (!update.addend->hasInt64()) {
        bool isExtended = update.addend->opcode() == B3::SExt32 || update.addend->opcode() == B3::ZExt32;
        amount = isExtended ? update.addend->child(0) : test->appendNew<B3::Value>(proc, B3::Trunc, origin, update.addend);
    }
    auto amountIn = [&](B3::BasicBlock* block) {
        return amount ? amount : block->appendNew<B3::Const32Value>(proc, origin, std::abs(update.constantAddend));
    };
    B3::Value* lowWord = appendLoadOfLowWord(test);
    B3::Value* wrappedSum = test->appendNew<B3::Value>(proc, update.addsToLowWord ? B3::Add : B3::Sub, origin, lowWord, amountIn(test));
    appendStoreOfLowWord(test, wrappedSum);
    B3::Value* leavesWord = test->appendNew<B3::Value>(proc, B3::Below, origin, update.addsToLowWord ? wrappedSum : lowWord, amountIn(test));
    test->appendNew<B3::Value>(proc, B3::Branch, origin, leavesWord);
    test->setSuccessors(B3::FrequentedBlock(exact, B3::FrequencyClass::Rare), fastPath);
    exact->addPredecessor(test);

    B3::Value* changedWord = appendLoadOfLowWord(exact);
    appendStoreOfLowWord(exact, exact->appendNew<B3::Value>(proc, update.addsToLowWord ? B3::Sub : B3::Add, origin, changedWord, amountIn(exact)));
    B3::Value* oldValue = appendClone(exact, update.load);
    B3::Value* sum = appendClone(exact, update.sum);
    for (B3::Value*& child : sum->children()) {
        if (child == update.load)
            child = oldValue;
    }
    B3::Value* compared = sum;
    if (update.biasedSum) {
        compared = appendClone(exact, update.biasedSum);
        compared->child(0) = sum;
    }
    B3::Value* comparison = appendClone(exact, update.comparison);
    comparison->child(0) = compared;
    exact->appendNew<B3::Value>(proc, B3::Branch, origin, comparison);
    B3::FrequentedBlock storing(exactStore, fastPath.frequency());
    if (isTakenWhereComparisonHolds == !fastPathIndex)
        exact->setSuccessors(storing, slowPath);
    else
        exact->setSuccessors(slowPath, storing);
    exactStore->addPredecessor(exact);
    slowPath.block()->replacePredecessor(test, exact);

    appendClone(exactStore, update.store)->child(0) = sum;
    exactStore->appendNew<B3::Value>(proc, B3::Jump, origin);
    exactStore->setSuccessors(B3::FrequentedBlock(fastPath.block()));
    fastPath.block()->addPredecessor(exactStore);

    update.store->replaceWithNopIgnoringType();
    proc.invalidateCFG();
}

static void apply(B3::Procedure& proc, const Update& update)
{
    proc.movedLoadLog().willMove(update.store);
    rematerializeSumOnExits(proc, update);
    switch (update.kind) {
    case UpdateKind::CompareThenAdd:
        emitCompareThenAdd(proc, update);
        return;
    case UpdateKind::AddThenBranchOnOverflow:
        emitAddThenBranchOnOverflow(proc, update);
        return;
    case UpdateKind::AddToLowWordThenBranchOnCarry:
        emitAddToLowWordThenBranchOnCarry(proc, update);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A load of a whole slot cannot take its value from a store to the low word of the slot: it waits until that store has reached the cache, which costs a turn of a small loop
// five times over. So the low word is for a slot that nothing reads whole soon after the update: not in the loop of the update, and not on the way out of it, unless that way is rare.
static void rejectLowWordUpdatesThatWholeReadsFollow(B3::Procedure& proc, const Vector<Update, 4>& updates, const HashSet<B3::Value*>& loadsThatStay, BitVector& isRejected)
{
    HashMap<B3::Value*, Vector<unsigned, 1>> lowWordUpdatesOfBase;
    HashSet<B3::Value*> loadsOfLowWordUpdates;
    for (unsigned index = 0; index < updates.size(); ++index) {
        const Update& update = updates[index];
        if (update.kind != UpdateKind::AddToLowWordThenBranchOnCarry || isRejected.quickGet(index))
            continue;
        lowWordUpdatesOfBase.add(update.store->lastChild(), Vector<unsigned, 1> { }).iterator->value.append(index);
        if (!loadsThatStay.contains(update.load))
            loadsOfLowWordUpdates.add(update.load);
    }
    if (lowWordUpdatesOfBase.isEmpty())
        return;

    constexpr double readsPerUpdateThatCostLessThanTheLowWordSaves = 1.0 / 32;
    B3::Dominators& dominators = proc.dominators();
    B3::NaturalLoops& loops = proc.naturalLoops();
    auto readFollows = [&](const Update& update, B3::BasicBlock* reader) {
        B3::BasicBlock* writer = update.store->owner;
        if (reader->frequency() < writer->frequency() * readsPerUpdateThatCostLessThanTheLowWordSaves)
            return false;
        if (const B3::NaturalLoop* loop = loops.innerMostLoopOf(writer))
            return loops.belongsTo(reader, *loop) || dominators.dominates(writer, reader);
        return dominators.dominates(writer, reader);
    };
    for (B3::BasicBlock* block : proc) {
        for (B3::Value* value : *block) {
            auto* read = value->as<B3::MemoryValue>();
            if (!read || read->opcode() != B3::Load || read->type() != B3::Int64 || loadsOfLowWordUpdates.contains(read))
                continue;
            auto found = lowWordUpdatesOfBase.find(read->lastChild());
            if (found == lowWordUpdatesOfBase.end())
                continue;
            bool isFollowed = std::ranges::any_of(found->value, [&](unsigned index) {
                return updates[index].store->offset() == read->offset() && readFollows(updates[index], block);
            });
            if (!isFollowed)
                continue;
            for (unsigned index : found->value) {
                if (updates[index].store->offset() == read->offset())
                    isRejected.quickSet(index);
            }
        }
    }
}

static bool updateIntegersInMemory(B3::Procedure& proc, const Source& source)
{
    if (!isX86_64())
        return false;

    proc.resetValueOwners();
    Vector<Update, 4> updates;
    for (B3::BasicBlock* block : proc) {
        for (B3::Value* value : *block) {
            if (std::optional<Update> update = matchUpdate(proc, source, value))
                updates.append(WTF::move(*update));
        }
    }
    if (updates.isEmpty())
        return false;

    BitVector isRejected(updates.size());
    HashSet<B3::Value*> loadsThatStay;
    HashMap<B3::Value*, unsigned> updateForValue;
    for (unsigned index = 0; index < updates.size(); ++index) {
        const Update& update = updates[index];
        for (B3::Value* part : { static_cast<B3::Value*>(update.load), update.extended, update.sum, update.truncated, update.biasedSum }) {
            if (!part)
                continue;
            auto added = updateForValue.add(part, index);
            if (!added.isNewEntry && added.iterator->value != index) {
                isRejected.quickSet(added.iterator->value);
                isRejected.quickSet(index);
            }
        }
    }
    for (B3::BasicBlock* block : proc) {
        for (B3::Value* user : *block) {
            for (B3::Value* child : user->children()) {
                auto found = updateForValue.find(child);
                if (found == updateForValue.end())
                    continue;
                Update& update = updates[found->value];
                bool isInternalUse = (child == update.load && user == update.extended) || (child == update.extended && user == update.sum)
                    || (child == update.truncated && user == update.store) || (child == update.sum && (user == update.truncated || user == update.comparison || user == update.biasedSum))
                    || (child == update.biasedSum && user == update.comparison);
                if (isInternalUse)
                    continue;
                if (update.kind != UpdateKind::CompareThenAdd && (child == update.load || child == update.extended)) {
                    loadsThatStay.add(update.load);
                    continue;
                }
                // Applying an update changes the graph of blocks, so the exit is kept while the dominators hold.
                if (B3::BasicBlock* exit = child == update.sum ? findExitDominating(proc, update, user) : nullptr) {
                    if (update.sumUsersOnExits.isEmpty() || update.sumUsersOnExits.last().user != user)
                        update.sumUsersOnExits.append({ user, exit });
                } else
                    isRejected.quickSet(found->value);
            }
        }
    }

    rejectLowWordUpdatesThatWholeReadsFollow(proc, updates, loadsThatStay, isRejected);

    bool changed = false;
    for (unsigned index = 0; index < updates.size(); ++index) {
        if (isRejected.quickGet(index))
            continue;
        apply(proc, updates[index]);
        changed = true;
    }
    return changed;
}

bool updateIntegersInMemory(B3::Procedure& proc)
{
    return updateIntegersInMemory(proc, { });
}

B3::ClientPhase updateIntegersInMemoryAfterOptimization(const Typed::Program& program, const Typed::Function& function)
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "updateIntegersInMemory"_s,
        createSharedTask<bool(B3::Procedure&)>([&program, &function](B3::Procedure& proc) {
            return updateIntegersInMemory(proc, { &program, &function });
        })
    };
}

namespace {

// The part of a loop that is reached without a rare edge reads and writes a variable. Memory is right wherever that part is left, and is read again wherever it is entered,
// so the rest of the loop and of the function finds what it found before.
class ListLengthPromotion {
public:
    explicit ListLengthPromotion(B3::Procedure& proc)
        : m_proc(proc)
        , m_heaps(AOTHeaps::of(proc))
        , m_lengths(m_heaps.rangeOf(m_heaps.Butterfly_publicLength))
    {
    }

    bool run()
    {
        Vector<B3::BasicBlock*, 4> blocksThatStore;
        for (B3::BasicBlock* block : m_proc) {
            for (B3::Value* value : *block) {
                if (value->opcode() == B3::Store && isLengthAccess(value)) {
                    blocksThatStore.append(block);
                    break;
                }
            }
        }
        if (blocksThatStore.isEmpty() || !m_proc.hasCycle())
            return false;

        m_proc.resetValueOwners();
        B3::NaturalLoops& loops = m_proc.naturalLoops();
        Vector<Promotion, 2> promotions;
        BitVector isDecided;
        BitVector isPromoted;
        for (B3::BasicBlock* block : blocksThatStore) {
            Vector<const B3::NaturalLoop*, 4> enclosing;
            for (const B3::NaturalLoop* loop = loops.innerMostLoopOf(block); loop; loop = loops.innerMostOuterLoop(*loop))
                enclosing.append(loop);
            for (const B3::NaturalLoop* loop : enclosing | std::views::reverse) {
                if (isPromoted.get(loop->index()))
                    break;
                if (isDecided.set(loop->index()))
                    continue;
                std::optional<Promotion> promotion = plan(*loop);
                if (!promotion)
                    continue;
                isPromoted.set(loop->index());
                promotions.append(WTF::move(*promotion));
                break;
            }
        }
        if (promotions.isEmpty())
            return false;

        EdgeBlocks edges(m_proc);
        for (Promotion& promotion : promotions) {
            promotion.length = m_proc.addVariable(B3::Int32);
            rewriteAccesses(promotion);
            storeOnWaysOut(promotion, edges);
        }
        // Behind the stores: a way out of one loop may be the way into another.
        for (Promotion& promotion : promotions)
            loadOnWaysIn(promotion, edges);
        edges.connect(promotions.first().origin);

        m_proc.resetReachability();
        m_proc.invalidateCFG();
        B3::fixSSA(m_proc);
        return true;
    }

private:
    struct Promotion {
        Vector<B3::BasicBlock*, 8> blocks;
        BitVector contains;
        B3::Value* storage { nullptr };
        B3::Value::OffsetType offset { 0 };
        B3::Origin origin;
        B3::Variable* length { nullptr };
        Vector<B3::BasicBlock*, 2> blocksThatStore;
    };

    // Blocks on edges. The edges change when all of them are filled.
    class EdgeBlocks {
    public:
        explicit EdgeBlocks(B3::Procedure& proc)
            : m_proc(proc)
        {
        }

        B3::BasicBlock*& tailOn(B3::BasicBlock* from, B3::BasicBlock* to)
        {
            uint64_t key = (static_cast<uint64_t>(from->index()) << 32 | to->index()) + 1;
            auto added = m_indices.add(key, m_edges.size());
            if (added.isNewEntry) {
                B3::BasicBlock* block = m_proc.addBlock(std::min(from->frequency(), to->frequency()));
                m_edges.append({ from, to, block, block });
            }
            return m_edges[added.iterator->value].tail;
        }

        void connect(B3::Origin origin)
        {
            for (const Edge& edge : m_edges) {
                edge.tail->appendNew<B3::Value>(m_proc, B3::Jump, origin);
                edge.tail->setSuccessors(B3::FrequentedBlock(edge.to));
                edge.from->replaceSuccessor(edge.to, edge.head);
            }
        }

    private:
        struct Edge {
            B3::BasicBlock* from;
            B3::BasicBlock* to;
            B3::BasicBlock* head;
            B3::BasicBlock* tail;
        };

        B3::Procedure& m_proc;
        Vector<Edge, 8> m_edges;
        HashMap<uint64_t, unsigned> m_indices;
    };

    bool isLengthAccess(B3::Value* value) const
    {
        auto* memory = value->as<B3::MemoryValue>();
        if (!memory || memory->range() != m_lengths || memory->isExotic() || memory->traps())
            return false;
        if (memory->opcode() == B3::Load)
            return memory->type() == B3::Int32;
        return memory->opcode() == B3::Store && memory->child(0)->type() == B3::Int32;
    }

    // The collector marks the elements below the length.
    bool maySeeLength(const B3::Effects& effects) const
    {
        return effects.reads.overlaps(m_lengths) || effects.writes.overlaps(m_lengths) || effects.exitsSideways || effects.fence || m_heaps.mayCollect(effects);
    }

    // By the edges: the frequencies of blocks are not current here.
    void findBlocksReachedOften(const B3::NaturalLoop& loop, Promotion& promotion)
    {
        B3::NaturalLoops& loops = m_proc.naturalLoops();
        promotion.blocks.append(loop.header());
        promotion.contains.set(loop.header()->index());
        for (unsigned next = 0; next < promotion.blocks.size(); ++next) {
            for (const B3::FrequentedBlock& successor : promotion.blocks[next]->successors()) {
                if (successor.frequency() == B3::FrequencyClass::Rare || !loops.belongsTo(successor.block(), loop))
                    continue;
                if (!promotion.contains.set(successor.block()->index()))
                    promotion.blocks.append(successor.block());
            }
        }
    }

    // The read in front of the loop reads what the loop is going to read or write anyway, or lies in a word of which the function has read the other half.
    bool mayReadInFrontOf(B3::BasicBlock* header, const Promotion& promotion)
    {
        for (B3::Value* value : *header) {
            if (isLengthAccess(value))
                return true;
            B3::Effects effects = value->effects();
            if (effects.exitsSideways || effects.terminal)
                break;
        }
        static_assert(!(sizeof(IndexingHeader) % 8));
        B3::Dominators& dominators = m_proc.dominators();
        for (B3::BasicBlock* block = dominators.idom(header); block; block = dominators.idom(block)) {
            for (B3::Value* value : *block) {
                auto* memory = value->as<B3::MemoryValue>();
                if (memory && memory->lastChild() == promotion.storage && (memory->offset() & ~7) == (promotion.offset & ~7))
                    return true;
            }
            if (block == promotion.storage->owner)
                break;
        }
        return false;
    }

    std::optional<Promotion> plan(const B3::NaturalLoop& loop)
    {
        if (loop.header() == m_proc.at(0))
            return std::nullopt;
        Promotion promotion;
        findBlocksReachedOften(loop, promotion);
        for (B3::BasicBlock* block : promotion.blocks) {
            bool stores = false;
            for (B3::Value* value : *block) {
                if (!maySeeLength(value->effects()))
                    continue;
                if (!isLengthAccess(value))
                    return std::nullopt;
                auto* memory = value->as<B3::MemoryValue>();
                if (!promotion.storage) {
                    promotion.storage = memory->lastChild();
                    promotion.offset = memory->offset();
                    promotion.origin = memory->origin();
                    if (m_proc.naturalLoops().belongsTo(promotion.storage->owner, loop))
                        return std::nullopt;
                } else if (memory->lastChild() != promotion.storage || memory->offset() != promotion.offset)
                    return std::nullopt;
                stores |= memory->isStore();
            }
            if (stores)
                promotion.blocksThatStore.append(block);
        }
        if (promotion.blocksThatStore.isEmpty() || !mayReadInFrontOf(loop.header(), promotion))
            return std::nullopt;
        return promotion;
    }

    B3::Value* appendStore(const Promotion& promotion, B3::BasicBlock* block, B3::Value* length)
    {
        return block->appendNew<B3::MemoryValue>(m_proc, B3::Store, promotion.origin, length, promotion.storage, promotion.offset, m_lengths);
    }

    B3::Value* appendLoad(const Promotion& promotion, B3::BasicBlock* block)
    {
        return block->appendNew<B3::MemoryValue>(m_proc, B3::Load, B3::Int32, promotion.origin, promotion.storage, promotion.offset, m_lengths);
    }

    void rewriteAccesses(const Promotion& promotion)
    {
        B3::InsertionSet insertionSet(m_proc);
        for (B3::BasicBlock* block : promotion.blocks) {
            for (unsigned index = 0; index < block->size(); ++index) {
                B3::Value* value = block->at(index);
                if (!isLengthAccess(value))
                    continue;
                if (value->opcode() == B3::Load) {
                    value->replaceWithIdentity(insertionSet.insert<B3::VariableValue>(index, B3::Get, value->origin(), promotion.length));
                    continue;
                }
                m_proc.movedLoadLog().willMove(value);
                insertionSet.insert<B3::VariableValue>(index, B3::Set, value->origin(), promotion.length, value->child(0));
                value->replaceWithNop();
            }
            insertionSet.execute(block);
        }
    }

    void storeOnWaysOut(const Promotion& promotion, EdgeBlocks& edges)
    {
        B3::Dominators& dominators = m_proc.dominators();
        for (B3::BasicBlock* block : promotion.blocks) {
            bool hasStored = std::ranges::any_of(promotion.blocksThatStore, [&](B3::BasicBlock* storing) {
                return dominators.dominates(storing, block);
            });
            for (unsigned successorIndex = 0; successorIndex < block->numSuccessors(); ++successorIndex) {
                B3::BasicBlock* successor = block->successorBlock(successorIndex);
                if (promotion.contains.get(successor->index()))
                    continue;
                bool isRepeated = false;
                for (unsigned earlier = 0; earlier < successorIndex; ++earlier)
                    isRepeated |= block->successorBlock(earlier) == successor;
                if (isRepeated)
                    continue;

                B3::BasicBlock*& tail = edges.tailOn(block, successor);
                B3::Value* length = tail->appendNew<B3::VariableValue>(m_proc, B3::Get, promotion.origin, promotion.length);
                if (hasStored) {
                    appendStore(promotion, tail, length);
                    continue;
                }
                // The loop may have stored nothing so far, and then the storage may be one that nobody may write to.
                B3::BasicBlock* stores = m_proc.addBlock(tail->frequency());
                B3::BasicBlock* next = m_proc.addBlock(tail->frequency());
                tail->appendNew<B3::Value>(m_proc, B3::Branch, promotion.origin, tail->appendNew<B3::Value>(m_proc, B3::NotEqual, promotion.origin, length, appendLoad(promotion, tail)));
                tail->setSuccessors(B3::FrequentedBlock(stores), B3::FrequentedBlock(next));
                appendStore(promotion, stores, length);
                stores->appendNew<B3::Value>(m_proc, B3::Jump, promotion.origin);
                stores->setSuccessors(B3::FrequentedBlock(next));
                tail = next;
            }
        }
    }

    void loadOnWaysIn(const Promotion& promotion, EdgeBlocks& edges)
    {
        for (B3::BasicBlock* block : promotion.blocks) {
            for (B3::BasicBlock* predecessor : block->predecessors()) {
                if (promotion.contains.get(predecessor->index()))
                    continue;
                B3::BasicBlock* tail = edges.tailOn(predecessor, block);
                tail->appendNew<B3::VariableValue>(m_proc, B3::Set, promotion.origin, promotion.length, appendLoad(promotion, tail));
            }
        }
    }

    B3::Procedure& m_proc;
    const AOTHeaps& m_heaps;
    B3::HeapRange m_lengths;
};

} // anonymous namespace

bool isLengthOfList(B3::Procedure& proc, const B3::MemoryValue& memory)
{
    AOTHeaps& heaps = AOTHeaps::of(proc);
    return memory.range() == heaps.rangeOf(heaps.Butterfly_publicLength);
}

B3::ClientPhase keepListLengthsInRegistersAfterOptimization()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "keepListLengthsInRegisters"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return ListLengthPromotion(proc).run();
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
