/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRBuilder.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ExpandedResult {
    TypeID type;
    ValueID firstRead;
};

struct ExpansionResultUse {
    uint32_t resultIndex { 0 };
    Use use;
};

} // anonymous namespace

static TypeID resultTypeAt(const Program& program, const Function& function, TypeArena& types, ValueID producer, uint32_t resultIndex)
{
    Value read = IRBuilder::make(Opcode::ResultAt, { producer });
    read.setIndex(resultIndex);
    return resultTypeOf(program, function, read, types);
}

static bool isResultRead(const Function& function, ValueID id, ValueID producer)
{
    const Value& value = function.at(id);
    return value.opcode == Opcode::ResultAt && value.resultsOperand() == producer;
}

static void becomeParameter(Function& function, ValueID id)
{
    function.edit(id, ValueEdit::Definition).become(Opcode::Parameter);
}

static void copyExpectedType(Function& function, ValueID to, ValueID from)
{
    function.edit(to, ValueEdit::Type).setExpected(function.at(from).expected());
}

static void replaceProducerWithResults(Function& function, ValueID producer, std::span<const ValueID> parameters, BlockID continuation)
{
    Value& results = function.edit(producer, ValueEdit::Definition);
    results.become(Opcode::Results);
    results.setValuesOperands(parameters);
    function.edit(continuation, BlockEdit::Values).values.insert(0, producer);
}

static Vector<ValueID, 1> convertResultReads(IRBuilder& builder, TypeArena& types, std::span<const ValueID> reads, ValueID producer, uint32_t numberOfResults, BlockID continuation)
{
    Function& function = builder.function();
    Vector<ValueID, 1> parameters(numberOfResults);
    {
        const BlockValues& values = function.at(continuation).values;
        BitVector indexes(values.size());
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (!isResultRead(function, values[index], producer))
                continue;
            ValueID& parameter = parameters[function.at(values[index]).index()];
            if (parameter)
                continue;
            parameter = values[index];
            indexes.quickSet(index);
        }
        builder.eraseValues(continuation, indexes);
    }

    Vector<ValueID, 2> firstReads(numberOfResults);
    for (ValueID read : reads) {
        ValueID& firstRead = firstReads[function.at(read).index()];
        if (!firstRead)
            firstRead = read;
    }

    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
        ValueID& parameter = parameters[resultIndex];
        if (parameter) {
            becomeParameter(function, parameter);
            function.edit(continuation, BlockEdit::Parameters).parameters.append(parameter);
            continue;
        }
        parameter = builder.addParameter(continuation, resultTypeAt(builder.context().program, function, types, producer, resultIndex));
        if (firstReads[resultIndex])
            copyExpectedType(function, parameter, firstReads[resultIndex]);
    }

    for (ValueID read : reads) {
        if (isResultRead(function, read, producer))
            becomeUpcast(function, read, parameters[function.at(read).index()]);
    }

    replaceProducerWithResults(function, producer, parameters.span(), continuation);
    return parameters;
}

