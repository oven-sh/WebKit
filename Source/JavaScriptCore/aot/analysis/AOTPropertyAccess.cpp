/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccess.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTSlotRepresentation.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet namedAccesses {
    Opcode::PropGet, Opcode::PropSet, Opcode::PropDefine, Opcode::PropHas, Opcode::PropHasOwn,
    Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas,
};

static constexpr bool isPrivateAccess(Opcode opcode) { return familyOf(opcode) == OperationFamily::PrivateAccess; }

static constexpr bool hasNamedAccessColumns(Opcode opcode)
{
    Access access = accessOf(opcode);
    if (access == Access::None || !receiverOf(opcode) || !PayloadWord::accessSite(opcode))
        return false;
    bool storesValue = access == Access::Store || access == Access::Define;
    if (storesValue != storedOf(opcode).has_value())
        return false;
    if (isPrivateAccess(opcode))
        return PayloadWord::layout(opcode) && PayloadWord::field(opcode);
    return PayloadWord::key(opcode).has_value();
}

static constexpr bool haveNamedAccessColumns(const OpcodeSet& opcodes)
{
    bool result = true;
    for (Opcode opcode : allMembersOf<Opcode>)
        result &= !opcodes.contains(opcode) || hasNamedAccessColumns(opcode);
    return result;
}

static_assert(haveNamedAccessColumns(namedAccesses));

