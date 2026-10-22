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
#include "AOTIRPrinter.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <utility>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(CodeShape::Unchosen < CodeShape::Compact && CodeShape::Compact < CodeShape::Balanced && CodeShape::Balanced < CodeShape::Fast);

static constexpr CodeShape moreCompactOf(CodeShape a, CodeShape b)
{
    return a < b ? a : b;
}

static ValueID assumeTypeOf(IRBuilder& builder, ValueID typed, ValueID sameValue, ProgramPoint before)
{
    Function& function = builder.function();
    TypeID type = function.at(typed).type;
    if (function.at(sameValue).type == type)
        return sameValue;

    Representation representation = function.at(sameValue).representation;
    Value cast = IRBuilder::make(Opcode::AssumeType, { sameValue });
    cast.setCastType(type);
    ValueID result = builder.insertBefore(before, WTF::move(cast));
    function.edit(result).representation = representation;
    return result;
}

static std::optional<Value> makeOnlyValueOf(const Type& type)
{
    if (type.isInteger() && type.integers.isSingleValue()) {
        Value constant = IRBuilder::make(Opcode::Integer);
        constant.setInteger(type.integers.minimum);
        return constant;
    }
    if (type.isBoolean() && type.isSingleValue()) {
        Value constant = IRBuilder::make(Opcode::Boolean);
        constant.setBoolean(type.primitives & Primitive::True);
        return constant;
    }
    return std::nullopt;
}

ConversionInserter::ConversionInserter(FunctionPassContext& pass, IRBuilder& builder)
    : m_pass(pass)
    , m_builder(builder)
    , m_useLists(pass.analyses.useLists())
    , m_sharedConversionsOf(pass.function.values.size())
    , m_numberOfValuesAtStart(pass.function.values.size())
{
    RELEASE_ASSERT(&builder.function() == &pass.function);
}

void ConversionInserter::convertOperand(ValueID user, uint32_t operandIndex, Representation wanted, ProgramPoint pointOfUser)
{
    Function& function = m_pass.function;
    m_builder.setOrigin(function.at(user).origin());
    ValueID converted = convert(function.at(user).operand(operandIndex), wanted, pointOfUser, SharingScope::Block);
    function.edit(user).setOperand(operandIndex, converted);
}