IRBuilder::Expansion IRBuilder::cutForExpansion(ProgramPoint point)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(point.block != m_block);
    RELEASE_ASSERT(point.index < m_function.at(point.block).values.size());
    ValueID id = m_function.at(point.block).values[point.index];

    std::optional<Target> handler = m_function.at(point.block).handler;
    setHandler(handler);
    BlockID continuation = addBlock(m_function.at(point.block).frequency);
    {
        auto& from = m_function.edit(point.block, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
        auto& to = m_function.edit(continuation, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
        to.values.append(from.values.subspan(point.index + 1));
        from.values.shrink(point.index);
        to.terminator = std::exchange(from.terminator, Terminator { });
        to.shape = from.shape;
    }

    setOrigin(m_function.at(id).origin());
    moveToEndOf(point.block);
    return { continuation, { }, WTF::move(handler) };
}

IRBuilder::Expansion IRBuilder::beginExpansion(ProgramPoint point)
{
    ValueID id = m_function.at(point.block).values[point.index];
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_context.program, m_function, m_function.at(id));
    Vector<ExpandedResult, 2> results;
    Vector<ExpansionResultUse, 8> uses;
    Vector<BlockID, 4> blocksOfReads;
    if (numberOfResults) {
        RELEASE_ASSERT(m_analyses);
        const UseLists& useLists = m_analyses->useLists();
        for (uint32_t resultIndex = 0; resultIndex < *numberOfResults; ++resultIndex)
            results.append(ExpandedResult { resultTypeAt(m_context.program, m_function, m_types, id, resultIndex), { } });
        for (const Use& producerUse : useLists.usesOf(id)) {
            if (producerUse.kind != Use::Kind::Operand || m_function.at(producerUse.user).opcode != Opcode::ResultAt)
                continue;
            ValueID read = producerUse.user;
            uint32_t resultIndex = m_function.at(read).index();
            if (!results[resultIndex].firstRead)
                results[resultIndex].firstRead = read;
            for (const Use& use : useLists.usesOf(read))
                uses.append(ExpansionResultUse { resultIndex, use });
            if (blocksOfReads.isEmpty() || blocksOfReads.last() != producerUse.block)
                blocksOfReads.append(producerUse.block);
        }
    }
    Vector<BlockID, 8> changedBlocks { point.block };
    for (const ExpansionResultUse& rewritten : uses)
        changedBlocks.append(rewritten.use.block);

    Expansion expansion = cutForExpansion(point);
    if (numberOfResults) {
        auto blockAfterCut = [&](BlockID block) {
            return block == point.block ? expansion.continuation : block;
        };
        for (const ExpandedResult& result : results) {
            ValueID parameter = addParameter(expansion.continuation, result.type);
            if (result.firstRead)
                copyExpectedType(m_function, parameter, result.firstRead);
            expansion.results.append(parameter);
        }
        for (auto& [resultIndex, use] : uses) {
            use.block = blockAfterCut(use.block);
            replaceUse(m_function, use, expansion.results[resultIndex]);
        }
        for (BlockID recordedBlock : blocksOfReads) {
            BlockID block = blockAfterCut(recordedBlock);
            auto& values = m_function.at(block).values;
            BitVector indexes(values.size());
            for (uint32_t index = 0; index < values.size(); ++index) {
                if (isResultRead(m_function, values[index], id))
                    indexes.quickSet(index);
            }
            eraseValues(block, indexes);
        }
        replaceProducerWithResults(m_function, id, expansion.results.span(), expansion.continuation);
    } else if (resultRuleOf(m_function.at(id).opcode) == ResultRule::None)
        becomeUndefined(m_function, id);
    else {
        becomeParameter(m_function, id);
        m_function.edit(expansion.continuation, BlockEdit::Parameters).parameters.append(id);
        expansion.results.append(id);
    }
    didChangeControlFlow(changedBlocks.span());
    return expansion;
}

IRBuilder::Expansion IRBuilder::beginExpansion(ProgramPoint point, std::span<const ValueID> resultReads)
{
    ValueID id = m_function.at(point.block).values[point.index];
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_context.program, m_function, m_function.at(id));
    RELEASE_ASSERT(numberOfResults);
    for (ValueID read : resultReads)
        RELEASE_ASSERT(isResultRead(m_function, read, id));
    if (UseLists* useLists = m_analyses ? m_analyses->updatingUseLists() : nullptr) {
        for (ValueID read : resultReads)
            useLists->didChangeOperandsOf(read);
    }

    Expansion expansion = cutForExpansion(point);
    expansion.results = convertResultReads(*this, m_types, resultReads, id, *numberOfResults, expansion.continuation);
    didChangeControlFlow(singleElementSpan(point.block));
    return expansion;
}

