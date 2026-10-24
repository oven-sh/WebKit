/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfAccesses.h"

#if ENABLE(AOT)

#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowObjectOperations.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessElements.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ProofState : uint8_t { Unknown, Proven };

enum class ObjectStep : uint8_t { OwnKeys, OwnValues, PrototypeChain };

} // anonymous namespace

static constexpr uint64_t maxMembersPerStep = defaultLimitValue(LimitID::MaxAccessPlanMembers, hostArchitecture());

static ReceiverCandidates candidatesOfProvenType(const Program& program, const Type& layouts)
{
    return candidatesOf(program, layouts, nullptr, { });
}

// Nothing where the type admits an object that has none of the layouts, or more layouts than one step looks at.
static std::optional<LayoutList> provenLayoutsOf(const Program& program, const Type& layouts)
{
    ReceiverCandidates candidates = candidatesOfProvenType(program, layouts);
    if (!candidates.isProven() || candidates.hasOtherParts || candidates.proven.size() > maxMembersPerStep)
        return std::nullopt;
    return WTF::move(candidates.proven);
}

static ProofState addArm(const PropertyAccessPlan& plan, Access access, ProvenAccess& result)
{
    if (!plan.isResolved() || !plan.needsNoTest())
        return ProofState::Unknown;

    switch (plan.form) {
    case AccessForm::Slot:
        insertInUniquedSortedVector(result.operations, plan.operation);
        return ProofState::Proven;
    case AccessForm::Constant:
        result.mayFindNothing = true;
        return ProofState::Proven;
    case AccessForm::Variable:
        insertInUniquedSortedVector(result.operations, plan.operation);
        // A read of a binding in its temporal dead zone throws: ResolveAccesses emits CheckInitialized after such a read.
        if (access == Access::Read)
            insertInUniquedSortedVector(result.operations, Opcode::CheckInitialized);
        return ProofState::Proven;
    case AccessForm::Namespace:
        insertInUniquedSortedVector(result.operations, Opcode::ModuleNamespace);
        return ProofState::Proven;
    case AccessForm::Builtin:
        insertInUniquedSortedVector(result.builtins, plan.member.builtin);
        return ProofState::Proven;
    case AccessForm::Method:
        if (plan.member.kind == MemberKind::Field)
            insertInUniquedSortedVector(result.operations, Opcode::GetDeferredClosureSlot);
        else
            insertInUniquedSortedVector(result.methods, plan.member.method);
        return ProofState::Proven;
    case AccessForm::Call:
        insertInUniquedSortedVector(result.accessors, plan.member.accessorFor(access));
        return ProofState::Proven;
    case AccessForm::GetterInSlot:
        return ProofState::Unknown;
    case AccessForm::ByName:
    case AccessForm::Split:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofState addMethodArm(const MemberArm& arm, ProvenAccess& result)
{
    if (!arm.answer.needsNoTest())
        return ProofState::Unknown;
    insertInUniquedSortedVector(result.methods, arm.answer.method);
    return ProofState::Proven;
}

static ProofState addMemberArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, ProvenAccess& result)
{
    if (arm.answer.kind == MemberKind::Method && query.access == Access::Read)
        return addMethodArm(arm, result);
    return addArm(planMemberArm(context, query, arm), query.access, result);
}

static ProofState addProvenArms(const PropertyAccessContext& context, const PropertyAccessQuery& query, ProvenAccess& result)
{
    const ReceiverCandidates& receivers = query.receivers;
    RELEASE_ASSERT(receivers.predicted.isEmpty() && !receivers.mayBeNullish);
    if (!receivers.isProven() || receivers.hasOtherParts || receivers.proven.size() > query.maxMembers)
        return ProofState::Unknown;
    if (receivers.proven.isEmpty())
        return ProofState::Proven;

    PropertyAccessPlan plan = planPropertyAccess(context, query);
    if (plan.isResolved())
        return addArm(plan, query.access, result);
    if (plan.partition.arms.isEmpty() || !plan.partition.rest.isEmpty())
        return ProofState::Unknown;

    for (const MemberArm& arm : plan.partition.arms) {
        if (addMemberArm(context, query, arm, result) == ProofState::Unknown)
            return ProofState::Unknown;
    }
    return ProofState::Proven;
}

static bool isLookup(Access);

// What NewObject makes is an ordinary object, and stays one whoever gets hold of it: no trap and no setter stands in front of a definition.
static bool definesPropertyOfObjectMadeHere(const Function& function, const Value& access)
{
    if (access.opcode != Opcode::PropDefine)
        return false;
    return function.at(sameObjectThroughCasts(function, access.objectOperand())).opcode == Opcode::NewObject;
}

// The same holds of a value whose type is proven to be records alone: a NewObject somewhere has made it. A join of two allocations is such a value, and a parameter.
static bool isRecordWhoeverMadeIt(const Program& program, const Function& function, ValueID object)
{
    const Type& type = program.typeOf(function, object);
    if (!type.hasOnlyLayouts(program))
        return false;
    return std::ranges::all_of(program.at(type.layouts).members, [&](LayoutID layout) {
        return program.at(layout).kind == LayoutKind::Record;
    });
}

static ProvenAccess computeProvenNamedAccessOf(const Program& program, const Function& function, const Value& access, PredictedReceivers predictedReceivers = PredictedReceivers::LeftOut)
{
    std::optional<PropertyAccessQuery> query = propertyAccessQueryOf(program, function, access, ExpectedClassObjects::Ignored, predictedReceivers);
    if (!query)
        return ProvenAccess::unknown();
    query->receivers.mayBeNullish = false;
    query->receivers.predicted.clear();
    query->isOnlyCalled = !query->isPrivate;
    query->usesOnlyPlansWithoutTests = true;

    ProvenAccess result;
    if (addProvenArms({ program, nullptr }, *query, result) == ProofState::Unknown) {
        if (definesPropertyOfObjectMadeHere(function, access) || (access.opcode == Opcode::PropDefine && isRecordWhoeverMadeIt(program, function, access.objectOperand()))) {
            ProvenAccess definition;
            definition.isComplete = true;
            definition.definesOwnProperty = true;
            definition.key = query->key;
            return definition;
        }
        // What holds for a lookup under any key holds under this one. Were it otherwise, a pass that finds out the key of a lookup would widen its effects.
        if (query->start != MemberStart::Instance || query->isPrivate || !isLookup(query->access))
            return ProvenAccess::unknown();
        return provenLookupUnderAnyKey(program, program.typeOf(function, access.operand(*receiverOf(access.opcode))));
    }

    result.isComplete = true;
    result.key = query->key;
    if (query->start == MemberStart::Instance)
        result.receivers = program.typeOf(function, access.operand(*receiverOf(access.opcode))).layouts;
    return result;
}

// No key for an access that propertyAccessQueryOf() refuses at once, nor where an id is one that an arena gave out: the cache holds ids of the program only.
static std::optional<PropertyAccessCache::ProvenAccessKey> findKeyOfNamedAccess(const Program& program, const Function& function, const Value& access)
{
    Opcode opcode = access.opcode;
    if (!isNamedAccess(opcode) || (FlagMask::isFreeName(opcode) && access.isFreeName()))
        return std::nullopt;

    ValueID receiver = access.operand(*receiverOf(opcode));
    PropertyAccessCache::ProvenAccessKey key;
    key.opcode = opcode;
    key.key = keyOfNamedAccess(program, access);
    if (familyOf(opcode) == OperationFamily::PrivateAccess)
        key.declaringLayout = access.layout();
    key.receiver = function.at(receiver).type;
    if (std::optional<uint8_t> stored = storedOf(opcode))
        key.stored = function.at(access.operand(*stored)).type;
    if (TypeTable::isProvisional(key.key) || TypeTable::isProvisional(key.receiver) || TypeTable::isProvisional(key.stored))
        return std::nullopt;

    // The three questions about the allocation of the receiver, with one walk through the casts. Few receivers are made where they are used.
    ValueID object = sameObjectThroughCasts(function, receiver);
    const Value& allocation = function.at(object);
    if (allocation.opcode != Opcode::NewObject)
        return key;
    key.definesPropertyOfObjectMadeHere = opcode == Opcode::PropDefine;
    if (std::optional<uint32_t> field = program.at(allocation.layout()).findField(key.key)) {
        key.isListedByAllocation = std::ranges::contains(allocation.fields(), *field);
        key.isLeftOutByUnfinishedAllocation = object == receiver && !key.isListedByAllocation;
    }
    return key;
}

static uint32_t hashOf(const PropertyAccessCache::ProvenAccessKey& key)
{
    uint64_t hash = static_cast<uint64_t>(key.opcode) << 3 | static_cast<uint64_t>(key.definesPropertyOfObjectMadeHere) << 2 | static_cast<uint64_t>(key.isListedByAllocation) << 1 | static_cast<uint64_t>(key.isLeftOutByUnfinishedAllocation);
    for (uint32_t word : { key.key.index(), key.declaringLayout.index(), key.receiver.index(), key.stored.index() })
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
    return static_cast<uint32_t>(hash >> 32);
}

static const ProvenAccess& provenNamedAccessOf(const Program& program, const Function& function, const Value& access, ProvenAccess& storage LIFETIME_BOUND)
{
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache)
        return storage = computeProvenNamedAccessOf(program, function, access);
    std::optional<PropertyAccessCache::ProvenAccessKey> key = findKeyOfNamedAccess(program, function, access);
    if (!key)
        return storage = computeProvenNamedAccessOf(program, function, access);

    const ProvenAccess& kept = findOrCompute(cache->provenAccesses, *key, hashOf(*key), [&] {
        return PropertyAccessCache::ProvenNamedAccess { *key, computeProvenNamedAccessOf(program, function, access) };
    }).access;
    RELEASE_ASSERT(!cache->checksAnswers || kept == computeProvenNamedAccessOf(program, function, access, PredictedReceivers::Listed));
    return kept;
}

