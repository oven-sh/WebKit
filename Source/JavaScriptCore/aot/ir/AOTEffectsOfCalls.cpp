/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsDescription.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIR.h"
#include "AOTSchemaWalkers.h"
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

CalleeList calleesOfCall(const Program& program, const Value& call)
{
    CalleeList namedCallees = CalleeList::none();
    forEachFunctionReference(call, [&](FunctionID callee, FunctionReferenceKind kind) {
        if (isCall(kind))
            namedCallees.add(callee);
    });
    if (!namedCallees.functions.isEmpty())
        return namedCallees;
    if (Value::opcodesWithCallees.contains(call.opcode))
        return calleesOf(program, call.callees());
    return CalleeList::unknown();
}

Type testedWrittenTypeOf(const Type& written)
{
    return written.withoutProofs();
}

Type testedProvenTypeOf(const Type& type)
{
    return type.withoutClassProofs().withUntestableBuiltinsWidened();
}

bool mayFailEntryTests(const Program& program, const Type& value, const DeclaredType& declared)
{
    const Type& proven = program.at(declared.type);
    if (value.isSubsetOf(proven, program))
        return false;
    if (declared.written && !value.isSubsetOf(testedWrittenTypeOf(program.at(declared.written)), program))
        return true;
    return !value.isSubsetOf(testedProvenTypeOf(proven), program);
}

static bool mayRefuseRestArguments(const Program& program, const Function& callee, const EnteringOperands& operands)
{
    if (callee.parameters.isEmpty())
        return true;
    const Type& rest = program.at(callee.parameters.last().type);
    const ClassPart* array = rest.findClassPart(BuiltinClasses::Array);
    if (!array)
        return !rest.anyObject;
    if (!array->contentsAreProven)
        return false;

    const Type& element = program.at(array->contentsAt(program.builtins(), ContentsSlot::Element));
    if (!operands.arguments)
        return !element.isAnything();
    size_t firstRestArgument = std::min<size_t>(callee.numberOfPositionalParameters(), operands.arguments->size());
    return !std::ranges::all_of(operands.arguments->subspan(firstRestArgument), [&](TypeID argument) {
        return program.at(argument).isSubsetOf(element, program);
    });
}

static bool mayRefuseOperands(const Program& program, const Function& callee, BuiltinCallMode mode, const EnteringOperands& operands)
{
    // The entrypoint that takes new.target gets `this` from whoever constructs, made for that new.target.
    bool testsThis = mode != BuiltinCallMode::Construct || callee.has(FunctionFlags::HasNewTarget);
    if (testsThis && callee.has(FunctionFlags::HasThis) && mayFailEntryTests(program, program.at(operands.thisValue), callee.thisType))
        return true;
    size_t numberOfPositionalParameters = callee.numberOfPositionalParameters();
    for (size_t index = 0; index < numberOfPositionalParameters; ++index) {
        if (mayFailEntryTests(program, program.at(operands.argumentAt(index)), callee.parameters[index]))
            return true;
    }
    return callee.has(FunctionFlags::HasRestParameter) && mayRefuseRestArguments(program, callee, operands);
}

static bool leavesResultTestToCallers(const Program& program, const Function& callee)
{
    return std::ranges::any_of(callee.results, [&](const DeclaredType& result) {
        return result.written && !program.at(result.type).isSubsetOf(program.at(result.written).withoutProofs(), program);
    });
}

static bool mayAnswerWithRejectedPromise(const Function& callee)
{
    return callee.has(FunctionFlags::IsAsync) || callee.role == FunctionRole::CoroutineRamp || callee.role == FunctionRole::BoundFunction;
}

// A call by value reaches the body through the entrypoint of the callee. The entrypoint tests `this` and the arguments against the types the callee
// declares, collects the rest arguments and tests a result whose test the body leaves to its callers. The summary of the body has none of this.
static void describeEntrypoint(const Program& program, const Function& callee, BuiltinCallMode mode, const EnteringOperands& operands, EffectDescription& description)
{
    if (throwsBeforeEntry(callee, mode))
        description.addThrow();
    if (callee.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest }))
        description.addOwn(declaredBoundOf(Opcode::CreateRest));
    if (leavesResultTestToCallers(program, callee))
        description.addOwn(declaredBoundOf(Opcode::Claim));
    if (!mayRefuseOperands(program, callee, mode, operands))
        return;
    description.addOwn(declaredBoundOf(Opcode::Claim));
    if (mayAnswerWithRejectedPromise(callee)) {
        description.addOwn(declaredBoundOf(Opcode::NewPromise));
        description.addOwn(declaredBoundOf(Opcode::RejectPromise));
    }
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

// What a construction does before it enters the callee is in no summary either. A class constructor reads the prototype of new.target and allocates in its body,
// or leaves both to its parent. For any other constructor whoever constructs does both and hands over the object.
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
    for (FunctionID callee : callees.functions) {
        description.addFunction(callee);
        describeEntrypoint(program, program.at(callee), mode, operands, description);
        if (constructs)
            describeCreationOfThis(program, callee, operands, description);
    }
    for (BuiltinID callee : callees.builtins)
        describeCallOf(program, callee, mode, description);
    // A native constructor that is its own new.target allocates under the prototype its realm keeps. It reads the prototype of any other new.target.
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

    Vector<TypeID, 4> arguments;
    if (!callees.functions.isEmpty() && formOfArguments(call.opcode) == BuiltinCallForm::Whole) {
        arguments = typesOfArguments(function, call, BuiltinCallForm::Whole);
        ValueID receiver = receiverOf(call);
        operands.thisValue = receiver ? function.at(receiver).type : TypeTable::undefined;
        operands.arguments = arguments.span();
    }
    describeCallees(program, callees, mode, operands, description);
}

// A callback whose callers are all known is no part of unknown code. The call that hands it to a built-in is one of those callers.
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
    for (FunctionID callee : callees.functions)
        description.addFunction(callee);
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
    CalleeList callees = calleesOfCall(program, value);
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
