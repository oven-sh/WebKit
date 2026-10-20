/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversions.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTFunctionAnalyses.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static_assert(CodeShape::Unchosen < CodeShape::Compact && CodeShape::Compact < CodeShape::Balanced && CodeShape::Balanced < CodeShape::Fast);

static constexpr CodeShape moreCompactOf(CodeShape a, CodeShape b)
{
    return a < b ? a : b;
}

ConversionInserter::ConversionInserter(FunctionPassContext& pass, IRBuilder& builder)
    : m_pass(pass)
    , m_builder(builder)
    , m_useLists(pass.analyses.useLists())
    , m_sharedConversionsOf(pass.function.values.size())
{
    RELEASE_ASSERT(&builder.function() == &pass.function);
}

void ConversionInserter::convertOperand(ValueID user, uint32_t operandIndex, Representation wanted, ProgramPoint pointOfUser)
{
    Function& function = m_pass.function;
    m_builder.setOrigin(function.at(user).origin());
    ValueID converted = convert(function.at(user).operand(operandIndex), wanted, pointOfUser, SharingScope::Block);
    function.at(user).setOperand(operandIndex, converted);
}

void ConversionInserter::convertTerminatorOperand(BlockID block, uint32_t operandIndex, Representation wanted)
{
    Function& function = m_pass.function;
    Terminator& terminator = function.at(block).terminator;
    RELEASE_ASSERT(terminator.kind != TerminatorKind::TestAndBranch && terminator.kind != TerminatorKind::ReturnCall);
    m_builder.setOrigin(terminator.origin());
    ValueID converted = convert(terminator.operand(operandIndex), wanted, pointBeforeTerminatorOf(function, block), SharingScope::Block);
    terminator.setOperand(operandIndex, converted);
}

void ConversionInserter::convertEdgeArgument(Edge edge, uint32_t argumentIndex, Representation wanted)
{
    if (edge.isHandlerEdge()) {
        convertHandlerArgument(edge.from, argumentIndex, wanted);
        return;
    }
    if (m_pass.function.at(edge.from).terminator.kind == TerminatorKind::Jump) {
        convertJumpArgument(edge.from, argumentIndex, wanted);
        return;
    }
    convertOnSplitEdge(edge, EdgeConversion { argumentIndex, std::nullopt, wanted });
}

void ConversionInserter::convertEdgeResult(Edge edge, uint32_t parameterIndex, Representation passed, Representation wanted)
{
    RELEASE_ASSERT(!edge.isHandlerEdge());
    convertOnSplitEdge(edge, EdgeConversion { parameterIndex, passed, wanted });
}

void ConversionInserter::finish()
{
    m_builder.applyInsertions();
    if (m_splitEdges.isEmpty())
        return;

    size_t numberOfBlocksBeforeSplitting = m_pass.function.blocks.size();
    for (SplitEdge& split : m_splitEdges) {
        RELEASE_ASSERT(targetOf(m_pass.function, split.edge).block.index() < numberOfBlocksBeforeSplitting);
        split.added = splitEdge(split);
    }
    for (const SplitEdge& split : m_splitEdges) {
        if (m_pass.function.at(split.added).terminator.kind == TerminatorKind::Unreachable)
            continue;
        for (const EdgeConversion& conversion : split.conversions)
            convertJumpArgument(split.added, conversion.parameterIndex, conversion.wanted);
    }
    m_splitEdges.clear();
    m_builder.applyInsertions();
}

ValueID ConversionInserter::convert(ValueID source, Representation wanted, ProgramPoint before, SharingScope scope)
{
    Function& function = m_pass.function;
    Representation from = function.at(source).representation;
    if (from == wanted)
        return source;
    if (ValueID shared = findSharedConversion(scope, before.block, source, wanted))
        return shared;

    std::optional<ConversionPath> path = findConversion(m_pass.program.typeOf(function, source), from, wanted);
    if (!path) {
        reportUnsupportedConversion(source, before.block);
        return source;
    }

    ValueID current = source;
    for (ConversionStep step : *path) {
        if (ValueID shared = findSharedConversion(scope, before.block, source, step.to)) {
            current = shared;
            continue;
        }
        current = m_builder.insertBefore(before, IRBuilder::make(step.opcode, { current }));
        function.at(current).representation = step.to;
        shareConversion(scope, before.block, source, step.to, current);
        ++m_numberOfConversions;
    }
    return current;
}

