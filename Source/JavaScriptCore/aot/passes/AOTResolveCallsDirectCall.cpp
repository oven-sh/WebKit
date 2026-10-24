/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCallsDirectCall.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowApply.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isValueInside(const ValueInsertion& insertion, ValueID value, TypeID type)
{
    const Program& program = insertion.context.program;
    return program.typeOf(insertion.function, value).isSubsetOf(program.at(type), program);
}

static TypeID requiredTypeOf(const ValueInsertion& insertion, const DeclaredType& target)
{
    return TypingContext { insertion.context.program, insertion.function, insertion.types }.typeIDOf(target);
}

static ValueID insertWithOrigin(const ValueInsertion& insertion, Value&& value)
{
    ValueID id = insertion.insert(WTF::move(value));
    insertion.function.edit(id, ValueEdit::Definition).setOrigin(insertion.origin);
    return id;
}

static auto withInsertionAtEnd(IRBuilder& builder, const auto& functor)
{
    auto append = [&](auto&& value) { return builder.append(WTF::move(value)); };
    InsertValue insert(append);
    return functor(ValueInsertion { builder.context(), builder.function(), builder.types(), builder.origin(), insert });
}

// Whoever asks knows that the value has the type, where the types of the operations do not show it: flow narrows a parameter from every call that reaches it, and a pass gives what the operation it replaces gave.
ValueID convertTo(const ValueInsertion& insertion, ValueID value, const DeclaredType& target)
{
    TypeID required = requiredTypeOf(insertion, target);
    if (isValueInside(insertion, value, required))
        return value;
    if (insertion.context.build.validates())
        insertWithOrigin(insertion, makeValidateType(value, required));
    return insertWithOrigin(insertion, makeAssumeType(value, required));
}

ValueID convertTo(IRBuilder& builder, ValueID value, const DeclaredType& target)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) { return convertTo(insertion, value, target); });
}

// The list has no place among the declared parameters: what flow has proven of it is assumed at the head of the callee. Who makes an array predicts its storage from its elements as its readers do.
static TypeID provenElementTypeOfHiddenRest(const Program& program, const Function& callee)
{
    std::optional<unsigned> index = callee.hiddenRestParameterIndex();
    if (!index || !callee.hasBody())
        return TypeTable::anything;
    const Block& entry = callee.at(entryBlock);
    for (ValueID id : entry.values) {
        const Value& value = callee.at(id);
        if (value.opcode != Opcode::AssumeType || value.valueOperand() != entry.parameters[*index])
            continue;
        if (const ClassPart* part = program.at(value.castType()).findClassPart(BuiltinClasses::Array); part && part->contentsAreProven)
            return part->contentsAt(program.builtins(), ContentsSlot::Element);
    }
    return TypeTable::anything;
}

Value makeDirectCall(const ValueInsertion& insertion, const DirectCallRequest& request)
{
    const Program& program = insertion.context.program;
    const Function& callee = program.at(request.callee);
    RELEASE_ASSERT(request.captures.size() == callee.captures.size());
    RELEASE_ASSERT(request.calleeValue || !callee.has(FunctionFlags::TakesCallee));
    RELEASE_ASSERT(!request.restArray || callee.has(FunctionFlags::HasRestParameter));

    auto insert = [&](Value&& value) { return insertWithOrigin(insertion, WTF::move(value)); };
    auto convert = [&](ValueID value, const DeclaredType& target) {
        return convertTo(insertion, value ? value : insert(IRBuilder::make(Opcode::Undefined)), target);
    };
    size_t numberOfPositionalParameters = callee.numberOfPositionalParameters();
    auto insertArrayOfOtherArguments = [&](TypeID contents, bool convertsElements) {
        RELEASE_ASSERT(!request.restArray);
        Vector<ValueID, 8> elements;
        for (ValueID element : request.arguments.subspan(std::min(numberOfPositionalParameters, request.arguments.size())))
            elements.append(convertsElements ? convert(element, DeclaredType { .type = contents }) : element);
        Value array = IRBuilder::make(Opcode::NewArray);
        array.setContents(contents);
        array.setElementsOperands(elements.span());
        return insert(WTF::move(array));
    };
    auto restArgument = [&] {
        TypeID required = requiredTypeOf(insertion, callee.parameters.last());
        ValueID rest = request.restArray;
        if (!rest) {
            const ClassPart* part = program.at(required).findClassPart(BuiltinClasses::Array);
            rest = insertArrayOfOtherArguments(part ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything, part && part->contentsAreProven);
        }
        if (!isValueInside(insertion, rest, required))
            return convert(rest, callee.parameters.last());
        if (insertion.function.at(rest).type == required)
            return rest;
        Value upcast = IRBuilder::make(Opcode::Upcast, { rest });
        upcast.setCastType(required);
        return insert(WTF::move(upcast));
    };
    auto hiddenRest = [&] {
        TypeID contents = provenElementTypeOfHiddenRest(program, callee);
        return insertArrayOfOtherArguments(contents, contents != TypeTable::anything);
    };
    auto argumentCount = [&] {
        RELEASE_ASSERT(!request.restArray);
        Value count = IRBuilder::make(Opcode::Integer);
        count.setInteger(static_cast<int64_t>(request.arguments.size()));
        return insert(WTF::move(count));
    };

    Arguments all;
    unsigned numberOfEntryParameters = callee.numberOfEntryParameters();
    all.reserveInitialCapacity(numberOfEntryParameters);
    size_t nextCapture = 0;
    size_t nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        switch (callee.entryParameterKind(index)) {
        case EntryParameter::Capture:
            all.append(convert(request.captures[nextCapture], callee.captures[nextCapture]));
            ++nextCapture;
            break;
        case EntryParameter::This:
            all.append(convert(request.thisValue, callee.thisType));
            break;
        case EntryParameter::Callee:
            all.append(request.calleeValue);
            break;
        case EntryParameter::NewTarget:
            all.append(request.newTarget ? request.newTarget : insert(IRBuilder::make(Opcode::Undefined)));
            break;
        case EntryParameter::ArgumentCount:
            all.append(request.argumentCount ? request.argumentCount : argumentCount());
            break;
        case EntryParameter::Parameter:
            if (nextParameter < numberOfPositionalParameters)
                all.append(convert(nextParameter < request.arguments.size() ? request.arguments[nextParameter] : ValueID(), callee.parameters[nextParameter]));
            else
                all.append(restArgument());
            ++nextParameter;
            break;
        case EntryParameter::HiddenRest:
            all.append(request.hiddenRest ? request.hiddenRest : hiddenRest());
            break;
        }
    }

    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(request.callee);
    call.setArgumentsOperands(all.span());
    if (request.accessSite)
        call.setAccessSite(request.accessSite);
    call.setIsConstruction(request.isConstruction);
    call.setIsNeverInlined(request.isNeverInlined);
    call.setIsDemotedTailCall(request.isDemotedTailCall);
    call.setOrigin(insertion.origin);
    return call;
}

