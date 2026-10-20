/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveCallsDirectCall.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCheckCause.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfProtocols.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTInlineCopy.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static bool isValueInside(const Program& program, const Function& function, ValueID value, TypeID type)
{
    return program.typeOf(function, value).isSubsetOf(program.at(type), program);
}

static CheckCause computeClaimCause(const Program& program, const Function& function, ValueID value, const DeclaredType& target)
{
    if (!target.site)
        return CheckCause::AddedByPass;
    if (!program.typeOf(function, value).isAnything())
        return CheckCause::MayFail;
    return computeUnknownObjectCause(function, value);
}

static ValueID insertWithOrigin(const ValueInsertion& insertion, Value&& value, Origin origin)
{
    ValueID id = insertion.insert(WTF::move(value));
    insertion.function.at(id).setOrigin(origin);
    return id;
}

static ValueID insertClaim(const ValueInsertion& insertion, ValueID value, const DeclaredType& target, Origin origin)
{
    Value claim = IRBuilder::make(Opcode::Claim, { value });
    claim.setCastType(target.type);
    claim.setSite(target.site);
    claim.setCause(computeClaimCause(insertion.context.program, insertion.function, value, target));
    return insertWithOrigin(insertion, WTF::move(claim), origin);
}

static TypeID internUnlessEqual(TypeArena& types, TypeID id, Type&& type)
{
    return type == types.at(id) ? id : types.intern(WTF::move(type));
}

template<typename Functor>
static ValueID insertConversion(const ValueInsertion& insertion, ValueID value, const DeclaredType& target, const Functor& computeClaimOrigin)
{
    const Program& program = insertion.context.program;
    TypeArena& types = insertion.types;
    if (target.written) {
        TypeID written = internUnlessEqual(types, target.written, testedWrittenTypeOf(types.at(target.written)));
        if (!isValueInside(program, insertion.function, value, written))
            value = insertClaim(insertion, value, DeclaredType { .type = written, .site = target.site }, computeClaimOrigin());
    }
    TypeID proven = internUnlessEqual(types, target.type, testedProvenTypeOf(types.at(target.type)));
    if (!isValueInside(program, insertion.function, value, proven)) {
        if (target.written)
            value = insertClaim(insertion, value, DeclaredType { .type = proven }, insertion.origin);
        else
            value = insertClaim(insertion, value, DeclaredType { .type = proven, .site = target.site }, computeClaimOrigin());
    }
    if (isValueInside(program, insertion.function, value, target.type))
        return value;
    if (insertion.context.build.validates())
        insertWithOrigin(insertion, makeValidateType(value, target.type), insertion.origin);
    return insertWithOrigin(insertion, makeAssumeType(value, target.type), insertion.origin);
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

ValueID convertTo(const ValueInsertion& insertion, ValueID value, const DeclaredType& target)
{
    if (isValueInside(insertion.context.program, insertion.function, value, target.type))
        return value;
    return insertConversion(insertion, value, target, [&] {
        return insertion.origin;
    });
}

ValueID convertTo(IRBuilder& builder, ValueID value, const DeclaredType& target)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) {
        return convertTo(insertion, value, target);
    });
}

static bool isAsyncFunction(const Function& function)
{
    if (function.role == FunctionRole::CoroutineRamp)
        return function.creator.as<CreatorRecord::Coroutine>().kind == CoroutineKind::AsyncFunction;
    return function.has(FunctionFlags::IsAsync) && !function.has(FunctionFlags::IsGenerator);
}

bool rejectsOnFailedConversion(const Program& program, FunctionID id)
{
    const Function* callee = &program.at(id);
    while (const auto* boundFunction = callee->creator.tryAs<CreatorRecord::BoundFunction>())
        callee = &program.at(boundFunction->target);
    return isAsyncFunction(*callee);
}

