/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCreateBoundFunctions.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCreateEntrypointsInternal.h"
#include "AOTDerivedFunctions.h"
#include "AOTFlowResults.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static bool canBeBound(FunctionKind kind)
{
    switch (kind) {
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Plain:
        return true;
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

// instanceof asks the target of a bound function. Closures that capture nothing, keep their prototype link and never get a `prototype` or a Symbol.hasInstance of their own all give one answer,
// so the run time asks the closure that it keeps for the target.
static bool answersInstanceOfWithAnyClosure(const Program& program, const FlowResults& flow, FunctionID targetID)
{
    // Each closure of a function that constructs has a prototype object of its own, which `new` and instanceof on the bound function go by.
    if (!program.at(targetID).captures.isEmpty() || program.at(targetID).constructorForm() != ConstructorForm::NotConstructor)
        return false;
    std::array<PropertyKeyID, 2> keysOfInstanceOf { program.keyOf(BuiltinNames::prototype), program.keyOf(program.builtins().nameOf(WellKnownSymbol::HasInstance)) };
    return flow.areClosuresAsCreated(targetID, keysOfInstanceOf);
}

static Function createBoundFunctionWithoutBody(FunctionID targetID, uint32_t numberOfBoundArguments, const Function& target, bool capturesTargetClosure)
{
    Function bound = createDerivedHeader(target, targetID, FunctionRole::BoundFunction, numberOfBoundArguments);
    bound.length = target.length > numberOfBoundArguments ? target.length - numberOfBoundArguments : 0;
    bound.flags = target.flags & OptionSet<FunctionFlags> { FunctionFlags::IsAsync, FunctionFlags::IsChecked };
    bound.results = target.results;
    bound.creator = CreatorRecord::BoundFunction {
        .target = targetID,
        .numberOfBoundArguments = numberOfBoundArguments,
        .numberOfCapturedArguments = std::min<uint32_t>(numberOfBoundArguments, target.numberOfPositionalParameters()),
        .capturesTargetClosure = capturesTargetClosure,
    };
    return bound;
}

static ValueID addEntryParameter(IRBuilder& builder, BlockID block, const DeclaredType& declared)
{
    Function& function = builder.function();
    TypeID proven = TypingContext { builder.context().program, function, builder.types() }.typeIDOf(declared);
    ValueID parameter = builder.addParameter(block, proven);
    if (declared.type != proven)
        function.edit(parameter, ValueEdit::Type).setExpected(declared.type);
    return parameter;
}

Function buildBoundFunction(const DerivedFunctionKey& key, DerivedFunctionInputs& inputs)
{
    return createBoundFunction(inputs.program, inputs.flow, inputs.types, inputs.build, key.target, key.parameter);
}

Function createBoundFunction(const Program& program, const FlowResults& flow, TypeArena& types, BuildContext& build, FunctionID targetID, uint32_t numberOfBoundArguments)
{
    const Function& target = program.at(targetID);
    RELEASE_ASSERT(!target.hasAny(flagsOfUnboundTargets));

    Function bound = createBoundFunctionWithoutBody(targetID, numberOfBoundArguments, target, !answersInstanceOfWithAnyClosure(program, flow, targetID));
    CreatorRecord::BoundFunction record = bound.creator.as<CreatorRecord::BoundFunction>();

    IRBuilder builder({ build, program, &flow, nullptr }, bound, types);
    builder.setOrigin({ target.position, 0 });
    BlockID block = builder.addBlock();
    auto addCapture = [&](const DeclaredType& capture) {
        bound.captures.append(capture);
        return addEntryParameter(builder, block, capture);
    };

    Vector<ValueID, 8> targetCaptures;
    for (const DeclaredType& capture : target.captures)
        targetCaptures.append(addCapture(capture));
    ValueID boundThis;
    if (target.has(FunctionFlags::HasThis))
        boundThis = addCapture(target.thisType);
    Vector<ValueID, 8> arguments;
    for (uint32_t i = 0; i < record.numberOfCapturedArguments; ++i)
        arguments.append(addCapture(target.parameters[i]));
    // The body does not use the target closure. The run time reads it for instanceof.
    if (record.capturesTargetClosure)
        addCapture(DeclaredType { .type = TypeTable::function });
    for (size_t i = record.numberOfCapturedArguments; i < target.parameters.size(); ++i) {
        bound.parameters.append(target.parameters[i]);
        arguments.append(addEntryParameter(builder, block, target.parameters[i]));
    }

    builder.moveToEndOf(block);
    ValueID result = appendDirectCall(builder, {
        .callee = targetID,
        .captures = targetCaptures.span(),
        .thisValue = boundThis,
        .arguments = arguments.span(),
    });
    emitReturnOfEntrypointCall(builder, result);
    return bound;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
