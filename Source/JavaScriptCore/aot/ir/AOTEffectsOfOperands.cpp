/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfOperands.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTHeldOperands.h"
#include "AOTIR.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include "JSString.h"
#include <algorithm>
#include <array>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(numberOfNamedOperandsOf(Opcode::CallBuiltin) == 1 && numberOfNamedOperandsOf(Opcode::CallInline) == 1 && numberOfNamedOperandsOf(TerminatorKind::TryCallInline) == 1);

namespace {

constexpr OperandUses listedUses { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::RetainsContents };

struct Retention {
    RetainedIn retainedIn { RetainedIn::UnknownMemory };
    uint32_t retainingOperand { 0 };

    friend bool operator==(const Retention&, const Retention&) = default;
};

} // anonymous namespace

static const Type& operandTypeOf(const Program& program, const Function& function, const Instruction& user, uint32_t operandIndex)
{
    ValueID operand = user.operand(operandIndex);
    RELEASE_ASSERT(operand);
    return program.typeOf(function, operand);
}

static void forEachDeclaredOperand(Opcode opcode, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(opcode) + (hasVariadicOperand(opcode) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex)
        functor(operandIndex);
}

static OperandBehavior behaviorOf(const OperandInfo& info)
{
    return { info.uses, info.protocols, info.namedMembers, { }, info.retainedIn, info.retainingOperand.value_or(0), OperandDeclarer::Operation, NamedMembers::AreOnlyRead };
}

OperandBehavior declaredBehaviorOf(Opcode opcode, uint32_t operandIndex)
{
    return behaviorOf(operandInfoOf(opcode, operandIndex));
}

OperandBehavior declaredBehaviorOf(TerminatorKind kind, uint32_t operandIndex)
{
    return behaviorOf(operandInfoOf(kind, operandIndex));
}

