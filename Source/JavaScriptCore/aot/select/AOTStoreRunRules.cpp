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
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RunKey {
    ValueID receiver;
    LayoutSetID receivers;
    bool isDefinition { false };

    friend bool operator==(const RunKey&, const RunKey&) = default;
};

struct RunMember {
    ValueID store;
    ValueID stored;
    PropertyKeyID key;
    ValueID guard;
    bool mayAdd { false };
};

struct StoreRun {
    SelectionGroup group;
    RunKey key;
    Vector<RunMember, 4> members;
    Opcode operation;
};

struct RunRule {
    SelectionRuleID id;
    Opcode operation;
    uint64_t minMembers;
    LimitID maxMembers;
    size_t minAddingMembers { 0 };
};

using MemberTest = ScopedLambda<std::optional<RunMember>(ValueID, const Value&, BlockID)>;

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

static SelectionGroup groupOf(std::span<const RunMember> members, Opcode operation)
{
    SelectionGroup group;
    group.root = members.back().store;
    group.hasSlowPath = operation == Opcode::TryPutByIds;
    for (const RunMember& member : members)
        group.members.append(GroupMember { member.store });
    return group;
}

static void claimLongestRuns(SelectionContext& context, const RunRule& rule, const RunKey& key, std::span<const RunMember> members, Vector<StoreRun>& runs)
{
    size_t begin = 0;
    while (begin + rule.minMembers <= members.size()) {
        size_t next = begin + 1;
        for (size_t end = members.size(); end >= begin + rule.minMembers; --end) {
            std::span<const RunMember> candidate = members.subspan(begin, end - begin);
            if (static_cast<size_t>(std::ranges::count_if(candidate, &RunMember::mayAdd)) < rule.minAddingMembers)
                break;
            SelectionGroup group = groupOf(candidate, rule.operation);
            if (context.tryClaim(rule.id, group) != ClaimResult::Claimed)
                continue;
            runs.append(StoreRun { WTF::move(group), key, Vector<RunMember, 4>(candidate), rule.operation });
            next = end;
            break;
        }
        begin = next;
    }
}

static bool continuesList(SelectionContext& context, const RunRule& rule, std::span<const RunMember> list, const RunKey& listKey, const RunKey& key, const RunMember& member)
{
    return key == listKey
        && !std::ranges::contains(list, member.key, &RunMember::key)
        && !context.pass().exceeds(rule.maxMembers, list.size() + 1, member.store);
}

static void collectRuns(SelectionContext& context, const RunRule& rule, const MemberTest& describeMember, Vector<StoreRun>& runs)
{
    RELEASE_ASSERT(rule.minMembers >= minRunMembers);
    const Function& function = context.function();
    for (BlockID block : context.analyses().cfg().reversePostOrder()) {
        Vector<RunMember, 4> list;
        RunKey listKey;
        auto closeList = [&] {
            claimLongestRuns(context, rule, listKey, list.span(), runs);
            list.shrink(0);
        };

        for (ValueID id : function.at(block).values) {
            if (context.isClaimed(id) || context.isOnSlowPath(id))
                continue;
            const Value& value = function.at(id);
            std::optional<RunMember> member = describeMember(id, value, block);
            if (!member)
                continue;
            RunKey key = runKeyOf(value);
            if (!continuesList(context, rule, list.span(), listKey, key, *member))
                closeList();
            listKey = key;
            list.append(WTF::move(*member));
        }
        closeList();
    }
}

static void setRunOperands(Value& value, const StoreRun& run)
{
    value.setObjectOperand(run.key.receiver);
    Vector<ValueID, 4> storedValues;
    for (const RunMember& member : run.members)
        storedValues.append(member.stored);
    value.setValuesOperands(storedValues.span());
}

static Vector<PlacedKey, 4> placedKeysOf(GroupRewriter& rewriter, const StoreRun& run)
{
    Vector<PlacedKey, 4> keys;
    for (const RunMember& member : run.members) {
        const Value& store = rewriter.original(member.store);
        keys.append(PlacedKey { .key = member.key, .level = store.level(), .accessSite = store.accessSite(), .mayAdd = member.mayAdd });
    }
    return keys;
}

// A store that creates a field stands under tests of its own name, so the stores of one constructor name different guards.
static ValueID insertGuardOf(GroupRewriter& rewriter, const StoreRun& run)
{
    Vector<ValueID, 4> guards;
    for (const RunMember& member : run.members) {
        if (member.guard)
            guards.appendIfNotContains(member.guard);
    }
    if (guards.isEmpty())
        return { };
    if (guards.size() == 1)
        return guards.first();

    Value allGuards = IRBuilder::make(Opcode::AllGuards);
    allGuards.setGuardsOperands(guards.span());
    return rewriter.insert(WTF::move(allGuards));
}