static ProvenAccess provenOperation(Opcode operation)
{
    ProvenAccess result;
    result.isComplete = true;
    result.operations.append(operation);
    return result;
}

static PropertyKeyID findConstantKey(const Program& program, const ConstantKey& constantKey)
{
    switch (constantKey.kind) {
    case ConstantKeyKind::None:
        return { };
    case ConstantKeyKind::Text:
        return program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = constantKey.text }).value_or(PropertyKeyID());
    case ConstantKeyKind::Symbol:
        return constantKey.symbol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value createNamedAccess(const Value& elementAccess, Opcode namedOperation, PropertyKeyID key)
{
    Value named(namedOperation);
    named.setObjectOperand(elementAccess.objectOperand());
    if (storedOf(namedOperation))
        named.setValueOperand(elementAccess.valueOperand());
    named.setKey(key);
    named.setReceivers(elementAccess.receivers());
    return named;
}

static ProvenAccess provenLookupByKeyOf(const Program&, const Function&, const Value& access);

const ProvenAccess& provenAccessOf(const Program& program, const Function& function, const Value& access, ProvenAccess& storage)
{
    if (!elementAccessOf(access.opcode))
        return provenNamedAccessOf(program, function, access, storage);

    std::optional<ElementAccessPlan> plan = planElementAccessOf(program, function, access);
    if (!plan)
        return storage = ProvenAccess::unknown();

    switch (plan->form) {
    case ElementAccessForm::NamedKey: {
        PropertyKeyID key = findConstantKey(program, plan->constantKey);
        if (!key)
            return storage = provenLookupByKeyOf(program, function, access);
        return provenNamedAccessOf(program, function, createNamedAccess(access, plan->operation, key), storage);
    }
    case ElementAccessForm::Array:
    case ElementAccessForm::TypedArray:
        if (!plan->needsNoTest())
            return storage = ProvenAccess::unknown();
        return storage = provenOperation(plan->operation);
    case ElementAccessForm::String:
        return storage = provenOperation(Opcode::StringRangeCharacterOrUndefined);
    case ElementAccessForm::ByKey:
    case ElementAccessForm::ArgumentsElement:
        return storage = provenLookupByKeyOf(program, function, access);
    case ElementAccessForm::Split:
        return storage = provenLookupByKeyOf(program, function, access);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProvenAccess provenAccessOf(const Program& program, const Function& function, const Value& access)
{
    ProvenAccess storage;
    const ProvenAccess& answer = provenAccessOf(program, function, access, storage);
    if (&answer == &storage)
        return storage;
    return answer;
}

static PropertyAccessQuery createReadQuery(MemberStart start, PropertyKeyID key, ReceiverCandidates&& receivers)
{
    PropertyAccessQuery query;
    query.start = start;
    query.key = key;
    query.receivers = WTF::move(receivers);
    query.isOnlyCalled = true;
    query.usesOnlyPlansWithoutTests = true;
    return query;
}

static ProofState addReadOfLayouts(const PropertyAccessContext& context, PropertyKeyID key, const Type& layouts, ProvenAccess& result)
{
    return addProvenArms(context, createReadQuery(MemberStart::Instance, key, candidatesOfProvenType(context.program, layouts)), result);
}

static ProofState addReadOfFunctions(const PropertyAccessContext& context, PropertyKeyID key, const Type& functions, ProvenAccess& result)
{
    bool readsPrototypeProperty = key == context.program.keyOf(BuiltinNames::prototype);
    for (FunctionID function : functions.functions) {
        // Nothing can delete a prototype property that exists from creation or turn it into an accessor, so the read ends at the function and runs no code.
        if (readsPrototypeProperty && hasPrototypeProperty(context.program, context.program.at(function))) {
            result.mayFindPrototypeProperty = true;
            continue;
        }
        LayoutID classLayout = classOfClassObject(context.program, Type::forFunction(function));
        if (!classLayout)
            return ProofState::Unknown;
        ReceiverCandidates classObject { .proof = ReceiverProof::Type, .level = KnowledgeLevel::Proven, .proven = { classLayout } };
        if (addProvenArms(context, createReadQuery(MemberStart::ClassObject, key, WTF::move(classObject)), result) == ProofState::Unknown)
            return ProofState::Unknown;
    }
    return ProofState::Proven;
}

static ProofState addBuiltinChainAnswer(const PropertyAccessContext& context, PropertyKeyID key, MemberAnswer&& answer, ProvenAccess& result)
{
    return addMemberArm(context, createReadQuery(MemberStart::Instance, key, { }), { WTF::move(answer), { } }, result);
}

static ProofState addReadOfBuiltins(const PropertyAccessContext& context, PropertyKeyID key, const Type& builtins, ProvenAccess& result)
{
    for (BuiltinID builtin : builtins.builtins) {
        if (addBuiltinChainAnswer(context, key, findBuiltinObjectMember(context, builtin, key, Access::Read), result) == ProofState::Unknown)
            return ProofState::Unknown;
    }
    return ProofState::Proven;
}

static ProofState addReadOfBuiltinInstances(const PropertyAccessContext& context, PropertyKeyID key, const Type& instances, ProvenAccess& result)
{
    const BuiltinTable& table = context.program.builtins();
    for (const ClassPart& part : instances.classes) {
        if (!part.provenPristine.contains(PristineAspects::Names))
            return ProofState::Unknown;
        MemberAnswer answer = findBuiltinInstanceMember(context, part.builtinClass, key, Access::Read);
        if (answer.kind == MemberKind::Builtin && table.at(answer.builtin).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
            return ProofState::Unknown;
        if (addBuiltinChainAnswer(context, key, WTF::move(answer), result) == ProofState::Unknown)
            return ProofState::Unknown;
    }
    return ProofState::Proven;
}

static ProofState addReadOfPrimitives(const PropertyAccessContext& context, PropertyKeyID key, BuiltinClassID wrapperClass, ProvenAccess& result)
{
    MemberAnswer answer = findBuiltinInstanceMember(context, wrapperClass, key, Access::Read);
    // A locked member of a wrapper prototype is the engine's, and the entry that looks it up declares what happens.
    if (answer.kind == MemberKind::Builtin && answer.needsNoTest())
        return ProofState::Proven;
    return addBuiltinChainAnswer(context, key, WTF::move(answer), result);
}

static ProofState addReadOfPart(const PropertyAccessContext& context, PropertyKeyID key, const UnionPart& part, ProvenAccess& result)
{
    switch (part.kind) {
    case UnionPartKind::Nullish:
    case UnionPartKind::Absent:
        return ProofState::Proven;
    case UnionPartKind::Boolean:
        return addReadOfPrimitives(context, key, BuiltinClasses::Boolean, result);
    case UnionPartKind::Number:
        return addReadOfPrimitives(context, key, BuiltinClasses::Number, result);
    case UnionPartKind::String:
        return addReadOfPrimitives(context, key, BuiltinClasses::String, result);
    case UnionPartKind::Symbol:
        return addReadOfPrimitives(context, key, BuiltinClasses::Symbol, result);
    case UnionPartKind::BigInt:
        return addReadOfPrimitives(context, key, BuiltinClasses::BigInt, result);
    case UnionPartKind::Layouts:
        return addReadOfLayouts(context, key, part.type, result);
    case UnionPartKind::BuiltinClass:
        return addReadOfBuiltinInstances(context, key, part.type, result);
    case UnionPartKind::Functions:
        return addReadOfFunctions(context, key, part.type, result);
    case UnionPartKind::Builtins:
        return addReadOfBuiltins(context, key, part.type, result);
    case UnionPartKind::OtherObjects:
        return ProofState::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofState checkPrototypeChain(const Program&, LayoutID);

// A tuple is an array.
static bool inheritsFromObjectPrototypeAlone(const Program& program, const Layout& layout)
{
    if (layout.kind == LayoutKind::BuiltinResult)
        return !program.builtins().at(layout.builtinLayout).isTuple;
    return layout.kind == LayoutKind::Record;
}

// No operation of the program names the key, so no layout has a field or an accessor under it.
static ProvenAccess provenReadOfUnnamedKey(const Program& program, const Type& receiver, BuiltinName name)
{
    const BuiltinTable& table = program.builtins();
    BuiltinID inherited = table.findOwnMember(Builtins::ObjectPrototype, name);
    if ((inherited && table.at(inherited).kind != BuiltinKind::Method) || !isClosedUnion(receiver))
        return ProvenAccess::unknown();

    for (const UnionPart& part : unionPartsOf(receiver)) {
        if (part.kind == UnionPartKind::Nullish || part.kind == UnionPartKind::Absent)
            continue;
        if (part.kind != UnionPartKind::Layouts)
            return ProvenAccess::unknown();
        std::optional<LayoutList> layouts = provenLayoutsOf(program, part.type);
        if (!layouts)
            return ProvenAccess::unknown();
        for (LayoutID layout : *layouts) {
            bool isProven = inheritsFromObjectPrototypeAlone(program, program.at(layout))
                && receiverStateOf(program, layout).cannot(ObjectMutation::AddProperty)
                && checkPrototypeChain(program, layout) == ProofState::Proven;
            if (!isProven)
                return ProvenAccess::unknown();
        }
    }
    ProvenAccess result;
    result.isComplete = true;
    if (inherited)
        result.builtins.append(inherited);
    else
        result.mayFindNothing = true;
    return result;
}

ProvenAccess provenReadOf(const Program& program, const Type& receiver, BuiltinName name)
{
    if (!name || !isClosedUnion(receiver))
        return ProvenAccess::unknown();
    PropertyKeyID key = program.keyOf(name);
    if (!key)
        return provenReadOfUnnamedKey(program, receiver, name);

    PropertyAccessContext context { program, nullptr };
    ProvenAccess result;
    for (const UnionPart& part : unionPartsOf(receiver)) {
        if (addReadOfPart(context, key, part, result) == ProofState::Unknown)
            return ProvenAccess::unknown();
    }
    result.isComplete = true;
    result.key = key;
    return result;
}

static std::optional<ObjectStep> objectStepOf(Protocol protocol)
{
    switch (protocol) {
    case Protocol::OwnKeys:
        return ObjectStep::OwnKeys;
    case Protocol::OwnValues:
        return ObjectStep::OwnValues;
    case Protocol::PrototypeChain:
        return ObjectStep::PrototypeChain;
    case Protocol::ToPrimitive:
    case Protocol::Thenable:
    case Protocol::Iterate:
    case Protocol::Species:
    case Protocol::ToJSON:
    case Protocol::HasInstance:
    case Protocol::Named:
    case Protocol::CallsNamed:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofState addOwnValues(const Program& program, LayoutID layout, ProvenAccess& result)
{
    ReceiverState state = receiverStateOf(program, layout);
    if (state.cannot(ObjectMutation::RedefineProperty) && state.cannot(ObjectMutation::AddProperty))
        return ProofState::Proven;

    const LayoutFlow& flow = program.at(layout).flow;
    if (!flow.ownAccessorsAreComplete)
        return ProofState::Unknown;
    for (const ClassMember& accessor : flow.ownAccessors) {
        if (accessor.kind == MethodKind::Getter)
            insertInUniquedSortedVector(result.accessors, accessor.function);
    }
    return ProofState::Proven;
}

static ProofState checkClassChain(const Program& program, LayoutID instanceLayout)
{
    MemberConditions conditions;
    LayoutID current = instanceLayout;
    for (size_t steps = 0; steps <= program.layouts.size(); ++steps) {
        NextHolder next = nextHolderOf(program, current, ClassSide::Prototype, conditions);
        if (!conditions.isEmpty())
            return ProofState::Unknown;

        switch (next.step) {
        case ChainStep::Unknown:
            return ProofState::Unknown;
        case ChainStep::CompiledClass:
            current = next.layout;
            break;
        case ChainStep::BuiltinClass:
            // A locked object rejects a new prototype, so a chain of locked objects never gains a proxy.
            return program.builtins().hasLockedPrototypeChain(next.builtinClass) ? ProofState::Proven : ProofState::Unknown;
        case ChainStep::NoBase:
            return ProofState::Proven;
        }
    }
    return ProofState::Unknown;
}

static ProofState checkPrototypeChain(const Program& program, LayoutID layout)
{
    if (!receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype))
        return ProofState::Unknown;

    switch (program.at(layout).kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return ProofState::Proven;
    case LayoutKind::Instance:
        return checkClassChain(program, layout);
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return ProofState::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofState addStepOnLayout(const Program& program, LayoutID layout, ObjectStep step, ProvenAccess& result)
{
    switch (step) {
    case ObjectStep::OwnKeys:
        return ProofState::Proven;
    case ObjectStep::OwnValues:
        return addOwnValues(program, layout, result);
    case ObjectStep::PrototypeChain:
        return checkPrototypeChain(program, layout);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofState addStepOnLayouts(const Program& program, const Type& receivers, ObjectStep step, ProvenAccess& result)
{
    std::optional<LayoutList> layouts = provenLayoutsOf(program, receivers);
    if (!layouts)
        return ProofState::Unknown;

    for (LayoutID layout : *layouts) {
        if (addStepOnLayout(program, layout, step, result) == ProofState::Unknown)
            return ProofState::Unknown;
    }
    return ProofState::Proven;
}

// A class may define an accessor, and the key may name it.
static bool inheritsFromBuiltinsOnly(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return true;
    case LayoutKind::Instance:
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProofState checkLookupByKeyOnLayouts(const Program& program, const Type& receivers)
{
    std::optional<LayoutList> layouts = provenLayoutsOf(program, receivers);
    if (!layouts)
        return ProofState::Unknown;

    for (LayoutID layout : *layouts) {
        if (!inheritsFromBuiltinsOnly(program.at(layout).kind))
            return ProofState::Unknown;
        ProvenAccess ownValues;
        if (addOwnValues(program, layout, ownValues) == ProofState::Unknown || !ownValues.accessors.isEmpty())
            return ProofState::Unknown;
        if (checkPrototypeChain(program, layout) == ProofState::Unknown)
            return ProofState::Unknown;
    }
    return ProofState::Proven;
}

static ProofState checkLookupByKeyOnPart(const Program& program, const UnionPart& part)
{
    switch (part.kind) {
    case UnionPartKind::Nullish:
    case UnionPartKind::Absent:
        return ProofState::Proven;
    case UnionPartKind::Layouts:
        return checkLookupByKeyOnLayouts(program, part.type);
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::BuiltinClass:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return ProofState::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isLookup(Access access)
{
    switch (access) {
    case Access::Read:
    case Access::Has:
        return true;
    case Access::None:
    case Access::Store:
    case Access::Define:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ProvenAccess provenLookupByKeyOf(const Program& program, const Function& function, const Value& access)
{
    if (!isLookup(*elementAccessOf(access.opcode)))
        return ProvenAccess::unknown();
    return provenLookupUnderAnyKey(program, program.typeOf(function, access.objectOperand()));
}

ProvenAccess provenLookupUnderAnyKey(const Program& program, const Type& receiver)
{
    if (!isClosedUnion(receiver))
        return ProvenAccess::unknown();

    for (const UnionPart& part : unionPartsOf(receiver)) {
        if (checkLookupByKeyOnPart(program, part) == ProofState::Unknown)
            return ProvenAccess::unknown();
    }
    ProvenAccess result;
    result.isComplete = true;
    result.isLookupByKey = true;
    return result;
}

static ProofState addStepOnPart(const Program& program, const UnionPart& part, ObjectStep step, ProvenAccess& result)
{
    switch (part.kind) {
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
        return ProofState::Proven;
    case UnionPartKind::Layouts:
        return addStepOnLayouts(program, part.type, step, result);
    case UnionPartKind::BuiltinClass:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
    case UnionPartKind::OtherObjects:
        return ProofState::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProvenAccess provenStepOf(const Program& program, const Type& receiver, Protocol keylessProtocol)
{
    std::optional<ObjectStep> step = objectStepOf(keylessProtocol);
    if (!step || !isClosedUnion(receiver))
        return ProvenAccess::unknown();

    ProvenAccess result;
    for (const UnionPart& part : unionPartsOf(receiver)) {
        if (addStepOnPart(program, part, *step, result) == ProofState::Unknown)
            return ProvenAccess::unknown();
    }
    result.isComplete = true;
    return result;
}

static ProofState addTypesOfFields(const Program& program, LayoutID id, SortedSet<TypeID, 8>& types)
{
    const Layout& layout = program.at(id);
    for (uint32_t index = 0; index < layout.fields.size(); ++index) {
        const Field& field = layout.fields[index];
        // A hidden field is no property: only compiled code reads it.
        if (field.flags.contains(FieldFlags::IsHidden))
            continue;
        // The declared type bounds the slot. It bounds the property where no store and no definition moves the field out of the slot.
        if (!field.declared.type || field.declared.flags.contains(PositionFlags::IsExpected) || fieldTypeStateOf(program, { id, index }) != FieldTypeState::Proven)
            return ProofState::Unknown;
        types.add(field.declared.type);
    }
    return ProofState::Proven;
}

static ProofState addTypesOfOwnValues(const Program& program, LayoutID instanceLayout, SortedSet<TypeID, 8>& types)
{
    ReceiverState state = receiverStateOf(program, instanceLayout);
    if (!state.cannot(ObjectMutation::AddProperty) || !state.cannot(ObjectMutation::RedefineProperty))
        return ProofState::Unknown;

    LayoutID current = instanceLayout;
    for (size_t steps = 0; steps <= program.layouts.size(); ++steps) {
        if (addTypesOfFields(program, current, types) == ProofState::Unknown)
            return ProofState::Unknown;

        const Layout& layout = program.at(current);
        switch (layout.kind) {
        case LayoutKind::Record:
        case LayoutKind::Container:
        case LayoutKind::BuiltinResult:
            return ProofState::Proven;
        case LayoutKind::Instance:
            break;
        case LayoutKind::Environment:
        case LayoutKind::Closure:
        case LayoutKind::CoroutineFrame:
        case LayoutKind::ModuleNamespace:
            return ProofState::Unknown;
        }

        switch (layout.flow.baseState) {
        case BaseState::None:
            return ProofState::Proven;
        case BaseState::Layout:
            current = layout.flow.base;
            break;
        case BaseState::Unknown:
        case BaseState::BuiltinClass:
            return ProofState::Unknown;
        }
    }
    return ProofState::Unknown;
}

std::optional<SortedSet<TypeID, 8>> provenOwnValueTypesOf(const Program& program, const Type& receivers)
{
    std::optional<LayoutList> layouts = provenLayoutsOf(program, receivers);
    if (!layouts)
        return std::nullopt;

    SortedSet<TypeID, 8> types;
    for (LayoutID layout : *layouts) {
        if (addTypesOfOwnValues(program, layout, types) == ProofState::Unknown)
            return std::nullopt;
    }
    return types;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