static std::optional<uint32_t> operandIndexOf(BuiltinOperand operand)
{
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        return 0;
    case BuiltinOperand::Kind::Argument:
        return 1 + operand.argumentIndex();
    case BuiltinOperand::Kind::ArgumentsFrom:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Retention retentionIn(const BuiltinPlace& place)
{
    switch (place.kind) {
    case BuiltinPlaceKind::Result:
    case BuiltinPlaceKind::ResultSlot:
    case BuiltinPlaceKind::ResultField:
        return { RetainedIn::Result, 0 };
    case BuiltinPlaceKind::Operand:
    case BuiltinPlaceKind::Slot:
    case BuiltinPlaceKind::IteratedValues:
    case BuiltinPlaceKind::OwnValues: {
        if (!place.operand)
            return { };
        std::optional<uint32_t> retainingOperand = operandIndexOf(*place.operand);
        if (!retainingOperand)
            return { };
        return { RetainedIn::Operand, *retainingOperand };
    }
    case BuiltinPlaceKind::None:
    case BuiltinPlaceKind::NewValue:
    case BuiltinPlaceKind::NativeMemory:
    case BuiltinPlaceKind::Prototype:
    case BuiltinPlaceKind::Member:
    case BuiltinPlaceKind::NativeCell:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Retention retentionOf(const BuiltinOverload& overload, BuiltinOperand operand)
{
    std::optional<Retention> result;
    for (const BuiltinTransfer& transfer : overload.flow.transfers) {
        if (transfer.from.kind != BuiltinPlaceKind::Operand || transfer.from.operand != operand)
            continue;
        Retention retention = retentionIn(transfer.to);
        if (!result)
            result = retention;
        else if (*result != retention)
            result = Retention { };
    }
    return result.value_or(Retention { });
}

OperandBehavior declaredBehaviorOf(const BuiltinOverload& overload, BuiltinOperand operand)
{
    constexpr OperandBehavior ignoredArgument { .uses = { }, .protocols = { } };

    if (overload.isUndeclared())
        return { };

    const BuiltinTable& table = BuiltinTable::singleton();
    const BuiltinParameter* parameter = nullptr;
    switch (operand.kind()) {
    case BuiltinOperand::Kind::Receiver:
        parameter = table.parameterFor(overload, operand);
        if (!parameter)
            return { };
        break;
    case BuiltinOperand::Kind::Argument:
        parameter = table.parameterFor(overload, operand);
        if (!parameter)
            return ignoredArgument;
        break;
    case BuiltinOperand::Kind::ArgumentsFrom:
        parameter = table.parameterFor(overload, BuiltinOperand::argument(operand.argumentIndex()));
        if (!parameter)
            return ignoredArgument;
        if (parameter->operand.kind() != BuiltinOperand::Kind::ArgumentsFrom)
            return { };
        break;
    }

    Retention retention = retentionOf(overload, parameter->operand);
    NamedMembers namedMemberUse = parameter->roles.contains(BuiltinOperandRole::MembersAreOnlyInspected) ? NamedMembers::AreOnlyInspected : NamedMembers::AreOnlyRead;
    bool resultOfCallEscapes = std::ranges::any_of(overload.flow.callbacks, [&](const BuiltinCallback& callback) {
        return callback.callee == parameter->operand && callback.resultUse == BuiltinCallbackResultUse::Escapes;
    });
    bool membersAreOfObjectsOnly = parameter->roles.contains(BuiltinOperandRole::MembersAreOfObjectsOnly);
    bool functionsAreConvertedUnderNamesOnly = parameter->roles.contains(BuiltinOperandRole::FunctionsAreConvertedUnderNamesOnly);
    bool isDescribedWhenRefused = parameter->roles.contains(BuiltinOperandRole::DescribedWhenRefused);
    bool onlyBoxedPrimitivesAreConverted = parameter->roles.contains(BuiltinOperandRole::OnlyBoxedPrimitivesAreConverted);
    return { parameter->uses, parameter->protocols, parameter->namedMembers, parameter->calledMembers, retention.retainedIn, retention.retainingOperand, OperandDeclarer::Builtin, namedMemberUse, resultOfCallEscapes, membersAreOfObjectsOnly, functionsAreConvertedUnderNamesOnly, parameter->convertedMembers, isDescribedWhenRefused, parameter->accepted, onlyBoxedPrimitivesAreConverted };
}

const BuiltinParameter* findParameterDescribedInMessages(const BuiltinTable& table)
{
    auto overloads = table.overloadsOf(Builtins::PrivateGlobalsMakeErrorWithCode);
    return overloads.empty() ? nullptr : table.parameterFor(overloads.front(), BuiltinOperand::argument(1));
}

Vector<BuiltinName> namesReadFromOperands(const BuiltinTable& table)
{
    Vector<BuiltinName> names;
    for (Protocol protocol : allMembersOf<Protocol>)
        names.append(table.keysReadBy(protocol));

    auto addNamesOfEntry = [&](auto entry) {
        uint32_t numberOfOperandInfos = numberOfNamedOperandsOf(entry) + hasVariadicOperand(entry);
        for (uint32_t index = 0; index < numberOfOperandInfos; ++index)
            names.append(namedMembersOf(entry, index));
    };
    for (Opcode opcode : allMembersOf<Opcode>)
        addNamesOfEntry(opcode);
    for (TerminatorKind kind : allMembersOf<TerminatorKind>)
        addNamesOfEntry(kind);

    auto addNamesOfParameter = [&](const BuiltinParameter& parameter) {
        names.append(parameter.namedMembers.span());
        names.append(parameter.calledMembers.span());
        names.append(parameter.convertedMembers.span());
    };
    for (unsigned index = 0; index < table.overloadCount(); ++index) {
        const BuiltinOverload& overload = table.at(BuiltinOverloadID(index));
        if (overload.receiver)
            addNamesOfParameter(*overload.receiver);
        for (const BuiltinParameter& parameter : overload.parameters)
            addNamesOfParameter(parameter);
    }

    AOT::sort(names, { }, &BuiltinName::index);
    removeRepeatedElements(names);
    return names;
}

static void describeCalledValue(const Program& program, const Type& callee, EffectDescription& description)
{
    CalleeList callees = calleesOf(program, callee);
    describeCallees(program, callees, BuiltinCallMode::Call, description);
    if (callees.listed().builtins.empty())
        return;
    EnteringOperands operands;
    operands.newTarget = TypeID();
    describeCallees(program, callees, BuiltinCallMode::Construct, operands, description);
}

static bool mayReturnObject(const Program& program, const Type& calledValue)
{
    CalleeList callees = calleesOf(program, calledValue);
    std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
    if (!functions)
        return true;
    return std::ranges::any_of(functions->value(), [&](FunctionID callee) {
        const Function& function = program.at(callee);
        if (function.results.size() != 1 || function.isUnsplitCoroutine() || function.result().isExpected())
            return true;
        TypeID type = *function.result().established();
        return !type || program.at(type).hasObjects();
    });
}

static bool hasOnlyInstances(const Type& operand, NOESCAPE const Invocable<bool(const ClassPart&)> auto& predicate)
{
    if (operand.anyObject || operand.anyFunction || operand.layouts || !operand.functions.isEmpty() || !operand.builtins.isEmpty())
        return false;
    return std::ranges::all_of(operand.classes, predicate);
}

static bool isFreeOfSettersAndTraps(const Type& operand)
{
    return hasOnlyInstances(operand, [](const ClassPart& part) {
        return part.provenPristine.containsAll({ PristineAspects::Storage, PristineAspects::Names });
    });
}

static bool convertsStoredValues(const BuiltinTable& table, BuiltinClassID builtinClass)
{
    return table.isSameOrSubclassOf(builtinClass, BuiltinClasses::Array)
        || table.isSameOrSubclassOf(builtinClass, BuiltinClasses::TypedArray)
        || table.at(builtinClass).origin == BuiltinOrigin::Host;
}

static bool mayConvertStoredValues(const Program& program, LayoutID id)
{
    const BuiltinTable& table = program.builtins();
    LayoutID current = id;
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        switch (layout.kind) {
        case LayoutKind::Record:
        case LayoutKind::Container:
            return false;
        case LayoutKind::BuiltinResult:
            return table.at(layout.builtinLayout).isTuple;
        case LayoutKind::Instance:
            break;
        case LayoutKind::Environment:
        case LayoutKind::Closure:
        case LayoutKind::CoroutineFrame:
        case LayoutKind::ModuleNamespace:
            return true;
        }

        switch (layout.flow.baseState) {
        case BaseState::None:
            return false;
        case BaseState::Layout:
            current = layout.flow.base;
            break;
        case BaseState::BuiltinClass:
            return convertsStoredValues(table, layout.flow.baseClass);
        case BaseState::Unknown:
            return layout.has(LayoutFlags::HasHeritage);
        }
    }
    return true;
}

static bool mayConvertStoredValues(const Program& program, const Type& operand)
{
    if (operand.anyObject || operand.anyFunction || !operand.builtins.isEmpty())
        return true;

    bool hasConvertingInstance = std::ranges::any_of(operand.classes, [&](const ClassPart& part) {
        return convertsStoredValues(program.builtins(), part.builtinClass);
    });
    return hasConvertingInstance || std::ranges::any_of(membersOf(program, operand), [&](LayoutID member) {
        return mayConvertStoredValues(program, member);
    });
}

static bool hasOnlyOrdinaryObjectsOfProgram(const Program& program, const Type& operand)
{
    if (operand.anyObject || operand.anyFunction || !operand.classes.isEmpty() || !operand.functions.isEmpty() || !operand.builtins.isEmpty())
        return false;
    return std::ranges::none_of(membersOf(program, operand), [&](LayoutID member) {
        return mayConvertStoredValues(program, member);
    });
}

static constexpr uint64_t maxReachableTypes = defaultLimitValue(LimitID::MaxAccessPlanMembers, hostArchitecture());

namespace {

// How long the text is that JSON.stringify makes of a value, at the most, by the types of all that the value reaches through ordinary objects of the program.
// A type is asked once, however many fields have it. A type that is reached from itself may be that of an object that holds itself.
class TextOfData {
public:
    TextOfData(const Program& program, uint16_t admittedPrimitives)
        : m_program(program)
        , m_admittedPrimitives(admittedPrimitives)
    {
    }

    bool fitsInString(const Type& type) { return longestOf(type).value_or(std::numeric_limits<uint64_t>::max()) <= JSString::MaxLength; }

private:
    static constexpr uint64_t charactersOfGap = 10;
    static constexpr uint64_t charactersOfLineStart = "\n"_s.length() + charactersOfGap * maxReachableTypes;

    static uint64_t quoted(uint64_t characters) { return "\"\""_s.length() + "\\u0000"_s.length() * characters; }

    std::optional<uint64_t> longestOf(const Type&);

    const Program& m_program;
    uint16_t m_admittedPrimitives;
    UncheckedKeyHashMap<const Type*, std::optional<uint64_t>> m_longestOfObjects;
};

std::optional<uint64_t> TextOfData::longestOf(const Type& type)
{
    if (type.primitives & ~m_admittedPrimitives)
        return std::nullopt;
    if (!type.hasObjects())
        return quoted(longestTextOf(m_program, type));

    auto entry = m_longestOfObjects.add(&type, std::nullopt);
    if (!entry.isNewEntry)
        return entry.iterator->value;
    if (m_longestOfObjects.size() > maxReachableTypes || !hasOnlyOrdinaryObjectsOfProgram(m_program, type))
        return std::nullopt;
    std::optional<Vector<const Field*, 8>> fields = provenOwnFieldsOf(m_program, Type::forLayoutSet(type.layouts));
    if (!fields)
        return std::nullopt;

    uint64_t longest = quoted(longestTextOf(m_program, type)) + "{}"_s.length() + charactersOfLineStart;
    for (const Field* field : *fields) {
        if (!field->key || field->form.contains(FieldForm::IsAccessor))
            return std::nullopt;
        std::optional<uint64_t> value = longestOf(m_program.at(*field->declared.established()));
        if (!value)
            return std::nullopt;
        StringID name = m_program.at(field->key).text;
        longest += charactersOfLineStart + quoted(name ? m_program.string(name).size() : 0) + ": ,"_s.length() + *value;
        if (longest > JSString::MaxLength)
            return std::nullopt;
    }
    m_longestOfObjects.set(&type, longest);
    return longest;
}

} // anonymous namespace

static bool isAdmitted(const Program& program, BuiltinTypeID admittedID, const Type& operand)
{
    const BuiltinType& admitted = program.builtins().at(admittedID);
    uint16_t primitives = primitivesOfBuiltinType(admitted);
    if (primitives & Primitive::String)
        primitives |= Primitive::LiteralString;
    if (primitives & Primitive::Symbol)
        primitives |= Primitive::LiteralSymbol;

    if (admitted.flags.contains(BuiltinTypeFlag::HasOnlyIntegers) && operand.primitives & Primitive::Integer && !operand.integers.isSubsetOf({ admitted.integers.minimum, admitted.integers.maximum }))
        return false;
    if (!admitted.flags.contains(BuiltinTypeFlag::HasPlainData))
        return operand.hasOnlyPrimitives(primitives);
    return operand.isJavaScriptValue() && TextOfData(program, primitives | Primitive::LiteralString).fitsInString(operand);
}

static bool mayRefuseChanges(const Program& program, const Type& operand, OperandUses changes)
{
    if (!hasOnlyOrdinaryObjectsOfProgram(program, operand))
        return true;
    if (changes.containsOnly({ OperandUse::ChangesIntegrity }))
        return false;
    ReceiverState state = receiverStateOf(program, operand.layouts);
    return !state.cannot(ObjectMutation::ChangeIntegrity) || !state.cannot(ObjectMutation::RedefineProperty) || !state.cannot(ObjectMutation::ChangePrototype);
}

bool mayFailOnOperand(const Program& program, const BuiltinParameter& parameter, const Type& operand)
{
    constexpr Protocols protocolsNeedingMembers { Protocol::Iterate, Protocol::CallsNamed, Protocol::Species, Protocol::HasInstance };
    constexpr uint16_t primitivesWithStringAndNumber = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString;
    constexpr OperandUses changingUses {
        OperandUse::WritesContents,
        OperandUse::MovesElements,
        OperandUse::StoresByKey,
        OperandUse::DeletesByKey,
        OperandUse::AssignsFrom,
        OperandUse::DefinesName,
        OperandUse::TakesDescriptor,
        OperandUse::ChangesIntegrity,
        OperandUse::ChangesPrototype,
    };

    if (parameter.admitted) {
        if (!isAdmitted(program, parameter.admitted, operand))
            return true;
    } else if (parameter.protocols.containsAny(protocolsNeedingMembers))
        return true;
    else if (parameter.protocols.contains(Protocol::ToPrimitive) && !operand.hasOnlyPrimitives(primitivesWithStringAndNumber))
        return true;
    OperandUses changes = parameter.uses & changingUses;
    return !changes.isEmpty() && operand.hasObjects() && mayRefuseChanges(program, operand, changes);
}

bool isDescribedByModel(const BuiltinOverload& overload)
{
    return overload.code.model && overload.effects.capability == BuiltinCapability::JS && !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
}

static CalleeList membersRunBy(const Program& program, const Type& operand, const OperandBehavior& behavior)
{
    return membersRunBy(program, operand, behavior.protocols, behavior.namedMembers, behavior.calledMembers, behavior.namedMemberUse);
}

static bool forEachProvenContents(const Program& program, const Type& operand, NOESCAPE const Invocable<void(const Type&)> auto& functor)
{
    const BuiltinTable& table = program.builtins();
    return hasOnlyInstances(operand, [&](const ClassPart& part) {
        if (part.builtinClass == BuiltinClasses::RegExp && part.provenPristine.contains(PristineAspects::AsCreated)) {
            functor(program.at(TypeTable::number));
            return true;
        }
        return part.contentsAreProven && std::ranges::all_of(table.at(part.builtinClass).slots, [&](BuiltinSlotID slot) {
            std::optional<uint8_t> indexInType = table.at(slot).indexInType;
            if (indexInType)
                functor(program.at(*part.provenContentsAt(*indexInType)));
            return indexInType.has_value();
        });
    });
}

static bool holdsOnlyPrimitives(const Program& program, const Type& operand)
{
    bool mayHoldObject = false;
    bool isProven = forEachProvenContents(program, operand, [&](const Type& contents) {
        mayHoldObject |= contents.hasObjects();
    });
    return isProven && !mayHoldObject;
}

using MembersBeingDescribed = Vector<BuiltinID, 4>;

static void describeOperandCode(const Program&, const Type& operand, const OperandBehavior&, MembersBeingDescribed&, EffectDescription&, TypeID operandID = { });

static constexpr OperandUses usesOfReaders {
    OperandUse::Inspects,
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::ReadsContents,
    OperandUse::RetainsContents,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

static constexpr Effects readsOfObjectsOperandsReach = [] {
    constexpr AbstractHeapSet heapsOnlyCompiledCodeReads = compiledCodeHeaps | AbstractHeapSet { AbstractHeap::StringBuilderContents };
    Effects effects = Effects::none();
    effects.reads = (perObjectHeaps - heapsOnlyCompiledCodeReads) | AbstractHeapSet { AbstractHeap::TypedArrayStorage, AbstractHeap::TypedArrayElements };
    return effects;
}();

static bool admitsEveryObject(const BuiltinTable& table, BuiltinTypeID testedID, const Type& objects)
{
    if (!testedID || table.at(testedID).isAnything())
        return true;

    const BuiltinType& tested = table.at(testedID);
    if (tested.flags.contains(BuiltinTypeFlag::HasAnyObject))
        return true;
    return hasOnlyInstances(objects, [&](const ClassPart& part) {
        return std::ranges::any_of(tested.parts, [&](const BuiltinTypePart& admitting) {
            return admitting.kind == BuiltinTypePartKind::Instance
                && admitting.contents.empty()
                && table.isSameOrSubclassOf(part.builtinClass, admitting.builtinClass)
                && part.provenPristine.containsAll(admitting.pristine);
        });
    });
}

// A function is described by its name, and an object of a class that the parameter takes is not refused. Of any other object that it takes, native code may refuse a member.
static bool mayBeDescribedInMessage(const BuiltinTable& table, const OperandBehavior& behavior, const Type& operand)
{
    if (!behavior.isDescribedWhenRefused)
        return false;
    if (!operand.anyObject && !operand.layouts && operand.classes.isEmpty() && operand.builtins.isEmpty())
        return false;
    bool takesAnyObject = !behavior.accepted || table.at(behavior.accepted).flags.containsAny({ BuiltinTypeFlag::IsAnything, BuiltinTypeFlag::HasAnyObject, BuiltinTypeFlag::HasPlainData });
    return takesAnyObject || !admitsEveryObject(table, behavior.accepted, operand);
}

static void forEachOverloadCalledOn(const BuiltinTable& table, const Type& receivers, BuiltinDispatchTarget target, NOESCAPE const Invocable<void(BuiltinOverloadID)> auto& functor)
{
    if (target.isOverload()) {
        functor(target.overload);
        return;
    }

    const BuiltinDispatchNode& node = table.at(target.node);
    for (const BuiltinDispatchBranch& branch : node.branches) {
        forEachOverloadCalledOn(table, receivers, branch.target, functor);
        if (node.operand.isReceiver() && admitsEveryObject(table, branch.tested, receivers))
            return;
    }
}

namespace {

class ReachableValueWalk {
public:
    ReachableValueWalk(const Program& program, const OperandBehavior& behavior, MembersBeingDescribed& membersBeingDescribed, EffectDescription& description)
        : m_program(program)
        , m_behavior(behavior)
        , m_membersBeingDescribed(membersBeingDescribed)
        , m_description(description)
    {
    }

    void run(const Type& start);

private:
    void reach(const Type&);
    void visit(const UnionPart&);
    void visitLayouts(const Type& layouts);
    void visitInstances(const Type& instances);
    void runProtocolsOn(const Type& receivers, Protocols);
    bool findsNamedMembersInFields(const Type& receivers) const;
    void describeCallOn(const Type& receivers, BuiltinID member);
    void describeOverloadOn(const Type& receivers, BuiltinOverloadID);
    bool covers(const OperandBehavior&) const;

    const Program& m_program;
    const OperandBehavior& m_behavior;
    MembersBeingDescribed& m_membersBeingDescribed;
    EffectDescription& m_description;
    Vector<const Type*, 8> m_reached;
    size_t m_numberOfVisitedTypes { 0 };
};

void ReachableValueWalk::run(const Type& start)
{
    constexpr Protocols followedProtocols {
        Protocol::OwnKeys,
        Protocol::OwnValues,
        Protocol::PrototypeChain,
        Protocol::ToPrimitive,
        Protocol::ToJSON,
        Protocol::Iterate,
        Protocol::Named,
        Protocol::CallsNamed,
    };
    if (!followedProtocols.containsAll(m_behavior.protocols)) {
        m_description.addUnknownCode();
        return;
    }

    reach(start);
    while (m_numberOfVisitedTypes < m_reached.size()) {
        for (const UnionPart& part : unionPartsOf(*m_reached[m_numberOfVisitedTypes++]))
            visit(part);
    }
}

void ReachableValueWalk::reach(const Type& type)
{
    if (m_reached.contains(&type))
        return;
    if (m_reached.size() >= maxReachableTypes) {
        m_description.addUnknownCode();
        return;
    }
    m_reached.append(&type);
}

void ReachableValueWalk::visit(const UnionPart& part)
{
    constexpr Protocols memberProtocols { Protocol::Named, Protocol::CallsNamed, Protocol::Iterate };

    switch (part.kind) {
    case UnionPartKind::Nullish:
    case UnionPartKind::Absent:
        return;
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::String:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
        if (!m_description.callsUnknownCode)
            runProtocolsOn(part.type, m_behavior.membersAreOfObjectsOnly ? m_behavior.protocols - memberProtocols : m_behavior.protocols);
        return;
    case UnionPartKind::Layouts:
        visitLayouts(part.type);
        return;
    case UnionPartKind::BuiltinClass:
        visitInstances(part.type);
        return;
    case UnionPartKind::Functions:
        describeCalledValue(m_program, part.type, m_description);
        m_description.addUnknownCode();
        return;
    case UnionPartKind::Builtins:
    case UnionPartKind::OtherObjects:
        m_description.addUnknownCode();
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ReachableValueWalk::visitLayouts(const Type& layouts)
{
    m_description.addOwn(readsOfObjectsOperandsReach);

    std::optional<Vector<const Field*, 8>> fields = provenOwnFieldsOf(m_program, layouts);
    bool areBelowNoBuiltinClass = fields.has_value();
    runProtocolsOn(layouts, areBelowNoBuiltinClass && m_behavior.onlyBoxedPrimitivesAreConverted ? m_behavior.protocols - Protocol::ToPrimitive : m_behavior.protocols);
    if (!fields) {
        m_description.addUnknownCode();
        return;
    }
    m_description.followsFieldsOfOperands = true;
    for (const Field* field : *fields)
        reach(m_program.at(*field->declared.established()));
}

void ReachableValueWalk::visitInstances(const Type& instances)
{
    constexpr Protocols stepsOnProperties { Protocol::OwnKeys, Protocol::OwnValues, Protocol::PrototypeChain };

    m_description.addOwn(readsOfObjectsOperandsReach);
    if (!isFreeOfSettersAndTraps(instances)) {
        m_description.addUnknownCode();
        return;
    }
    runProtocolsOn(instances, m_behavior.protocols - stepsOnProperties);

    bool isProven = forEachProvenContents(m_program, instances, [&](const Type& contents) {
        reach(contents);
    });
    if (!isProven)
        m_description.addUnknownCode();
}

void ReachableValueWalk::runProtocolsOn(const Type& receivers, Protocols protocols)
{
    constexpr Protocols protocolsKeepingResults { Protocol::ToJSON, Protocol::CallsNamed };

    CalleeList membersWithKeptResults = membersRunBy(m_program, receivers, protocols & protocolsKeepingResults, { }, m_behavior.calledMembers, m_behavior.namedMemberUse);
    describeCallees(m_program, membersWithKeptResults, BuiltinCallMode::Call, m_description);

    CalleeList members = membersRunBy(m_program, receivers, protocols - protocolsKeepingResults, m_behavior.namedMembers, { }, NamedMembers::AreOnlyInspected);
    for (BuiltinID member : members.listed().builtins)
        describeCallOn(receivers, member);
    members.removeBuiltins();
    describeCallees(m_program, members, BuiltinCallMode::Call, m_description);

    bool returnsNothingReachable = membersWithKeptResults.runsNothing() && members.listed().functions.empty();
    if (!returnsNothingReachable || (protocols.contains(Protocol::Named) && !findsNamedMembersInFields(receivers)))
        m_description.addUnknownCode();
}

bool ReachableValueWalk::findsNamedMembersInFields(const Type& receivers) const
{
    return std::ranges::all_of(m_behavior.namedMembers, [&](BuiltinName name) {
        ProvenAccess read = provenReadOf(m_program, receivers, name);
        return read.isComplete && read.methods.isEmpty() && read.builtins.isEmpty() && !read.mayFindPrototypeProperty;
    });
}

void ReachableValueWalk::describeCallOn(const Type& receivers, BuiltinID member)
{
    const BuiltinTable& table = m_program.builtins();
    std::optional<BuiltinDispatchTarget> root = table.dispatchOf(table.canonicalID(member), BuiltinCallMode::Call);
    if (!root || m_membersBeingDescribed.contains(member)) {
        describeCallOf(m_program, member, BuiltinCallMode::Call, m_description);
        return;
    }

    m_membersBeingDescribed.append(member);
    forEachOverloadCalledOn(table, receivers, *root, [&](BuiltinOverloadID overload) {
        describeOverloadOn(receivers, overload);
    });
    m_membersBeingDescribed.removeLast();
}

void ReachableValueWalk::describeOverloadOn(const Type& receivers, BuiltinOverloadID id)
{
    describeOverload(m_program, id, TypeID(), { }, m_description);

    const BuiltinOverload& overload = m_program.builtins().at(id);
    bool isDeclaredByParameters = !overload.isUndeclared() && !overload.libraryFunction() && !isDescribedByModel(overload);
    if (!isDeclaredByParameters)
        return;

    if (overload.receiver) {
        OperandBehavior behaviorOfReceiver = declaredBehaviorOf(overload, BuiltinOperand::receiver());
        if (!covers(behaviorOfReceiver))
            describeOperandCode(m_program, receivers, behaviorOfReceiver, m_membersBeingDescribed, m_description);
    }

    const Type& anything = m_program.at(TypeTable::anything);
    for (const BuiltinParameter& parameter : overload.parameters)
        describeOperandCode(m_program, anything, declaredBehaviorOf(overload, parameter.operand), m_membersBeingDescribed, m_description);
    if (!overload.parameters.empty())
        m_description.addThrow();
}

bool ReachableValueWalk::covers(const OperandBehavior& behavior) const
{
    constexpr Protocols protocolsWithLists { Protocol::Named, Protocol::CallsNamed };
    return behavior.uses.containsOnly(usesOfReaders) && (m_behavior.protocols - protocolsWithLists).containsAll(behavior.protocols);
}

} // anonymous namespace

static void describeCodeOfReachableValues(const Program& program, const Type& operand, const OperandBehavior& behavior, MembersBeingDescribed& membersBeingDescribed, EffectDescription& description)
{
    bool hasProvenObjects = operand.layouts || !operand.classes.isEmpty();
    if (!hasProvenObjects || !behavior.uses.containsOnly(usesOfReaders)) {
        description.addUnknownCode();
        return;
    }
    ReachableValueWalk(program, behavior, membersBeingDescribed, description).run(operand);
}

static void describeCodeOfContents(const Program& program, const Type& operand, const OperandBehavior& behavior, MembersBeingDescribed& membersBeingDescribed, EffectDescription& description)
{
    ReachableValueWalk walk(program, behavior, membersBeingDescribed, description);
    bool isProven = forEachProvenContents(program, operand, [&](const Type& contents) {
        walk.run(contents);
    });
    if (!isProven)
        description.addUnknownCode();
    if (operand.primitives & Primitive::AnyString)
        describeCallees(program, membersRunBy(program, Type::fromPrimitives(Primitive::String), behavior), BuiltinCallMode::Call, description);
}

// An object remembers that it has been a prototype, and what is done to it later goes by that.
static constexpr Effects effectsOfBecomingPrototype = [] {
    Effects effects = Effects::none();
    effects.writes = AbstractHeapSet { AbstractHeap::PrototypeLinks };
    return effects;
}();

static bool isCreatedAsPrototype(const Program& program, const Type& objects)
{
    if (objects.anyObject || objects.anyFunction || objects.layouts || !objects.classes.isEmpty() || !objects.functions.isEmpty())
        return false;
    return std::ranges::all_of(objects.builtins, [&](BuiltinID builtin) {
        return program.builtins().at(builtin).kind == BuiltinKind::Prototype;
    });
}

Effects effectsOfCreatingPrototypeProperty(const Program& program, const Type& functions)
{
    Effects effects = Effects::none();
    if (functions.anyFunction || functions.anyObject) {
        effects.writes.add(AbstractHeap::MemberGuards);
        return effects;
    }
    for (FunctionID function : functions.functions) {
        if (prototypePropertyMayEndGuards(program, function))
            effects.writes.add(program, MemoryLocation::memberGuards(program.at(function).constructedClass));
    }
    return effects;
}

void describeOperandCode(const Program& program, const Type& operand, const OperandBehavior& behavior, EffectDescription& description)
{
    MembersBeingDescribed membersBeingDescribed;
    describeOperandCode(program, operand, behavior, membersBeingDescribed, description);
}

static bool isSameList(std::span<const BuiltinName> a, std::span<const BuiltinName> b)
{
    return a.data() == b.data() && a.size() == b.size();
}

bool OperandBehavior::saysTheSameFromTheSameLists(const OperandBehavior& other) const
{
    return uses == other.uses && protocols == other.protocols && isSameList(namedMembers, other.namedMembers) && isSameList(calledMembers, other.calledMembers)
        && retainedIn == other.retainedIn && retainingOperand == other.retainingOperand && declarer == other.declarer && namedMemberUse == other.namedMemberUse
        && resultOfCallEscapes == other.resultOfCallEscapes && membersAreOfObjectsOnly == other.membersAreOfObjectsOnly
        && functionsAreConvertedUnderNamesOnly == other.functionsAreConvertedUnderNamesOnly && isSameList(convertedMembers, other.convertedMembers)
        && isDescribedWhenRefused == other.isDescribedWhenRefused && accepted == other.accepted && onlyBoxedPrimitivesAreConverted == other.onlyBoxedPrimitivesAreConverted;
}

static uint32_t hashOf(TypeID operand, const OperandBehavior& behavior, const EffectDescription& description)
{
    uint64_t flags = static_cast<uint64_t>(behavior.retainedIn) << 16 | static_cast<uint64_t>(behavior.declarer) << 8 | static_cast<uint64_t>(behavior.namedMemberUse) << 7
        | static_cast<uint64_t>(behavior.resultOfCallEscapes) << 6 | static_cast<uint64_t>(behavior.membersAreOfObjectsOnly) << 5 | static_cast<uint64_t>(behavior.functionsAreConvertedUnderNamesOnly) << 4
        | static_cast<uint64_t>(behavior.isDescribedWhenRefused) << 3 | static_cast<uint64_t>(behavior.onlyBoxedPrimitivesAreConverted) << 2
        | static_cast<uint64_t>(description.callsUnknownCode) << 1 | static_cast<uint64_t>(description.leavesOutOwnEffects);
    uint64_t hash = operand.index();
    for (uint64_t word : { static_cast<uint64_t>(behavior.uses.toRaw()), static_cast<uint64_t>(behavior.protocols.toRaw()), flags, static_cast<uint64_t>(behavior.retainingOperand) << 32 | behavior.accepted.index(),
        static_cast<uint64_t>(std::bit_cast<uintptr_t>(behavior.namedMembers.data())), static_cast<uint64_t>(std::bit_cast<uintptr_t>(behavior.calledMembers.data())), static_cast<uint64_t>(std::bit_cast<uintptr_t>(behavior.convertedMembers.data())) })
        hash = (hash ^ hash >> 32 ^ word) * 0x9e3779b97f4a7c15ull;
    return static_cast<uint32_t>(hash >> 32);
}

// What is added depends on the description only by the two flags that an entry is found by.
void describeOperandCode(const Program& program, TypeID operand, const OperandBehavior& behavior, EffectDescription& description)
{
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache || TypeTable::isProvisional(operand)) {
        describeOperandCode(program, program.at(operand), behavior, description);
        return;
    }

    auto describeAnew = [&] {
        EffectDescription added;
        added.callsUnknownCode = description.callsUnknownCode;
        added.leavesOutOwnEffects = description.leavesOutOwnEffects;
        MembersBeingDescribed membersBeingDescribed;
        describeOperandCode(program, program.at(operand), behavior, membersBeingDescribed, added, operand);
        return added;
    };
    auto isWanted = [&](const PropertyAccessCache::CodeOfOperand& entry) {
        return entry.operand == operand && entry.calledUnknownCode == description.callsUnknownCode && entry.added.leavesOutOwnEffects == description.leavesOutOwnEffects
            && entry.behavior.saysTheSameFromTheSameLists(behavior);
    };
    uint32_t hash = hashOf(operand, behavior, description);
    const PropertyAccessCache::CodeOfOperand* known = cache->codeOfOperands.find(hash, isWanted);
    if (!known)
        known = &cache->codeOfOperands.add(hash, { operand, behavior, description.callsUnknownCode, describeAnew() }, isWanted);
    RELEASE_ASSERT(!cache->checksAnswers || known->added == describeAnew());
    description.add(known->added);
}

static void describeOperandCode(const Program& program, const Type& operand, const OperandBehavior& behavior, MembersBeingDescribed& membersBeingDescribed, EffectDescription& description, TypeID operandID)
{
    constexpr OperandUses usesRunningSettersAndTraps { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DeletesByKey };
    constexpr OperandUses usesStoringValues { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::TakesDescriptor };

    bool hasObjects = operand.hasObjects();
    if (mayBeDescribedInMessage(program.builtins(), behavior, operand)) {
        if (const BuiltinParameter* described = findParameterDescribedInMessages(program.builtins())) {
            OperandBehavior behaviorOfMessage { described->uses, described->protocols, described->namedMembers, described->calledMembers };
            if (operandID && membersBeingDescribed.isEmpty())
                describeOperandCode(program, operandID, behaviorOfMessage, description);
            else
                describeOperandCode(program, operand, behaviorOfMessage, membersBeingDescribed, description);
        } else
            description.addUnknownCode();
    }
    if (!behavior.protocols.isEmpty()) {
        if (hasObjects && behavior.uses.contains(OperandUse::AppliesTransitively))
            describeCodeOfReachableValues(program, operand, behavior, membersBeingDescribed, description);
        else if (behavior.uses.contains(OperandUse::AppliesToContents))
            describeCodeOfContents(program, operand, behavior, membersBeingDescribed, description);
        else {
            describeCallees(program, membersRunBy(program, operand, behavior), BuiltinCallMode::Call, description);
            description.addOwn(readsOfMemberLookups(operand, behavior.protocols));
        }
        description.addOwn(effectsOfCreatingPrototypeProperty(program, operand));
    }
    if (!hasObjects)
        return;
    if (behavior.uses.contains(OperandUse::BecomesPrototype) && !isCreatedAsPrototype(program, operand))
        description.addOwn(effectsOfBecomingPrototype);
    if (behavior.declarer == OperandDeclarer::Builtin && behavior.uses.containsAny(usesRunningSettersAndTraps) && !isFreeOfSettersAndTraps(operand))
        description.addUnknownCode();
    if (behavior.declarer == OperandDeclarer::Builtin && behavior.uses.containsAny(usesStoringValues) && !description.callsUnknownCode && mayConvertStoredValues(program, operand))
        description.addUnknownCode();
    if (!behavior.uses.containsAny(callingUses))
        return;
    describeCalledValue(program, operand, description);
    if (behavior.resultOfCallEscapes && mayReturnObject(program, operand))
        description.addUnknownCode();
}

static bool mayTakeCallee(const Program& program, const Type& calledValue)
{
    CalleeList callees = calleesOf(program, calledValue);
    std::optional<ProvenValue<Callees>> all = callees.all();
    return !all || std::ranges::any_of(all->value().functions, [&](FunctionID callee) {
        return program.at(callee).calleeParameterIndex().has_value();
    });
}

static bool isPassedToOperandCode(const Program& program, const Type& operand, const OperandBehavior& behavior)
{
    if (behavior.uses.containsAny(callingUses) && mayTakeCallee(program, operand))
        return true;
    if (behavior.protocols.isEmpty() && behavior.declarer != OperandDeclarer::Builtin)
        return false;

    OperandBehavior protocolBehavior = behavior;
    protocolBehavior.uses.remove(callingUses);
    EffectDescription description;
    description.leavesOutOwnEffects = true;
    describeOperandCode(program, operand, protocolBehavior, description);
    return description.callsUnknownCode || !description.functions.isEmpty();
}

static OperandSummary summarizeDeclaredUses(const OperandBehavior& behavior)
{
    if (behavior.uses.isEmpty())
        return { };
    return { behavior.uses, behavior.retainedIn, behavior.retainingOperand };
}

OperandSummary joinOf(const OperandSummary& a, const OperandSummary& b)
{
    OperandUses uses = a.uses | b.uses;
    if (!b.uses.contains(OperandUse::Retains))
        return { uses, a.retainedIn, a.retainingOperand };
    if (!a.uses.contains(OperandUse::Retains))
        return { uses, b.retainedIn, b.retainingOperand };
    if (a.retainedIn == b.retainedIn && a.retainingOperand == b.retainingOperand)
        return { uses, a.retainedIn, a.retainingOperand };
    return { uses, RetainedIn::UnknownMemory, 0 };
}

template<typename Entry>
static OperandSummary summarizeDeclaredOperand(const Program& program, const Type& type, Entry entry, uint32_t operandIndex, UsesAskedFor askedFor)
{
    OperandBehavior behavior = declaredBehaviorOf(entry, operandIndex);
    OperandSummary summary = summarizeDeclaredUses(behavior);
    if (operandKindOf(entry, operandIndex) == OperandKind::Results)
        summary.uses.add(OperandUse::Returns);
    if (askedFor == UsesAskedFor::AllButPassesToUserCode || !mayBeReference(type))
        return summary;

    bool isHandedToUnnamedCode = !behavior.uses.contains(OperandUse::RetainsContents) && declaredBoundOf(entry).mayCallUnknownCode;
    if (isHandedToUnnamedCode || isPassedToOperandCode(program, type, behavior))
        summary.uses.add(OperandUse::PassesToUserCode);
    return summary;
}

static bool isArgumentOfHasInstanceMethod(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
    bool runsHasInstance = false;
    forEachDeclaredOperand(user.opcode, [&](uint32_t declaredIndex) {
        runsHasInstance |= protocolsOf(user.opcode, declaredIndex).contains(Protocol::HasInstance);
    });
    if (!runsHasInstance)
        return false;

    bool isArgument = false;
    user.forEachOperand([&](ValueID other, uint32_t otherIndex) {
        if (otherIndex == operandIndex || !protocolsOf(user.opcode, otherIndex).contains(Protocol::HasInstance))
            return;
        isArgument |= isPassedToOperandCode(program, program.typeOf(function, other), { .uses = { }, .protocols = Protocol::HasInstance });
    });
    return isArgument;
}

std::optional<uint32_t> entryParameterOf(const Function& callee, const Value& call, uint32_t operandIndex)
{
    if (effectRuleOf(call.opcode) != EffectRule::Call || isHeldOperand(call, operandIndex))
        return std::nullopt;

    std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
    if (!argument) {
        if (OperandPosition::thisValue(call.opcode) == operandIndex)
            return callee.thisParameterIndex();
        if (OperandPosition::newTarget(call.opcode) == operandIndex)
            return callee.newTargetParameterIndex();
        if (operandUsesOf(call.opcode, operandIndex).containsAny(callingUses))
            return callee.calleeParameterIndex();
        return std::nullopt;
    }
    if (!VariadicOperandPosition::arguments(call.opcode))
        return std::nullopt;

    if (operandKindOf(call.opcode, operandIndex) == OperandKind::Declared) {
        bool followsCalleeHeader = !Value::opcodesWithCallees.contains(call.opcode) || call.opcode == Opcode::CallTable;
        if (!followsCalleeHeader || *argument >= callee.numberOfEntryParameters())
            return std::nullopt;
        return argument;
    }

    if (callee.hasAny({ FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest }))
        return std::nullopt;
    if (*argument >= callee.numberOfPositionalParameters())
        return std::nullopt;
    return callee.firstParameterIndex() + *argument;
}

Vector<uint32_t, 2> entryParametersOfCalledValue(const Function& callee, const Value& call)
{
    Vector<uint32_t, 2> parameters;
    if (std::optional<unsigned> parameter = callee.calleeParameterIndex())
        parameters.append(*parameter);

    std::optional<unsigned> newTarget = OperandPosition::newTarget(call.opcode);
    if (!newTarget || call.operand(*newTarget))
        return parameters;
    if (std::optional<unsigned> parameter = callee.newTargetParameterIndex())
        parameters.append(*parameter);
    return parameters;
}

std::optional<uint32_t> findOperandPassedFor(const Function& callee, const Value& call, uint32_t entryParameter)
{
    std::optional<uint32_t> result;
    call.forEachOperand([&](ValueID, uint32_t operandIndex) {
        if (!result && entryParameterOf(callee, call, operandIndex) == entryParameter)
            result = operandIndex;
    });
    return result;
}

static OperandSummary summarizeArgumentOf(const EffectContext& context, const Value& call, FunctionID callee, std::optional<uint32_t> entryParameter)
{
    if (!entryParameter)
        return { };

    OperandSummary summary = CalleeSummary(context, callee)->operand(*entryParameter);
    summary.uses.add(OperandUse::Inspects);
    if (summary.retainedIn != RetainedIn::Operand)
        return summary;

    if (std::optional<uint32_t> retainingOperand = findOperandPassedFor(context.program.at(callee), call, summary.retainingOperand)) {
        summary.retainingOperand = *retainingOperand;
        return summary;
    }
    summary.retainedIn = RetainedIn::UnknownMemory;
    summary.retainingOperand = 0;
    return summary;
}

static OperandSummary summarizeCallOperand(const EffectContext& context, const Type& type, const Value& call, uint32_t operandIndex, UsesAskedFor askedFor)
{
    OperandBehavior behavior = declaredBehaviorOf(call.opcode, operandIndex);
    if (behavior.uses.contains(OperandUse::RetainsContents))
        return summarizeDeclaredOperand(context.program, type, call.opcode, operandIndex, askedFor);

    CalleeList callees = calleesOfCall(context.program, call);
    bool isCalledValue = behavior.uses.containsAny(callingUses);
    if (isCalledValue && call.opcode == Opcode::Call && !callees.listsEveryCallee())
        return summarizeDeclaredUses(behavior);
    std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone();
    if (!functions || maySuspend(call.opcode))
        return { };

    // Nothing that is joined to a summary that allows everything changes it.
    constexpr OperandSummary allowsEverything;
    if (isCalledValue) {
        OperandSummary summary = summarizeDeclaredUses(behavior);
        for (FunctionID callee : functions->value()) {
            if (summary == allowsEverything)
                break;
            for (uint32_t parameter : entryParametersOfCalledValue(context.program.at(callee), call))
                summary = joinOf(summary, summarizeArgumentOf(context, call, callee, parameter));
        }
        return summary;
    }

    OperandSummary summary { OperandUse::Inspects, RetainedIn::UnknownMemory, 0 };
    for (FunctionID callee : functions->value()) {
        if (summary == allowsEverything)
            break;
        summary = joinOf(summary, summarizeArgumentOf(context, call, callee, entryParameterOf(context.program.at(callee), call, operandIndex)));
    }
    return summary;
}

static bool mayRunUnnamedCode(const BuiltinOverload& overload)
{
    if (overload.isUndeclared())
        return true;
    switch (overload.effects.capability) {
    case BuiltinCapability::JS:
        return !overload.flags.contains(BuiltinOverloadFlag::RunsOnlyOperandCode);
    case BuiltinCapability::Heap:
    case BuiltinCapability::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinOperand builtinOperandOf(const Instruction& call, uint32_t operandIndex)
{
    std::optional<uint32_t> argument = call.variadicOperandIndex(operandIndex);
    return argument ? BuiltinOperand::argument(*argument) : BuiltinOperand::receiver();
}

BuiltinOperand builtinOperandAt(const Value& builtinCall, uint32_t operandIndex)
{
    return builtinOperandOf(builtinCall, operandIndex);
}

template<typename Carrier>
static OperandSummary summarizeOperandOfOverload(const Program&, const Type&, const Carrier& call, const BuiltinOverload&, BuiltinOperand, UsesAskedFor);

template<typename Carrier>
static OperandSummary summarizeArgumentOfAnyOverload(const Program& program, const Type& type, const Carrier& call, const BuiltinOverload& named, BuiltinOperand operand, UsesAskedFor askedFor)
{
    std::optional<OperandSummary> joined;
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(named.builtin)) {
        if (overload.mode != named.mode)
            continue;
        if (mayRunUnnamedCode(overload))
            return { };
        OperandSummary summary = summarizeOperandOfOverload(program, type, call, overload, operand, askedFor);
        joined = joined ? joinOf(*joined, summary) : summary;
    }
    return joined.value_or(OperandSummary { });
}

template<typename Carrier>
static OperandSummary summarizeBuiltinOperand(const Program& program, const Type& type, const Carrier& call, uint32_t operandIndex, UsesAskedFor askedFor)
{
    const BuiltinOverload& overload = program.builtins().at(call.overload());
    if (mayRunUnnamedCode(overload))
        return { };

    BuiltinOperand operand = builtinOperandOf(call, operandIndex);
    switch (builtinCallOf(program, call).form) {
    case BuiltinCallForm::AnyReceiver:
        if (operand.isReceiver())
            return { };
        return summarizeArgumentOfAnyOverload(program, type, call, overload, operand, askedFor);
    case BuiltinCallForm::List:
        if (!operand.isReceiver())
            return { listedUses, RetainedIn::UnknownMemory, 0 };
        break;
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        break;
    }
    return summarizeOperandOfOverload(program, type, call, overload, operand, askedFor);
}

template<typename Carrier>
static OperandSummary summarizeOperandOfOverload(const Program& program, const Type& type, const Carrier& call, const BuiltinOverload& overload, BuiltinOperand operand, UsesAskedFor askedFor)
{
    OperandBehavior behavior = declaredBehaviorOf(overload, operand);
    OperandSummary summary { behavior.uses, behavior.retainedIn, behavior.retainingOperand };
    summary.uses.add(OperandUse::Inspects);
    if (summary.retainedIn == RetainedIn::Operand && (summary.retainingOperand >= call.numberOfOperands() || !call.operand(summary.retainingOperand))) {
        summary.retainedIn = RetainedIn::UnknownMemory;
        summary.retainingOperand = 0;
    }
    if (holdsOnlyPrimitives(program, type))
        summary.uses.remove(OperandUse::AppliesToContents);
    if (askedFor == UsesAskedFor::All && isPassedToOperandCode(program, type, behavior))
        summary.uses.add(OperandUse::PassesToUserCode);
    return summary;
}

static OperandSummary summarizeAccessOperand(const Program& program, const Function& function, const Type& type, const Value& access, uint32_t operandIndex, UsesAskedFor askedFor)
{
    OperandBehavior behavior = declaredBehaviorOf(access.opcode, operandIndex);
    OperandSummary summary = summarizeDeclaredUses(behavior);
    if (askedFor == UsesAskedFor::AllButPassesToUserCode || !mayBeReference(type))
        return summary;

    auto isHandedToAccessor = [&] {
        ProvenAccess storage;
        const ProvenAccess& proven = provenAccessOf(program, function, access, storage);
        return proven.isComplete ? !proven.accessors.isEmpty() : declaredBoundOf(access.opcode).mayCallUnknownCode;
    };
    if (isPassedToOperandCode(program, type, behavior) || isHandedToAccessor())
        summary.uses.add(OperandUse::PassesToUserCode);
    return summary;
}

OperandSummary summarizeOperand(const EffectContext& context, const Function& function, const Value& user, uint32_t operandIndex, UsesAskedFor askedFor)
{
    const Type& type = operandTypeOf(context.program, function, user, operandIndex);
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Call:
        return summarizeCallOperand(context, type, user, operandIndex, askedFor);
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return summarizeBuiltinOperand(context.program, type, user, operandIndex, askedFor);
    case EffectRule::Access:
        return summarizeAccessOperand(context.program, function, type, user, operandIndex, askedFor);
    case EffectRule::Declared:
    case EffectRule::Operator: {
        OperandSummary summary = summarizeDeclaredOperand(context.program, type, user.opcode, operandIndex, askedFor);
        if (askedFor == UsesAskedFor::All && mayBeReference(type) && isArgumentOfHasInstanceMethod(context.program, function, user, operandIndex))
            summary.uses.add(OperandUse::PassesToUserCode);
        if (user.opcode == Opcode::Eq && summary.uses.contains(OperandUse::PassesToUserCode)) {
            EffectDescription description = describeCodeRunBy(context.program, function, user);
            if (!description.callsUnknownCode && description.functions.isEmpty())
                summary.uses.remove(OperandUse::PassesToUserCode);
        }
        return summary;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OperandSummary summarizeOperand(const EffectContext& context, const Function& function, const Terminator& user, uint32_t operandIndex, UsesAskedFor askedFor)
{
    const Type& type = operandTypeOf(context.program, function, user, operandIndex);
    switch (effectRuleOf(user.kind)) {
    case EffectRule::Inline:
        return summarizeBuiltinOperand(context.program, type, user, operandIndex, askedFor);
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
    case EffectRule::Builtin:
        return summarizeDeclaredOperand(context.program, type, user.kind, operandIndex, askedFor);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool writesReceiver(Access access)
{
    switch (access) {
    case Access::Store:
    case Access::Define:
        return true;
    case Access::None:
    case Access::Read:
    case Access::Has:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

bool isWrittenOperand(Opcode opcode, uint32_t operandIndex)
{
    if (operandUsesOf(opcode, operandIndex).containsAny(operandWritingUses))
        return true;
    if (receiverOf(opcode) == operandIndex && writesReceiver(accessOf(opcode)))
        return true;

    bool isRetainingOperand = false;
    forEachDeclaredOperand(opcode, [&](uint32_t retainedIndex) {
        isRetainingOperand |= retainedInOf(opcode, retainedIndex) == RetainedIn::Operand && retainingOperandOf(opcode, retainedIndex) == operandIndex;
    });
    return isRetainingOperand;
}

static OperandConfinement computeConfinementOf(Opcode opcode)
{
    bool mayReachOtherObjects = false;
    bool namesWrittenOperand = false;
    forEachDeclaredOperand(opcode, [&](uint32_t operandIndex) {
        mayReachOtherObjects |= operandUsesOf(opcode, operandIndex).containsAny(unconfinedUses | callingUses) || !protocolsOf(opcode, operandIndex).isEmpty();
        namesWrittenOperand |= isWrittenOperand(opcode, operandIndex);
    });
    if (mayReachOtherObjects || !namesWrittenOperand)
        return OperandConfinement::Unknown;
    return OperandConfinement::WritesOnlyNamedOperands;
}

OperandConfinement confinementOf(Opcode opcode)
{
    static const std::array<OperandConfinement, numberOfOpcodes> answers = [] {
        std::array<OperandConfinement, numberOfOpcodes> result { };
        for (Opcode member : allMembersOf<Opcode>)
            result[static_cast<unsigned>(member)] = computeConfinementOf(member);
        return result;
    }();
    return answers[static_cast<unsigned>(opcode)];
}

static OperandMemory memoryAccessedByCallees(const EffectContext& context, const Value& call, uint32_t operandIndex, ProvenValue<std::span<const FunctionID>> callees)
{
    bool isCalledValue = operandUsesOf(call.opcode, operandIndex).containsAny(callingUses);
    OperandMemory memory = OperandMemory::none();
    for (FunctionID id : callees.value()) {
        const Function& callee = context.program.at(id);
        CalleeSummary summary(context, id);
        if (isCalledValue) {
            for (uint32_t parameter : entryParametersOfCalledValue(callee, call))
                memory.merge(summary->memory(parameter));
            continue;
        }
        std::optional<uint32_t> parameter = entryParameterOf(callee, call, operandIndex);
        memory.merge(parameter ? summary->memory(*parameter) : OperandMemory());
    }
    return memory;
}

OperandMemory memoryAccessedThrough(const EffectContext& context, const Function& function, const Value& user, uint32_t operandIndex, const EffectsOnEdges& effectsOfUser)
{
    if (!mayBeReference(operandTypeOf(context.program, function, user, operandIndex)))
        return OperandMemory::none();

    if (effectRuleOf(user.opcode) == EffectRule::Call && !maySuspend(user.opcode)) {
        CalleeList callees = calleesOfCall(context.program, user);
        if (std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone())
            return memoryAccessedByCallees(context, user, operandIndex, *functions);
    }

    Effects effects = effectsOfUser.onAnyEdge();
    if (effects.mayRunOtherCode())
        return OperandMemory();

    OperandMemory memory { effects.reads.restrictedTo(perObjectHeaps), effects.writes.restrictedTo(perObjectHeaps) };
    uint32_t declaredIndex = std::min(operandIndex, numberOfNamedOperandsOf(user.opcode));
    if (confinementOf(user.opcode) != OperandConfinement::WritesOnlyNamedOperands || isWrittenOperand(user.opcode, declaredIndex))
        return memory;
    if (describeEffects(context.program, function, user).functions.isEmpty())
        memory.writes = LocationMasks();
    return memory;
}

bool mayBeReference(const Type& type)
{
    return type.hasObjects() || type.environment || type.opaque;
}

static ResultFreshness freshnessOfDeclaredResult(const Program& program, const Function& function, BuiltinOverloadID overload, ValueID receiver)
{
    const BuiltinTable& table = program.builtins();
    BuiltinTypeID resultType = table.at(overload).result.type;
    if (!table.contains(resultType))
        return ResultFreshness::Unknown;

    switch (table.at(resultType).freshness) {
    case BuiltinFreshness::No:
        return ResultFreshness::Unknown;
    case BuiltinFreshness::Unaliased:
        return ResultFreshness::Unaliased;
    case BuiltinFreshness::Yes:
        return ResultFreshness::Fresh;
    case BuiltinFreshness::UnlessSpecies:
        if (!receiver || membersRunBy(program, program.typeOf(function, receiver), Protocol::Species, { }, { }, NamedMembers::AreOnlyRead).mayRunUserCode())
            return ResultFreshness::Unknown;
        return ResultFreshness::Fresh;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Carrier>
static ResultFreshness freshnessOfBuiltinResult(const Program& program, const Function& function, const Carrier& carrier)
{
    BuiltinCall call = builtinCallOf(program, carrier);
    switch (call.form) {
    case BuiltinCallForm::Whole:
        return freshnessOfDeclaredResult(program, function, call.overload, call.receiver);
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
    case BuiltinCallForm::AnyReceiver:
    case BuiltinCallForm::List:
        return ResultFreshness::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool answersWithDeclaredResult(Opcode opcode)
{
    return Value::opcodesWithOverload.contains(opcode) && resultRuleOf(opcode) == ResultRule::Declared;
}

static bool computeWhetherResultMayBeFresh(Opcode opcode)
{
    switch (effectRuleOf(opcode)) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return true;
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
        break;
    }
    if (answersWithDeclaredResult(opcode))
        return true;
    if (boundToOf(opcode) == BoundTo::Site)
        return false;

    bool retainsOperandInResult = false;
    bool returnsOperand = false;
    forEachDeclaredOperand(opcode, [&](uint32_t operandIndex) {
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        retainsOperandInResult |= info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result;
        returnsOperand |= info.uses.contains(OperandUse::Returns);
    });
    return !returnsOperand && (resultRuleOf(opcode) == ResultRule::Allocation || retainsOperandInResult);
}

bool resultMayBeFresh(Opcode opcode)
{
    static const std::array<bool, numberOfOpcodes> answers = [] {
        std::array<bool, numberOfOpcodes> result { };
        for (Opcode member : allMembersOf<Opcode>)
            result[static_cast<unsigned>(member)] = computeWhetherResultMayBeFresh(member);
        return result;
    }();
    return answers[static_cast<unsigned>(opcode)];
}

ResultFreshness freshnessOf(const EffectContext& context, const Function& function, const Value& value)
{
    auto computeEffects = [&] {
        return effectsOf(context, function, value);
    };
    return freshnessOf(context, function, value, ScopedLambda<EffectsOnEdges()>(computeEffects));
}

ResultFreshness freshnessOf(const EffectContext& context, const Function& function, const Value& value, NOESCAPE const ScopedLambda<EffectsOnEdges()>& effectsOfValue)
{
    if (!resultMayBeFresh(value.opcode))
        return ResultFreshness::Unknown;

    switch (effectRuleOf(value.opcode)) {
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return freshnessOfBuiltinResult(context.program, function, value);
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
        break;
    }

    if (answersWithDeclaredResult(value.opcode))
        return freshnessOfDeclaredResult(context.program, function, value.overload(), ValueID());

    if (value.opcode == Opcode::NewClosure && namesClosureOfTable(context.program, value))
        return ResultFreshness::Unknown;
    // What it writes is where the other view keeps its bytes. Nothing there can hold the new view.
    if (value.opcode == Opcode::TypedArrayRange)
        return ResultFreshness::Fresh;

    Effects effects = effectsOfValue().onAnyEdge();
    if (!effects.writes.containsOnly(bookkeepingHeaps) || effects.mayCallUnknownCode)
        return ResultFreshness::Unknown;
    return ResultFreshness::Fresh;
}

ResultFreshness freshnessOf(const EffectContext& context, const Function& function, const Terminator& terminator)
{
    if (!resultMayBeFresh(terminator.kind))
        return ResultFreshness::Unknown;
    return freshnessOfBuiltinResult(context.program, function, terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
