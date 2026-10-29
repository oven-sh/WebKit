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
#include "AOTSlotRepresentation.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <utility>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

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

namespace {

enum class AccessFormTrait : uint8_t {
    IsResolved = 1 << 0,
    NamesOperation = 1 << 1,
    IsBoundToInstance = 1 << 2,
};

} // namespace

static OptionSet<AccessFormTrait> traitsOfAccessForm(AccessForm form)
{
    switch (form) {
    case AccessForm::ByName:
    case AccessForm::Split:
        return { };
    case AccessForm::Slot:
    case AccessForm::Constant:
        return { AccessFormTrait::IsResolved, AccessFormTrait::NamesOperation };
    case AccessForm::Variable:
    case AccessForm::GetterInSlot:
    case AccessForm::SetterInSlot:
        return { AccessFormTrait::IsResolved, AccessFormTrait::NamesOperation, AccessFormTrait::IsBoundToInstance };
    case AccessForm::Namespace:
    case AccessForm::Builtin:
    case AccessForm::Method:
    case AccessForm::Call:
        return { AccessFormTrait::IsResolved, AccessFormTrait::IsBoundToInstance };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void PropertyAccessPlan::dump(PrintStream& out) const
{
    out.print("form=", enumName(form), " level=", nameOf(level));
    if (traitsOfAccessForm(form).contains(AccessFormTrait::NamesOperation)) {
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
        dumpProofs(out, requiredProofs);
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

static std::optional<bool> fillsFieldAtAllocation(const Program& program, const Value& allocation, PropertyKeyID key)
{
    if (allocation.opcode != Opcode::NewObject)
        return std::nullopt;
    std::optional<uint32_t> field = program.at(allocation.layout()).findField(key);
    if (!field)
        return std::nullopt;
    return std::ranges::contains(allocation.fields(), *field);
}

bool isAllocationListing(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    return fillsFieldAtAllocation(program, function.at(sameObjectThroughCasts(function, object)), key) == true;
}

bool isUnfinishedAllocationLeavingOut(const Program& program, const Function& function, ValueID object, PropertyKeyID key)
{
    return fillsFieldAtAllocation(program, function.at(object), key) == false;
}

static FunctionID expectedClassObjectOf(const Program& program, const Function& function, ValueID value)
{
    TypeID expected = function.at(value).expected().predicted();
    if (!expected || !classOfClassObject(program, program.at(expected)))
        return { };
    return program.at(expected).singleFunction();
}

std::optional<PropertyAccessQuery> propertyAccessQueryOf(const Program& program, const Function& function, const Value& access, ExpectedClassObjects expectedClassObjects, PredictedReceivers predictedReceivers)
{
    Opcode opcode = access.opcode;
    if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
        return std::nullopt;

    PropertyAccessQuery query;
    query.access = accessOf(opcode);
    query.key = keyOfNamedAccess(program, access);
    query.isOwnLookup = opcode == Opcode::PropHasOwn;
    query.asksForDeclaredMember = opcode == Opcode::FindsDeclaredMember;
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
        query.receivers.expected = LayoutList { program.at(classObject).constructedClass };
        return query;
    }

    query.start = MemberStart::Instance;
    if (predictedReceivers == PredictedReceivers::Listed) {
        LayoutSetID stamp = PayloadWord::receivers(opcode) ? access.receivers() : LayoutSetID();
        query.receivers = receiverCandidates(program, function, receiver, stamp);
    } else {
        query.receivers = candidatesOf(program, receiverType, nullptr, { });
        query.receivers.expected = LayoutList();
    }
    query.isListedByAllocation = isAllocationListing(program, function, receiver, query.key);
    query.isLeftOutByUnfinishedAllocation = isUnfinishedAllocationLeavingOut(program, function, receiver, query.key);
    return query;
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

static bool isReadWithoutLookup(const Program& program, const PropertyAccessQuery& query, const MemberAnswer& answer)
{
    if (!program.at(answer.method).captures.isEmpty())
        return false;
    if (needsNoClosure(program, answer.method) && query.readIsOnlyCalled())
        return true;
    return !query.isPrivate && answer.holder && isEvaluatedOnce(program, program.at(answer.holder));
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

    auto setConstant = [&](Opcode operation, bool booleanConstant = false) {
        plan.form = AccessForm::Constant;
        plan.operation = operation;
        plan.booleanConstant = booleanConstant;
    };
    auto planKnownValue = [&](AccessForm readForm) {
        if (query.access == Access::Read)
            plan.form = readForm;
        else if (query.access == Access::Has)
            setConstant(Opcode::Boolean, true);
    };

    switch (query.asksForDeclaredMember && !isMemberOfClass(answer) ? MemberKind::Unknown : answer.kind) {
    case MemberKind::Unknown:
        break;
    case MemberKind::Absent:
        if (query.access == Access::Has)
            setConstant(Opcode::Boolean, false);
        else if (query.access == Access::Read && !query.isPrivate)
            setConstant(Opcode::Undefined);
        break;
    case MemberKind::Field:
        planFieldArm(context, query, arm, plan);
        break;
    case MemberKind::Method:
        planKnownValue(isReadWithoutLookup(program, query, answer) ? AccessForm::Method : AccessForm::ByName);
        break;
    case MemberKind::Accessor:
        if (query.access == Access::Read && !answer.getter)
            setConstant(Opcode::Undefined);
        else if (query.access == Access::Store && answer.setter)
            plan.form = AccessForm::Call;
        else
            planKnownValue(AccessForm::Call);
        break;
    case MemberKind::Constant:
        if (query.access == Access::Has)
            setConstant(Opcode::Boolean, true);
        else if (query.access == Access::Read) {
            setConstant(answer.constant == MemberConstant::Null ? Opcode::Null : answer.constant == MemberConstant::Integer ? Opcode::Integer : Opcode::Boolean, answer.constant == MemberConstant::True);
            plan.integerConstant = answer.integer;
        }
        break;
    case MemberKind::Variable:
        if (query.access != Access::Has)
            planVariableArm(context, query, arm, plan);
        else if (isVariablePropertyPresent(program, query, answer.variable))
            setConstant(Opcode::Boolean, true);
        else
            plan.cause = UnprovenCause::PropertyAdded;
        break;
    case MemberKind::Namespace:
        planKnownValue(AccessForm::Namespace);
        break;
    case MemberKind::Builtin:
        planKnownValue(isObjectKind(program.builtins().at(answer.builtin).kind) ? AccessForm::Builtin : AccessForm::ByName);
        break;
    }

    if (plan.form == AccessForm::ByName) {
        plan.operation = Opcode::Undefined;
        plan.booleanConstant = false;
        plan.storedValueFits = false;
        plan.capturesReceiver = false;
        plan.emptySlotMeansNoOwnProperty = false;
        plan.testsDeclaredType = false;
        plan.requiredProofs = { };
        plan.conditions.clear();
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
    if (receivers.isProven())
        return receivers.proven.isEmpty() ? MemberSelection { } : MemberSelection { MemberSource::Proven };
    if (receivers.expected.predicted().isEmpty())
        return { MemberSource::None, UnprovenCause::UnknownObject };
    if (receivers.expected.predicted().size() > query.maxMembers)
        return { MemberSource::None, UnprovenCause::LimitReached, LimitID::MaxAccessPlanMembers };
    return { MemberSource::Expected };
}

static MemberQuery memberQueryFor(const PropertyAccessQuery& query, LayoutID layout)
{
    return { query.start, layout, query.key, query.access, query.isOwnLookup };
}

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
            }).value;
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
        if (query.receivers.proof != ReceiverProof::Type)
            mergeArmsWithSameMember(partition, query.receivers.isProven() ? ArmsToMerge::All : ArmsToMerge::WhereFewerAreProven);
        if (query.access == Access::Store && query.stored)
            separateSlotsThatHoldOneValue(context.program, query, partition);
        return partition;
    }
    case MemberStart::ClassObject: {
        RELEASE_ASSERT(members.size() == 1);
        MemberPartition partition;
        MemberAnswer answer = findMember(context, memberQueryFor(query, members[0]));
        if (answer.isKnown())
            partition.arms.append(MemberArm { WTF::move(answer), members });
        else
            partition.rest = members;
        return partition;
    }
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

AOT_THRESHOLD(size_t, maxOtherReceiversForEachTestedOneInLibrary, 3);

// A function of the library is handed the objects of all its callers, and which of them come often is not known. Where few of them find the name in one place, a test for those few fails for the others, which go by name behind it.
static bool isTestedForFewOfManyReceiversOfLibrary(const Program& program, const PropertyAccessQuery& query, const PropertyAccessPlan& plan)
{
    if (query.module && program.at(query.module).kind != ModuleKind::RuntimeSource && !program.at(query.module).flags.contains(ModuleFlags::IsLibrary))
        return false;
    size_t tested = plan.receivers.size();
    size_t expected = query.receivers.expected.predicted().size();
    return expected > tested && tested * maxOtherReceiversForEachTestedOneInLibrary < expected - tested;
}

static PropertyAccessPlan planExpectedMembers(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberPartition& partition)
{
    PropertyAccessPlan heaviest;
    std::tuple<bool, bool, size_t, bool> weightOfHeaviest { false, false, 0, false };
    for (const MemberArm& arm : partition.arms) {
        PropertyAccessPlan plan = planMemberArm(context, query, arm);
        if (!plan.isResolved())
            continue;
        bool hasLayoutOfModule = query.module && std::ranges::any_of(plan.receivers, [&](LayoutID layout) {
            return context.program.at(layout).module == query.module;
        });
        bool hasLayoutOfExpectedType = std::ranges::any_of(plan.receivers, [&](LayoutID layout) {
            return query.receivers.ofExpectedType.predicted().contains(layout);
        });
        std::tuple<bool, bool, size_t, bool> weight { arm.answer.kind != MemberKind::Absent, hasLayoutOfExpectedType, plan.receivers.size(), hasLayoutOfModule };
        if (weight <= weightOfHeaviest)
            continue;
        heaviest = WTF::move(plan);
        weightOfHeaviest = weight;
    }
    if (heaviest.isResolved() && isTestedForFewOfManyReceiversOfLibrary(context.program, query, heaviest))
        heaviest = PropertyAccessPlan();
    if (!heaviest.isResolved()) {
        heaviest.cause = partition.arms.isEmpty() ? causeOfRest(context, query, partition) : partition.arms[0].answer.cause;
        return heaviest;
    }
    heaviest.needsMissPath = true;
    switch (query.start) {
    case MemberStart::Instance:
        heaviest.receiverTest = ReceiverTest::Type;
        return heaviest;
    case MemberStart::ClassObject:
        RELEASE_ASSERT(query.expectedClassObject);
        heaviest.receiverTest = ReceiverTest::ClassObject;
        heaviest.testedClassObject = query.expectedClassObject;
        return heaviest;
    case MemberStart::ClassPrototype:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void addInstanceProof(const PropertyAccessContext& context, const PropertyAccessQuery& query, PropertyAccessPlan& plan)
{
    if (query.start == MemberStart::ClassObject)
        return;
    bool receiverIsTested = plan.receiverTest != ReceiverTest::None || query.receivers.proof == ReceiverProof::Guard;
    bool narrowsReceiverType = plan.receiverTest == ReceiverTest::Type;
    bool isBoundToInstance = query.isPrivate || !plan.conditions.isEmpty() || traitsOfAccessForm(plan.form).contains(AccessFormTrait::IsBoundToInstance);
    if (!plan.isResolved() || !receiverIsTested || !(narrowsReceiverType || isBoundToInstance))
        return;
    if (instanceCountOf(context) == InstanceCount::AtMostOne)
        return;
    plan.requiredProofs.add(Proof::OwnInstance);
    plan.needsMissPath = true;
}

PropertyAccessPlan planPropertyAccess(const PropertyAccessContext& context, const PropertyAccessQuery& query)
{
    PropertyAccessPlan plan;
    if (keyLookupOf(context.program, query.key) != KeyLookup::Ordinary)
        return plan;

    const ReceiverCandidates& receivers = query.receivers;
    // A class below nothing lays its private names on what its own constructor allocates.
    if (query.isPrivate && query.access == Access::Has && receivers.isMadeByHostAlone && !context.program.at(query.declaringLayout).has(LayoutFlags::HasHeritage)) {
        plan.form = AccessForm::Constant;
        plan.operation = Opcode::Boolean;
        plan.booleanConstant = false;
        plan.level = KnowledgeLevel::Proven;
        if (receivers.mayBeNullish) {
            plan.requiredProofs.add(Proof::NotNullish);
            plan.needsMissPath = true;
            plan.level = KnowledgeLevel::Expected;
        }
        return plan;
    }
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
        if (partition.hasOneArm())
            plan = planMemberArm(context, query, partition.arms[0]);
        else {
            bool canSplitOffArm = std::ranges::any_of(partition.arms, [&](const MemberArm& arm) {
                return planMemberArm(context, query, arm).isResolved();
            });
            plan.form = canSplitOffArm ? AccessForm::Split : AccessForm::ByName;
            plan.receivers = receivers.proven;
            plan.cause = causeOfRest(context, query, partition);
        }
        if (receivers.mayBeNullish && plan.isResolved()) {
            plan.requiredProofs.add(Proof::NotNullish);
            plan.needsMissPath = true;
        }
        memberLevel = receivers.level;
        break;
    case MemberSource::Expected:
        partition = partitionFor(context, query, receivers.expected.predicted());
        plan = planExpectedMembers(context, query, partition);
        memberLevel = KnowledgeLevel::Expected;
        break;
    }
    if (selection.cause != UnprovenCause::Unknown)
        plan.cause = selection.cause;
    plan.limit = selection.limit;

    addInstanceProof(context, query, plan);
    plan.partition = WTF::move(partition);
    if (plan.form == AccessForm::ByName)
        plan.level = KnowledgeLevel::Unknown;
    else
        plan.level = traitsOfAccessForm(plan.form).contains(AccessFormTrait::IsResolved) && !plan.needsNoTest() ? KnowledgeLevel::Expected : memberLevel;
    return plan;
}

LimitID limitOfPropertyAccessPlan(const Program& program, const PropertyAccessQuery& query)
{
    if (keyLookupOf(program, query.key) != KeyLookup::Ordinary || (query.receivers.isProven() && query.receivers.hasOtherParts))
        return LimitID::None;
    return selectMembers(query).limit;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
