/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RequestEdits {
    explicit RequestEdits(FunctionPassContext& context)
        : builder(context.analyses.context(), context.function, context.types, &context.analyses)
    {
    }

    IRBuilder builder;
    std::optional<ValueSubstitution> substitution;
    BitVector erasedValues;
    Vector<BlockID, 8> blocksWithErasedValues;
};

} // anonymous namespace

static StatisticID statisticOf(CastResolution cast)
{
    switch (cast) {
    case CastResolution::Nothing:
        return StatisticID::RequestsResolvedToNothing;
    case CastResolution::Upcast:
        return StatisticID::RequestsResolvedToUpcast;
    case CastResolution::Check:
        return StatisticID::RequestsResolvedToCheck;
    case CastResolution::Assume:
        return StatisticID::RequestsResolvedToClaim;
    case CastResolution::Unresolved:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void replaceWithOperand(FunctionPassContext& context, RequestEdits& edits, BlockID block, ValueID request)
{
    if (!edits.substitution)
        edits.substitution.emplace(context.function);
    edits.substitution->replace(request, context.function.at(request).valueOperand());
    edits.erasedValues.set(request.index());
    if (edits.blocksWithErasedValues.isEmpty() || edits.blocksWithErasedValues.last() != block)
        edits.blocksWithErasedValues.append(block);
}

static void applyEdits(FunctionPassContext& context, RequestEdits& edits)
{
    Function& function = context.function;
    if (!edits.substitution)
        return;

    for (BlockID block : edits.blocksWithErasedValues) {
        const BlockValues& values = function.at(block).values;
        BitVector indexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (edits.erasedValues.get(values[index].index()))
                indexes.set(index);
        }
        edits.builder.eraseValues(block, indexes);
    }
    edits.substitution->apply(function, &context.analyses);
    compactFunction(function);
    context.analyses.didChangeControlFlow();
}

static bool isResolvedByTypeValues(const Value& value)
{
    return value.opcode == Opcode::Convert && !value.declared().has(PositionFlags::IsInt32Promise);
}

static Vector<ProgramPoint, 16> collectRequests(const Function& function)
{
    Vector<ProgramPoint, 16> requests;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (isResolvedByTypeValues(function.at(values[index])))
                requests.append(ProgramPoint { BlockID(blockIndex), index });
        }
    }
    return requests;
}

static void resolveRequests(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<ProgramPoint, 16> requests = collectRequests(function);
    if (requests.isEmpty())
        return;

    TypingContext typing { context.program, function, context.types };
    Statistics& statistics = context.build.statistics();
    RequestEdits edits(context);
    for (ProgramPoint point : requests) {
        ValueID id = function.at(point.block).values[point.index];
        const Value& request = function.at(id);
        ConvertResolution resolution = resolveConvert(typing, typing.typeOf(request.valueOperand()), request.declared());
        statistics.count(statisticOf(resolution.cast));
        if (!becomeResolved(function, id, resolution))
            replaceWithOperand(context, edits, point.block, id);
    }
    applyEdits(context, edits);
}

static void resolveTailCasts(FunctionPassContext& context)
{
    TypingContext typing { context.program, context.function, context.types };
    for (Block& block : context.function.editAllBlocks()) {
        Terminator& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        Type source = typing.typeOf(terminator.callOperand());
        Vector<TailCast, 4> casts;
        for (TailCast cast : terminator.casts()) {
            cast.resolution = resolveConvert(typing, source, cast.declared).cast;
            source = typeOfConvert(typing, source, source, cast.declared);
            casts.append(cast);
        }
        terminator.setCasts(casts.span());
    }
}

PassOutcome typeValues(FunctionPassContext& context)
{
    if (!context.function.hasBody())
        return PassOutcome::Unchanged;

    TypePropagationInputs inputs { context.program, context.id, context.function, context.analyses, context.types, context.build, &context.typingMemo };
    if (hasTypedValues(context.function))
        finishTypedValues(inputs);
    else
        typeValuesFromNever(inputs);
    resolveRequests(context);
    resolveTailCasts(context);
    static_assert(OperandIndex::Convert::value == OperandIndex::Upcast::value && OperandIndex::Convert::value == OperandIndex::Check::value);
    context.analyses.invalidate(valueAnalyses - AnalysisSet { FunctionAnalysis::UseLists, FunctionAnalysis::Liveness });
    finishTypedValues(inputs);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
