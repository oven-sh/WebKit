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
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <array>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ExpandedResult {
    TypeID type;
    ValueID firstRead;
};

struct ResultUse {
    uint32_t resultIndex { 0 };
    Use use;
};

struct ResultReads {
    Vector<ExpandedResult, 2> results;
    Vector<ResultUse, 8> uses;
    Vector<BlockID, 4> blocks;
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
    function.edit(id).become(Opcode::Parameter);
}

static void copyExpectedType(Function& function, ValueID to, ValueID from)
{
    function.edit(to).setExpected(function.at(from).expected());
}

static void replaceProducerWithResults(Function& function, ValueID producer, std::span<const ValueID> parameters, BlockID continuation)
{
    Value& results = function.edit(producer);
    results.become(Opcode::Results);
    results.setValuesOperands(parameters);
    function.edit(continuation).values.insert(0, producer);
}

static ResultReads collectResultReads(const Program& program, const Function& function, TypeArena& types, const UseLists& useLists, ValueID producer, uint32_t numberOfResults)
{
    ResultReads reads;
    for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex)
        reads.results.append(ExpandedResult { resultTypeAt(program, function, types, producer, resultIndex), { } });
    for (const Use& producerUse : useLists.usesOf(producer)) {
        if (producerUse.kind != Use::Kind::Operand || function.at(producerUse.user).opcode != Opcode::ResultAt)
            continue;
        ValueID read = producerUse.user;
        uint32_t resultIndex = function.at(read).index();
        auto& result = reads.results[resultIndex];
        if (!result.firstRead)
            result.firstRead = read;
        for (const Use& use : useLists.usesOf(read))
            reads.uses.append(ResultUse { resultIndex, use });
        if (reads.blocks.isEmpty() || reads.blocks.last() != producerUse.block)
            reads.blocks.append(producerUse.block);
    }
    return reads;
}

static Vector<ValueID, 1> replaceResultReads(IRBuilder& builder, ResultReads&& reads, ValueID producer, BlockID cutBlock, BlockID continuation)
{
    Function& function = builder.function();
    auto blockAfterCut = [&](BlockID block) {
        return block == cutBlock ? continuation : block;
    };

    Vector<ValueID, 1> parameters;
    for (auto& result : reads.results) {
        ValueID parameter = builder.addParameter(continuation, result.type);
        if (result.firstRead)
            copyExpectedType(function, parameter, result.firstRead);
        parameters.append(parameter);
    }

    for (auto& [resultIndex, use] : reads.uses) {
        use.block = blockAfterCut(use.block);
        replaceUse(function, use, parameters[resultIndex]);
    }

    for (BlockID recordedBlock : reads.blocks) {
        BlockID block = blockAfterCut(recordedBlock);
        auto& values = function.at(block).values;
        BitVector indexes(values.size());
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (isResultRead(function, values[index], producer))
                indexes.quickSet(index);
        }
        builder.eraseValues(block, indexes);
    }

    replaceProducerWithResults(function, producer, parameters.span(), continuation);
    return parameters;
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
            function.edit(continuation).parameters.append(parameter);
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
        auto& from = m_function.edit(point.block);
        auto& to = m_function.edit(continuation);
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
    ResultReads reads;
    if (numberOfResults) {
        RELEASE_ASSERT(m_analyses);
        reads = collectResultReads(m_context.program, m_function, m_types, m_analyses->useLists(), id, *numberOfResults);
    }
    Vector<BlockID, 8> changedBlocks { point.block };
    for (const ResultUse& rewritten : reads.uses)
        changedBlocks.append(rewritten.use.block);

    Expansion expansion = cutForExpansion(point);
    if (numberOfResults)
        expansion.results = replaceResultReads(*this, WTF::move(reads), id, point.block, expansion.continuation);
    else if (resultRuleOf(m_function.at(id).opcode) == ResultRule::None)
        becomeUndefined(m_function, id);
    else {
        becomeParameter(m_function, id);
        m_function.edit(expansion.continuation).parameters.append(id);
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
    // A read becomes a cast of a parameter where it stands, and only the lists know where that is.
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
    std::ranges::stable_sort(m_reads, { }, &Read::producer);
}

auto IRBuilder::DeferredResultReads::readsOf(ValueID producer) const -> std::span<const Read>
{
    auto range = std::ranges::equal_range(m_reads.span(), producer, { }, &Read::producer);
    return { range.begin(), range.end() };
}

void IRBuilder::DeferredResultReads::replace(ValueID read, ValueID replacement)
{
    m_substitution.replace(read, replacement);
    m_isReplaced.set(read.index());
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
    std::span reads = deferred.readsOf(id);
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
    for (auto [producer, read] : reads)
        deferred.replace(read, expansion.results[m_function.at(read).index()]);
    replaceProducerWithResults(m_function, id, expansion.results.span(), expansion.continuation);
    didChangeControlFlow(singleElementSpan(point.block));
    return expansion;
}

void IRBuilder::endExpansion(const Expansion& expansion, std::span<const ValueID> results)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    RELEASE_ASSERT(results.size() == expansion.results.size());
    // No value has the type never: what computes such a result never completes, so no edge leads on from here.
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
    Block& block = m_function.edit(predecessor);
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
