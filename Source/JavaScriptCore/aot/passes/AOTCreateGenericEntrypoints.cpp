/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateEntrypointsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTDerivedFunctions.h"
#include "AOTEffectsCheck.h"
#include "AOTErrorMessages.h"
#include "AOTFlowResults.h"
#include "AOTResolveCalls.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTStringArena.h"
#include "AOTThreshold.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct EntryArguments {
    ValueID closure;
    ValueID thisValue;
    ValueID newTarget;
    Vector<ValueID, 8> fixedArguments;
    ValueID restArray;
    ValueID argumentCount;
    ValueID hiddenRest;
};

} // anonymous namespace

bool hasAllocatingConstructEntrypoint(const Program& program, const Function& target)
{
    // A second entrypoint keeps the body out of the first. A long constructor spends its time in the body, and gains little by an object that is made in line. The length of the source never changes.
    AOT_THRESHOLD(uint32_t, mostCharactersOfShortConstructor, 1000);
    if (target.text.end - target.text.start > mostCharactersOfShortConstructor)
        return false;
    // The constructor functions of the runtime's own modules are constructed by name.
    if (target.role == FunctionRole::Library || target.enclosingRuntimeFunction || (target.module && program.at(target.module).kind == ModuleKind::RuntimeSource))
        return false;
    if (target.hasAny({ FunctionFlags::HasNewTarget, FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || target.role == FunctionRole::CoroutineRamp || target.results.size() != 1)
        return false;
    return isConstructorFunction(program, target) && program.at(target.constructedClass).flow.allocation != AllocationState::NeverAllocated;
}

EntrypointVariants entrypointVariantsOf(const Program& program, const Function& target)
{
    if (!target.has(FunctionFlags::HasNewTarget))
        return hasAllocatingConstructEntrypoint(program, target) ? EntrypointVariants::Both : EntrypointVariants::WithoutNewTarget;

    switch (target.kind) {
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
        return EntrypointVariants::WithNewTarget;
    case FunctionKind::Plain:
        if (target.hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) || target.role == FunctionRole::CoroutineRamp)
            return EntrypointVariants::WithoutNewTarget;
        return EntrypointVariants::Both;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return EntrypointVariants::WithoutNewTarget;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ArgumentPassing argumentPassingOf(const Function& target)
{
    bool readsArgumentList = target.hasAny({ FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount });
    return readsArgumentList ? ArgumentPassing::Buffer : ArgumentPassing::Registers;
}

// The run time enters the bodies of all modules with one closure, which it made for the first of them: Modules::callBody().
static TypeID closureTypeOf(TypeArena& types, const Function& target, FunctionID id)
{
    bool isBodyOfModule = target.kind == FunctionKind::Module || target.kind == FunctionKind::CommonJsModule;
    return isBodyOfModule ? TypeTable::function : types.intern(Type::forFunction(id));
}

static uint32_t keyParameterOf(const Program& program, const Function& target, const GenericEntrypointRequest& request)
{
    if (!request.takesNewTarget || entrypointVariantsOf(program, target) != EntrypointVariants::Both)
        return callEntrypointParameter;
    return request.allocatesThis ? allocatingConstructEntrypointParameter : constructEntrypointParameter;
}

static Function createEntryWithoutBody(const Program& program, TypeArena& types, const Function& target, const GenericEntrypointRequest& request)
{
    const DeclaredType anything { .type = TypeTable::anything };
    ArgumentPassing passing = argumentPassingOf(target);

    Function entry = createDerivedHeader(target, request.target, FunctionRole::GenericEntrypoint, keyParameterOf(program, target, request));
    entry.flags.add(FunctionFlags::HasThis);
    entry.thisType = anything;
    entry.captures.append(DeclaredType { .type = closureTypeOf(types, target, request.target) });
    entry.results.append(anything);
    switch (passing) {
    case ArgumentPassing::Registers:
        if (request.takesNewTarget)
            entry.flags.add(FunctionFlags::HasNewTarget);
        entry.parameters.fill(anything, target.numberOfPositionalParameters());
        break;
    case ArgumentPassing::Buffer:
        break;
    }
    entry.creator = CreatorRecord::Entrypoint {
        .target = request.target,
        .arguments = passing,
        .takesNewTarget = request.takesNewTarget,
        .constructEntrypoint = request.constructEntrypoint,
    };
    if (request.isEnteredByHost)
        entry.capabilities.remove(FunctionCapabilities::IsNeverCalledByHost);
    return entry;
}

static ValueID addArgument(IRBuilder& builder, ArgumentPassing passing, uint32_t index)
{
    switch (passing) {
    case ArgumentPassing::Registers:
        return builder.addParameter(builder.currentBlock(), TypeTable::anything);
    case ArgumentPassing::Buffer: {
        Value argument = IRBuilder::make(Opcode::GetArgument);
        argument.setArgumentIndex(index);
        return builder.append(WTF::move(argument));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ValueID appendArgumentListRead(IRBuilder& builder, Opcode opcode, uint32_t firstArgument)
{
    Value read = IRBuilder::make(opcode);
    read.setFirstArgument(firstArgument);
    return builder.append(WTF::move(read));
}

static EntryArguments addEntryArguments(IRBuilder& builder, const Function& target)
{
    const Function& entry = builder.function();
    const CreatorRecord::Entrypoint& record = entry.creator.as<CreatorRecord::Entrypoint>();
    unsigned numberOfPositionalParameters = target.numberOfPositionalParameters();

    BlockID block = builder.addBlock();
    builder.moveToEndOf(block);

    EntryArguments arguments;
    arguments.closure = builder.addParameter(block, entry.captures[0].type);
    arguments.thisValue = builder.addParameter(block, entry.thisType.type);
    uint32_t nextArgumentIndex = 0;
    if (record.takesNewTarget)
        arguments.newTarget = addArgument(builder, record.arguments, nextArgumentIndex++);
    uint32_t firstSourceArgument = nextArgumentIndex;
    for (unsigned i = 0; i < numberOfPositionalParameters; ++i)
        arguments.fixedArguments.append(addArgument(builder, record.arguments, nextArgumentIndex++));
    if (target.has(FunctionFlags::HasRestParameter))
        arguments.restArray = appendArgumentListRead(builder, Opcode::CreateRest, nextArgumentIndex);
    if (target.has(FunctionFlags::HasHiddenRest))
        arguments.hiddenRest = appendArgumentListRead(builder, Opcode::CreateRest, nextArgumentIndex);
    if (target.has(FunctionFlags::HasArgumentCount))
        arguments.argumentCount = appendArgumentListRead(builder, Opcode::GetRestLength, firstSourceArgument);
    return arguments;
}

static String messageOfEntryThatNothingEnters(const AnalysisContext& context, const Function& target)
{
    const StringTable& strings = context.program.strings;
    ASCIILiteral advice = context.build.recordedOptions().adviceToKeepFunctionSource();
    // An obfuscated executable says nothing about its sources.
    bool mayNameSources = !context.build.options().obfuscates();
    if (mayNameSources && target.name && target.name != strings.emptyString())
        return functionNotCompiledMessage(strings.toWTFString(target.name), advice);
    if (mayNameSources && target.module)
        return functionNotCompiledMessage(strings.toWTFString(context.program.pathForLineColumns(target.module)), context.program.lineColumnFor(target.module, target.position).line, advice);
    return functionNotCompiledMessage(advice);
}

// No call that flow follows comes here.
static void emitEntryThatNothingEnters(IRBuilder& builder, const Function& target, StringArena& strings)
{
    const AnalysisContext& context = builder.context();
    if (context.build.validates() && context.flow && !context.flow->unseenCodeCanRun() && target.role != FunctionRole::Library) {
        builder.emitUnreachable();
        return;
    }
    String message = messageOfEntryThatNothingEnters(context, target);
    Value error = IRBuilder::make(Opcode::ThrowNewError);
    error.setErrorKind(ErrorKind::Error);
    error.setMessage(strings.intern(StringView(message)));
    builder.append(WTF::move(error));
    builder.emitUnreachable();
}

// A body is removed once nothing can enter it. The closure may still be created, and its identity is the entrypoint.
static bool canSeenCodeEnter(const Function& target)
{
    return target.hasBody();
}

static void appendTargetCall(IRBuilder& builder, FunctionID target, const EntryArguments& arguments)
{
    Vector<ValueID, 8> captures = appendCaptures(builder, target, arguments.closure);
    ValueID call = appendDirectCall(builder, {
        .callee = target,
        .captures = captures.span(),
        .thisValue = arguments.thisValue,
        .calleeValue = arguments.closure,
        .newTarget = arguments.newTarget,
        .arguments = arguments.fixedArguments.span(),
        .restArray = arguments.restArray,
        .argumentCount = arguments.argumentCount,
        .hiddenRest = arguments.hiddenRest,
    });
    emitReturnOfEntrypointCall(builder, call);
}

// new.target is the closure here: whoever enters has seen to that.
static void appendConstruction(IRBuilder& builder, FunctionID target, const EntryArguments& arguments, StringArena& strings)
{
    AOT_THRESHOLD(size_t, mostValuesOfCopiedConstructor, 64);
    ValueID object = appendThisOfConstructorFunction(builder, target, arguments.closure);
    // Flow narrows `this` from every call that reaches the function. `function Base() {}` that is only called with the objects of others, by Base.call(this) and by super(), never sees an object of its own layout.
    TypingContext typing { builder.context().program, builder.function(), builder.types() };
    if (typing.typeOf(object).castTo(typing.typeOf(builder.context().program.at(target).thisType), builder.types()).isNever()) {
        emitEntryThatNothingEnters(builder, builder.context().program.at(target), strings);
        return;
    }
    Vector<ValueID, 8> captures = appendCaptures(builder, target, arguments.closure);
    ValueID returned = appendDirectCall(builder, {
        .callee = target,
        .captures = captures.span(),
        .thisValue = object,
        .calleeValue = arguments.closure,
        .arguments = arguments.fixedArguments.span(),
        .restArray = arguments.restArray,
        .argumentCount = arguments.argumentCount,
        .hiddenRest = arguments.hiddenRest,
        .isConstruction = true,
        .isNeverInlined = builder.context().program.at(target).values.size() > mostValuesOfCopiedConstructor,
    });
    emitReturnOfConstruction(builder, returned, object);
}

Function createGenericEntrypoint(const AnalysisContext& context, TypeArena& types, StringArena& strings, const GenericEntrypointRequest& request)
{
    const Function& target = context.program.at(request.target);
    Function entry = createEntryWithoutBody(context.program, types, target, request);
    IRBuilder builder(context, entry, types);
    builder.setOrigin({ target.position, 0 });
    EntryArguments arguments = addEntryArguments(builder, target);
    if (!canSeenCodeEnter(target)) {
        emitEntryThatNothingEnters(builder, target, strings);
        return entry;
    }
    if (request.allocatesThis)
        appendConstruction(builder, request.target, arguments, strings);
    else
        appendTargetCall(builder, request.target, arguments);
    if (context.build.validates())
        checkCallByValueCoversEntrypoint(context.program, request.target, entry, context.build.diagnostics());
    return entry;
}

Function buildGenericEntrypoint(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Function& target = inputs.program.at(key.target);
    EntrypointVariants variants = entrypointVariantsOf(inputs.program, target);
    RELEASE_ASSERT(canBeCalledByValueOf(target.role));
    bool allocatesThis = hasAllocatingConstructEntrypoint(inputs.program, target);

    GenericEntrypointRequest request { .target = key.target };
    if (key.parameter == constructEntrypointParameter || key.parameter == allocatingConstructEntrypointParameter) {
        RELEASE_ASSERT(variants == EntrypointVariants::Both && allocatesThis == (key.parameter == allocatingConstructEntrypointParameter));
        request.takesNewTarget = true;
        request.allocatesThis = allocatesThis;
    } else {
        RELEASE_ASSERT(key.parameter == callEntrypointParameter);
        request.takesNewTarget = variants == EntrypointVariants::WithNewTarget;
        if (variants == EntrypointVariants::Both)
            request.constructEntrypoint = inputs.derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, key.target, allocatesThis ? allocatingConstructEntrypointParameter : constructEntrypointParameter));
    }
    return createGenericEntrypoint({ inputs.build, inputs.program, &inputs.flow, nullptr }, inputs.types, inputs.strings, request);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
