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
#include "AOTFlowResults.h"
#include "AOTFlowUnseenCode.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTStringArena.h"
#include "AOTTypeArena.h"
#include <algorithm>

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

EntrypointVariants entrypointVariantsOf(const Function& target)
{
    if (!target.has(FunctionFlags::HasNewTarget))
        return EntrypointVariants::WithoutNewTarget;

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

static Function createEntryWithoutBody(TypeArena& types, const Function& target, const GenericEntrypointRequest& request)
{
    const DeclaredType anything { .type = TypeTable::anything };
    bool isConstructVariant = request.takesNewTarget && entrypointVariantsOf(target) == EntrypointVariants::Both;
    ArgumentPassing passing = argumentPassingOf(target);

    Function entry = createDerivedHeader(target, request.target, FunctionRole::GenericEntrypoint, isConstructVariant ? constructEntrypointParameter : callEntrypointParameter);
    entry.flags.add(FunctionFlags::HasThis);
    entry.thisType = anything;
    entry.captures.append(DeclaredType { .type = types.intern(Type::forFunction(request.target)) });
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
        .group = { },
        .arguments = passing,
        .takesNewTarget = request.takesNewTarget,
        .constructEntrypoint = request.constructEntrypoint,
        .groupEntrypoint = request.groupEntrypoint,
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

static bool canUnseenJavaScriptRun(const FlowResults& flow)
{
    return !flow.hasResults() || std::ranges::any_of(flow.unseenCode(), [](const UnseenCodeRecord& record) {
        return admitsUnknownJavaScript(record.cause);
    });
}

// No code that flow follows comes here. Code that nobody has seen may find the closure on an object that native code hung where such code can name it, and is refused like everything else flow did not see.
static void emitEntryThatNothingEnters(IRBuilder& builder, const FlowResults& flow, StringArena& strings)
{
    if (canUnseenJavaScriptRun(flow)) {
        Value error = IRBuilder::make(Opcode::ThrowNewError);
        error.setErrorKind(ErrorKind::Error);
        error.setMessage(strings.intern(StringView("Cannot call this function: nothing in the program calls it, so it was not compiled into this executable. Call it in the program itself, or compile with --admit=runtime-code (bun compile differs from bun run here: https://bun.com/docs/bundler/aot-compatibility)"_s)));
        builder.append(WTF::move(error));
    }
    builder.emitUnreachable();
}

// A body is removed once nothing can enter it. The closure may still be created, and its identity is the entrypoint.
static bool canSeenCodeEnter(const Program& program, const FlowResults& flow, const GenericEntrypointRequest& request)
{
    return request.callers == GenericCallers::Unknown && program.at(versionToEnter(program, flow, request.target, CalleeSetID())).hasBody();
}

static void appendTargetCall(IRBuilder& builder, const FlowResults& flow, FunctionID targetID, const EntryArguments& arguments)
{
    FunctionID callee = versionToEnter(builder.context().program, flow, targetID, CalleeSetID());
    Vector<ValueID, 8> captures = appendCaptures(builder, targetID, arguments.closure);
    ValueID result = appendDirectCall(builder, {
        .callee = callee,
        .captures = captures.span(),
        .thisValue = arguments.thisValue,
        .calleeValue = arguments.closure,
        .newTarget = arguments.newTarget,
        .arguments = arguments.fixedArguments.span(),
        .restArray = arguments.restArray,
        .argumentCount = arguments.argumentCount,
        .hiddenRest = arguments.hiddenRest,
        .resultCheck = resultCheckOf(flow, callee),
    });
    emitReturnOfEntrypointCall(builder, flow, callee, result, EntrypointResultTest::MadeUnlessCallerTests);
}

Function createGenericEntrypoint(const AnalysisContext& context, TypeArena& types, StringArena& strings, const GenericEntrypointRequest& request)
{
    RELEASE_ASSERT(context.flow);
    RELEASE_ASSERT(request.callers == GenericCallers::Unknown || !request.isEnteredByHost);

    const Function& target = context.program.at(request.target);
    Function entry = createEntryWithoutBody(types, target, request);
    IRBuilder builder(context, entry, types);
    builder.setOrigin({ target.position, 0 });
    EntryArguments arguments = addEntryArguments(builder, target);
    if (!canSeenCodeEnter(context.program, *context.flow, request)) {
        emitEntryThatNothingEnters(builder, *context.flow, strings);
        return entry;
    }
    appendTargetCall(builder, *context.flow, request.target, arguments);
    if (context.build.validates())
        checkCallByValueCoversEntrypoint(context.program, request.target, entry, context.build.diagnostics());
    return entry;
}

Function buildGenericEntrypoint(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    const Function& target = inputs.program.at(key.target);
    EntrypointVariants variants = entrypointVariantsOf(target);
    RELEASE_ASSERT(canBeCalledByValueOf(target.role));

    GenericEntrypointRequest request { .target = key.target };
    if (key.parameter == constructEntrypointParameter) {
        RELEASE_ASSERT(variants == EntrypointVariants::Both);
        request.takesNewTarget = true;
    } else {
        RELEASE_ASSERT(key.parameter == callEntrypointParameter);
        request.takesNewTarget = variants == EntrypointVariants::WithNewTarget;
        if (variants == EntrypointVariants::Both)
            request.constructEntrypoint = inputs.derivedFunctions.ensure(derivedFunctionKey(FunctionRole::GenericEntrypoint, key.target, constructEntrypointParameter));
    }
    return createGenericEntrypoint({ inputs.build, inputs.program, &inputs.flow, nullptr }, inputs.types, inputs.strings, request);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
