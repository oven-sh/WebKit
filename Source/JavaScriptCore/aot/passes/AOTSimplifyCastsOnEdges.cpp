/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyCastsInternal.h"

#if ENABLE(AOT)

#include "AOTFlowApply.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct CastCandidate {
    BlockID block;
    uint32_t parameterIndex;
    ValueID parameter;
    ValueID cast;
    bool hasTest;
};

} // anonymous namespace

static std::optional<CastCandidate> findCastCandidate(FunctionPassContext& context, const CFG& cfg, BlockID id)
{
    const Function& function = context.function;
    const Block& block = function.at(id);
    if (block.isHandlerBlock() || block.parameters.isEmpty() || block.values.isEmpty())
        return std::nullopt;

    // In a validating build the test of an assumed type stands before the cast and goes to the edges with it.
    bool hasTest = function.at(block.values[0]).opcode == Opcode::ValidateType && block.values.size() > 1;
    ValueID castID = block.values[hasTest];
    const Value& cast = function.at(castID);
    if (cast.opcode != Opcode::Check && cast.opcode != Opcode::AssumeType)
        return std::nullopt;

    ValueID parameter = cast.operand(0);
    if (hasTest) {
        const Value& test = function.at(block.values[0]);
        if (cast.opcode != Opcode::AssumeType || test.valueOperand() != parameter || test.validatedType() != cast.castType())
            return std::nullopt;
    }
    size_t parameterIndex = block.parameters.find(parameter);
    if (parameterIndex == notFound || parameterIndex >= numberOfSharedExplicitParameters(function, cfg, id))
        return std::nullopt;
    if (context.analyses.useLists().useCount(parameter) != 1u + hasTest)
        return std::nullopt;
    if (!context.types.at(function.at(parameter).type).isJavaScriptValue())
        return std::nullopt;
    return CastCandidate { id, static_cast<uint32_t>(parameterIndex), parameter, castID, hasTest };
}

static bool edgeNeedsCast(const FunctionPassContext& context, const CastCandidate& candidate, Edge edge)
{
    const Function& function = context.function;
    ValueID definition = context.analyses.definitionThroughUpcasts(targetOf(function, edge).arguments[candidate.parameterIndex]);
    return !context.types.at(function.at(definition).type).isSubsetOf(context.types.at(function.at(candidate.cast).type), context.program);
}

static bool canMoveCast(const FunctionPassContext& context, const CFG& cfg, const CastCandidate& candidate)
{
    const Function& function = context.function;
    std::span<const Edge> edges = cfg.incomingEdges(candidate.block);
    bool castHasCode = function.at(candidate.cast).opcode == Opcode::Check;
    size_t numberOfEdgesNeedingCast = 0;
    for (Edge edge : edges) {
        if (edge.isHandlerEdge() || edge.from == candidate.block || function.at(edge.from).terminator.kind != TerminatorKind::Jump)
            return false;
        if (!edgeNeedsCast(context, candidate, edge))
            continue;
        if (castHasCode && function.at(edge.from).handler != function.at(candidate.block).handler)
            return false;
        ++numberOfEdgesNeedingCast;
    }
    if (castHasCode && numberOfEdgesNeedingCast > 1)
        return false;
    return numberOfEdgesNeedingCast < edges.size();
}

static bool moveCast(FunctionPassContext& context, IRBuilder& builder, const CFG& cfg, const CastCandidate& candidate)
{
    Function& function = context.function;
    const Value cast = function.at(candidate.cast);
    builder.setOrigin(cast.origin());
    bool didChangeEdge = false;
    for (Edge edge : cfg.incomingEdges(candidate.block)) {
        ValueID& argument = editTarget(function, edge).arguments[candidate.parameterIndex];
        ValueID passed = context.analyses.definitionThroughUpcasts(argument);
        if (edgeNeedsCast(context, candidate, edge)) {
            if (candidate.hasTest)
                builder.insertBefore(pointBeforeTerminatorOf(function, edge.from), makeValidateType(argument, cast.castType()));
            Value castOnEdge = cast;
            castOnEdge.setOperand(0, argument);
            castOnEdge.setExpected({ });
            passed = builder.insertBefore(pointBeforeTerminatorOf(function, edge.from), WTF::move(castOnEdge));
        }
        didChangeEdge |= passed != argument;
        argument = passed;
    }
    return didChangeEdge;
}

size_t moveCastsToEdges(FunctionPassContext& context)
{
    const CFG& cfg = context.analyses.cfg();
    Vector<CastCandidate, 4> candidates;
    for (BlockID block : cfg.reversePostOrder()) {
        if (block == cfg.root())
            continue;
        if (auto candidate = findCastCandidate(context, cfg, block))
            candidates.append(*candidate);
    }

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    Vector<ValueID, 4> movedParameters;
    bool didChangeEdge = false;
    for (const CastCandidate& candidate : candidates) {
        if (!canMoveCast(context, cfg, candidate))
            continue;
        didChangeEdge |= moveCast(context, builder, cfg, candidate);
        movedParameters.append(candidate.parameter);
    }
    if (movedParameters.isEmpty())
        return 0;

    builder.applyInsertions();
    // The arguments of the edges were written past the builder.
    context.analyses.didChangeValues();
    // A join may widen, so the cast at the head stays until the type of its parameter is inside it.
    TypePropagationOutcome outcome = narrowValueTypes(TypePropagationInputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo }, movedParameters.span());
    if (!didChangeEdge && !outcome.didChange())
        return 0;
    return movedParameters.size();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
