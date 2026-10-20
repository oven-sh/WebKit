/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateBoundFunctions.h"

#if ENABLE(AOT)

#include "AOTCreateEntrypointsInternal.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowResults.h"
#include "AOTResolveCallsCallees.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTTypeArena.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool canBeBound(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Arrow:
    case FunctionKind::Method:
        return true;
    case FunctionKind::Plain:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OptionSet<FunctionFlags> flagsOfUnboundTargets { FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasHiddenRest, FunctionFlags::HasArgumentCount, FunctionFlags::TakesCallee };

bool canCreateBoundFunction(const Program& program, FunctionID targetID)
{
    const Function& target = program.at(targetID);
    return canBeBound(target.kind) && !target.hasAny(flagsOfUnboundTargets) && mayBeCalledDirectly(program, targetID);
}

static Function createBoundFunctionWithoutBody(FunctionID targetID, uint32_t numberOfBoundArguments, const Function& target, const Function& declared)
{
    Function bound = createDerivedHeader(target, targetID, FunctionRole::BoundFunction, numberOfBoundArguments);
    bound.length = target.length > numberOfBoundArguments ? target.length - numberOfBoundArguments : 0;
    bound.flags = target.flags & OptionSet<FunctionFlags> { FunctionFlags::IsAsync, FunctionFlags::IsChecked };
    bound.results = declared.results;
    bound.creator = CreatorRecord::BoundFunction {
        .target = targetID,
        .numberOfBoundArguments = numberOfBoundArguments,
        .numberOfCapturedArguments = std::min<uint32_t>(numberOfBoundArguments, declared.numberOfPositionalParameters()),
        .capturesTargetClosure = true,
    };
    return bound;
}

Function buildBoundFunction(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    return createBoundFunction(inputs.program, inputs.flow, inputs.types, inputs.build, key.target, key.parameter);
}

Function createBoundFunction(const Program& program, const FlowResults& flow, TypeArena& types, BuildContext& build, FunctionID targetID, uint32_t numberOfBoundArguments)
{
    const Function& target = program.at(targetID);
    RELEASE_ASSERT(!target.hasAny(flagsOfUnboundTargets));

    // Unknown code can call the closure, so it declares the types as written, not the types narrowed to the known callers of the target.
    FunctionID declaredID = versionToEnter(program, flow, targetID, CalleeSetID());
    const Function& declared = program.at(declaredID);

    Function bound = createBoundFunctionWithoutBody(targetID, numberOfBoundArguments, target, declared);
    uint32_t numberOfCapturedArguments = bound.creator.as<CreatorRecord::BoundFunction>().numberOfCapturedArguments;

    IRBuilder builder({ build, program, &flow, nullptr }, bound, types);
    builder.setOrigin({ target.position, 0 });
    BlockID block = builder.addBlock();
    auto addCapture = [&](const DeclaredType& capture) {
        bound.captures.append(capture);
        return builder.addParameter(block, capture.type);
    };

    Vector<ValueID, 8> targetCaptures;
    for (const DeclaredType& capture : declared.captures)
        targetCaptures.append(addCapture(capture));
    ValueID boundThis;
    if (target.has(FunctionFlags::HasThis))
        boundThis = addCapture(declared.thisType);
    Vector<ValueID, 8> arguments;
    for (uint32_t i = 0; i < numberOfCapturedArguments; ++i)
        arguments.append(addCapture(declared.parameters[i]));
    // The body does not use the target closure. The run time reads it for instanceof.
    addCapture(DeclaredType { .type = TypeTable::function });
    for (size_t i = numberOfCapturedArguments; i < declared.parameters.size(); ++i) {
        bound.parameters.append(declared.parameters[i]);
        arguments.append(builder.addParameter(block, declared.parameters[i].type));
    }

    builder.moveToEndOf(block);
    ValueID result = appendDirectCall(builder, {
        .callee = declaredID,
        .captures = targetCaptures.span(),
        .thisValue = boundThis,
        .arguments = arguments.span(),
        .resultCheck = resultCheckOf(flow, declaredID),
    });
    emitReturnOfEntrypointCall(builder, flow, declaredID, result);
    return bound;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
