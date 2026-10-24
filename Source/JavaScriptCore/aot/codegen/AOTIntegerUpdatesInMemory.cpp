/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIntegerUpdatesInMemory.h"

#if ENABLE(AOT)

#include "AOTThreshold.h"
#include "AOTVector.h"
#include "B3BasicBlockInlines.h"
#include "B3Const32Value.h"
#include "B3DominatingConditions.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3MemoryValue.h"
#include "B3Procedure.h"
#include "B3ValueInlines.h"
#include <limits>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/SharedTask.h>

namespace JSC { namespace AOT {

namespace {

enum class UpdateKind : uint8_t { CompareThenAdd, AddThenBranchOnOverflow };

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
    Vector<B3::BasicBlock*, 4> path;
    Vector<B3::Value*, 2> sumUsersOnExits;
};

struct StoredRange {
    std::optional<int64_t> min;
    std::optional<int64_t> max;
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

static std::optional<StoredRange> computeStoredRange(const Update& update, B3::BasicBlock* test, B3::DominatingCondition condition)
{
    B3::Value* comparison = condition.value;
    B3::Opcode opcode = comparison->opcode();
    if (!isSignedOrder(opcode) && !comparesUnsignedOrder(opcode))
        return std::nullopt;
    if (test->successorBlock(0) == test->successorBlock(1) || !comparison->child(1)->hasInt64())
        return std::nullopt;
    int64_t bound = comparison->child(1)->asInt64();
    if (!fitsInt32(bound >> 2))
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
    if (biased->opcode() != B3::Add || biased->child(0) != update.sum || !biased->child(1)->hasInt64() || !fitsInt32(biased->child(1)->asInt64() >> 2))
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

static std::optional<Update> matchUpdate(B3::Procedure& proc, B3::Value* value)
{
    AOT_THRESHOLD(unsigned, maxPathLength, 4);
    auto* store = value->as<B3::MemoryValue>();
    if (!store || store->opcode() != B3::Store || store->child(0)->opcode() != B3::Trunc)
        return std::nullopt;
    B3::Value* truncated = store->child(0);
    B3::Value* sum = truncated->child(0);
    if (sum->opcode() != B3::Add && sum->opcode() != B3::Sub)
        return std::nullopt;

    auto matchExtendedLoad = [&](B3::Value* extended) -> B3::MemoryValue* {
        if (extended->opcode() != B3::SExt32)
            return nullptr;
        auto* load = extended->child(0)->as<B3::MemoryValue>();
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

    Update update { UpdateKind::CompareThenAdd, store, truncated, sum, extended, load, nullptr, nullptr, addend, addend->hasInt64() ? static_cast<int32_t>(addend->asInt64()) : 0, 0, std::nullopt, { }, { } };
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
        if (tryUseOverflowFlag(proc, update, *storedRange))
            return update;
        update.biasedSum = nullptr;
    }

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
    for (B3::Value* user : update.sumUsersOnExits) {
        B3::BasicBlock* exit = findExitDominating(proc, update, user);
        B3::Value* sum = sumForExit.ensure(exit, [&] {
            unsigned index = 0;
            while (exit->at(index)->opcode() == B3::Phi)
                ++index;
            B3::Value* oldValue = proc.clone(update.load);
            insertionSet.insertValue(index, oldValue);
            B3::Value* extended = proc.clone(update.extended);
            extended->child(0) = oldValue;
            insertionSet.insertValue(index, extended);
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
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool updateIntegersInMemory(B3::Procedure& proc)
{
    if (!isX86_64())
        return false;

    proc.resetValueOwners();
    Vector<Update, 4> updates;
    for (B3::BasicBlock* block : proc) {
        for (B3::Value* value : *block) {
            if (std::optional<Update> update = matchUpdate(proc, value))
                updates.append(WTF::move(*update));
        }
    }
    if (updates.isEmpty())
        return false;

    BitVector isRejected(updates.size());
    HashMap<B3::Value*, unsigned> updateForValue;
    for (unsigned index = 0; index < updates.size(); ++index) {
        const Update& update = updates[index];
        for (B3::Value* part : { static_cast<B3::Value*>(update.load), update.extended, update.sum, update.truncated, update.biasedSum }) {
            if (!part)
                continue;
            auto added = updateForValue.add(part, index);
            if (!added.isNewEntry) {
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
                if (update.kind == UpdateKind::AddThenBranchOnOverflow && (child == update.load || child == update.extended))
                    continue;
                if (child == update.sum && findExitDominating(proc, update, user))
                    update.sumUsersOnExits.appendIfNotContains(user);
                else
                    isRejected.quickSet(found->value);
            }
        }
    }

    bool changed = false;
    for (unsigned index = 0; index < updates.size(); ++index) {
        if (isRejected.quickGet(index))
            continue;
        apply(proc, updates[index]);
        changed = true;
    }
    return changed;
}

B3::ClientPhase updateIntegersInMemoryAfterOptimization()
{
    return {
        B3::ClientPhase::Point::AfterOptimization,
        "updateIntegersInMemory"_s,
        createSharedTask<bool(B3::Procedure&)>([](B3::Procedure& proc) {
            return updateIntegersInMemory(proc);
        })
    };
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