static void setByNameRunPayload(Value& attempt, const StoreRun& run)
{
    Vector<PropertyKeyID, 4> keys;
    for (const RunMember& member : run.members)
        keys.append(member.key);
    attempt.setKeys(keys.span());
    attempt.setIsDefinition(run.key.isDefinition);
}

static void rewriteStoreRun(GroupRewriter& rewriter, const StoreRun& run)
{
    if (run.operation == Opcode::PutSlots) {
        Vector<PlacedKey, 4> placedKeys = placedKeysOf(rewriter, run);
        ValueID guard = insertGuardOf(rewriter, run);
        Value& root = rewriter.replaceRoot(Opcode::PutSlots);
        setRunOperands(root, run);
        if (guard)
            root.setGuardOperand(guard);
        root.setPlacedKeys(placedKeys.span());
        root.setReceivers(run.key.receivers);
        return;
    }

    RELEASE_ASSERT(run.operation == Opcode::TryPutByIds);
    Value attempt = IRBuilder::make(Opcode::TryPutByIds);
    setByNameRunPayload(attempt, run);
    setRunOperands(attempt, run);
    rewriter.emitSlowPath(rewriter.insert(WTF::move(attempt)));
}

static std::optional<RunMember> slotRunMember(SelectionContext& context, ValueID id, const Value& value)
{
    if (!slotStores.contains(value.opcode) || isBorrowedInnerObject(context.function(), value.objectOperand()))
        return std::nullopt;
    const SlotAgreement& agreement = context.slots().agreementOf(context.program(), value.key(), value.receivers());
    if (!agreement.hasOneLocation() || !agreement.coversEveryMember())
        return std::nullopt;
    const PlacedSlot& slot = agreement.groups.first().slot;
    if (slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure)
        return std::nullopt;
    return RunMember { id, value.valueOperand(), value.key(), value.guardOperand(), value.opcode == Opcode::PutSlotOrAdd };
}

void selectSlotStoreRun(SelectionContext& context)
{
    if (context.candidates(Opcode::PutSlotOrAdd).size() < minAddingSlotStores)
        return;

    auto describeMember = [&](ValueID id, const Value& value, BlockID) -> std::optional<RunMember> {
        return slotRunMember(context, id, value);
    };

    Vector<StoreRun> runs;
    collectRuns(context, RunRule { SelectionRuleID::SlotStoreRun, Opcode::PutSlots, minSlotsPerRun, LimitID::MaxSelectionGroupMembers, minAddingSlotStores }, describeMember, runs);
    context.rewriteGroups(SelectionRuleID::SlotStoreRun, runs, rewriteStoreRun);
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
    if (receiver.hasOnlyLayouts(program))
        return receiver.layouts;
    LayoutSetID arriving = store.receivers();
    if (!arriving || !program.at(arriving).isComplete)
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

static std::optional<RunMember> byNameRunMember(SelectionContext& context, ValueID id, const Value& value, BlockID block)
{
    if (!storeRunOpcodes.contains(value.opcode))
        return std::nullopt;
    bool isDefinition = value.opcode == Opcode::PropDefine;
    if (!isDefinition && value.isFreeName())
        return std::nullopt;
    const Program& program = context.program();
    PropertyKeyID key = value.key();
    if (!isOrdinaryKey(program, key))
        return std::nullopt;

    CodeShape shape = context.shapeOf(block);
    const Type& receiver = context.typeOf(value.objectOperand());
    if (LayoutSetID receivers = compiledReceiversOf(program, receiver, value)) {
        if (!paysOnCompiledObject(program, receivers, key, shape, isDefinition))
            return std::nullopt;
    } else if (!isOrdinaryObjectReceiver(receiver) || !paysOnOrdinaryObject(context.function(), value.objectOperand(), receiver, shape, isDefinition))
        return std::nullopt;

    // A run may do no more than what it replaces, and an access that the types prove harmless does less than a run is declared to do.
    if (!hasEffectsOfUnknownCode(context, id) && !declaredBoundOf(Opcode::TryPutByIds).isSubsetOf(context.analyses().effectsOf(id).normal))
        return std::nullopt;
    return RunMember { .store = id, .stored = value.valueOperand(), .key = key, .guard = { } };
}

void selectPropertyRun(SelectionContext& context)
{
    uint64_t minMembers = std::max(context.pass().limit(LimitID::MinByIdsPerRun), minRunMembers);
    if (context.candidates(Opcode::PropSet).size() + context.candidates(Opcode::PropDefine).size() < minMembers)
        return;

    auto describeMember = [&](ValueID id, const Value& value, BlockID block) -> std::optional<RunMember> {
        return byNameRunMember(context, id, value, block);
    };

    Vector<StoreRun> runs;
    collectRuns(context, RunRule { SelectionRuleID::PropertyRun, Opcode::TryPutByIds, minMembers, LimitID::MaxSelectionGroupMembers }, describeMember, runs);
    context.rewriteGroups(SelectionRuleID::PropertyRun, runs, rewriteStoreRun);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
