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

static bool isValueInside(const Program& program, const Function& function, ValueID value, TypeID type)
{
    return program.typeOf(function, value).isSubsetOf(program.at(type), program);
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

template<typename Functor>
static auto withInsertionAtEnd(IRBuilder& builder, const Functor& functor)
{
    auto append = [&](auto&& value) {
        return builder.append(WTF::move(value));
    };
    InsertValue insert(append);
    return functor(ValueInsertion { builder.context(), builder.function(), builder.types(), builder.origin(), insert });
}

// Whoever asks knows that the value has the type, where the types of the operations do not show it: flow narrows a parameter from every call that reaches it, and a pass gives what the operation it replaces gave.
ValueID convertTo(const ValueInsertion& insertion, ValueID value, const DeclaredType& target)
{
    TypeID required = requiredTypeOf(insertion, target);
    if (isValueInside(insertion.context.program, insertion.function, value, required))
        return value;
    if (insertion.context.build.validates())
        insertWithOrigin(insertion, makeValidateType(value, required));
    return insertWithOrigin(insertion, makeAssumeType(value, required));
}

ValueID convertTo(IRBuilder& builder, ValueID value, const DeclaredType& target)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) {
        return convertTo(insertion, value, target);
    });
}

static ValueID argumentAt(const DirectCallRequest& request, size_t index)
{
    return index < request.arguments.size() ? request.arguments[index] : ValueID();
}

static std::span<const ValueID> argumentsFrom(const DirectCallRequest& request, size_t firstArgument)
{
    return request.arguments.subspan(std::min(firstArgument, request.arguments.size()));
}

static TypeID arrayElementTypeOf(const Program& program, const Type& type)
{
    const ClassPart* part = type.findClassPart(BuiltinClasses::Array);
    return part ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything;
}

static bool hasProvenArrayContents(const Type& type)
{
    const ClassPart* part = type.findClassPart(BuiltinClasses::Array);
    return part && part->contentsAreProven;
}

// The list has no place among the declared parameters: what flow has proven of it is assumed at the head of the callee. Who makes an array predicts its storage from its elements as its readers do.
static TypeID provenElementTypeOfHiddenRest(const Program& program, const Function& callee)
{
    std::optional<unsigned> index = callee.hiddenRestParameterIndex();
    if (!index || !callee.hasBody())
        return TypeTable::anything;
    const Block& entry = callee.at(entryBlock);
    ValueID list = entry.parameters[*index];
    for (ValueID id : entry.values) {
        const Value& value = callee.at(id);
        if (value.opcode == Opcode::AssumeType && value.valueOperand() == list && hasProvenArrayContents(program.at(value.castType())))
            return arrayElementTypeOf(program, program.at(value.castType()));
    }
    return TypeTable::anything;
}

namespace {

class DirectCallBuilder {
public:
    DirectCallBuilder(const ValueInsertion&, const DirectCallRequest&);

    Arguments createArguments();
    Value makeCall(const Arguments&) const;

private:
    ValueID insert(Value&&);
    ValueID upcastTo(ValueID, TypeID);
    ValueID convertArgument(ValueID, const DeclaredType&);
    ValueID createArgumentCount();
    ValueID createParameterArgument(size_t parameterIndex);
    ValueID createRestArgument();
    ValueID createRestArray();
    ValueID createHiddenRest();