IRBuilder::DeferredResultReads::DeferredResultReads(IRBuilder& builder, std::span<const ValueID> producers)
    : m_builder(builder)
    , m_substitution(builder.m_function)
{
    RELEASE_ASSERT(builder.m_analyses);
    const UseLists& useLists = builder.m_analyses->useLists();
    for (ValueID producer : producers) {
        for (const Use& use : useLists.usesOf(producer)) {
            if (use.kind == Use::Kind::Operand && builder.m_function.at(use.user).opcode == Opcode::ResultAt)
                m_reads.append(Read { producer, use.user });
        }
    }
    AOT::stableSort(m_reads, { }, &Read::producer);
}

void IRBuilder::DeferredResultReads::finish()
{
    if (m_substitution.isEmpty())
        return;

    Function& function = m_builder.m_function;
    m_substitution.apply(function, m_builder.m_analyses);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        BitVector indexes(values.size());
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (m_isReplaced.get(values[index].index()))
                indexes.quickSet(index);
        }
        if (!indexes.isEmpty())
            m_builder.eraseValues(BlockID(blockIndex), indexes);
    }
    m_isReplaced.clearAll();
}

IRBuilder::Expansion IRBuilder::beginExpansion(ProgramPoint point, DeferredResultReads& deferred)
{
    RELEASE_ASSERT(&deferred.m_builder == this);
    ValueID id = m_function.at(point.block).values[point.index];
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_context.program, m_function, m_function.at(id));
    if (!numberOfResults)
        return beginExpansion(point);

    Vector<ExpandedResult, 2> results;
    for (uint32_t resultIndex = 0; resultIndex < *numberOfResults; ++resultIndex)
        results.append(ExpandedResult { resultTypeAt(m_context.program, m_function, m_types, id, resultIndex), { } });
    auto reads = std::ranges::equal_range(deferred.m_reads.span(), id, { }, &DeferredResultReads::Read::producer);
    for (auto [producer, read] : reads) {
        ExpandedResult& result = results[m_function.at(read).index()];
        if (!result.firstRead)
            result.firstRead = read;
    }

    Expansion expansion = cutForExpansion(point);
    for (const ExpandedResult& result : results) {
        ValueID parameter = addParameter(expansion.continuation, result.type);
        if (result.firstRead)
            copyExpectedType(m_function, parameter, result.firstRead);
        expansion.results.append(parameter);
    }
    for (auto [producer, read] : reads) {
        deferred.m_substitution.replace(read, expansion.results[m_function.at(read).index()]);
        deferred.m_isReplaced.set(read.index());
    }
    replaceProducerWithResults(m_function, id, expansion.results.span(), expansion.continuation);
    didChangeControlFlow(singleElementSpan(point.block));
    return expansion;
}

void IRBuilder::endExpansion(const Expansion& expansion, std::span<const ValueID> results)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(results.size() == expansion.results.size());
    bool neverCompletes = std::ranges::any_of(results, [&](ValueID result) {
        return m_function.at(result).type == TypeTable::never;
    });
    if (neverCompletes) {
        emitUnreachable();
        return;
    }
    emitJump(expansion.continuation, Arguments(results));
}

void IRBuilder::mergeContinuation(const Expansion& expansion, BlockID predecessor)
{
    RELEASE_ASSERT(!m_block && m_insertions.isEmpty());
    Block& block = m_function.edit(predecessor, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest });
    RELEASE_ASSERT(block.terminator.kind == TerminatorKind::Jump && block.terminator.targets[0].block == expansion.continuation);
    RELEASE_ASSERT(block.handler == m_function.at(expansion.continuation).handler);
    Arguments arguments = WTF::move(block.terminator.targets[0].arguments);
    Block continuation = std::exchange(m_function.edit(expansion.continuation), Block { });
    for (unsigned index = 0; index < continuation.parameters.size(); ++index) {
        becomeUpcast(m_function, continuation.parameters[index], arguments[index]);
        block.values.append(continuation.parameters[index]);
    }
    block.values.appendVector(continuation.values);
    block.terminator = WTF::move(continuation.terminator);
    block.frequency = rarerOf(block.frequency, continuation.frequency);
    didChangeControlFlow(std::array { predecessor, expansion.continuation });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
