/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfOperands.h"

#if ENABLE(AOT)

#include "AOTBuiltinCalls.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTIR.h"
#include "AOTPropertyAccessReceivers.h"
#include <algorithm>

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
    return { parameter->uses, parameter->protocols, parameter->namedMembers, parameter->calledMembers, retention.retainedIn, retention.retainingOperand, OperandDeclarer::Builtin, namedMemberUse, resultOfCallEscapes, membersAreOfObjectsOnly, functionsAreConvertedUnderNamesOnly, parameter->convertedMembers };
}

static void describeCalledValue(const Program& program, const Type& callee, EffectDescription& description)
{
    CalleeList callees = calleesOf(program, callee);
    describeCallees(program, callees, BuiltinCallMode::Call, description);
    if (callees.builtins.isEmpty())
        return;
    // Native code that constructs what it is handed passes it as new.target as well.
    EnteringOperands operands;
    operands.newTarget = TypeID();
    describeCallees(program, callees, BuiltinCallMode::Construct, operands, description);
}

static bool mayReturnObject(const Program& program, const Type& calledValue)
{
    CalleeList callees = calleesOf(program, calledValue);
    if (!callees.isComplete || !callees.builtins.isEmpty())
        return true;
    return std::ranges::any_of(callees.functions, [&](FunctionID callee) {
        const Function& function = program.at(callee);
        if (function.results.size() != 1 || function.isUnsplitCoroutine())
            return true;
        TypeID type = function.result().type;
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

static CalleeList membersRunBy(const Program& program, const Type& operand, const OperandBehavior& behavior)
{
    return membersRunBy(program, operand, behavior.protocols, behavior.namedMembers, behavior.calledMembers, behavior.namedMemberUse);
}

// Answers false where the type of something that the operand holds is not proven.
static bool forEachProvenContents(const Program& program, const Type& operand, NOESCAPE const Invocable<void(const Type&)> auto& functor)
{
    const BuiltinTable& table = program.builtins();
    return hasOnlyInstances(operand, [&](const ClassPart& part) {
        // A RegExp holds lastIndex and nothing else, and one that stays as created holds a number there.
        if (part.builtinClass == BuiltinClasses::RegExp && part.provenPristine.contains(PristineAspects::AsCreated)) {
            functor(program.at(TypeTable::number));
            return true;
        }
        return part.contentsAreProven && std::ranges::all_of(table.at(part.builtinClass).slots, [&](BuiltinSlotID slot) {
            std::optional<uint8_t> indexInType = table.at(slot).indexInType;
            if (indexInType)
                functor(program.at(part.contentsAt(*indexInType)));
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

static void describeOperandCode(const Program&, const Type& operand, const OperandBehavior&, MembersBeingDescribed&, EffectDescription&);

static constexpr uint64_t maxReachableTypes = defaultLimitValue(LimitID::MaxAccessPlanMembers, hostArchitecture());

static constexpr OperandUses usesOfReaders {
    OperandUse::Inspects,
    OperandUse::Retains,
    OperandUse::Returns,
    OperandUse::ReadsContents,
    OperandUse::RetainsContents,
    OperandUse::AppliesTransitively,
    OperandUse::AppliesToContents,
};

// An entry declares the heaps of its operands. Native code also reads the objects that it finds in them.
static constexpr Effects readsOfReachableObjects = [] {
    constexpr AbstractHeapSet heapsOnlyCompiledCodeReads = compiledCodeHeaps | AbstractHeapSet { AbstractHeap::StringBuilderContents };
    Effects effects = Effects::none();
    effects.reads = (perObjectHeaps - heapsOnlyCompiledCodeReads) | AbstractHeapSet { AbstractHeap::TypedArrayStorage, AbstractHeap::TypedArrayElements };
    return effects;
}();

// A type of the table that says what an instance holds decides nothing here.
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

// A branch that takes every receiver ends the list. Nobody lists the arguments, so a test of an argument excludes nothing.
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

// Runs the protocols of an operand on a value, on what the value holds, and so on. Whatever is not proven on the way is unknown code.
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
    // What any other protocol reads goes on to code that runs later, or is an object that no field lists.
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
        // A lookup on a primitive names no function of the program.
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
        // A protocol may call what a field holds. Unknown code leaves out a function whose callers are all known.
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
    m_description.addOwn(readsOfReachableObjects);
    runProtocolsOn(layouts, m_behavior.protocols);

    std::optional<SortedSet<TypeID, 8>> ownValues = provenOwnValueTypesOf(m_program, layouts);
    if (!ownValues) {
        m_description.addUnknownCode();
        return;
    }
    for (TypeID type : *ownValues)
        reach(m_program.at(type));
}

void ReachableValueWalk::visitInstances(const Type& instances)
{
    constexpr Protocols stepsOnProperties { Protocol::OwnKeys, Protocol::OwnValues, Protocol::PrototypeChain };

    m_description.addOwn(readsOfReachableObjects);
    // An instance that is pristine in both aspects has the properties its class gives it and no other, none of them an accessor, and inherits from built-in objects alone.
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

    // The walk comes to what a member holds, so the conversion of it is not counted with the read.
    CalleeList members = membersRunBy(m_program, receivers, protocols - protocolsKeepingResults, m_behavior.namedMembers, { }, NamedMembers::AreOnlyInspected);
    for (BuiltinID member : members.builtins)
        describeCallOn(receivers, member);
    members.builtins.clear();
    describeCallees(m_program, members, BuiltinCallMode::Call, m_description);

    // What a member with a kept result returns is reachable as well, and so is what any function of the program returns.
    bool returnsNothingReachable = membersWithKeptResults.runsNothing() && members.functions.isEmpty();
    if (!returnsNothingReachable || (protocols.contains(Protocol::Named) && !findsNamedMembersInFields(receivers)))
        m_description.addUnknownCode();
}

// A member that is found on a prototype is an object that no field lists.
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
    describeOverload(m_program, id, nullptr, { }, m_description);

    const BuiltinOverload& overload = m_program.builtins().at(id);
    bool isDeclaredByParameters = !overload.isUndeclared() && !overload.libraryFunction() && !overload.code.model;
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

// The walk runs its protocols on the receiver and on all that the receiver reaches.
bool ReachableValueWalk::covers(const OperandBehavior& behavior) const
{
    constexpr Protocols protocolsWithLists { Protocol::Named, Protocol::CallsNamed };
    return behavior.uses.containsOnly(usesOfReaders) && (m_behavior.protocols - protocolsWithLists).containsAll(behavior.protocols);
}

} // anonymous namespace

static void describeCodeOfReachableValues(const Program& program, const Type& operand, const OperandBehavior& behavior, MembersBeingDescribed& membersBeingDescribed, EffectDescription& description)
{
    // A use that writes, defines or calls applies to every reachable object as well.
    bool hasProvenObjects = operand.layouts || !operand.classes.isEmpty();
    if (!hasProvenObjects || !behavior.uses.containsOnly(usesOfReaders)) {
        description.addUnknownCode();
        return;
    }
    ReachableValueWalk(program, behavior, membersBeingDescribed, description).run(operand);
}

// The protocols also run on every object reachable from an object that the operand holds.
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

void describeOperandCode(const Program& program, const Type& operand, const OperandBehavior& behavior, EffectDescription& description)
{
    MembersBeingDescribed membersBeingDescribed;
    describeOperandCode(program, operand, behavior, membersBeingDescribed, description);
}

static void describeOperandCode(const Program& program, const Type& operand, const OperandBehavior& behavior, MembersBeingDescribed& membersBeingDescribed, EffectDescription& description)
{
    constexpr OperandUses usesRunningSettersAndTraps { OperandUse::StoresByKey, OperandUse::AssignsFrom, OperandUse::DeletesByKey };

    bool hasObjects = operand.hasObjects();
    if (!behavior.protocols.isEmpty()) {
        // Some protocols look a member up on the prototype of a primitive's wrapper.
        if (hasObjects && behavior.uses.contains(OperandUse::AppliesTransitively))
            describeCodeOfReachableValues(program, operand, behavior, membersBeingDescribed, description);
        else if (behavior.uses.contains(OperandUse::AppliesToContents))
            describeCodeOfContents(program, operand, behavior, membersBeingDescribed, description);
        else {
            describeCallees(program, membersRunBy(program, operand, behavior), BuiltinCallMode::Call, description);
            description.addOwn(readsOfMemberLookups(operand, behavior.protocols));
        }
    }
    if (!hasObjects)
        return;
    if (behavior.declarer == OperandDeclarer::Builtin && behavior.uses.containsAny(usesRunningSettersAndTraps) && !isFreeOfSettersAndTraps(operand))
        description.addUnknownCode();
    if (!behavior.uses.containsAny(callingUses))
        return;
    describeCalledValue(program, operand, description);
    // Native code converts, reads or serializes what the callback returned, which runs the code of an object.
    if (behavior.resultOfCallEscapes && mayReturnObject(program, operand))
        description.addUnknownCode();
}

static bool mayTakeCallee(const Program& program, const Type& calledValue)
{
    CalleeList callees = calleesOf(program, calledValue);
    return !callees.isComplete || std::ranges::any_of(callees.functions, [&](FunctionID callee) {
        return program.at(callee).calleeParameterIndex().has_value();
    });
}

static bool isPassedToOperandCode(const Program& program, const Type& operand, const OperandBehavior& behavior)
{
    if (behavior.uses.containsAny(callingUses) && mayTakeCallee(program, operand))
        return true;

    OperandBehavior protocolBehavior = behavior;
    protocolBehavior.uses.remove(callingUses);
    EffectDescription description;
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
static OperandSummary summarizeDeclaredOperand(const Program& program, const Type& type, Entry entry, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(entry, operandIndex);
    OperandSummary summary = summarizeDeclaredUses(behavior);
    if (operandKindOf(entry, operandIndex) == OperandKind::Results)
        summary.uses.add(OperandUse::Returns);
    if (!mayBeReference(type))
        return summary;

    bool isHandedToUnnamedCode = !behavior.uses.contains(OperandUse::RetainsContents) && declaredBoundOf(entry).mayCallUnknownCode;
    if (isHandedToUnnamedCode || isPassedToOperandCode(program, type, behavior))
        summary.uses.add(OperandUse::PassesToUserCode);
    return summary;
}

// The method that HasInstance runs on one operand takes the other operands of the entry as its arguments.
static bool isArgumentOfHasInstanceMethod(const Program& program, const Function& function, const Value& user, uint32_t operandIndex)
{
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
    if (effectRuleOf(call.opcode) != EffectRule::Call)
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
        bool followsCalleeHeader = !Value::opcodesWithCallees.contains(call.opcode);
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

    OperandSummary summary = context.summaries.of(callee).parameter(*entryParameter).operand;
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

static OperandSummary summarizeCallOperand(const EffectContext& context, const Type& type, const Value& call, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(call.opcode, operandIndex);
    if (behavior.uses.contains(OperandUse::RetainsContents))
        return summarizeDeclaredOperand(context.program, type, call.opcode, operandIndex);

    CalleeList callees = calleesOfCall(context.program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty() || maySuspend(call.opcode))
        return { };

    if (behavior.uses.containsAny(callingUses)) {
        OperandSummary summary = summarizeDeclaredUses(behavior);
        for (FunctionID callee : callees.functions) {
            for (uint32_t parameter : entryParametersOfCalledValue(context.program.at(callee), call))
                summary = joinOf(summary, summarizeArgumentOf(context, call, callee, parameter));
        }
        return summary;
    }

    OperandSummary summary { OperandUse::Inspects, RetainedIn::UnknownMemory, 0 };
    for (FunctionID callee : callees.functions)
        summary = joinOf(summary, summarizeArgumentOf(context, call, callee, entryParameterOf(context.program.at(callee), call, operandIndex)));
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
static OperandSummary summarizeOperandOfOverload(const Program&, const Type&, const Carrier& call, const BuiltinOverload&, BuiltinOperand);

// The function runs with a receiver that no overload was matched against, so it may behave as any of them.
template<typename Carrier>
static OperandSummary summarizeArgumentOfAnyOverload(const Program& program, const Type& type, const Carrier& call, const BuiltinOverload& named, BuiltinOperand operand)
{
    std::optional<OperandSummary> joined;
    for (const BuiltinOverload& overload : program.builtins().overloadsOf(named.builtin)) {
        if (overload.mode != named.mode)
            continue;
        if (mayRunUnnamedCode(overload))
            return { };
        OperandSummary summary = summarizeOperandOfOverload(program, type, call, overload, operand);
        joined = joined ? joinOf(*joined, summary) : summary;
    }
    return joined.value_or(OperandSummary { });
}

template<typename Carrier>
static OperandSummary summarizeBuiltinOperand(const Program& program, const Type& type, const Carrier& call, uint32_t operandIndex)
{
    const BuiltinOverload& overload = program.builtins().at(call.overload());
    if (mayRunUnnamedCode(overload))
        return { };

    BuiltinOperand operand = builtinOperandOf(call, operandIndex);
    switch (builtinCallOf(program, call).form) {
    case BuiltinCallForm::AnyReceiver:
        if (operand.isReceiver())
            return { };
        return summarizeArgumentOfAnyOverload(program, type, call, overload, operand);
    case BuiltinCallForm::List:
        if (!operand.isReceiver())
            return { listedUses, RetainedIn::UnknownMemory, 0 };
        break;
    case BuiltinCallForm::Whole:
    case BuiltinCallForm::Fields:
    case BuiltinCallForm::Prefix:
        break;
    }
    return summarizeOperandOfOverload(program, type, call, overload, operand);
}

template<typename Carrier>
static OperandSummary summarizeOperandOfOverload(const Program& program, const Type& type, const Carrier& call, const BuiltinOverload& overload, BuiltinOperand operand)
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
    if (isPassedToOperandCode(program, type, behavior))
        summary.uses.add(OperandUse::PassesToUserCode);
    return summary;
}

static OperandSummary summarizeAccessOperand(const Program& program, const Function& function, const Type& type, const Value& access, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(access.opcode, operandIndex);
    OperandSummary summary = summarizeDeclaredUses(behavior);
    if (!mayBeReference(type))
        return summary;

    ProvenAccess proven = provenAccessOf(program, function, access);
    bool isHandedToAccessor = proven.isComplete ? !proven.accessors.isEmpty() : declaredBoundOf(access.opcode).mayCallUnknownCode;
    if (isHandedToAccessor || isPassedToOperandCode(program, type, behavior))
        summary.uses.add(OperandUse::PassesToUserCode);
    return summary;
}

OperandSummary summarizeOperand(const EffectContext& context, const Function& function, const Value& user, uint32_t operandIndex)
{
    const Type& type = operandTypeOf(context.program, function, user, operandIndex);
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Call:
        return summarizeCallOperand(context, type, user, operandIndex);
    case EffectRule::Builtin:
    case EffectRule::Inline:
        return summarizeBuiltinOperand(context.program, type, user, operandIndex);
    case EffectRule::Access:
        return summarizeAccessOperand(context.program, function, type, user, operandIndex);
    case EffectRule::Declared:
    case EffectRule::Operator: {
        OperandSummary summary = summarizeDeclaredOperand(context.program, type, user.opcode, operandIndex);
        if (mayBeReference(type) && isArgumentOfHasInstanceMethod(context.program, function, user, operandIndex))
            summary.uses.add(OperandUse::PassesToUserCode);
        return summary;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OperandSummary summarizeOperand(const EffectContext& context, const Function& function, const Terminator& user, uint32_t operandIndex)
{
    const Type& type = operandTypeOf(context.program, function, user, operandIndex);
    switch (effectRuleOf(user.kind)) {
    case EffectRule::Inline:
        return summarizeBuiltinOperand(context.program, type, user, operandIndex);
    case EffectRule::Declared:
    case EffectRule::Operator:
    case EffectRule::Access:
    case EffectRule::Call:
    case EffectRule::Builtin:
        return summarizeDeclaredOperand(context.program, type, user.kind, operandIndex);
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

OperandConfinement confinementOf(Opcode opcode)
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

static OperandMemory memoryAccessedByCallees(const EffectContext& context, const Value& call, uint32_t operandIndex, const CalleeList& callees)
{
    bool isCalledValue = operandUsesOf(call.opcode, operandIndex).containsAny(callingUses);
    OperandMemory memory = OperandMemory::none();
    for (FunctionID id : callees.functions) {
        const Function& callee = context.program.at(id);
        CalleeSummary summary(context, id);
        if (isCalledValue) {
            for (uint32_t parameter : entryParametersOfCalledValue(callee, call))
                memory.merge(summary->parameter(parameter).memory);
            continue;
        }
        std::optional<uint32_t> parameter = entryParameterOf(callee, call, operandIndex);
        memory.merge(parameter ? summary->parameter(*parameter).memory : OperandMemory());
    }
    return memory;
}

OperandMemory memoryAccessedThrough(const EffectContext& context, const Function& function, const Value& user, uint32_t operandIndex, const EffectsOnEdges& effectsOfUser)
{
    if (!mayBeReference(operandTypeOf(context.program, function, user, operandIndex)))
        return OperandMemory::none();

    if (effectRuleOf(user.opcode) == EffectRule::Call && !maySuspend(user.opcode)) {
        CalleeList callees = calleesOfCall(context.program, user);
        if (callees.isComplete && callees.builtins.isEmpty())
            return memoryAccessedByCallees(context, user, operandIndex, callees);
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

ResultFreshness freshnessOf(const EffectContext& context, const Function& function, const Value& value)
{
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

    // An operation selected for a call of an overload answers with what the overload declares, as the call did.
    if (Value::opcodesWithOverload.contains(value.opcode) && resultRuleOf(value.opcode) == ResultRule::Declared)
        return freshnessOfDeclaredResult(context.program, function, value.overload(), ValueID());

    // A site answers with the same object at every evaluation, and an earlier one may have published it.
    if (boundToOf(value.opcode) == BoundTo::Site)
        return ResultFreshness::Unknown;
    // The closure table holds this closure, and an instance that was allocated without it inherits a property that names it.
    if (value.opcode == Opcode::NewClosure && hasSingletonClosure(context.program, value.function()))
        return ResultFreshness::Unknown;

    bool retainsOperandInResult = false;
    bool returnsOperand = false;
    forEachDeclaredOperand(value.opcode, [&](uint32_t operandIndex) {
        const OperandInfo& info = operandInfoOf(value.opcode, operandIndex);
        retainsOperandInResult |= info.uses.contains(OperandUse::Retains) && info.retainedIn == RetainedIn::Result;
        returnsOperand |= info.uses.contains(OperandUse::Returns);
    });
    if (returnsOperand || (resultRuleOf(value.opcode) != ResultRule::Allocation && !retainsOperandInResult))
        return ResultFreshness::Unknown;

    Effects effects = effectsOf(context, function, value).onAnyEdge();
    if (!effects.writes.containsOnly(bookkeepingHeaps) || effects.mayCallUnknownCode)
        return ResultFreshness::Unknown;
    return ResultFreshness::Fresh;
}

ResultFreshness freshnessOf(const EffectContext& context, const Function& function, const Terminator& terminator)
{
    if (effectRuleOf(terminator.kind) != EffectRule::Inline)
        return ResultFreshness::Unknown;
    return freshnessOfBuiltinResult(context.program, function, terminator);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