void ConversionInserter::convertTerminatorOperand(BlockID block, uint32_t operandIndex, Representation wanted)
{
    Function& function = m_pass.function;
    Terminator& terminator = function.edit(block).terminator;
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

    m_hasSplitEdges = true;
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

ValueID ConversionInserter::onlyValueArrivingAt(ValueID parameter) const
{
    const Function& function = m_pass.function;
    if (m_hasSplitEdges || parameter.index() >= m_numberOfValuesAtStart || function.at(parameter).opcode != Opcode::Parameter)
        return { };
    ValueLocation location = m_useLists.locationOf(parameter);
    std::span<const Edge> incomingEdges = m_pass.analyses.cfg().incomingEdges(location.block);
    if (location.block == entryBlock || incomingEdges.size() != 1 || incomingEdges[0].isHandlerEdge())
        return { };

    IncomingValue incoming = incomingValueOf(function, incomingEdges[0], location.index);
    if (incoming.kind == IncomingValue::Kind::Argument)
        return incoming.argument;
    EdgeResult result = edgeResultAt(function, incoming.result);
    return result.kind == EdgeResultKind::NarrowedOperand ? result.subject : ValueID();
}

ValueID ConversionInserter::findSameValueMadeBefore(ValueID source, Representation wanted) const
{
    const Function& function = m_pass.function;
    ValueID value = source;
    while (true) {
        if (isCastOrConversion(function.at(value).opcode))
            value = function.at(value).operand(0);
        else if (wanted == Representation::Cell32)
            return { };
        else
            value = onlyValueArrivingAt(value);
        if (!value)
            return { };
        if (function.at(value).representation == wanted)
            break;
    }
    if (m_pass.types.intersectionOf(function.at(value).type, function.at(source).type) != function.at(source).type)
        return { };
    return value;
}

ValueID ConversionInserter::convert(ValueID source, Representation wanted, ProgramPoint before, SharingScope scope)
{
    m_mayHaveChangedUses = true;
    Function& function = m_pass.function;
    Representation from = function.at(source).representation;
    if (from == wanted)
        return source;
    if (ValueID shared = findSharedConversion(scope, before.block, source, wanted))
        return shared;
    if (ValueID sameValue = findSameValueMadeBefore(source, wanted)) {
        ValueID reused = assumeTypeOf(m_builder, source, sameValue, before);
        shareConversion(scope, before.block, source, wanted, reused);
        return reused;
    }

    const Type& type = m_pass.program.typeOf(function, source);
    if (std::optional<Value> constant = makeOnlyValueOf(type); constant && isLegalRepresentation(type, wanted)) {
        ValueID made = m_builder.insertBefore(before, WTF::move(*constant));
        function.edit(made).representation = wanted;
        shareConversion(scope, before.block, source, wanted, made);
        ++m_numberOfConversions;
        return made;
    }

    std::optional<ConversionPath> path = findConversion(type, from, wanted);
    if (!path) {
        reportUnsupportedConversion(source, wanted, before.block);
        return source;
    }

    ValueID current = source;
    for (ConversionStep step : *path) {
        if (ValueID shared = findSharedConversion(scope, before.block, source, step.to)) {
            current = shared;
            continue;
        }
        current = m_builder.insertBefore(before, IRBuilder::make(step.opcode, { current }));
        function.edit(current).representation = step.to;
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
    Terminator& jump = function.edit(block).terminator;
    m_builder.setOrigin(jump.origin());
    ValueID converted = convert(jump.next().arguments[argumentIndex], wanted, pointBeforeTerminatorOf(function, block), SharingScope::Block);
    jump.next().arguments[argumentIndex] = converted;
}

void ConversionInserter::convertHandlerArgument(BlockID covered, uint32_t argumentIndex, Representation wanted)
{
    Function& function = m_pass.function;
    Target& handler = *function.edit(covered).handler;
    ValueID source = handler.arguments[argumentIndex];

    ProgramPoint afterDefinition = m_useLists.pointAfter(source);
    RELEASE_ASSERT(afterDefinition.block != covered);

    m_builder.setOrigin(function.at(source).origin());
    handler.arguments[argumentIndex] = convert(source, wanted, afterDefinition, SharingScope::Function);
}

void ConversionInserter::convertOnSplitEdge(Edge edge, const EdgeConversion& conversion)
{
    m_mayHaveChangedUses = true;
    if (m_splitEdges.isEmpty() || m_splitEdges.last().edge != edge)
        m_splitEdges.append(SplitEdge { edge, { }, { } });
    m_splitEdges.last().conversions.append(conversion);
}

BlockID ConversionInserter::splitEdge(const SplitEdge& split)
{
    Function& function = m_pass.function;
    BlockID added = m_builder.splitEdge(split.edge);

    const Target& next = function.at(added).terminator.next();
    function.edit(added).shape = moreCompactOf(function.at(split.edge.from).shape, function.at(next.block).shape);

    const CompactList<ValueID>& parameters = function.at(next.block).parameters;
    size_t firstResult = next.arguments.size() - function.at(added).parameters.size();
    for (size_t index = firstResult; index < next.arguments.size(); ++index)
        function.edit(next.arguments[index]).representation = function.at(parameters[index]).representation;

    for (const EdgeConversion& conversion : split.conversions) {
        if (conversion.passed)
            function.edit(next.arguments[conversion.parameterIndex]).representation = *conversion.passed;
    }

    // An edge that would pass a value of type never is not taken, and no code may follow such a value.
    bool passesNever = std::ranges::any_of(function.at(added).parameters, [&](ValueID parameter) {
        return function.at(parameter).type == TypeTable::never;
    });
    if (passesNever) {
        Terminator& terminator = function.edit(added).terminator;
        Origin origin = terminator.origin();
        terminator.become(TerminatorKind::Unreachable);
        terminator.setOrigin(origin);
    }
    return added;
}

void ConversionInserter::reportUnsupportedConversion(ValueID source, Representation wanted, BlockID block)
{
    StringPrintStream detail;
    detail.print("a value of type "_s);
    printType(detail, m_pass.program, m_pass.function.at(source).type);
    detail.print(" kept as "_s, nameOf(m_pass.function.at(source).representation), " is wanted as "_s, nameOf(wanted));
    m_pass.build.diagnostics().add(InternalError { .kind = InternalErrorKind::UnsupportedConversion, .function = m_pass.id, .block = block, .value = source, .detail = detail.toString() });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