static DeclaredType withoutSite(const DeclaredType& declared)
{
    return { .type = declared.type };
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

static bool areRestArgumentsInside(const Program& program, const Function& caller, const DirectCallRequest& request, const Function& callee)
{
    if (!canPassRestArguments(program, callee))
        return false;
    if (request.restArray)
        return true;
    const Type& restType = program.at(callee.parameters.last().type);
    if (!hasProvenArrayContents(restType))
        return true;
    const Type& elementType = program.at(arrayElementTypeOf(program, restType));
    return std::ranges::all_of(argumentsFrom(request, callee.numberOfPositionalParameters()), [&](ValueID argument) {
        return program.typeOf(caller, argument).isSubsetOf(elementType, program);
    });
}

static bool allOperandsSatisfy(const Program& program, const Function& caller, const DirectCallRequest& request, const Function& callee, NOESCAPE const Invocable<bool(const Type&, const DeclaredType&)> auto& accepts)
{
    auto typeOrUndefined = [&](ValueID value) -> const Type& {
        return value ? program.typeOf(caller, value) : program.at(TypeTable::undefined);
    };

    for (size_t index = 0; index < request.captures.size(); ++index) {
        if (!accepts(program.typeOf(caller, request.captures[index]), withoutSite(callee.captures[index])))
            return false;
    }
    if (callee.has(FunctionFlags::HasThis) && !accepts(typeOrUndefined(request.thisValue), callee.thisType))
        return false;

    size_t numberOfPositionalParameters = callee.numberOfPositionalParameters();
    for (size_t index = 0; index < numberOfPositionalParameters; ++index) {
        if (!accepts(typeOrUndefined(argumentAt(request, index)), callee.parameters[index]))
            return false;
    }
    return !callee.has(FunctionFlags::HasRestParameter) || areRestArgumentsInside(program, caller, request, callee);
}

static bool areArgumentsInside(const Program& program, const Function& caller, const DirectCallRequest& request, const Function& callee)
{
    return allOperandsSatisfy(program, caller, request, callee, [&](const Type& type, const DeclaredType& declared) {
        return type.isSubsetOf(program.at(declared.type), program);
    });
}

static bool passEntryTests(const Program& program, const Function& caller, const DirectCallRequest& request, const Function& callee)
{
    return allOperandsSatisfy(program, caller, request, callee, [&](const Type& type, const DeclaredType& declared) {
        return !mayFailEntryTests(program, type, declared);
    });
}

bool needsArgumentConversion(const Program& program, const Function& caller, const DirectCallRequest& request)
{
    return !areArgumentsInside(program, caller, request, program.at(request.callee));
}

static FunctionID findGenericVersion(const AnalysisContext& context, FunctionID callee)
{
    if (!context.flow)
        return { };
    FunctionID genericVersion = context.flow->function(callee).genericVersion;
    if (!genericVersion)
        return { };
    const Function& version = context.program.at(genericVersion);
    if (version.derivation.source != callee || !version.hasBody())
        return { };
    return genericVersion;
}

namespace {

struct CallTarget {
    FunctionID function;
    ResultCheck resultCheck { ResultCheck::ByCallee };
};

class DirectCallBuilder {
public:
    DirectCallBuilder(const ValueInsertion&, const DirectCallRequest&);

    bool needsRejectionPath() const;
    Arguments createArguments();
    Value makeCall(const Arguments&) const;
    ValueID appendWithRejection(IRBuilder&);
    ValueID failedConversion() const { return m_failedConversion; }

private:
    static CallTarget chooseTarget(const ValueInsertion&, const DirectCallRequest&);

    bool mayThrowBeforeCall() const;
    ValueID insert(Value&&);
    ValueID upcastTo(ValueID, TypeID);
    ValueID convertArgument(ValueID, const DeclaredType&);
    Origin parameterOrigin(TypeSiteID);
    bool isCalledByEntrypoint() const;
    std::optional<uint32_t> ensureCalleeFrame();
    ValueID createArgumentCount();
    ValueID createParameterArgument(size_t parameterIndex);
    ValueID createRestArgument();
    ValueID createRestArray();
    ValueID createHiddenRest();
    TypeID joinedResultType(ValueID call) const;

    const ValueInsertion& m_insertion;
    const Program& m_program;
    const DirectCallRequest& m_request;
    CallTarget m_target;
    const Function& m_callee;
    std::optional<uint32_t> m_calleeFrame;
    ValueID m_failedConversion;
};

DirectCallBuilder::DirectCallBuilder(const ValueInsertion& insertion, const DirectCallRequest& request)
    : m_insertion(insertion)
    , m_program(insertion.context.program)
    , m_request(request)
    , m_target(chooseTarget(insertion, request))
    , m_callee(m_program.at(m_target.function))
{
    RELEASE_ASSERT(request.captures.size() == m_callee.captures.size());
    RELEASE_ASSERT(request.calleeValue || !m_callee.has(FunctionFlags::TakesCallee));
    RELEASE_ASSERT(!request.restArray || (m_callee.has(FunctionFlags::HasRestParameter) && insertion.function.at(request.restArray).opcode == Opcode::CreateRest));
}

// The parameters of a proven version may hold layout sets and proven contents, which no conversion tests. The generic version is right for any call.
CallTarget DirectCallBuilder::chooseTarget(const ValueInsertion& insertion, const DirectCallRequest& request)
{
    const AnalysisContext& context = insertion.context;
    FunctionID genericVersion = findGenericVersion(context, request.callee);
    if (!genericVersion || !needsArgumentConversion(context.program, insertion.function, request))
        return { request.callee, request.resultCheck };
    return { genericVersion, resultCheckOf(*context.flow, genericVersion) };
}

bool DirectCallBuilder::needsRejectionPath() const
{
    return rejectsOnFailedConversion(m_program, m_target.function) && mayThrowBeforeCall();
}

bool DirectCallBuilder::mayThrowBeforeCall() const
{
    if (!passEntryTests(m_program, m_insertion.function, m_request, m_callee))
        return true;
    bool createsRestArray = m_callee.has(FunctionFlags::HasRestParameter) && !m_request.restArray;
    bool createsHiddenRest = m_callee.has(FunctionFlags::HasHiddenRest) && !m_request.hiddenRest;
    return (createsRestArray || createsHiddenRest) && mayThrow(Opcode::NewArray);
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
            all.append(convertArgument(m_request.captures[nextCapture], withoutSite(m_callee.captures[nextCapture])));
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

// A conversion of the type never always throws. Nothing behind it is reached and nothing may follow it in its block, so it stands for all that would.
ValueID DirectCallBuilder::insert(Value&& value)
{
    if (m_failedConversion)
        return m_failedConversion;
    return insertWithOrigin(m_insertion, WTF::move(value), m_insertion.origin);
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
    if (m_failedConversion)
        return m_failedConversion;
    if (!value)
        value = insert(IRBuilder::make(Opcode::Undefined));
    if (isValueInside(m_program, m_insertion.function, value, target.type))
        return value;
    ValueID converted = insertConversion(m_insertion, value, target, [&] {
        return target.site ? parameterOrigin(target.site) : m_insertion.origin;
    });
    if (m_program.typeOf(m_insertion.function, converted).isNever())
        m_failedConversion = converted;
    return converted;
}

Origin DirectCallBuilder::parameterOrigin(TypeSiteID id)
{
    Origin callOrigin = m_insertion.origin;
    const TypeSite& site = m_program.at(id);
    if (site.module != m_callee.module)
        return callOrigin;
    if (isCalledByEntrypoint())
        return { site.position, callOrigin.inlineFrame };
    if (std::optional<uint32_t> frame = ensureCalleeFrame())
        return { site.position, *frame };
    return callOrigin;
}

bool DirectCallBuilder::isCalledByEntrypoint() const
{
    const auto* entrypoint = m_insertion.function.creator.tryAs<CreatorRecord::Entrypoint>();
    if (!entrypoint)
        return false;
    FunctionID entered = findFunctionAsRead(&m_program, entrypoint->target);
    return entered && entered == findFunctionAsRead(&m_program, m_target.function);
}

std::optional<uint32_t> DirectCallBuilder::ensureCalleeFrame()
{
    if (m_calleeFrame)
        return m_calleeFrame;

    Function& caller = m_insertion.function;
    Origin callOrigin = m_insertion.origin;
    size_t frame = inlineFrameCountOf(caller);
    Subject subject { .function = findFunctionAsRead(&m_program, m_insertion.context.id), .location = caller.locationOf(callOrigin) };
    if (m_insertion.context.build.exceeds(LimitID::InlineFramesPerFunction, frame + 1, subject))
        return std::nullopt;

    if (caller.inlineFrames.isEmpty())
        caller.inlineFrames.append(InlineFrame { });
    caller.inlineFrames.append(InlineFrame {
        .callee = m_target.function,
        .callPosition = callOrigin.position,
        .parent = callOrigin.inlineFrame,
        .isTailCall = m_request.isDemotedTailCall,
        .isConstruction = m_request.isConstruction,
        .module = m_callee.module,
    });
    m_calleeFrame = static_cast<uint32_t>(frame);
    return m_calleeFrame;
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
    ValueID rest = m_request.restArray ? m_request.restArray : createRestArray();
    if (isValueInside(m_program, m_insertion.function, rest, restParameter.type))
        return upcastTo(rest, restParameter.type);
    return convertArgument(rest, canPassRestArguments(m_program, m_callee) ? withoutSite(restParameter) : restParameter);
}

ValueID DirectCallBuilder::createRestArray()
{
    const Type& restType = m_program.at(m_callee.parameters.last().type);
    TypeID elementType = arrayElementTypeOf(m_program, restType);
    bool testsElements = hasProvenArrayContents(restType);

    Vector<ValueID, 8> elements;
    for (ValueID element : argumentsFrom(m_request, m_callee.numberOfPositionalParameters()))
        elements.append(testsElements ? convertArgument(element, DeclaredType { .type = elementType }) : element);

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
    Value rest = IRBuilder::make(Opcode::NewArray);
    rest.setContents(TypeTable::anything);
    rest.setElementsOperands(argumentsFrom(m_request, m_callee.numberOfPositionalParameters()));
    return insert(WTF::move(rest));
}

Value DirectCallBuilder::makeCall(const Arguments& arguments) const
{
    Value call = IRBuilder::make(Opcode::CallDirect);
    call.setFunction(m_target.function);
    call.setArgumentsOperands(arguments.span());
    call.setResultCheck(m_target.resultCheck);
    if (m_request.accessSite)
        call.setAccessSite(m_request.accessSite);
    call.setIsConstruction(m_request.isConstruction);
    call.setIsNeverInlined(m_request.isNeverInlined);
    call.setIsDemotedTailCall(m_request.isDemotedTailCall);
    call.setOrigin(m_insertion.origin);
    return call;
}

TypeID DirectCallBuilder::joinedResultType(ValueID call) const
{
    TypeArena& types = m_insertion.types;
    return types.unionOf(m_insertion.function.at(call).type, types.intern(Type::builtinClass(BuiltinClasses::Promise)));
}

ValueID DirectCallBuilder::appendWithRejection(IRBuilder& builder)
{
    BlockFrequency frequency = builder.function().at(builder.currentBlock()).frequency;
    std::optional<Target> outerHandler = builder.handler();
    BlockID rejection = builder.addHandlerBlock(BlockRole::CatchHandler, { }, BlockFrequency::Rare);

    builder.setHandler(Target { rejection, { }, { } });
    BlockID conversions = builder.addBlock(frequency);
    builder.emitJump(conversions);
    builder.moveToEndOf(conversions);
    Arguments arguments = createArguments();

    builder.setHandler(outerHandler);
    BlockID join;
    ValueID result;
    if (m_failedConversion) {
        builder.emitUnreachable();
        join = builder.addBlock(frequency);
        result = builder.addParameter(join, m_insertion.types.intern(Type::builtinClass(BuiltinClasses::Promise)));
    } else {
        BlockID after = builder.addBlock(frequency);
        builder.emitJump(after);
        builder.moveToEndOf(after);
        ValueID call = builder.append(makeCall(arguments));
        join = builder.addBlock(frequency);
        result = builder.addParameter(join, joinedResultType(call));
        builder.emitJump(join, { call });
    }

    builder.moveToEndOf(rejection);
    ValueID error = builder.append(Opcode::CatchValue, { });
    ValueID promise = builder.append(Opcode::NewPromise, { });
    builder.append(Opcode::RejectPromise, { promise, error });
    builder.emitJump(join, { promise });

    builder.moveToEndOf(join);
    return result;
}

} // anonymous namespace

ValueID appendDirectCall(IRBuilder& builder, const DirectCallRequest& request)
{
    return withInsertionAtEnd(builder, [&](const ValueInsertion& insertion) {
        DirectCallBuilder call(insertion, request);
        if (call.needsRejectionPath())
            return call.appendWithRejection(builder);
        Arguments arguments = call.createArguments();
        if (ValueID failedConversion = call.failedConversion())
            return failedConversion;
        return builder.append(call.makeCall(arguments));
    });
}

std::optional<Value> tryMakeDirectCall(const ValueInsertion& insertion, const DirectCallRequest& request)
{
    DirectCallBuilder call(insertion, request);
    if (call.needsRejectionPath())
        return std::nullopt;
    return call.makeCall(call.createArguments());
}

static DeclaredType testedResultOf(IRBuilder& builder, FunctionID callee)
{
    const Program& program = builder.context().program;
    TypeArena& types = builder.types();
    const DeclaredType& declared = program.at(callee).results[0];
    TypingContext written { program, builder.function(), types, TypeLevel::Written };
    return DeclaredType { .type = types.intern(written.typeOf(declared).withoutProofs()), .site = declared.site };
}

bool needsResultCheck(IRBuilder& builder, ResultCheck check, FunctionID callee, ValueID result)
{
    switch (check) {
    case ResultCheck::ByCallee:
        return false;
    case ResultCheck::ByCallers: {
        const Type& tested = builder.types().at(testedResultOf(builder, callee).type);
        return !builder.types().at(builder.function().at(result).type).isSubsetOf(tested, builder.context().program);
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID appendResultCheck(IRBuilder& builder, ResultCheck check, FunctionID callee, ValueID result)
{
    switch (check) {
    case ResultCheck::ByCallee:
        return result;
    case ResultCheck::ByCallers:
        return convertTo(builder, result, testedResultOf(builder, callee));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID appendResultCheck(IRBuilder& builder, ValueID result)
{
    const Value& call = builder.function().at(result);
    if (call.opcode != Opcode::CallDirect)
        return result;
    return appendResultCheck(builder, call.resultCheck(), call.function(), result);
}

Vector<ValueID, 8> insertCaptures(const ValueInsertion& insertion, FunctionID function, ValueID closure)
{
    Vector<ValueID, 8> captures;
    uint32_t numberOfCaptures = insertion.context.program.at(function).captures.size();
    for (uint32_t index = 0; index < numberOfCaptures; ++index) {
        Value capture = IRBuilder::make(Opcode::GetCapture, { closure });
        capture.setFunction(function);
        capture.setIndex(index);
        captures.append(insertWithOrigin(insertion, WTF::move(capture), insertion.origin));
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
    case FunctionRole::GroupEntrypoint:
    case FunctionRole::GenericVersion:
    case FunctionRole::AllocatedThisVersion:
    case FunctionRole::CallSiteVersion:
    case FunctionRole::CoroutineRamp:
    case FunctionRole::CoroutineBody:
    case FunctionRole::BoundFunction:
    case FunctionRole::ResultWrapper:
    case FunctionRole::ResultWorker:
    case FunctionRole::ParameterWrapper:
    case FunctionRole::OutlinedBody:
    case FunctionRole::Continuation:
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
    const Type& rest = program.at(callee.parameters.last().type);
    return rest.anyObject || rest.findClassPart(BuiltinClasses::Array);
}

bool mayBeCalledDirectly(const Program& program, FunctionID id)
{
    const Function& callee = program.at(id);
    bool hasCallableRole = canBeCalledByValueOf(callee.role) || callee.role == FunctionRole::GenericVersion;
    if (!callee.hasBody() || !hasCallableRole || !canBeCallTarget(callee.kind))
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
