/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTFamilyTables.h"
#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RunKey {
    ValueID receiver;
    LayoutSetID receivers;
    bool isDefinition { false };

    friend bool operator==(const RunKey&, const RunKey&) = default;
};

struct StoreRun {
    SelectionGroup group;
};

struct RunRule {
    SelectionRuleID id;
    Opcode operation;
    uint64_t minMembers;
    size_t minAddingMembers;
    bool (*canJoin)(SelectionContext&, ValueID, const Value&, BlockID);
    void (*rewrite)(GroupRewriter&, const StoreRun&);
};

} // anonymous namespace

static constexpr uint64_t minRunMembers = 1;
static constexpr uint64_t minSlotsPerRun = 2;
static_assert(minSlotsPerRun >= minRunMembers, "a run has a last member, which is the root of its group");

// A PutSlot has no test and no key order update: it joins a run that pays and never makes one pay.
AOT_THRESHOLD(size_t, minAddingSlotStores, 2);

static constexpr OpcodeSet slotStores { Opcode::PutSlot, Opcode::PutSlotOrAdd };
static constexpr OpcodeSet storeRunOpcodes { Opcode::PropSet, Opcode::PropDefine };

// The members of a family hold a field in one place, so the stores of a constructor to whichever member it allocated have one key.
static RunKey runKeyOf(const Value& store)
{
    if (slotStores.contains(store.opcode))
        return { store.objectOperand(), store.receivers() };
    return { store.objectOperand(), { }, store.opcode == Opcode::PropDefine };
}

static void claimLongestRuns(SelectionContext& context, const RunRule& rule, std::span<const ValueID> stores, Vector<StoreRun>& runs)
{
    auto mayAdd = [&](ValueID store) {
        return context.function().at(store).opcode == Opcode::PutSlotOrAdd;
    };
    size_t begin = 0;
    while (begin + rule.minMembers <= stores.size()) {
        size_t next = begin + 1;
        for (size_t end = stores.size(); end >= begin + rule.minMembers; --end) {
            std::span<const ValueID> candidate = stores.subspan(begin, end - begin);
            if (static_cast<size_t>(std::ranges::count_if(candidate, mayAdd)) < rule.minAddingMembers)
                break;
            StoreRun run { { .root = candidate.back(), .members = { }, .hasSlowPath = rule.operation == Opcode::TryPutByIds } };
            for (ValueID store : candidate)
                run.group.members.append(GroupMember { store });
            if (context.tryClaim(rule.id, run.group) != ClaimResult::Claimed)
                continue;
            runs.append(WTF::move(run));
            next = end;
            break;
        }
        begin = next;
    }
}

static void selectRuns(SelectionContext& context, const RunRule& rule)
{
    RELEASE_ASSERT(rule.minMembers >= minRunMembers);
    const Function& function = context.function();
    Vector<StoreRun> runs;
    for (BlockID block : context.analyses().cfg().reversePostOrder()) {
        Vector<ValueID, 4> list;
        RunKey listKey;
        for (ValueID id : function.at(block).values) {
            if (context.isClaimed(id) || context.isOnSlowPath(id))
                continue;
            const Value& value = function.at(id);
            if (!rule.canJoin(context, id, value, block))
                continue;
            RunKey key = runKeyOf(value);
            bool repeatsName = list.containsIf([&](ValueID store) {
                return function.at(store).key() == value.key();
            });
            if (key != listKey || repeatsName || context.pass().exceeds(LimitID::MaxSelectionGroupMembers, list.size() + 1, id)) {
                claimLongestRuns(context, rule, list.span(), runs);
                list.shrink(0);
            }
            listKey = key;
            list.append(id);
        }
        claimLongestRuns(context, rule, list.span(), runs);
    }
    context.rewriteGroups(rule.id, runs, rule.rewrite);
}

static void rewriteSlotStoreRun(GroupRewriter& rewriter, const StoreRun& run)
{
    Vector<ValueID, 4> storedValues;
    Vector<PlacedKey, 4> placedKeys;
    Vector<ValueID, 4> guards;
    for (const GroupMember& member : run.group.members) {
        const Value& store = rewriter.original(member.value);
        storedValues.append(store.valueOperand());
        placedKeys.append(PlacedKey { .key = store.key(), .level = store.level(), .accessSite = store.accessSite(), .mayAdd = store.opcode == Opcode::PutSlotOrAdd });
        if (store.guardOperand())
            guards.appendIfNotContains(store.guardOperand());
    }

    // A store that creates a field stands under tests of its own name, so the stores of one constructor name different guards.
    ValueID guard = guards.isEmpty() ? ValueID() : guards.first();
    if (guards.size() > 1) {
        Value allGuards = IRBuilder::make(Opcode::AllGuards);
        allGuards.setGuardsOperands(guards.span());
        guard = rewriter.insert(WTF::move(allGuards));
    }

    const Value& last = rewriter.original(run.group.root);
    Value& root = rewriter.replaceRoot(Opcode::PutSlots);
    root.setObjectOperand(last.objectOperand());
    root.setValuesOperands(storedValues.span());
    if (guard)
        root.setGuardOperand(guard);
    root.setPlacedKeys(placedKeys.span());
    root.setReceivers(last.receivers());
}

