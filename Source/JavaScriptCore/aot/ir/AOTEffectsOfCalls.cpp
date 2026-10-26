/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIR.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSchemaWalkers.h"
#include "AOTVector.h"
#include <algorithm>

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

static bool mayThrowBeforeEntry(const Program& program, const Type& callee, BuiltinCallMode mode)
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

static bool mayEnterTableLookup(const Program& program, const Value& tableCall)
{
    MemberPartition partition = partitionMembers({ program, nullptr }, program.at(tableCall.receivers()).members.span(), tableCall.key(), Access::Read);
    return !partition.rest.isEmpty() || std::ranges::any_of(partition.arms, [](const MemberArm& arm) {
        return arm.answer.kind != MemberKind::Method || !arm.answer.receiverProofs.isEmpty() || !arm.answer.conditions.isEmpty();
    });
}

CalleeList calleesOfCall(const Program& program, const Value& call)
{
    CalleeList namedCallees = CalleeList::none();
    forEachFunctionReference(call, [&](FunctionID callee, FunctionReferenceKind kind) {
        if (isCall(kind))
            namedCallees.add(callee);
    });
    if (call.opcode == Opcode::CallTable && !mayEnterTableLookup(program, call))
        namedCallees.functions.removeFirst(call.function());
    if (!namedCallees.functions.isEmpty())
        return namedCallees;
    if (Value::opcodesWithCallees.contains(call.opcode))
        return calleesOf(program, call.callees());
    return CalleeList::unknown();
}

static bool listsWhatReceiverFinds(const Program& program, const Value& call)
{
    return call.opcode == Opcode::CallTable && call.callees() && calleesOf(program, call.callees()).isComplete;
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
}

static bool takesThisFromConstruction(const Function& callee)
{
    return callee.constructorForm() != ConstructorForm::NotConstructor && !isClassConstructor(callee);
}

static void describeReadOfConstructPrototype(const Program& program, const Type& newTarget, EffectDescription& description)
{
    description.addOwn(declaredBoundOf(Opcode::GetConstructPrototype));
    describeOperandCode(program, newTarget, declaredBehaviorOf(Opcode::GetConstructPrototype, OperandIndex::GetConstructPrototype::newTarget), description);
}

static void describeCreationOfThis(const Program& program, FunctionID callee, const EnteringOperands& operands, EffectDescription& description)
{
    if (!takesThisFromConstruction(program.at(callee)))
        return;
    describeReadOfConstructPrototype(program, operands.newTarget ? program.at(operands.newTarget) : Type::forFunction(callee), description);
    description.addOwn(declaredBoundOf(Opcode::NewObject));
}

void describeCallees(const Program& program, const CalleeList& callees, BuiltinCallMode mode, const EnteringOperands& operands, EffectDescription& description)
{
    bool constructs = mode == BuiltinCallMode::Construct;
    description.addFunctions(callees.functions.span());
    for (FunctionID callee : callees.functions) {
        describeEntrypoint(program.at(callee), mode, description);
        if (constructs)
            describeCreationOfThis(program, callee, operands, description);
    }
    for (BuiltinID callee : callees.builtins)
        describeCallOf(program, callee, mode, description);
    if (constructs && operands.newTarget && !callees.builtins.isEmpty())
        describeReadOfConstructPrototype(program, program.at(operands.newTarget), description);
    if (!callees.isComplete)
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

static void describeCalleesOfCall(const Program& program, const Function& function, const Value& call, const CalleeList& callees, BuiltinCallMode mode, EffectDescription& description)
{
    EnteringOperands operands;
    operands.newTarget = typeOfNewTarget(function, call);
    describeCallees(program, callees, mode, operands, description);
}

static void describeOperandsGivenToBuiltins(const Program& program, const Function& function, const Value& call, const CalleeList& callees, BuiltinCallMode mode, EffectDescription& description)
{
    if (callees.builtins.isEmpty())
        return;

    std::optional<BuiltinCallForm> form = formOfArguments(call.opcode);
    if (!form) {
        description.addOwn(Effects::unknownCode());
        return;
    }

    Vector<TypeID, 4> arguments = typesOfArguments(function, call, *form);
    ValueID receiver = receiverOf(call);
    const Type& receiverType = receiver ? program.typeOf(function, receiver) : program.at(TypeTable::undefined);
    for (BuiltinID callee : callees.builtins)
        describeOperandsOfCall(program, callee, mode, *form, receiverType, arguments.span(), description);
}

static void describeNamedCallees(const CalleeList& callees, EffectDescription& description)
{
    description.addFunctions(callees.functions.span());
    if (!callees.isComplete)
        description.addUnknownCode();
}

void describeCall(const Program& program, const Function& function, const Value& value, const Effects& declared, EffectDescription& description)
{
    if (declared.maySuspend) {
        describeBound(declared, description);
        return;
    }

    BuiltinCallMode mode = callModeOf(value.opcode);
    CalleeList callees = calleesThatBoundEffectsOf(program, value);
    if (listsWhatReceiverFinds(program, value)) {
        description.addOwn(effectsOfFindingCallee());
        forEachFunctionReference(value, [&](FunctionID entered, FunctionReferenceKind kind) {
            if (isCall(kind))
                description.addSlowPathFunction(entered);
        });
    }
    if (callsThatCopyArgumentsOfAnyNumber.contains(value.opcode))
        description.addThrow();
    if (Value::opcodesWithCallees.contains(value.opcode)) {
        describeCalleesOfCall(program, function, value, callees, mode, description);
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
