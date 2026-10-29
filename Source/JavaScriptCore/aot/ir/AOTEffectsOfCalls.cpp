/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTIR.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSchemaWalkers.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr bool isCall(FunctionReferenceKind kind)
{
    switch (kind) {
    case FunctionReferenceKind::Call:
        return true;
    case FunctionReferenceKind::Closure:
    case FunctionReferenceKind::Constructor:
    case FunctionReferenceKind::Dispatch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static constexpr BuiltinCallMode callModeOf(Opcode opcode)
{
    return OperandPosition::newTarget(opcode) ? BuiltinCallMode::Construct : BuiltinCallMode::Call;
}

static bool isClassConstructor(const Function& function)
{
    return function.kind == FunctionKind::BaseConstructor || function.kind == FunctionKind::DerivedConstructor;
}

static bool throwsBeforeEntry(const Function& callee, BuiltinCallMode mode)
{
    switch (mode) {
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Get:
    case BuiltinCallMode::Set:
        return isClassConstructor(callee);
    case BuiltinCallMode::Construct:
        return callee.constructorForm() == ConstructorForm::NotConstructor;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OpcodeSet callsThatCopyArgumentsOfAnyNumber { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList, Opcode::CallForwardVarargs, Opcode::ConstructSuperForwardVarargs };

static bool namesEveryCallee(const Type& callee)
{
    return callee.isObject() && callee.hasOnlyKnownCallees() && !callee.layouts;
}

bool mayThrowBeforeEntry(const Program& program, const Type& callee, BuiltinCallMode mode)
{
    if (!namesEveryCallee(callee))
        return true;

    const BuiltinTable& table = program.builtins();
    bool someFunctionThrows = std::ranges::any_of(callee.functions, [&](FunctionID function) {
        return throwsBeforeEntry(program.at(function), mode);
    });
    bool someBuiltinThrows = std::ranges::any_of(callee.builtins, [&](BuiltinID builtin) {
        return !table.dispatchOf(table.canonicalID(builtin), mode);
    });
    return someFunctionThrows || someBuiltinThrows;
}

static Effects effectsOfContentsRead(OperandKind kind)
{
    if (kind == OperandKind::Array)
        return listReadEffects;
    Effects effects = Effects::none();
    effects.reads = perObjectHeaps;
    return effects;
}

static bool mayEnterTableLookup(const Program& program, LayoutSetID receivers, PropertyKeyID key)
{
    MemberPartition partition = partitionMembers({ program, nullptr }, program.at(receivers).completeMembers(), key, Access::Read);
    return !partition.rest.isEmpty() || std::ranges::any_of(partition.arms, [](const MemberArm& arm) {
        return arm.answer.kind != MemberKind::Method || !arm.answer.receiverProofs.isEmpty() || !arm.answer.conditions.isEmpty();
    });
}

// Every table call of one name on one family asks the same, and the answer looks at every class of the family.
static bool mayEnterTableLookup(const Program& program, const Value& tableCall)
{
    LayoutSetID receivers = tableCall.receivers();
    PropertyKeyID key = tableCall.key();
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache || TypeTable::isProvisional(receivers) || TypeTable::isProvisional(key))
        return mayEnterTableLookup(program, receivers, key);

    using Entry = PropertyAccessCache::LookupOfTableCall;
    uint32_t hash = computeHash(receivers.index(), key.index());
    auto isWanted = [&](const Entry& entry) {
        return entry.receivers == receivers && entry.key == key;
    };
    const Entry* known = cache->lookupsOfTableCalls.find(hash, isWanted);
    if (!known)
        known = &cache->lookupsOfTableCalls.add(hash, Entry { receivers, key, mayEnterTableLookup(program, receivers, key) }, isWanted);
    RELEASE_ASSERT(!cache->checksAnswers || known->mayBeEntered == mayEnterTableLookup(program, receivers, key));
    return known->mayBeEntered;
}

std::span<const FunctionID> entrypointsOfTableCall(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallTable)
        return { };
    return program.at(call.entrypoints()).written().functions;
}

CalleeList calleesOfCall(const Program& program, const Value& call)
{
    CalleeList namedCallees = CalleeList::none();
    forEachFunctionReference(call, [&](FunctionID callee, FunctionReferenceKind kind) {
        if (isCall(kind))
            namedCallees.add(callee);
    });
    for (FunctionID entrypoint : entrypointsOfTableCall(program, call))
        namedCallees.add(entrypoint);
    if (call.opcode == Opcode::CallTable && !mayEnterTableLookup(program, call))
        namedCallees.remove(call.function());
    if (!namedCallees.runsNothing())
        return namedCallees;
    if (Value::opcodesWithCallees.contains(call.opcode))
        return calleesOf(program, call.callees());
    return CalleeList::unknown();
}

static CalleeSetID stampThatListsCalleesOf(const Program& program, const Value& call)
{
    if (!Value::opcodesWithCallees.contains(call.opcode))
        return { };
    bool namesCallee = false;
    bool namesOnlyTableLookup = call.opcode == Opcode::CallTable && entrypointsOfTableCall(program, call).empty();
    forEachFunctionReference(call, [&](FunctionID callee, FunctionReferenceKind kind) {
        if (!isCall(kind))
            return;
        namesCallee = true;
        namesOnlyTableLookup = namesOnlyTableLookup && callee == call.function();
    });
    if (namesCallee && (!namesOnlyTableLookup || mayEnterTableLookup(program, call)))
        return { };
    return call.callees();
}

CalleeSetID calleeSetOfCall(const Program& program, const Value& call)
{
    if (!entrypointsOfTableCall(program, call).empty())
        return mayEnterTableLookup(program, call) ? CalleeSetID() : call.entrypoints();
    return stampThatListsCalleesOf(program, call);
}

// A long set of entrypoints is read from the call by whoever wants the functions that are entered.
static void addFunctionsEnteredBeside(const Program& program, const Value& call, EffectDescription& description)
{
    forEachFunctionReference(call, [&](FunctionID entered, FunctionReferenceKind kind) {
        if (isCall(kind))
            description.addSlowPathFunction(entered);
    });
    if (call.opcode != Opcode::CallTable || program.at(call.entrypoints()).isLong())
        return;
    for (FunctionID entrypoint : entrypointsOfTableCall(program, call))
        description.addSlowPathFunction(entrypoint);
}

static bool listsWhatReceiverFinds(const Program& program, const Value& call)
{
    return call.opcode == Opcode::CallTable && call.callees() && program.at(call.callees()).listsEveryCallee();
}

CalleeSetID longCalleeSetRunBy(const Program& program, const Value& call)
{
    if (!Value::opcodesWithCallees.contains(call.opcode) || !call.callees())
        return { };
    const CalleeSet& named = program.at(call.callees());
    // A construction also runs what reads the prototype, and a built-in what its operands hold.
    if (!named.isLong() || !named.written().builtins.empty() || callModeOf(call.opcode) != BuiltinCallMode::Call || effectRuleOf(call.opcode) != EffectRule::Call)
        return { };
    if (declaredEffectsOf(program, call).maySuspend)
        return { };
    return listsWhatReceiverFinds(program, call) ? call.callees() : stampThatListsCalleesOf(program, call);
}

EffectDescription describeCodeRunBesideLongCalleeSet(const Program& program, const Value& call, CalleeSetID set)
{
    EffectDescription description;
    description.leavesOutOwnEffects = true;
    if (listsWhatReceiverFinds(program, call))
        addFunctionsEnteredBeside(program, call, description);
    if (!program.at(set).listsEveryCallee())
        description.addUnknownCode();
    return description;
}

static Effects effectsOfFindingCallee()
{
    Effects effects = Effects::unknownCode();
    effects.writes = { };
    effects.mayCallUnknownCode = false;
    return effects;
}

CalleeList calleesThatBoundEffectsOf(const Program& program, const Value& call)
{
    if (listsWhatReceiverFinds(program, call))
        return calleesOf(program, call.callees());
    return calleesOfCall(program, call);
}

static void describeEntrypoint(const Function& callee, BuiltinCallMode mode, EffectDescription& description)
{
    if (throwsBeforeEntry(callee, mode))
        description.addThrow();
    if (callee.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        description.addOwn(declaredBoundOf(Opcode::CreateRest));
    if (callee.has(FunctionCapabilities::RecordsCallee))
        description.addOwn(declaredBoundOf(Opcode::RecordCallee));
    if (callee.capabilities.containsAll({ FunctionCapabilities::RecordsCallee, FunctionCapabilities::RecordsCalleeOnceRenamed }))
        description.addOwn(declaredBoundOf(Opcode::AreFunctionNamesUnchanged));
}

static void describeReadOfConstructPrototype(const Program& program, const Type& newTarget, EffectDescription& description)
{
    description.addOwn(declaredBoundOf(Opcode::GetConstructPrototype));
    describeOperandCode(program, newTarget, declaredBehaviorOf(Opcode::GetConstructPrototype, OperandIndex::GetConstructPrototype::newTarget), description);
}

static void describeCreationOfThis(const Program& program, FunctionID callee, const EnteringOperands& operands, EffectDescription& description)
{
    if (!program.at(callee).takesThisFromConstruction())
        return;
    describeReadOfConstructPrototype(program, operands.newTarget ? program.at(operands.newTarget) : Type::forFunction(callee), description);
    // new.target selects the class of the object.
    LayoutID constructed = program.at(callee).constructedClass;
    if (operands.newTarget || !constructed)
        description.addOwn(declaredBoundOf(Opcode::NewObject));
    else
        description.addOwn(hasOnePrototypeObject(program, constructed) ? declaredBoundOf(NamedEffectSet::Allocates) : effectsOfAllocatingWithPrototype(program, constructed));
}

static NEVER_INLINE bool describeEntrypointsOfLongList(const Program& program, std::span<const FunctionID> callees, BuiltinCallMode mode, EffectDescription& description, const EffectSummaries& summaries)
{
    AnswersForCalleeLists* answers = description.leavesOutOwnEffects ? nullptr : summaries.answersForCalleeLists();
    if (!answers)
        return false;
    description.own.merge(answers->effectsOfEntering(callees, static_cast<uint64_t>(mode), [&] {
        EffectDescription entrypoints;
        for (FunctionID callee : callees)
            describeEntrypoint(program.at(callee), mode, entrypoints);
        return entrypoints.own;
    }));
    return true;
}

void describeCallees(const Program& program, const CalleeList& callees, BuiltinCallMode mode, const EnteringOperands& operands, EffectDescription& description, const EffectSummaries* summaries)
{
    bool constructs = mode == BuiltinCallMode::Construct;
    Callees listed = callees.listed();
    description.addFunctions(listed.functions);
    bool isLongList = summaries && !constructs && listed.functions.size() >= AnswersForCalleeLists::fewestCallees;
    if (!isLongList || !describeEntrypointsOfLongList(program, listed.functions, mode, description, *summaries)) {
        for (FunctionID callee : listed.functions) {
            describeEntrypoint(program.at(callee), mode, description);
            if (constructs)
                describeCreationOfThis(program, callee, operands, description);
        }
    }
    for (BuiltinID callee : listed.builtins)
        describeCallOf(program, callee, mode, description);
    if (constructs && operands.newTarget && !listed.builtins.empty())
        describeReadOfConstructPrototype(program, program.at(operands.newTarget), description);
    if (!callees.listsEveryCallee())
        description.addUnknownCode();
}

void describeCallees(const Program& program, const CalleeList& callees, BuiltinCallMode mode, EffectDescription& description)
{
    describeCallees(program, callees, mode, EnteringOperands { }, description);
}

static std::optional<BuiltinCallForm> formOfArguments(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Call:
    case Opcode::CallMethod:
    case Opcode::CallResolved:
    case Opcode::Construct:
    case Opcode::SuperConstruct:
        return BuiltinCallForm::Whole;
    case Opcode::CallList:
    case Opcode::ConstructList:
    case Opcode::SuperConstructList:
        return BuiltinCallForm::List;
    default:
        return std::nullopt;
    }
}

static ValueID receiverOf(const Value& call)
{
    if (OperandPosition::thisValue(call.opcode))
        return call.thisValueOperand();
    if (OperandPosition::receiver(call.opcode))
        return call.receiverOperand();
    return { };
}

static Vector<TypeID, 4> typesOfArguments(const Function& function, const Value& call, BuiltinCallForm form)
{
    Vector<TypeID, 4> arguments;
    if (form == BuiltinCallForm::List)
        arguments.append(function.at(call.listOperand()).type);
    else {
        for (ValueID argument : call.argumentsOperands())
            arguments.append(function.at(argument).type);
    }
    return arguments;
}

static TypeID typeOfNewTarget(const Function& function, const Value& call)
{
    if (!OperandPosition::newTarget(call.opcode))
        return TypeTable::anything;
    ValueID newTarget = call.newTargetOperand();
    return newTarget ? function.at(newTarget).type : TypeID();
}

static void describeCalleesOfCall(const Program& program, const Function& function, const Value& call, const CalleeList& callees, BuiltinCallMode mode, EffectDescription& description, const EffectSummaries* summaries)
{
    EnteringOperands operands;
    operands.newTarget = typeOfNewTarget(function, call);
    describeCallees(program, callees, mode, operands, description, summaries);
}

static void describeOperandsGivenToBuiltins(const Program& program, const Function& function, const Value& call, const CalleeList& callees, BuiltinCallMode mode, EffectDescription& description)
{
    if (callees.listed().builtins.empty())
        return;

    std::optional<BuiltinCallForm> form = formOfArguments(call.opcode);
    if (!form) {
        description.addOwn(Effects::unknownCode());
        return;
    }

    Vector<TypeID, 4> arguments = typesOfArguments(function, call, *form);
    ValueID receiver = receiverOf(call);
    TypeID receiverType = receiver ? function.at(receiver).type : TypeTable::undefined;
    for (BuiltinID callee : callees.listed().builtins)
        describeOperandsOfCall(program, callee, mode, *form, receiverType, arguments.span(), description);
}

static void describeNamedCallees(const CalleeList& callees, EffectDescription& description)
{
    description.addFunctions(callees.listed().functions);
    if (!callees.listsEveryCallee())
        description.addUnknownCode();
}

void describeCall(const Program& program, const Function& function, const Value& value, const Effects& declared, EffectDescription& description, const EffectSummaries* summaries)
{
    if (declared.maySuspend) {
        describeBound(declared, description);
        return;
    }

    BuiltinCallMode mode = callModeOf(value.opcode);
    CalleeList callees = calleesThatBoundEffectsOf(program, value);
    if (listsWhatReceiverFinds(program, value)) {
        description.addOwn(effectsOfFindingCallee());
        addFunctionsEnteredBeside(program, value, description);
    }
    if (callsThatCopyArgumentsOfAnyNumber.contains(value.opcode))
        description.addThrow();
    if (Value::opcodesWithCallees.contains(value.opcode)) {
        describeCalleesOfCall(program, function, value, callees, mode, description, summaries);
        describeOperandsGivenToBuiltins(program, function, value, callees, mode, description);
    } else
        describeNamedCallees(callees, description);

    value.forEachOperand([&](ValueID operand, uint32_t index) {
        const OperandInfo& info = operandInfoOf(value.opcode, index);
        if (info.uses.containsAny(callingUses) && mayThrowBeforeEntry(program, program.typeOf(function, operand), mode))
            description.addThrow();
        if (info.uses.contains(OperandUse::ReadsContents))
            description.addOwn(effectsOfContentsRead(info.kind));
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