static bool canJoinSlotStoreRun(SelectionContext& context, ValueID, const Value& value, BlockID)
{
    if (!slotStores.contains(value.opcode))
        return false;
    const SlotAgreement& agreement = context.slots().agreementOf(context.program(), value.key(), value.receivers());
    if (!agreement.hasOneLocation() || !agreement.coversEveryMember())
        return false;
    const PlacedSlot& slot = agreement.groups.first().slot;
    return !slot.deferredFunction && slot.representation != SlotRepresentation::DeferredClosure;
}

void selectSlotStoreRun(SelectionContext& context)
{
    if (context.candidates(Opcode::PutSlotOrAdd).size() >= minAddingSlotStores)
        selectRuns(context, { SelectionRuleID::SlotStoreRun, Opcode::PutSlots, minSlotsPerRun, minAddingSlotStores, canJoinSlotStoreRun, rewriteSlotStoreRun });
}

static bool isOrdinaryObjectReceiver(const Type& receiver)
{
    if (receiver.primitives || receiver.layouts)
        return false;
    if (receiver.anyObject)
        return true;
    const ClassPart* instance = receiver.singleBuiltinClass();
    return instance && instance->builtinClass == BuiltinClasses::Object;
}

// The layouts of a receiver that is a compiled object for certain: its type lists them, or flow saw nothing else arrive at the access.
static LayoutSetID compiledReceiversOf(const Program& program, const Type& receiver, const Value& store)
{
    if (receiver.primitives)
        return { };
    if (receiver.hasOnlyLayouts())
        return receiver.layouts;
    LayoutSetID arriving = store.receivers();
    if (!arriving || !program.at(arriving).all())
        return { };
    return arriving;
}

// The emitted code of a run stores into the cell of an ordinary object, at the place its site cache remembers.
static bool paysOnOrdinaryObject(const Function& function, ValueID receiver, const Type& type, CodeShape shape, bool isDefinition)
{
    if (shape != CodeShape::Compact)
        return true;
    return isDefinition && (!type.anyObject || function.at(receiver).opcode == Opcode::NewDictionary);
}

// A compiled object keeps the names that are no fields in its extra properties, which only the operation reaches.
static bool paysOnCompiledObject(const Program& program, LayoutSetID receivers, PropertyKeyID key, CodeShape shape, bool isDefinition)
{
    return isDefinition && shape == CodeShape::Compact && !program.familyTables().memberOf(receivers, key).holders;
}

static bool canJoinPropertyRun(SelectionContext& context, ValueID id, const Value& value, BlockID block)
{
    if (!storeRunOpcodes.contains(value.opcode))
        return false;
    bool isDefinition = value.opcode == Opcode::PropDefine;
    if (!isDefinition && value.isFreeName())
        return false;
    const Program& program = context.program();
    PropertyKeyID key = value.key();
    if (!isOrdinaryKey(program, key))
        return false;
    // A run is refused whole where one of its names meets a setter. Such a store stands alone, and the stores on either side of it are runs of their own.
    if (!isDefinition && context.slots().isDefinedByDescriptor(key))
        return false;

    CodeShape shape = context.shapeOf(block);
    const Type& receiver = context.typeOf(value.objectOperand());
    if (LayoutSetID receivers = compiledReceiversOf(program, receiver, value)) {
        if (!paysOnCompiledObject(program, receivers, key, shape, isDefinition))
            return false;
    } else if (!isOrdinaryObjectReceiver(receiver) || !paysOnOrdinaryObject(context.function(), value.objectOperand(), receiver, shape, isDefinition))
        return false;

    // A run may do no more than what it replaces, and an access that the types prove harmless does less than a run is declared to do.
    return hasEffectsOfUnknownCode(context, id) || declaredBoundOf(Opcode::TryPutByIds).isSubsetOf(context.analyses().effectsOf(id).normal);
}

static void rewritePropertyRun(GroupRewriter& rewriter, const StoreRun& run)
{
    Vector<PropertyKeyID, 4> keys;
    Vector<ValueID, 4> storedValues;
    for (const GroupMember& member : run.group.members) {
        const Value& store = rewriter.original(member.value);
        RELEASE_ASSERT(storeRunOpcodes.contains(store.opcode));
        keys.append(store.key());
        storedValues.append(store.valueOperand());
    }

    const Value& last = rewriter.original(run.group.root);
    Value attempt = IRBuilder::make(Opcode::TryPutByIds);
    attempt.setKeys(keys.span());
    attempt.setIsDefinition(last.opcode == Opcode::PropDefine);
    attempt.setObjectOperand(last.objectOperand());
    attempt.setValuesOperands(storedValues.span());
    rewriter.emitSlowPath(rewriter.insert(WTF::move(attempt)));
}

void selectPropertyRun(SelectionContext& context)
{
    uint64_t minMembers = std::max(context.pass().limit(LimitID::MinByIdsPerRun), minRunMembers);
    if (context.candidates(Opcode::PropSet).size() + context.candidates(Opcode::PropDefine).size() >= minMembers)
        selectRuns(context, { SelectionRuleID::PropertyRun, Opcode::TryPutByIds, minMembers, 0, canJoinPropertyRun, rewritePropertyRun });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