    const ValueInsertion& m_insertion;
    const Program& m_program;
    const DirectCallRequest& m_request;
    const Function& m_callee;
};

DirectCallBuilder::DirectCallBuilder(const ValueInsertion& insertion, const DirectCallRequest& request)
    : m_insertion(insertion)
    , m_program(insertion.context.program)
    , m_request(request)
    , m_callee(m_program.at(request.callee))
{
    RELEASE_ASSERT(request.captures.size() == m_callee.captures.size());
    RELEASE_ASSERT(request.calleeValue || !m_callee.has(FunctionFlags::TakesCallee));
    RELEASE_ASSERT(!request.restArray || m_callee.has(FunctionFlags::HasRestParameter));
}

Arguments DirectCallBuilder::createArguments()
{
    Arguments all;
    unsigned numberOfEntryParameters = m_callee.numberOfEntryParameters();
    all.reserveInitialCapacity(numberOfEntryParameters);

    size_t nextCapture = 0;
    size_t nextParameter = 0;
    for (unsigned index = 0; index < numberOfEntryParameters; ++index) {
        switch (m_callee.entryParameterKind(index)) {
        case EntryParameter::Capture:
            all.append(convertArgument(m_request.captures[nextCapture], m_callee.captures[nextCapture]));
            ++nextCapture;
            break;
        case EntryParameter::This:
            all.append(convertArgument(m_request.thisValue, m_callee.thisType));
            break;
        case EntryParameter::Callee:
            all.append(m_request.calleeValue);
            break;
        case EntryParameter::NewTarget:
            all.append(m_request.newTarget ? m_request.newTarget : insert(IRBuilder::make(Opcode::Undefined)));
            break;
        case EntryParameter::ArgumentCount:
            all.append(createArgumentCount());
            break;
        case EntryParameter::Parameter:
            all.append(createParameterArgument(nextParameter++));
            break;
        case EntryParameter::HiddenRest:
            all.append(createHiddenRest());
            break;
        }
    }
    return all;
}

ValueID DirectCallBuilder::insert(Value&& value)
{
    return insertWithOrigin(m_insertion, WTF::move(value));
}

ValueID DirectCallBuilder::upcastTo(ValueID value, TypeID type)
{
    if (m_insertion.function.at(value).type == type)
        return value;
    Value upcast = IRBuilder::make(Opcode::Upcast, { value });
    upcast.setCastType(type);
    return insert(WTF::move(upcast));
}

ValueID DirectCallBuilder::convertArgument(ValueID value, const DeclaredType& target)
{
    if (!value)
        value = insert(IRBuilder::make(Opcode::Undefined));
    return convertTo(m_insertion, value, target);
}

ValueID DirectCallBuilder::createArgumentCount()
{
    if (m_request.argumentCount)
        return m_request.argumentCount;

    RELEASE_ASSERT(!m_request.restArray);
    Value count = IRBuilder::make(Opcode::Integer);
    count.setInteger(static_cast<int64_t>(m_request.arguments.size()));
    return insert(WTF::move(count));
}

ValueID DirectCallBuilder::createParameterArgument(size_t parameterIndex)
{
    if (parameterIndex < m_callee.numberOfPositionalParameters())
        return convertArgument(argumentAt(m_request, parameterIndex), m_callee.parameters[parameterIndex]);
    return createRestArgument();
}

ValueID DirectCallBuilder::createRestArgument()
{
    const DeclaredType& restParameter = m_callee.parameters.last();
    TypeID required = requiredTypeOf(m_insertion, restParameter);
    ValueID rest = m_request.restArray ? m_request.restArray : createRestArray();
    if (isValueInside(m_program, m_insertion.function, rest, required))
        return upcastTo(rest, required);
    return convertArgument(rest, restParameter);
}

ValueID DirectCallBuilder::createRestArray()
{
    const Type& restType = m_program.at(requiredTypeOf(m_insertion, m_callee.parameters.last()));
    TypeID elementType = arrayElementTypeOf(m_program, restType);
    bool convertsElements = hasProvenArrayContents(restType);

    Vector<ValueID, 8> elements;
    for (ValueID element : argumentsFrom(m_request, m_callee.numberOfPositionalParameters()))
        elements.append(convertsElements ? convertArgument(element, DeclaredType { .type = elementType }) : element);

    Value rest = IRBuilder::make(Opcode::NewArray);
    rest.setContents(elementType);
    rest.setElementsOperands(elements.span());
    return insert(WTF::move(rest));
}

ValueID DirectCallBuilder::createHiddenRest()
{
    if (m_request.hiddenRest)
        return m_request.hiddenRest;

    RELEASE_ASSERT(!m_request.restArray);
    TypeID elementType = provenElementTypeOfHiddenRest(m_program, m_callee);
    Vector<ValueID, 8> elements;
    for (ValueID element : argumentsFrom(m_request, m_callee.numberOfPositionalParameters()))
        elements.append(elementType == TypeTable::anything ? element : convertArgument(element, DeclaredType { .type = elementType }));

    Value rest = IRBuilder::make(Opcode::NewArray);
    rest.setContents(elementType);
    rest.setElementsOperands(elements.span());
    return insert(WTF::move(rest));
}

Value DirectCallBuilder::makeCall(const Arguments& arguments) const
{
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(m_request.callee);
    call.setArgumentsOperands(arguments.span());
    if (m_request.accessSite)
        call.setAccessSite(m_request.accessSite);
    call.setIsConstruction(m_request.isConstruction);
    call.setIsNeverInlined(m_request.isNeverInlined);
    call.setIsDemotedTailCall(m_request.isDemotedTailCall);
    call.setOrigin(m_insertion.origin);
    return call;
}

} // anonymous namespace

ValueID appendDirectCall(IRBuilder& builder, const DirectCallRequest& request)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) {
        DirectCallBuilder call(insertion, request);
        return builder.append(call.makeCall(call.createArguments()));
    });
}

Value makeDirectCall(const ValueInsertion& insertion, const DirectCallRequest& request)
{
    DirectCallBuilder call(insertion, request);
    return call.makeCall(call.createArguments());
}

void appendCalleeValidation(IRBuilder& builder, ValueID callee)
{
    if (!builder.context().build.validates())
        return;
    const Function& function = builder.function();
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
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) {
        return insertCaptures(insertion, function, closure);
    });
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
    switch (kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canBeConstructTarget(FunctionRole role)
{
    switch (role) {
    case FunctionRole::Source:
    case FunctionRole::Library:
        return true;
    case FunctionRole::Model:
    case FunctionRole::Signature:
    case FunctionRole::GenericEntrypoint:
    case FunctionRole::TableEntrypoint:
    case FunctionRole::TableLookup:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::ConstructBody:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CalleeVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::AsyncWorker:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::EntryChecks:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Serializer:
    case FunctionRole::Parser:
    case FunctionRole::ValidatingParser:
    case FunctionRole::Validator:
    case FunctionRole::ChainStep:
    case FunctionRole::StubBody:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    if (callee.isUnsplitCoroutine() || callee.results.size() != 1)
        return false;
    return canPassRestArguments(program, callee);
}

bool canCallWithoutClosure(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    return callee.captures.isEmpty() && !callee.has(FunctionFlags::TakesCallee) && mayBeCalledDirectly(program, id);
}

bool canConstructDirectly(const Program& program, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!constructor.hasBody() || !canBeConstructTarget(constructor.role))
        return false;
    if (constructor.constructorForm() == ConstructorForm::NotConstructor)
        return false;
    if (constructor.results.size() != 1 || !constructor.constructedClass)
        return false;
    return canPassRestArguments(program, constructor);
}

bool canConstructWithoutClosure(const Program& program, FunctionID id)
{
    const Function& constructor = program.at(id);
    if (!constructor.hasBody() || !canBeConstructTarget(constructor.role) || constructor.kind != FunctionKind::BaseConstructor)
        return false;
    if (!constructor.captures.isEmpty() || constructor.hasAny({ FunctionFlags::TakesCallee, FunctionFlags::HasThis }))
        return false;
    return constructor.results.size() == 1 && canPassRestArguments(program, constructor);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
