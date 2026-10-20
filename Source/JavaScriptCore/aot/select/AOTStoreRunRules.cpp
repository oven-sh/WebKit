/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTRuleSupport.h"
#include "AOTSelectionContext.h"
#include "AOTSlotPlan.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RunKey {
    ValueID receiver;
    ValueID guard;
    LayoutSetID receivers;

    friend bool operator==(const RunKey&, const RunKey&) = default;
};

struct RunMember {
    ValueID store;
    ValueID stored;
    PropertyKeyID key;
    bool mayAdd { false };
};

struct StoreRun {
    SelectionGroup group;
    ValueID receiver;
    ValueID guard;
    LayoutSetID receivers;
    Vector<RunMember, 4> members;
    Opcode operation;
};

struct RunRule {
    SelectionRuleID id;
    Opcode operation;
    uint64_t minMembers;
    LimitID maxMembers;
    size_t minAddingMembers { 0 };
    bool appliesToCompactCode { true };
};

using MemberTest = ScopedLambda<std::optional<RunMember>(ValueID, const Value&)>;

} // anonymous namespace

static constexpr uint64_t minRunMembers = 1;
static constexpr uint64_t minSlotsPerRun = 2;
static_assert(minSlotsPerRun >= minRunMembers, "a run has a last member, which is the root of its group");

// A PutSlot has no test and no key order update: it joins a run that pays and never makes one pay.
static constexpr size_t minAddingSlotStores = 2;

static constexpr OpcodeSet slotStores { Opcode::PutSlot, Opcode::PutSlotOrAdd };

static RunKey runKeyOf(const Value& store)
{
    if (slotStores.contains(store.opcode))
        return { store.objectOperand(), store.guardOperand(), store.receivers() };
    return { store.objectOperand(), { }, { } };
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
            runs.append(StoreRun { WTF::move(group), key.receiver, key.guard, key.receivers, Vector<RunMember, 4>(candidate), rule.operation });
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
        if (!rule.appliesToCompactCode && context.shapeOf(block) == CodeShape::Compact)
            continue;
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
            std::optional<RunMember> member = describeMember(id, value);
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
    value.setObjectOperand(run.receiver);
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

static void setByNameRunPayload(Value& attempt, const StoreRun& run)
{
    Vector<PropertyKeyID, 4> keys;
    for (const RunMember& member : run.members)
        keys.append(member.key);
    attempt.setKeys(keys.span());
}

static void rewriteRun(GroupRewriter& rewriter, const StoreRun& run)
{
    if (run.operation == Opcode::PutSlots) {
        Vector<PlacedKey, 4> placedKeys = placedKeysOf(rewriter, run);
        Value& root = rewriter.replaceRoot(Opcode::PutSlots);
        setRunOperands(root, run);
        if (run.guard)
            root.setGuardOperand(run.guard);
        root.setPlacedKeys(placedKeys.span());
        root.setReceivers(run.receivers);
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
    if (!slotStores.contains(value.opcode))
        return std::nullopt;
    SlotAgreement agreement = context.slots().agreementOf(context.program(), value.key(), value.receivers());
    if (!agreement.hasOneLocation() || !agreement.coversEveryMember())
        return std::nullopt;
    const PlacedSlot& slot = agreement.groups.first().slot;
    if (slot.deferredFunction || slot.representation == SlotRepresentation::DeferredClosure)
        return std::nullopt;
    return RunMember { id, value.valueOperand(), value.key(), value.opcode == Opcode::PutSlotOrAdd };
}

void selectSlotStoreRun(SelectionContext& context)
{
    if (context.candidates(Opcode::PutSlotOrAdd).size() < minAddingSlotStores)
        return;

    auto describeMember = [&](ValueID id, const Value& value) -> std::optional<RunMember> {
        return slotRunMember(context, id, value);
    };

    Vector<StoreRun> runs;
    collectRuns(context, RunRule { SelectionRuleID::SlotStoreRun, Opcode::PutSlots, minSlotsPerRun, LimitID::MaxSelectionGroupMembers, minAddingSlotStores }, describeMember, runs);
    context.rewriteGroups(SelectionRuleID::SlotStoreRun, runs, rewriteRun);
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

static std::optional<RunMember> byNameRunMember(SelectionContext& context, ValueID id, const Value& value)
{
    if (value.opcode != Opcode::PropSet || value.isFreeName())
        return std::nullopt;
    if (!isOrdinaryObjectReceiver(context.typeOf(value.objectOperand())))
        return std::nullopt;
    PropertyKeyID key = value.key();
    if (!isOrdinaryKey(context.program(), key))
        return std::nullopt;
    return RunMember { id, value.valueOperand(), key };
}

void selectPropertyRun(SelectionContext& context)
{
    uint64_t minMembers = std::max(context.pass().limit(LimitID::MinByIdsPerRun), minRunMembers);
    if (context.candidates(Opcode::PropSet).size() < minMembers)
        return;

    auto describeMember = [&](ValueID id, const Value& value) -> std::optional<RunMember> {
        return byNameRunMember(context, id, value);
    };

    // A property run pays through its site cache, which Compact code does not probe.
    Vector<StoreRun> runs;
    collectRuns(context, RunRule { .id = SelectionRuleID::PropertyRun, .operation = Opcode::TryPutByIds, .minMembers = minMembers, .maxMembers = LimitID::MaxSelectionGroupMembers, .appliesToCompactCode = false }, describeMember, runs);
    context.rewriteGroups(SelectionRuleID::PropertyRun, runs, rewriteRun);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