ValueID ConversionInserter::findSharedConversion(SharingScope scope, BlockID block, ValueID source, Representation wanted) const
{
    if (source.index() >= m_sharedConversionsOf.size())
        return { };

    const SharedConversionLists& lists = m_sharedConversionsOf[source.index()];
    if (ValueID converted = findConversionInList(lists.lastAfterDefinition, wanted))
        return converted;

    switch (scope) {
    case SharingScope::Block:
        return lists.block == block ? findConversionInList(lists.lastInBlock, wanted) : ValueID();
    case SharingScope::Function:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ValueID ConversionInserter::findConversionInList(std::optional<uint32_t> last, Representation wanted) const
{
    for (std::optional<uint32_t> index = last; index; index = m_sharedConversions[*index].previous) {
        const SharedConversion& conversion = m_sharedConversions[*index];
        if (conversion.to == wanted)
            return conversion.converted;
    }
    return { };
}

void ConversionInserter::shareConversion(SharingScope scope, BlockID block, ValueID source, Representation to, ValueID converted)
{
    if (source.index() >= m_sharedConversionsOf.size())
        m_sharedConversionsOf.grow(m_pass.function.values.size());

    SharedConversionLists& lists = m_sharedConversionsOf[source.index()];
    uint32_t index = static_cast<uint32_t>(m_sharedConversions.size());
    switch (scope) {
    case SharingScope::Block:
        if (lists.block != block)
            lists.lastInBlock = std::nullopt;
        lists.block = block;
        m_sharedConversions.append(SharedConversion { to, converted, std::exchange(lists.lastInBlock, index) });
        return;
    case SharingScope::Function:
        m_sharedConversions.append(SharedConversion { to, converted, std::exchange(lists.lastAfterDefinition, index) });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ConversionInserter::convertJumpArgument(BlockID block, uint32_t argumentIndex, Representation wanted)
{
    Function& function = m_pass.function;
    Terminator& jump = function.at(block).terminator;
    m_builder.setOrigin(jump.origin());
    ValueID converted = convert(jump.next().arguments[argumentIndex], wanted, pointBeforeTerminatorOf(function, block), SharingScope::Block);
    jump.next().arguments[argumentIndex] = converted;
}

void ConversionInserter::convertHandlerArgument(BlockID covered, uint32_t argumentIndex, Representation wanted)
{
    Function& function = m_pass.function;
    Target& handler = *function.at(covered).handler;
    ValueID source = handler.arguments[argumentIndex];

    ProgramPoint afterDefinition = m_useLists.pointAfter(source);
    RELEASE_ASSERT(afterDefinition.block != covered);

    m_builder.setOrigin(function.at(source).origin());
    handler.arguments[argumentIndex] = convert(source, wanted, afterDefinition, SharingScope::Function);
}

void ConversionInserter::convertOnSplitEdge(Edge edge, const EdgeConversion& conversion)
{
    if (m_splitEdges.isEmpty() || m_splitEdges.last().edge != edge)
        m_splitEdges.append(SplitEdge { edge, { }, { } });
    m_splitEdges.last().conversions.append(conversion);
}

BlockID ConversionInserter::splitEdge(const SplitEdge& split)
{
    Function& function = m_pass.function;
    BlockID added = m_builder.splitEdge(split.edge);

    const Target& next = function.at(added).terminator.next();
    function.at(added).shape = moreCompactOf(function.at(split.edge.from).shape, function.at(next.block).shape);

    const CompactList<ValueID>& parameters = function.at(next.block).parameters;
    size_t firstResult = next.arguments.size() - function.at(added).parameters.size();
    for (size_t index = firstResult; index < next.arguments.size(); ++index)
        function.at(next.arguments[index]).representation = function.at(parameters[index]).representation;

    for (const EdgeConversion& conversion : split.conversions) {
        if (conversion.passed)
            function.at(next.arguments[conversion.parameterIndex]).representation = *conversion.passed;
    }

    // An edge that would pass a value of type never is not taken, and no code may follow such a value.
    bool passesNever = std::ranges::any_of(function.at(added).parameters, [&](ValueID parameter) {
        return function.at(parameter).type == TypeTable::never;
    });
    if (passesNever) {
        Terminator& terminator = function.at(added).terminator;
        Origin origin = terminator.origin();
        terminator.become(TerminatorKind::Unreachable);
        terminator.setOrigin(origin);
    }
    return added;
}

void ConversionInserter::reportUnsupportedConversion(ValueID source, BlockID block)
{
    m_pass.build.diagnostics().add(InternalError { .kind = InternalErrorKind::UnsupportedConversion, .function = m_pass.id, .block = block, .value = source });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
