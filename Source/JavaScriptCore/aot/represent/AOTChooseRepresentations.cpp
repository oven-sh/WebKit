/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTConversions.h"
#include "AOTRepresentationWebs.h"
#include <array>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConversionWalk {
    const RepresentationInputs& inputs;
    Function& function;
    ConversionInserter& inserter;
};

} // anonymous namespace

static void uncoverEntryBlock(FunctionPassContext& pass, IRBuilder& builder)
{
    const Block& entry = pass.function.at(entryBlock);
    if (!entry.handler)
        return;

    builder.setOrigin(entry.terminator.origin());
    builder.splitBlock(ProgramPoint::atHeadOf(entryBlock));
    builder.removeHandler(entryBlock);
    pass.analyses.didChangeControlFlow();
}

static void writeRepresentations(FunctionPassContext& pass, const RepresentationInputs& inputs)
{
    RepresentationWebs webs(inputs, pass.analyses);
    webs.build();
    webs.choose();

    Function& function = pass.function;
    for (BlockID id : pass.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        for (ValueID parameter : block.parameters)
            function.at(parameter).representation = webs.representationOf(parameter);
        for (ValueID value : block.values)
            function.at(value).representation = webs.representationOf(value);
    }
}

static bool needsConversion(const Function& function, ValueID value, Representation wanted)
{
    return hasRegister(wanted) && function.at(value).representation != wanted;
}

static void convertValueOperands(const ConversionWalk& walk, BlockID block)
{
    Function& function = walk.function;
    const BlockValues& values = function.at(block).values;
    for (uint32_t index = 0; index < values.size(); ++index) {
        ValueID user = values[index];
        uint32_t numberOfOperands = function.at(user).numberOfOperands();
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            ValueID operand = function.at(user).operand(operandIndex);
            if (!operand)
                continue;
            Representation wanted = wantedRepresentationOf(walk.inputs, function.at(user), operandIndex);
            if (needsConversion(function, operand, wanted))
                walk.inserter.convertOperand(user, operandIndex, wanted, { block, index });
        }
    }
}

static void convertTerminatorOperands(const ConversionWalk& walk, BlockID block)
{
    const Terminator& terminator = walk.function.at(block).terminator;
    for (uint32_t operandIndex = 0; operandIndex < terminator.numberOfOperands(); ++operandIndex) {
        ValueID operand = terminator.operand(operandIndex);
        if (!operand)
            continue;
        Representation wanted = wantedRepresentationOf(walk.inputs, terminator, operandIndex);
        if (needsConversion(walk.function, operand, wanted))
            walk.inserter.convertTerminatorOperand(block, operandIndex, wanted);
    }
}

static void convertEdgeResults(const ConversionWalk& walk, Edge edge)
{
    const Function& function = walk.function;
    const CompactList<ValueID>& parameters = function.at(targetOf(function, edge).block).parameters;
    uint32_t firstResult = firstEdgeResultParameter(function, edge.from, edge.targetIndex);
    uint32_t numberOfResults = numberOfEdgeResults(function, edge.from, edge.targetIndex);
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        uint32_t parameterIndex = firstResult + resultIndex;
        Representation passed = edgeResultRepresentationOf(walk.inputs, { edge.from, edge.targetIndex, resultIndex });
        Representation wanted = function.at(parameters[parameterIndex]).representation;
        if (hasRegister(wanted) && passed != wanted)
            walk.inserter.convertEdgeResult(edge, parameterIndex, passed, wanted);
    }
}

static void convertEdgeArguments(const ConversionWalk& walk, Edge edge)
{
    const Function& function = walk.function;
    const Target& target = targetOf(function, edge);
    const CompactList<ValueID>& parameters = function.at(target.block).parameters;
    for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
        ValueID argument = target.arguments[argumentIndex];
        if (!argument)
            continue;
        Representation wanted = function.at(parameters[argumentIndex]).representation;
        if (needsConversion(function, argument, wanted))
            walk.inserter.convertEdgeArgument(edge, argumentIndex, wanted);
    }
}

static void insertConversions(const ConversionWalk& walk, std::span<const BlockID> reversePostOrder)
{
    for (BlockID block : reversePostOrder) {
        convertValueOperands(walk, block);
        convertTerminatorOperands(walk, block);
        forEachOutgoingEdge(walk.function, block, [&](Edge edge, const Target&) {
            if (!edge.isHandlerEdge())
                convertEdgeResults(walk, edge);
            convertEdgeArguments(walk, edge);
        });
    }
    walk.inserter.finish();
}

static void countRepresentations(FunctionPassContext& pass, uint32_t numberOfConversions)
{
    const Function& function = pass.function;
    std::array<uint64_t, numberOfEnumerators<Representation>> numberOfValuesIn { };
    auto countValue = [&](ValueID id) {
        ++numberOfValuesIn[std::to_underlying(function.at(id).representation)];
    };
    for (const Block& block : function.blocks) {
        for (ValueID parameter : block.parameters)
            countValue(parameter);
        for (ValueID value : block.values)
            countValue(value);
    }

    auto count = [&](StatisticID statistic, uint64_t amount) {
        if (amount)
            pass.build.statistics().count(statistic, amount);
    };
    count(StatisticID::ConversionsInserted, numberOfConversions);
    count(StatisticID::ValuesInInt32, numberOfValuesIn[std::to_underlying(Representation::Int32)]);
    count(StatisticID::ValuesInInt64, numberOfValuesIn[std::to_underlying(Representation::Int64)]);
    count(StatisticID::ValuesInDouble, numberOfValuesIn[std::to_underlying(Representation::Double)]);
    count(StatisticID::ValuesBoxed, numberOfValuesIn[std::to_underlying(Representation::JSValue)]);
    count(StatisticID::ReferencesCompressed, numberOfValuesIn[std::to_underlying(Representation::Cell32)]);
}

PassOutcome chooseRepresentations(FunctionPassContext& pass)
{
    RELEASE_ASSERT(pass.inputs.placement && pass.inputs.callingConventions && !pass.function.representationsAreChosen);
    Function& function = pass.function;
    size_t numberOfBlocksBefore = function.blocks.size();

    IRBuilder builder(pass.analyses.context(), function, pass.types);
    uncoverEntryBlock(pass, builder);

    RepresentationInputs inputs { pass.program, pass.id, function, *pass.inputs.placement, *pass.inputs.callingConventions };
    writeRepresentations(pass, inputs);

    ConversionInserter inserter(pass, builder);
    insertConversions({ inputs, function, inserter }, pass.analyses.cfg().reversePostOrder());

    if (function.blocks.size() != numberOfBlocksBefore)
        pass.analyses.didChangeControlFlow();
    else
        pass.analyses.didChangeValues();

    function.representationsAreChosen = true;
    countRepresentations(pass, inserter.numberOfConversions());

    Vector<InternalError> errors;
    checkRepresentations(inputs, errors);
    if (!errors.isEmpty())
        pass.build.diagnostics().append(WTF::move(errors));
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
