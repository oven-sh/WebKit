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
#include "AOTResultRules.h"
#include "AOTValueIdentity.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ProofState : uint8_t { Unknown, Proven };

enum class Ancestry : uint8_t { Unknown, BuiltinsOnly, Classes };

} // anonymous namespace

static constexpr uint64_t maxMembersPerStep = defaultLimitValue(LimitID::MaxAccessPlanMembers, hostArchitecture());

static Ancestry ancestryOf(LayoutKind kind)
{
    switch (kind) {
    case LayoutKind::Record:
    case LayoutKind::Container:
    case LayoutKind::BuiltinResult:
        return Ancestry::BuiltinsOnly;
    case LayoutKind::Instance:
        return Ancestry::Classes;
    case LayoutKind::Environment:
    case LayoutKind::Closure:
    case LayoutKind::CoroutineFrame:
    case LayoutKind::ModuleNamespace:
        return Ancestry::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isLookup(Access access) { return access == Access::Read || access == Access::Has; }

static std::optional<LayoutList> provenLayoutsOf(const Program& program, const Type& layouts)
{
    ReceiverCandidates candidates = candidatesOf(program, layouts, nullptr, { });
    if (!candidates.isProven() || candidates.hasOtherParts || candidates.proven.size() > maxMembersPerStep)
        return std::nullopt;
    return WTF::move(candidates.proven);
}

static bool isNullishOrOfLayoutsThat(const Program& program, const Type& receiver, NOESCAPE const Invocable<bool(LayoutID)> auto& passes)
{
    if (!isClosedUnion(receiver))
        return false;
    for (const UnionPart& part : unionPartsOf(receiver)) {
        if (part.kind == UnionPartKind::Nullish || part.kind == UnionPartKind::Absent)
            continue;
        std::optional<LayoutList> layouts = part.kind == UnionPartKind::Layouts ? provenLayoutsOf(program, part.type) : std::nullopt;
        if (!layouts || !std::ranges::all_of(*layouts, passes))
            return false;
    }
    return true;
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
    case AccessForm::SetterInSlot:
        return ProofState::Unknown;
    case AccessForm::ByName:
    case AccessForm::Split:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool addMemberArm(const PropertyAccessContext& context, const PropertyAccessQuery& query, const MemberArm& arm, ProvenAccess& result)
{
    if (arm.answer.kind != MemberKind::Method || query.access != Access::Read)
        return addArm(planMemberArm(context, query, arm), query.access, result) == ProofState::Proven;
    if (!arm.answer.needsNoTest())
        return false;
    insertInUniquedSortedVector(result.methods, arm.answer.method);
    return true;
}

static bool addProvenArms(const PropertyAccessContext& context, const PropertyAccessQuery& query, ProvenAccess& result)
{
    const ReceiverCandidates& receivers = query.receivers;
    RELEASE_ASSERT(receivers.predicted.isEmpty() && !receivers.mayBeNullish);
    if (!receivers.isProven() || receivers.hasOtherParts || receivers.proven.size() > query.maxMembers)
        return false;
    if (receivers.proven.isEmpty())
        return true;

    PropertyAccessPlan plan = planPropertyAccess(context, query);
    if (plan.isResolved())
        return addArm(plan, query.access, result) == ProofState::Proven;
    if (plan.partition.arms.isEmpty() || !plan.partition.rest.isEmpty())
        return false;
    return std::ranges::all_of(plan.partition.arms, [&](const MemberArm& arm) {
        return addMemberArm(context, query, arm, result);
    });
}

static bool definesPropertyOfOrdinaryObject(const Program& program, const Function& function, const Value& access)
{
    if (access.opcode != Opcode::PropDefine)
        return false;
    if (function.at(sameObjectThroughCasts(function, access.objectOperand())).opcode == Opcode::NewObject)
        return true;
    const Type& type = program.typeOf(function, access.objectOperand());
    return type.hasOnlyLayouts(program) && std::ranges::all_of(program.at(type.layouts).members, [&](LayoutID layout) {
        return program.at(layout).kind == LayoutKind::Record;
    });
}

static bool readsNameOfClosuresWithoutCode(const Program& program, const Type& receiver, PropertyKeyID key)
{
    NameClass nameClass = program.nameClassOf(key);
    if (program.at(key).kind != PropertyKeyKind::Text || (nameClass != NameClass::Ordinary && nameClass != NameClass::ObjectPrototypeMember && nameClass != NameClass::CallOrApply && nameClass != NameClass::Bind))
        return false;
    for (BuiltinID above : { Builtins::FunctionPrototype, Builtins::ObjectPrototype }) {
        BuiltinID member = findOwnMember(program, above, key);
        if (!listsEveryMemberUnder(program, above, key) || (member && program.builtins().at(member).kind != BuiltinKind::Method))
            return false;
    }
    Type closures = receiver.withoutPrimitives(Primitive::UndefinedOrNull);
    if (closures.functions.isEmpty() || closures != closures.withOnlyObjects() || closures.anyObject || closures.anyFunction || closures.layouts || !closures.classes.isEmpty() || !closures.builtins.isEmpty())
        return false;
    return std::ranges::all_of(closures.functions, [&](FunctionID closureFunction) {
        return program.at(closureFunction).has(FunctionCapabilities::ClosuresAreReadWithoutCode);
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
    if (!addProvenArms({ program, nullptr }, *query, result)) {
        if (definesPropertyOfOrdinaryObject(program, function, access))
            return { .isComplete = true, .definesOwnProperty = true, .key = query->key };
        if (query->access == Access::Read && !query->isPrivate && readsNameOfClosuresWithoutCode(program, program.typeOf(function, access.operand(*receiverOf(access.opcode))), query->key))
            return { .isComplete = true, .mayFindStoredValue = true, .key = query->key };
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
    std::optional<PropertyAccessCache::ProvenAccessKey> key = cache ? findKeyOfNamedAccess(program, function, access) : std::nullopt;
    if (!key)
        return storage = computeProvenNamedAccessOf(program, function, access);

    const ProvenAccess& kept = findOrCompute(cache->provenAccesses, *key, hashOf(*key), [&] {
        return PropertyAccessCache::ProvenNamedAccess { *key, computeProvenNamedAccessOf(program, function, access) };
    }).value;
    RELEASE_ASSERT(!cache->checksAnswers || kept == computeProvenNamedAccessOf(program, function, access, PredictedReceivers::Listed));
    return kept;
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

const ProvenAccess& provenAccessOf(const Program& program, const Function& function, const Value& access, ProvenAccess& storage)
{
    std::optional<Access> elementAccess = elementAccessOf(access.opcode);
    if (!elementAccess)
        return provenNamedAccessOf(program, function, access, storage);

    std::optional<ElementAccessPlan> plan = planElementAccessOf(program, function, access);
    if (!plan)
        return storage = ProvenAccess::unknown();

    switch (plan->form) {
    case ElementAccessForm::NamedKey:
        if (PropertyKeyID key = findConstantKey(program, plan->constantKey))
            return provenNamedAccessOf(program, function, createNamedAccess(access, plan->operation, key), storage);
        break;
    case ElementAccessForm::Array:
    case ElementAccessForm::TypedArray:
        if (!plan->needsNoTest())
            return storage = ProvenAccess::unknown();
        return storage = { .isComplete = true, .operations = { plan->operation } };
    case ElementAccessForm::String:
        return storage = { .isComplete = true, .operations = { Opcode::StringRangeCharacterOrUndefined } };
    case ElementAccessForm::ByKey:
    case ElementAccessForm::ArgumentsElement:
    case ElementAccessForm::Split:
        break;
    }
    if (!isLookup(*elementAccess))
        return storage = ProvenAccess::unknown();
    return storage = provenLookupUnderAnyKey(program, program.typeOf(function, access.objectOperand()));
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

static bool addReadOfFunctions(const PropertyAccessContext& context, PropertyKeyID key, const Type& functions, ProvenAccess& result)
{
    bool readsPrototypeProperty = key == context.program.keyOf(BuiltinNames::prototype);
    for (FunctionID function : functions.functions) {
        if (readsPrototypeProperty && hasPrototypeProperty(context.program, context.program.at(function))) {
            result.mayFindPrototypeProperty = true;
            continue;
        }
        LayoutID classLayout = classOfClassObject(context.program, Type::forFunction(function));
        if (!classLayout)
            return false;
        ReceiverCandidates classObject { .proof = ReceiverProof::Type, .level = KnowledgeLevel::Proven, .proven = { classLayout } };
        if (!addProvenArms(context, createReadQuery(MemberStart::ClassObject, key, WTF::move(classObject)), result))
            return false;
    }
    return true;
}

static bool addBuiltinChainAnswer(const PropertyAccessContext& context, PropertyKeyID key, MemberAnswer&& answer, ProvenAccess& result)
{
    return addMemberArm(context, createReadQuery(MemberStart::Instance, key, { }), { WTF::move(answer), { } }, result);
}

static bool addReadOfBuiltinInstances(const PropertyAccessContext& context, PropertyKeyID key, const Type& instances, ProvenAccess& result)
{
    const BuiltinTable& table = context.program.builtins();
    for (const ClassPart& part : instances.classes) {
        if (!part.provenPristine.contains(PristineAspects::Names))
            return false;
        MemberAnswer answer = findBuiltinInstanceMember(context, part.builtinClass, key, Access::Read);
        if (answer.kind == MemberKind::Builtin && table.at(answer.builtin).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances))
            return false;
        if (!addBuiltinChainAnswer(context, key, WTF::move(answer), result))
            return false;
    }
    return true;
}

static bool addReadOfPrimitives(const PropertyAccessContext& context, PropertyKeyID key, BuiltinClassID wrapperClass, ProvenAccess& result)
{
    MemberAnswer answer = findBuiltinInstanceMember(context, wrapperClass, key, Access::Read);
    if (answer.kind == MemberKind::Builtin && answer.needsNoTest())
        return true;
    return addBuiltinChainAnswer(context, key, WTF::move(answer), result);
}

static bool addReadOfPart(const PropertyAccessContext& context, PropertyKeyID key, const UnionPart& part, ProvenAccess& result)
{
    switch (part.kind) {
    case UnionPartKind::Nullish:
    case UnionPartKind::Absent:
        return true;
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
        return addProvenArms(context, createReadQuery(MemberStart::Instance, key, candidatesOf(context.program, part.type, nullptr, { })), result);
    case UnionPartKind::BuiltinClass:
        return addReadOfBuiltinInstances(context, key, part.type, result);
    case UnionPartKind::Functions:
        return addReadOfFunctions(context, key, part.type, result);
    case UnionPartKind::Builtins:
        for (BuiltinID builtin : part.type.builtins) {
            if (!addBuiltinChainAnswer(context, key, findBuiltinObjectMember(context, builtin, key, Access::Read), result))
                return false;
        }
        return true;
    case UnionPartKind::OtherObjects:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasProvenClassChain(const Program& program, LayoutID instanceLayout)
{
    MemberConditions conditions;
    LayoutID current = instanceLayout;
    for (size_t steps = 0; steps <= program.layouts.size(); ++steps) {
        NextHolder next = nextHolderOf(program, current, ClassSide::Prototype, conditions);
        if (!conditions.isEmpty())
            return false;

        switch (next.step) {
        case ChainStep::Unknown:
            return false;
        case ChainStep::CompiledClass:
            current = next.layout;
            break;
        case ChainStep::BuiltinClass:
            return program.builtins().hasLockedPrototypeChain(next.builtinClass);
        case ChainStep::NoBase:
            return true;
        }
    }
    return false;
}

static bool hasProvenPrototypeChain(const Program& program, LayoutID layout)
{
    if (!receiverStateOf(program, layout).cannot(ObjectMutation::ChangePrototype))
        return false;
    Ancestry ancestry = ancestryOf(program.at(layout).kind);
    return ancestry == Ancestry::BuiltinsOnly || (ancestry == Ancestry::Classes && hasProvenClassChain(program, layout));
}

static bool inheritsFromObjectPrototypeAlone(const Program& program, const Layout& layout)
{
    if (layout.kind == LayoutKind::BuiltinResult)
        return !program.builtins().at(layout.builtinLayout).isTuple;
    return layout.kind == LayoutKind::Record;
}

static ProvenAccess provenReadOfUnnamedKey(const Program& program, const Type& receiver, BuiltinName name)
{
    const BuiltinTable& table = program.builtins();
    BuiltinID inherited = table.findOwnMember(Builtins::ObjectPrototype, name);
    if (inherited && table.at(inherited).kind != BuiltinKind::Method)
        return ProvenAccess::unknown();

    bool isProven = isNullishOrOfLayoutsThat(program, receiver, [&](LayoutID layout) {
        return inheritsFromObjectPrototypeAlone(program, program.at(layout))
            && receiverStateOf(program, layout).cannot(ObjectMutation::AddProperty)
            && hasProvenPrototypeChain(program, layout);
    });
    if (!isProven)
        return ProvenAccess::unknown();
    if (inherited)
        return { .isComplete = true, .builtins = { inherited } };
    return { .isComplete = true, .mayFindNothing = true };
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
        if (!addReadOfPart(context, key, part, result))
            return ProvenAccess::unknown();
    }
    result.isComplete = true;
    result.key = key;
    return result;
}

static bool addOwnValues(const Program& program, LayoutID layout, ProvenAccess& result)
{
    ReceiverState state = receiverStateOf(program, layout);
    if (state.cannot(ObjectMutation::RedefineProperty) && state.cannot(ObjectMutation::AddProperty))
        return true;

    const LayoutFlow& flow = program.at(layout).flow;
    if (!flow.ownAccessorsAreComplete)
        return false;
    for (const ClassMember& accessor : flow.ownAccessors) {
        if (accessor.kind == MethodKind::Getter)
            insertInUniquedSortedVector(result.accessors, accessor.function);
    }
    return true;
}

ProvenAccess provenLookupUnderAnyKey(const Program& program, const Type& receiver)
{
    bool isProven = isNullishOrOfLayoutsThat(program, receiver, [&](LayoutID layout) {
        if (ancestryOf(program.at(layout).kind) != Ancestry::BuiltinsOnly)
            return false;
        ProvenAccess ownValues;
        return addOwnValues(program, layout, ownValues) && ownValues.accessors.isEmpty() && hasProvenPrototypeChain(program, layout);
    });
    if (!isProven)
        return ProvenAccess::unknown();
    return { .isComplete = true, .isLookupByKey = true };
}

static bool addStepOnLayout(const Program& program, LayoutID layout, Protocol keylessProtocol, ProvenAccess& result)
{
    switch (keylessProtocol) {
    case Protocol::OwnKeys:
        return true;
    case Protocol::OwnValues:
        return addOwnValues(program, layout, result);
    case Protocol::PrototypeChain:
        return hasProvenPrototypeChain(program, layout);
    case Protocol::ToPrimitive:
    case Protocol::Thenable:
    case Protocol::Iterate:
    case Protocol::Species:
    case Protocol::ToJSON:
    case Protocol::HasInstance:
    case Protocol::Named:
    case Protocol::CallsNamed:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ProvenAccess provenStepOf(const Program& program, const Type& receiver, Protocol keylessProtocol)
{
    bool isStepOnObject = keylessProtocol == Protocol::OwnKeys || keylessProtocol == Protocol::OwnValues || keylessProtocol == Protocol::PrototypeChain;
    if (!isStepOnObject || !isClosedUnion(receiver))
        return ProvenAccess::unknown();

    ProvenAccess result;
    for (const UnionPart& part : unionPartsOf(receiver)) {
        switch (part.kind) {
        case UnionPartKind::Nullish:
        case UnionPartKind::Boolean:
        case UnionPartKind::Number:
        case UnionPartKind::String:
        case UnionPartKind::Symbol:
        case UnionPartKind::BigInt:
        case UnionPartKind::Absent:
            continue;
        case UnionPartKind::Layouts: {
            std::optional<LayoutList> layouts = provenLayoutsOf(program, part.type);
            bool isProven = layouts && std::ranges::all_of(*layouts, [&](LayoutID layout) {
                return addStepOnLayout(program, layout, keylessProtocol, result);
            });
            if (isProven)
                continue;
            return ProvenAccess::unknown();
        }
        case UnionPartKind::BuiltinClass:
        case UnionPartKind::Functions:
        case UnionPartKind::Builtins:
        case UnionPartKind::OtherObjects:
            return ProvenAccess::unknown();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    result.isComplete = true;
    return result;
}

static bool addTypesOfOwnValues(const Program& program, LayoutID instanceLayout, SortedSet<TypeID, 8>& types)
{
    ReceiverState state = receiverStateOf(program, instanceLayout);
    if (!state.cannot(ObjectMutation::AddProperty) || !state.cannot(ObjectMutation::RedefineProperty))
        return false;

    LayoutID current = instanceLayout;
    for (size_t steps = 0; steps <= program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        for (uint32_t index = 0; index < layout.fields.size(); ++index) {
            const Field& field = layout.fields[index];
            if (field.flags.contains(FieldFlags::IsHidden))
                continue;
            if (!field.declared.type || field.declared.flags.contains(PositionFlags::IsExpected) || fieldTypeStateOf(program, { current, index }) != FieldTypeState::Proven)
                return false;
            types.add(field.declared.type);
        }

        Ancestry ancestry = ancestryOf(layout.kind);
        if (ancestry != Ancestry::Classes)
            return ancestry == Ancestry::BuiltinsOnly;

        switch (layout.flow.baseState) {
        case BaseState::None:
            return true;
        case BaseState::Layout:
            current = layout.flow.base;
            break;
        case BaseState::Unknown:
        case BaseState::BuiltinClass:
            return false;
        }
    }
    return false;
}

std::optional<SortedSet<TypeID, 8>> provenOwnValueTypesOf(const Program& program, const Type& receivers)
{
    std::optional<LayoutList> layouts = provenLayoutsOf(program, receivers);
    if (!layouts)
        return std::nullopt;

    SortedSet<TypeID, 8> types;
    for (LayoutID layout : *layouts) {
        if (!addTypesOfOwnValues(program, layout, types))
            return std::nullopt;
    }
    return types;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