static bool namesOperation(AccessForm form)
{
    switch (form) {
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Variable:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
        return true;
    case AccessForm::ByName:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpRequiredProofs(PrintStream& out, ProofSet proofs)
{
    CommaPrinter comma;
    out.print("[");
    for (Proof proof : allMembersOf<Proof>) {
        if (proofs.contains(proof))
            out.print(comma, nameOf(proof));
    }
    out.print("]");
}

void PropertyAccessPlan::dump(PrintStream& out) const
{
    out.print("form=", enumName(form), " level=", nameOf(level));
    if (namesOperation(form)) {
        out.print(" operation=", nameOf(operation));
        if (operation == Opcode::Boolean)
            out.print(" value=", booleanConstant ? "true"_s : "false"_s);
    }
    out.print(" receivers=");
    dumpLayouts(out, receivers);
    if (receiverTest != ReceiverTest::None)
        out.print(" receiverTest=", enumName(receiverTest));
    if (testedClassObject)
        out.print(" classObject=F", testedClassObject.index());
    if (!requiredProofs.isEmpty()) {
        out.print(" proofs=");
        dumpRequiredProofs(out, requiredProofs);
    }
    if (testsDeclaredType)
        out.print(" testsDeclaredType");
    if (!conditions.isEmpty()) {
        out.print(" conditions=");
        dumpConditions(out, conditions);
    }
    if (stores())
        out.print(storedValueFits ? " storedValueFits"_s : " testsStoredValue"_s);
    if (needsMissPath)
        out.print(" missPath");
    if (member.isKnown())
        out.print(" member=(", member, ")");
    if (cause != UnprovenCause::Unknown)
        out.print(" cause=", nameOf(cause));
    if (limit != LimitID::None)
        out.print(" limit=", nameOf(limit));
}

bool isNamedAccess(Opcode opcode)
{
    return namedAccesses.contains(opcode);
}

PropertyKeyID keyOfNamedAccess(const Program& program, const Value& access)
{
    if (PayloadWord::key(access.opcode))
        return access.key();
    RELEASE_ASSERT(isPrivateAccess(access.opcode));
    return fieldAt(program, { access.layout(), access.field() }).key;
}

bool needsNoClosure(const Program& program, FunctionID id)
{
    const Function& function = program.at(id);
    return function.captures.isEmpty() && !function.has(FunctionFlags::TakesCallee);
}

bool isAllocationListing(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    const Value& allocation = function.at(sameObjectThroughCasts(function, object));
    if (allocation.opcode != Opcode::NewObject)
        return false;
    std::optional<uint32_t> field = program.at(allocation.layout()).findField(key);
    return field && std::ranges::contains(allocation.fields(), *field);
}

bool isUnfinishedAllocationLeavingOut(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    const Value& allocation = function.at(object);
    if (allocation.opcode != Opcode::NewObject)
        return false;
    std::optional<uint32_t> field = program.at(allocation.layout()).findField(key);
    return field && !std::ranges::contains(allocation.fields(), *field);
}

// The closure of one function, which is the constructor of a class: what the value is expected to be where its type says less.
static FunctionID expectedClassObjectOf(const Program& program, const Function& function, ValueID value)
{
    TypeID expected = function.at(value).expected();
    if (!expected || !classOfClassObject(program, program.at(expected)))
        return { };
    return program.at(expected).singleFunction();
}

std::optional<PropertyAccessQuery> propertyAccessQueryOf(const Program& program, const Function& function, const Value& access, ExpectedClassObjects expectedClassObjects, PredictedReceivers predictedReceivers)
{
    Opcode opcode = access.opcode;
    if (!isNamedAccess(opcode))
        return std::nullopt;
    if (FlagMask::isFreeName(opcode) && access.isFreeName())
        return std::nullopt;

    PropertyAccessQuery query;
    query.access = accessOf(opcode);
    query.key = keyOfNamedAccess(program, access);
    query.isOwnLookup = opcode == Opcode::PropHasOwn;
    query.site = access.accessSite();
    query.module = function.module;
    if (std::optional<uint8_t> stored = storedOf(opcode))
        query.stored = &program.typeOf(function, access.operand(*stored));
    if (isPrivateAccess(opcode)) {
        query.isPrivate = true;
        query.declaringLayout = access.layout();
    }

    ValueID receiver = access.operand(*receiverOf(opcode));
    const Type& receiverType = program.typeOf(function, receiver);
    if (LayoutID classLayout = classOfClassObject(program, receiverType)) {
        query.start = MemberStart::ClassObject;
        query.receivers.proof = ReceiverProof::Type;
        query.receivers.level = KnowledgeLevel::Proven;
        query.receivers.proven.add(classLayout);
        query.receivers.mayBeNullish = receiverType.mayBeUndefinedOrNull();
        return query;
    }
    if (FunctionID classObject = expectedClassObjects == ExpectedClassObjects::Tested ? expectedClassObjectOf(program, function, receiver) : FunctionID()) {
        query.start = MemberStart::ClassObject;
        query.expectedClassObject = classObject;
        query.receivers.level = KnowledgeLevel::Expected;
        query.receivers.predicted.add(program.at(classObject).constructedClass);
        return query;
    }

    query.start = MemberStart::Instance;
    if (predictedReceivers == PredictedReceivers::Listed) {
        LayoutSetID stamp = PayloadWord::receivers(opcode) ? access.receivers() : LayoutSetID();
        query.receivers = receiverCandidates(program, function, receiver, stamp);
    } else {
        query.receivers = candidatesOf(program, receiverType, nullptr, { });
        query.receivers.predicted.clear();
    }
    query.isListedByAllocation = isAllocationListing(program, function, receiver, query.key);
    query.isLeftOutByUnfinishedAllocation = isUnfinishedAllocationLeavingOut(program, function, receiver, query.key);
    return query;
}

static void setUndefinedConstant(PropertyAccessPlan& plan)
{
    plan.form = AccessForm::Constant;
    plan.operation = Opcode::Undefined;
}

static void setBooleanConstant(PropertyAccessPlan& plan, bool value)
{
    plan.form = AccessForm::Constant;
    plan.operation = Opcode::Boolean;
    plan.booleanConstant = value;
}

static void planAbsentArm(const PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    switch (query.access) {
    case Access::Read:
        if (!query.isPrivate)
            setUndefinedConstant(plan);
        return;
    case Access::Has:
        setBooleanConstant(plan, false);
        return;
    case Access::Store:
    case Access::Define:
        return;
    case Access::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void planKnownValueArm(const PropertyAccessQuery& query, AccessForm readForm, PropertyAccessPlan& plan)
{
    switch (query.access) {
    case Access::Read:
        plan.form = readForm;
        return;
    case Access::Has:
        setBooleanConstant(plan, true);
        return;
    case Access::Store:
    case Access::Define:
        return;
    case Access::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void planAccessorArm(const PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    switch (query.access) {
    case Access::Read:
        if (plan.member.getter)
            plan.form = AccessForm::Call;
        else
            setUndefinedConstant(plan);
        return;
    case Access::Has:
        setBooleanConstant(plan, true);
        return;
    case Access::Store:
        if (plan.member.setter)
            plan.form = AccessForm::Call;
        return;
    case Access::Define:
        return;
    case Access::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isVariablePropertyPresent(const Program& program, const PropertyAccessQuery& query, VariableID id)
{
    const Variable& variable = program.at(id);
    bool isInitialized = variable.flow.initialization != InitializationState::Unknown;
    bool isStaticField = !!variable.owner;
    if (isStaticField)
        return isInitialized;
    bool readsBinding = query.isOwnLookup;
    return !readsBinding || isInitialized || variable.initialization != VariableInitialization::Uninitialized;
}

static void planVariableMember(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, PropertyAccessPlan& plan)
{
    if (query.access != Access::Has) {
        planVariableArm(context, query, arm, plan);
        return;
    }
    if (isVariablePropertyPresent(context.program, query, arm.answer.variable))
        setBooleanConstant(plan, true);
    else
        plan.cause = UnprovenCause::PropertyAdded;
}

static void resetToByName(PropertyAccessPlan& plan)
{
    plan.operation = Opcode::Undefined;
    plan.booleanConstant = false;
    plan.storedValueFits = false;
    plan.capturesReceiver = false;
    plan.emptySlotMeansNoOwnProperty = false;
    plan.testsDeclaredType = false;
    plan.requiredProofs = { };
    plan.conditions.clear();
}

// `o.read === read`, `typeof o.then`, `o.run.bind(o)`: the value is the closure that the closure table holds for the method.
static bool mayBeReadAsValue(const Program& program, const PropertyAccessQuery& query, const MemberAnswer& answer)
{
    return !query.isPrivate && answer.holder && isEvaluatedOnce(program, program.at(answer.holder));
}

// A function that takes its callee gets hold of the closure it is entered through, as a read of the value does.
static bool isReadWithoutLookup(const Program& program, const PropertyAccessQuery& query, const MemberAnswer& answer)
{
    if (!program.at(answer.method).captures.isEmpty())
        return false;
    return (needsNoClosure(program, answer.method) && query.readIsOnlyCalled()) || mayBeReadAsValue(program, query, answer);
}

PropertyAccessPlan planMemberArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm)
{
    RELEASE_ASSERT(query.access != Access::None);
    const Program& program = context.program;
    const MemberAnswer& answer = arm.answer;

    PropertyAccessPlan plan;
    plan.member = answer;
    plan.receivers = arm.layouts;
    plan.requiredProofs = answer.receiverProofs;
    plan.conditions = answer.conditions;
    plan.cause = answer.cause;

    switch (answer.kind) {
    case MemberKind::Unknown:
        break;
    case MemberKind::Absent:
        planAbsentArm(query, plan);
        break;
    case MemberKind::Field:
        planFieldArm(context, query, arm, plan);
        break;
    case MemberKind::Method:
        planKnownValueArm(query, isReadWithoutLookup(program, query, answer) ? AccessForm::Method : AccessForm::ByName, plan);
        break;
    case MemberKind::Accessor:
        planAccessorArm(query, plan);
        break;
    case MemberKind::Variable:
        planVariableMember(context, query, arm, plan);
        break;
    case MemberKind::Namespace:
        planKnownValueArm(query, AccessForm::Namespace, plan);
        break;
    case MemberKind::Builtin:
        planKnownValueArm(query, isObjectKind(program.builtins().at(answer.builtin).kind) ? AccessForm::Builtin : AccessForm::ByName, plan);
        break;
    }

    if (plan.form == AccessForm::ByName) {
        resetToByName(plan);
        return plan;
    }
    plan.needsMissPath = !plan.requiredProofs.isEmpty() || !plan.conditions.isEmpty() || (plan.stores() && !plan.storedValueFits);
    return plan;
}

namespace {

enum class MemberSource : uint8_t { None, Proven, Expected };

struct MemberSelection {
    MemberSource source { MemberSource::None };
    UnprovenCause cause { UnprovenCause::Unknown };
    LimitID limit { LimitID::None };
};

} // namespace

static MemberSelection selectMembers(const PropertyAccessQuery& query)
{
    const ReceiverCandidates& receivers = query.receivers;
    bool canPlanExpected = !receivers.predicted.isEmpty() && receivers.predicted.size() <= query.maxMembers;
    MemberSelection overLimit { canPlanExpected ? MemberSource::Expected : MemberSource::None, UnprovenCause::LimitReached, LimitID::MaxAccessPlanMembers };

    // Proven members are grouped by where they find the name, once for each list and name, however many they are. What costs code is the groups: MaxDispatchCases bounds those.
    if (receivers.isProven())
        return receivers.proven.isEmpty() ? MemberSelection { } : MemberSelection { MemberSource::Proven };
    if (receivers.predicted.isEmpty())
        return { MemberSource::None, UnprovenCause::UnknownObject };
    if (!canPlanExpected)
        return overLimit;
    return { MemberSource::Expected };
}

static MemberQuery memberQueryFor(const PropertyAccessQuery& query, LayoutID layout)
{
    return { query.start, layout, query.key, query.access, query.isOwnLookup };
}

static MemberPartition partitionClassObject(const PropertyAccessContext& context, const PropertyAccessQuery& query, const LayoutList& members)
{
    RELEASE_ASSERT(members.size() == 1);
    MemberPartition partition;
    MemberAnswer answer = findMember(context, memberQueryFor(query, members[0]));
    if (answer.isKnown())
        partition.arms.append(MemberArm { WTF::move(answer), members });
    else
        partition.rest = members;
    return partition;
}

// One store into several layouts asks its value to be what the slot of each of them holds.
static LayoutList findSlotsThatHoldOneValue(const Program& program, const LayoutList& layouts, PropertyKeyID key)
{
    LayoutList found;
    for (LayoutID layout : layouts) {
        std::optional<FieldReference> reference = findField(program, layout, key, BaseChain::Proven);
        if (!reference)
            continue;
        Type held = program.at(slotTypeOf(program, fieldAt(program, *reference))).withoutNullish();
        if (held.isNever() || held.isSingleValue())
            found.add(layout);
    }
    return found;
}

static void separateSlotsThatHoldOneValue(const Program& program, const PropertyAccessQuery& query, MemberPartition& partition)
{
    size_t numberOfArms = partition.arms.size();
    for (size_t index = 0; index < numberOfArms; ++index) {
        if (partition.arms[index].answer.kind != MemberKind::Field)
            continue;
        const LayoutList& layouts = partition.arms[index].layouts;
        std::optional<LayoutList> computed;
        const LayoutList* narrow = nullptr;
        PropertyAccessCache* cache = program.propertyAccessCache();
        if (cache && layouts.size() >= PropertyAccessCache::fewestLayouts && !TypeTable::isProvisional(query.key)) {
            PropertyAccessCache::DeclaredTypesKey key { cache->lists.idOf(layouts.span()), query.key };
            narrow = &findOrCompute(cache->narrowSlots, key, computeHash(key.layouts, key.key), [&] {
                return PropertyAccessCache::NarrowSlots { key, findSlotsThatHoldOneValue(program, layouts, query.key) };
            }).layouts;
        } else
            narrow = &computed.emplace(findSlotsThatHoldOneValue(program, layouts, query.key));
        LayoutList separated;
        for (LayoutID layout : *narrow) {
            std::optional<FieldReference> reference = findField(program, layout, query.key, BaseChain::Proven);
            if (!query.stored->isSubsetOf(program.at(slotTypeOf(program, fieldAt(program, *reference))), program))
                separated.add(layout);
        }
        if (separated.isEmpty() || separated.size() == partition.arms[index].layouts.size())
            continue;
        partition.arms[index].layouts.subtract(separated);
        partition.arms.append(MemberArm { partition.arms[index].answer, WTF::move(separated) });
    }
}

static MemberPartition partitionFor(const PropertyAccessContext& context, const PropertyAccessQuery& query, const LayoutList& members)
{
    switch (query.start) {
    case MemberStart::Instance: {
        if (query.isOwnLookup)
            return partitionOwnMembers(context, members.span(), query.key);
        MemberPartition partition = partitionMembers(context, members.span(), query.key, query.access);
        if (query.access == Access::Store && query.stored)
            separateSlotsThatHoldOneValue(context.program, query, partition);
        return partition;
    }
    case MemberStart::ClassObject:
        return partitionClassObject(context, query, members);
    case MemberStart::ClassPrototype:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static UnprovenCause causeOfRest(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberPartition& partition)
{
    if (partition.rest.isEmpty())
        return UnprovenCause::Unknown;
    return findMember(context, memberQueryFor(query, partition.rest[0])).cause;
}

static PropertyAccessPlan planProvenMembers(const PropertyAccessContext& context, const PropertyAccessQuery& query, const LayoutList& members, const MemberPartition& partition)
{
    if (partition.hasOneArm())
        return planMemberArm(context, query, partition.arms[0]);

    bool canSplitOffArm = std::ranges::any_of(partition.arms, [&](const MemberArm& arm) {
        return planMemberArm(context, query, arm).isResolved();
    });
    PropertyAccessPlan plan;
    plan.form = canSplitOffArm ? AccessForm::Split : AccessForm::ByName;
    plan.receivers = members;
    plan.cause = causeOfRest(context, query, partition);
    return plan;
}

static void setReceiverTest(const PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    plan.needsMissPath = true;
    switch (query.start) {
    case MemberStart::Instance:
        plan.receiverTest = ReceiverTest::Type;
        return;
    case MemberStart::ClassObject:
        RELEASE_ASSERT(query.expectedClassObject);
        plan.receiverTest = ReceiverTest::ClassObject;
        plan.testedClassObject = query.expectedClassObject;
        return;
    case MemberStart::ClassPrototype:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static PropertyAccessPlan planExpectedMembers(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberPartition& partition)
{
    // Whoever names a member expects to find it: the receivers that lack it come last, however many they are.
    PropertyAccessPlan heaviest;
    std::tuple<bool, size_t, bool> weightOfHeaviest { false, 0, false };
    for (const MemberArm& arm : partition.arms) {
        PropertyAccessPlan plan = planMemberArm(context, query, arm);
        if (!plan.isResolved())
            continue;
        bool hasLayoutOfModule = query.module && std::ranges::any_of(plan.receivers, [&](LayoutID layout) {
            return context.program.at(layout).module == query.module;
        });
        std::tuple<bool, size_t, bool> weight { arm.answer.kind != MemberKind::Absent, plan.receivers.size(), hasLayoutOfModule };
        if (weight <= weightOfHeaviest)
            continue;
        heaviest = WTF::move(plan);
        weightOfHeaviest = weight;
    }
    if (!heaviest.isResolved()) {
        heaviest.cause = partition.arms.isEmpty() ? causeOfRest(context, query, partition) : partition.arms[0].answer.cause;
        return heaviest;
    }
    setReceiverTest(query, heaviest);
    return heaviest;
}

// Undefined and null are members of every type of objects. The access throws for either, which is what the operation by name behind the failed test does.
static void addNullishProof(const PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    if (!query.receivers.mayBeNullish || !plan.isResolved())
        return;
    plan.requiredProofs.add(Proof::NotNullish);
    plan.needsMissPath = true;
}

static bool isBoundToInstance(const PropertyAccessQuery& query, const PropertyAccessPlan& plan)
{
    if (query.isPrivate || !plan.conditions.isEmpty())
        return true;
    switch (plan.form) {
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
        return true;
    case AccessForm::ByName:
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Split:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addInstanceProof(const PropertyAccessContext& context, const PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    // Only a closure of this Instance passes the test for a class object.
    if (query.start == MemberStart::ClassObject)
        return;
    bool receiverIsTested = plan.receiverTest != ReceiverTest::None || query.receivers.proof == ReceiverProof::Guard;
    // A permanent test gives the receiver a type that lists its layouts, and every later plan takes such a receiver for an object of this Instance.
    bool narrowsReceiverType = plan.receiverTest == ReceiverTest::Type;
    if (!plan.isResolved() || !receiverIsTested || !(narrowsReceiverType || isBoundToInstance(query, plan)))
        return;
    if (instanceCountOf(context) == InstanceCount::AtMostOne)
        return;
    plan.requiredProofs.add(Proof::OwnInstance);
    plan.needsMissPath = true;
}

static KnowledgeLevel levelOf(const PropertyAccessPlan& plan, KnowledgeLevel memberLevel)
{
    switch (plan.form) {
    case AccessForm::ByName:
        return KnowledgeLevel::Unknown;
    case AccessForm::Split:
        return memberLevel;
    case AccessForm::Slot:
    case AccessForm::Constant:
    case AccessForm::Variable:
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
        return plan.needsNoTest() ? memberLevel : KnowledgeLevel::Expected;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

PropertyAccessPlan planPropertyAccess(const PropertyAccessContext& context, const PropertyAccessQuery& query)
{
    PropertyAccessPlan plan;
    if (keyLookupOf(context.program, query.key) != KeyLookup::Ordinary)
        return plan;

    const ReceiverCandidates& receivers = query.receivers;
    if (receivers.isProven() && receivers.hasOtherParts) {
        plan.form = AccessForm::Split;
        plan.level = receivers.level;
        plan.receivers = receivers.proven;
        return plan;
    }

    MemberSelection selection = selectMembers(query);
    MemberPartition partition;
    KnowledgeLevel memberLevel = KnowledgeLevel::Unknown;
    switch (selection.source) {
    case MemberSource::None:
        break;
    case MemberSource::Proven:
        partition = partitionFor(context, query, receivers.proven);
        plan = planProvenMembers(context, query, receivers.proven, partition);
        addNullishProof(query, plan);
        memberLevel = receivers.level;
        break;
    case MemberSource::Expected:
        partition = partitionFor(context, query, receivers.predicted);
        plan = planExpectedMembers(context, query, partition);
        memberLevel = KnowledgeLevel::Expected;
        break;
    }
    if (selection.cause != UnprovenCause::Unknown)
        plan.cause = selection.cause;
    plan.limit = selection.limit;

    addInstanceProof(context, query, plan);
    plan.partition = WTF::move(partition);
    plan.level = levelOf(plan, memberLevel);
    return plan;
}

LimitID limitOfPropertyAccessPlan(const Program& program, const PropertyAccessQuery& query)
{
    if (keyLookupOf(program, query.key) != KeyLookup::Ordinary)
        return LimitID::None;
    if (query.receivers.isProven() && query.receivers.hasOtherParts)
        return LimitID::None;
    return selectMembers(query).limit;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