ValueID appendDirectCall(IRBuilder& builder, const DirectCallRequest& request)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) { return builder.append(makeDirectCall(insertion, request)); });
}

void appendCalleeValidation(IRBuilder& builder, ValueID callee)
{
    const Function& function = builder.function();
    if (builder.context().build.validates())
        builder.append(makeValidateCallee(sameObjectThroughCasts(function, callee), function.at(callee).type));
}

Vector<ValueID, 8> insertCaptures(const ValueInsertion& insertion, FunctionID function, ValueID closure)
{
    Vector<ValueID, 8> captures;
    uint32_t numberOfCaptures = insertion.context.program.at(function).captures.size();
    for (uint32_t index = 0; index < numberOfCaptures; ++index) {
        Value capture = IRBuilder::make(Opcode::GetCapture, { closure });
        capture.setFunction(function);
        capture.setIndex(index);
        captures.append(insertWithOrigin(insertion, WTF::move(capture)));
    }
    return captures;
}

Vector<ValueID, 8> appendCaptures(IRBuilder& builder, FunctionID function, ValueID closure)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) { return insertCaptures(insertion, function, closure); });
}

void emitReturnOfCall(IRBuilder& builder, ValueID result)
{
    if (builder.context().program.typeOf(builder.function(), result).isNever())
        builder.emitUnreachable();
    else
        builder.emitReturn(result);
}

TailCallRequirement tailCallRequirementOf(const Function& body)
{
    bool hasRequiredTailCall = std::ranges::any_of(body.blocks, [](const Block& block) {
        return block.terminator.kind == TerminatorKind::ReturnCall && block.terminator.requirement() == TailCallRequirement::Required;
    });
    return hasRequiredTailCall ? TailCallRequirement::Required : TailCallRequirement::Allowed;
}

static bool canBeCallTarget(FunctionKind kind)
{
    return kind == FunctionKind::Plain || kind == FunctionKind::Arrow || kind == FunctionKind::Method || kind == FunctionKind::Getter || kind == FunctionKind::Setter;
}

static bool canBeConstructTarget(const Function& constructor)
{
    return constructor.hasBody() && (constructor.role == FunctionRole::Source || constructor.role == FunctionRole::Library);
}

bool canPassRestArguments(const Program& program, const Function& callee)
{
    if (!callee.has(FunctionFlags::HasRestParameter))
        return true;
    if (callee.parameters.isEmpty())
        return false;
    const DeclaredType& declared = callee.parameters.last();
    if (declared.flags.contains(PositionFlags::IsExpected))
        return true;
    const Type& rest = program.at(declared.type);
    return rest.anyObject || rest.findClassPart(BuiltinClasses::Array);
}

bool mayBeCalledDirectly(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    if (!callee.hasBody() || !canBeCalledByValueOf(callee.role) || !canBeCallTarget(callee.kind))
        return false;
    return !callee.isUnsplitCoroutine() && callee.results.size() == 1 && canPassRestArguments(program, callee);
}

bool canCallWithoutClosure(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    return callee.captures.isEmpty() && !callee.has(FunctionFlags::TakesCallee) && mayBeCalledDirectly(program, id);
}

bool canConstructDirectly(const Program& program, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!canBeConstructTarget(constructor) || constructor.constructorForm() == ConstructorForm::NotConstructor)
        return false;
    return constructor.results.size() == 1 && constructor.constructedClass && canPassRestArguments(program, constructor);
}

bool canConstructWithoutClosure(const Program& program, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!canBeConstructTarget(constructor) || constructor.kind != FunctionKind::BaseConstructor)
        return false;
    if (!constructor.captures.isEmpty() || constructor.hasAny({ FunctionFlags::TakesCallee, FunctionFlags::HasThis }))
        return false;
    return constructor.results.size() == 1 && canPassRestArguments(program, constructor);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
