/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTCallGraph.h"
#include "AOTEffectsDescription.h"
#include "AOTExactBuffer.h"
#include "AOTFunctionAnalyses.h"
#include "AOTHeldOperands.h"
#include "AOTIR.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <bit>
#include <numeric>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PredecessorLists {
    std::span<const unsigned> of(unsigned block) const { return blocks.span().subspan(first[block], first[block + 1] - first[block]); }

    Vector<unsigned, 32> first;
    Vector<unsigned, 64> blocks;
};

} // anonymous namespace

template<typename Entry>
static bool rowMayThrow(Entry entry, const Effects& bound)
{
    return bound.mayThrow || effectRuleOf(entry) != EffectRule::Declared || mayRunOperandCode(entry);
}

namespace {

struct FactsOfRows {
    OpcodeSet opcodesThatMayThrow;
    OpcodeSet opcodesThatMaySuspend;
    TerminatorKindSet terminatorKindsThatMayThrow;
};

} // anonymous namespace

static const FactsOfRows& factsOfRows()
{
    static const FactsOfRows facts = [] {
        FactsOfRows result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            Effects bound = declaredBoundOf(opcode);
            if (rowMayThrow(opcode, bound))
                result.opcodesThatMayThrow.add(opcode);
            if (bound.maySuspend)
                result.opcodesThatMaySuspend.add(opcode);
        }
        for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
            if (rowMayThrow(kind, declaredBoundOf(kind)))
                result.terminatorKindsThatMayThrow.add(kind);
        }
        return result;
    }();
    return facts;
}

static void addBlocksReaching(const PredecessorLists& predecessors, BitVector& blocks)
{
    Vector<unsigned, 32> worklist;
    for (size_t block : blocks)
        worklist.append(block);
    while (!worklist.isEmpty()) {
        for (unsigned predecessor : predecessors.of(worklist.takeLast())) {
            if (!blocks.quickSet(predecessor))
                worklist.append(predecessor);
        }
    }
}

ExitReachability computeExitReachability(const Function& function)
{
    const FactsOfRows& rows = factsOfRows();
    unsigned numberOfBlocks = function.blocks.size();
    ExitReachability exits { BitVector(numberOfBlocks), BitVector(numberOfBlocks) };
    BitVector comesToTerminator(numberOfBlocks);
    BitVector comesToHandler(numberOfBlocks);
    PredecessorLists predecessors;
    predecessors.first.fill(0u, numberOfBlocks + 1);
    for (unsigned index = 0; index < numberOfBlocks; ++index) {
        const Block& block = function.blocks[index];
        bool reachesEnd = true;
        bool mayThrowInside = false;
        bool returns = false;
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            reachesEnd = reachesEnd && value.type != TypeTable::never;
            mayThrowInside = mayThrowInside || rows.opcodesThatMayThrow.contains(value.opcode);
            returns = returns || rows.opcodesThatMaySuspend.contains(value.opcode);
        }
        if (reachesEnd) {
            TerminatorKind kind = block.terminator.kind;
            mayThrowInside = mayThrowInside || rows.terminatorKindsThatMayThrow.contains(kind);
            returns = returns || returnsToCaller(kind);
            comesToTerminator.quickSet(index);
            for (const Target& target : block.terminator.targets)
                ++predecessors.first[target.block.index() + 1];
        }
        if (returns)
            exits.reachesReturn.quickSet(index);
        if (mayThrowInside && block.handler) {
            comesToHandler.quickSet(index);
            ++predecessors.first[block.handler->block.index() + 1];
        }
        if (mayThrowInside && !block.handler)
            exits.reachesThrow.quickSet(index);
    }
    if (numberOfBlocks == 1)
        return exits;

    std::partial_sum(predecessors.first.begin(), predecessors.first.end(), predecessors.first.begin());
    predecessors.blocks.grow(predecessors.first.last());
    Vector<unsigned, 32> next(predecessors.first.span().first(numberOfBlocks));
    for (size_t index : comesToTerminator) {
        for (const Target& target : function.blocks[index].terminator.targets)
            predecessors.blocks[next[target.block.index()]++] = index;
    }
    for (size_t index : comesToHandler)
        predecessors.blocks[next[function.blocks[index].handler->block.index()]++] = index;
    addBlocksReaching(predecessors, exits.reachesReturn);
    addBlocksReaching(predecessors, exits.reachesThrow);
    return exits;
}

static bool maySuspend(FunctionAnalyses& analyses)
{
    const OpcodeSet& opcodesThatMaySuspend = factsOfRows().opcodesThatMaySuspend;
    if (!analyses.opcodesOfValues().intersects(opcodesThatMaySuspend))
        return false;
    const Function& function = analyses.function();
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return opcodesThatMaySuspend.contains(function.at(id).opcode);
        });
    });
}

static uint64_t sortKeyOf(const SummaryNode& node)
{
    return static_cast<uint64_t>(static_cast<uint32_t>(node.function.index() + 1)) << 32 | static_cast<uint64_t>(node.part) << 30 | node.parameter;
}

static std::optional<uint32_t> mappedEntryParameter(const Function& callee, const Value& call, uint32_t operandIndex)
{
    if (Value::opcodesWithCallees.contains(call.opcode))
        return entryParameterOf(callee, call, operandIndex);
    if (isHeldOperand(call, operandIndex))
        return std::nullopt;
    return call.variadicOperandIndex(operandIndex);
}

static bool hasEmptyDescription(Opcode opcode)
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode member : allMembersOf<Opcode>) {
            if (effectRuleOf(member) == EffectRule::Declared && declaredBoundOf(member) == Effects::none() && !mayRunOperandCode(member) && member != Opcode::Check)
                result.add(member);
        }
        return result;
    }();
    return opcodes.contains(opcode);
}

static bool hasEmptyDescription(TerminatorKind kind)
{
    static const TerminatorKindSet kinds = [] {
        TerminatorKindSet result;
        for (TerminatorKind member : allMembersOf<TerminatorKind>) {
            if (effectRuleOf(member) != EffectRule::Inline && declaredBoundOf(member) == Effects::none() && !mayRunOperandCode(member))
                result.add(member);
        }
        return result;
    }();
    return kinds.contains(kind);
}

// A set of locations without a heap has no mask either, so the words after the heaps need not be read.
static ALWAYS_INLINE bool isPure(const Effects& effects)
{
    return effects.reads.isEmpty() && effects.writes.isEmpty() && effects.mayThrow == ThrowEffect(false) && !effects.mayCollect && !effects.mayCallUnknownCode && !effects.maySuspend && !effects.mayBeTerminated;
}

static bool isEmpty(const EffectDescription& description)
{
    return description.own == EffectsOnEdges::none() && description.functions.isEmpty() && description.slowPathFunctions.isEmpty() && !description.callsUnknownCode && !description.followsFieldsOfOperands;
}

static FunctionID genericVersionThatBodyRestartsIn(const Program& program, const Function& function)
{
    if (function.role != FunctionRole::Source || !function.derivation.source || function.blocks.isEmpty())
        return { };
    const auto* record = program.at(function.derivation.source).creator.tryAs<CreatorRecord::EntryChecks>();
    if (!record || &program.at(record->body) != &function)
        return { };
    const Function& generic = program.at(record->fallback);
    if (generic.role != FunctionRole::GenericVersion || generic.blocks.isEmpty())
        return { };
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    if (parameters.size() != generic.at(entryBlock).parameters.size())
        return { };
    bool isEntered = std::ranges::any_of(function.blocks, [&](const Block& block) {
        if (block.values.size() != 1 || block.handler || block.terminator.kind != TerminatorKind::ReturnCall)
            return false;
        const Value& call = function.at(block.values[0]);
        // A pass may have put another name of a parameter in its place: what RequireObjectCoercible hands on, a cast.
        return call.opcode == Opcode::CallDirect && call.function() == record->fallback && call.variadicOperands().size() == parameters.size();
    });
    return isEntered ? record->fallback : FunctionID();
}

namespace {

class BodySummarizer {
public:
    enum class ReadAs : uint8_t { Itself, ItselfAndCalleesByOperandTypes, GenericVersionGivenTypes, OutlinedBodyGivenTypes };

    BodySummarizer(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses, BodyReferenceCollector* references, ReadAs readAs)
        : m_context(context)
        , m_id(id)
        , m_analyses(analyses)
        , m_references(references)
        , m_function(analyses.function())
        , m_exits(computeExitReachability(m_function))
        , m_maySuspend(maySuspend(analyses))
        , m_readAs(readAs)
        , m_genericVersionEntered(readAs == ReadAs::ItselfAndCalleesByOperandTypes ? genericVersionThatBodyRestartsIn(context.program, m_function) : FunctionID())
        , m_wordsInRow((firstBitOfParameters + m_function.numberOfEntryParameters() + bitsInWord - 1) / bitsInWord)
        , m_parametersInMemoryAsTargets(FillWith { }, m_wordsInRow, uint64_t { 0 })
    {
    }

    BodySummary run()
    {
        uint32_t numberOfParameters = m_function.numberOfEntryParameters();
        m_summary.parameters.reserveInitialCapacity(numberOfParameters);
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
            m_summary.parameters.append(ParameterSummary { summarizeEntryParameter(m_analyses, parameter), OperandMemory::none() });
            if (mayBeReadOutOfMemory(parameter)) {
                m_parametersInMemory.append(parameter);
                setBitOfParameter(m_parametersInMemoryAsTargets.mutableSpan(), parameter);
            }
        }
        if (m_function.isUnsplitCoroutine())
            addDependencyToExitBits(SummaryNode::onThrow(m_id), bitOfReturn);
        for (unsigned index = 0; index < m_function.blocks.size(); ++index) {
            BlockID block(index);
            if (const std::optional<Target>& handler = m_function.at(block).handler)
                m_gatheredForExits[exitsOf(handler->block)].mayCollect = true;
            for (ValueID id : m_function.at(block).values)
                summarizeValue(block, id);
            summarizeTerminator(block);
        }
        handOverToExits();
        if (m_function.role == FunctionRole::CoroutineBody && m_function.creator.as<CreatorRecord::Coroutine>().kind == CoroutineKind::AsyncFunction) {
            if (m_summary.onReturn.mayThrow || m_summary.onThrow.mayThrow || !m_sources.isEmpty())
                m_summary.onReturn.merge(declaredBoundOf(Opcode::RejectPromise));
            addDependencyToExitBits(SummaryNode::onThrow(m_id), bitOfReturn);
        }
        // A stack overflow on entry is a RangeError at the call.
        m_summary.onReturn.mayThrow |= ThrowEffect::onlyStackOverflow();
        listDependencies();
        return WTF::move(m_summary);
    }

private:
    bool mayBeReadOutOfMemory(uint32_t entryParameter)
    {
        ValueID parameter = m_function.at(entryBlock).parameters[entryParameter];
        if (!mayBeReference(m_context.program.typeOf(m_function, parameter)))
            return false;
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> object = escape.gaveUp() ? std::nullopt : escape.objectDefinedBy(parameter);
        if (!object)
            return true;
        return std::ranges::any_of(escape.usesOf(*object), [](const ObjectUse& use) {
            return use.uses.contains(OperandUse::Retains);
        });
    }

    void addToExits(BlockID block, const Effects& normal, const Effects& handler)
    {
        m_gatheredForExits[exitsOf(block)].merge(normal);
        if (isPure(handler))
            return;
        if (const std::optional<Target>& target = m_function.at(block).handler)
            m_gatheredForExits[exitsOf(target->block)].merge(handler);
        else
            m_gatheredForThrowAlone.merge(handler);
    }

    void handOverToExits()
    {
        Effects all = m_gatheredForThrowAlone;
        for (uint32_t exits = 0; exits < m_gatheredForExits.size(); ++exits) {
            const Effects& effects = m_gatheredForExits[exits];
            if (exits & 1)
                m_summary.onReturn.merge(effects);
            if (exits & 2)
                m_summary.onThrow.merge(effects);
            m_summary.onReturn.mayThrow |= effects.mayThrow;
            m_summary.onReturn.mayCollect |= effects.mayCollect;
            m_summary.onReturn.mayBeTerminated |= effects.mayBeTerminated;
            all.merge(effects);
        }
        m_summary.onThrow.merge(m_gatheredForThrowAlone);

        OperandMemory memory { all.reads.restrictedTo(perObjectHeaps), all.writes.restrictedTo(perObjectHeaps) };
        for (uint32_t parameter : m_parametersInMemory)
            m_summary.parameters[parameter].memory.merge(memory);
    }

    // Every target is a node of the function itself, so the targets of a source are a row of bits: on return, on throw, then the entry parameters.
    static constexpr uint32_t bitsInWord = 64;
    static constexpr uint64_t bitOfReturn = 1;
    static constexpr uint64_t bitOfThrow = 2;
    static constexpr uint32_t firstBitOfParameters = 2;

    static void setBitOfParameter(std::span<uint64_t> row, uint32_t entryParameter)
    {
        uint32_t bit = firstBitOfParameters + entryParameter;
        row[bit / bitsInWord] |= uint64_t { 1 } << bit % bitsInWord;
    }

    std::span<uint64_t> targetsOf(SummaryNode source)
    {
        if (m_sources.isEmpty() || m_sources[m_positionOfLatestSource] != source) {
            uint32_t hash = static_cast<uint32_t>(sortKeyOf(source) * 0x9e3779b97f4a7c15ull >> 32);
            std::optional<uint32_t> position = m_indexOfSources.find(hash, [&](uint32_t candidate) {
                return m_sources[candidate] == source;
            });
            if (!position) {
                position = m_sources.size();
                m_indexOfSources.add(hash, *position);
                m_sources.append(source);
                m_targets.grow(m_targets.size() + m_wordsInRow);
                zeroSpan(m_targets.mutableSpan().last(m_wordsInRow));
            }
            m_positionOfLatestSource = *position;
        }
        return m_targets.mutableSpan().subspan(size_t { m_positionOfLatestSource } * m_wordsInRow, m_wordsInRow);
    }

    void addDependencyToExitBits(SummaryNode source, uint64_t exitBits)
    {
        targetsOf(source)[0] |= exitBits;
    }

    void addDependencyToParameter(SummaryNode source, uint32_t entryParameter)
    {
        setBitOfParameter(targetsOf(source), entryParameter);
    }

    uint64_t exitBitsOf(BlockID block) const
    {
        bool reachesThrow = m_exits.reachesThrow.get(block.index());
        return (m_exits.reachesReturn.get(block.index()) || !reachesThrow ? bitOfReturn : 0) | (reachesThrow ? bitOfThrow : 0);
    }

    void addDependencyToParametersInMemory(SummaryNode source)
    {
        if (!m_parametersInMemory.isEmpty())
            addDependencyToParametersInMemoryAnd(source, 0);
    }

    void addDependencyToParametersInMemoryAnd(SummaryNode source, uint64_t exitBits)
    {
        std::span<uint64_t> row = targetsOf(source);
        for (uint32_t word = 0; word < m_wordsInRow; ++word)
            row[word] |= m_parametersInMemoryAsTargets[word];
        row[0] |= exitBits;
    }

    void addDependencyToExits(BlockID block, SummaryNode normalSource, SummaryNode handlerSource)
    {
        const std::optional<Target>& handler = m_function.at(block).handler;
        addDependencyToParametersInMemoryAnd(normalSource, exitBitsOf(block));
        addDependencyToParametersInMemoryAnd(handlerSource, handler ? exitBitsOf(handler->block) : bitOfThrow);
    }

    // Sorted by source, then by target.
    void listDependencies()
    {
        Vector<uint32_t, 32> positions(m_sources.size(), [](size_t position) {
            return static_cast<uint32_t>(position);
        });
        AOT::sort(positions, std::ranges::less { }, [&](uint32_t position) {
            return sortKeyOf(m_sources[position]);
        });
        size_t numberOfDependencies = 0;
        for (uint64_t word : m_targets)
            numberOfDependencies += std::popcount(word);
        m_summary.dependencies.reserveInitialCapacity(numberOfDependencies);
        for (uint32_t position : positions) {
            SummaryNode source = m_sources[position];
            std::span<const uint64_t> row = m_targets.span().subspan(size_t { position } * m_wordsInRow, m_wordsInRow);
            for (uint32_t index = 0; index < m_wordsInRow; ++index) {
                for (uint64_t word = row[index]; word; word &= word - 1) {
                    uint32_t bit = index * bitsInWord + std::countr_zero(word);
                    SummaryNode target = bit >= firstBitOfParameters ? SummaryNode::parameterOf(m_id, bit - firstBitOfParameters) : bit ? SummaryNode::onThrow(m_id) : SummaryNode::onReturn(m_id);
                    m_summary.dependencies.append(SummaryDependency { source, target });
                }
            }
        }
    }

    uint32_t exitsOf(BlockID block) const
    {
        return (m_exits.reachesReturn.get(block.index()) ? 1 : 0) | (m_exits.reachesThrow.get(block.index()) ? 2 : 0);
    }

    uint32_t placeOfExits(BlockID block) const
    {
        const std::optional<Target>& handler = m_function.at(block).handler;
        return exitsOf(block) | (handler ? 4 | exitsOf(handler->block) << 3 : 0);
    }

    static uint32_t placeOfParameter(uint32_t entryParameter) { return 32 + entryParameter; }

    bool isFirstToRunAt(uint32_t place, const EffectDescription& description)
    {
        AOT_THRESHOLD(size_t, fewestFunctionsToRemember, 3);
        if (description.functions.size() < fewestFunctionsToRemember)
            return true;
        uint64_t mixed = place * 2 + description.callsUnknownCode;
        for (FunctionID function : description.functions)
            mixed = (mixed ^ function.index()) * 0x9e3779b97f4a7c15ull;
        uint32_t hash = static_cast<uint32_t>(mixed >> 32);
        auto isSame = [&](uint32_t position) {
            const CodeRunAtPlace& known = m_codeRunAtPlaces[position];
            return known.place == place && known.callsUnknownCode == description.callsUnknownCode && std::ranges::equal(known.functions, description.functions);
        };
        if (m_indexOfCodeRunAtPlaces.find(hash, isSame))
            return false;
        m_indexOfCodeRunAtPlaces.add(hash, m_codeRunAtPlaces.size());
        m_codeRunAtPlaces.append(CodeRunAtPlace { place, description.callsUnknownCode, Vector<FunctionID>(description.functions.span()) });
        return true;
    }

    void forEachBase(ValueID base, NOESCAPE const Invocable<void(const BaseObject&)> auto& functor)
    {
        forEachBaseObject(m_analyses, base, [&](const BaseObject& object) {
            bool mayExistAtResumption = m_maySuspend && object.kind == BaseObject::Kind::OwnAllocation;
            functor(mayExistAtResumption ? BaseObject { } : object);
        });
    }

    void addMemoryOfBase(BlockID block, ValueID base, const OperandMemory& memory, bool mayThrow)
    {
        forEachBase(base, [&](const BaseObject& object) {
            switch (object.kind) {
            case BaseObject::Kind::OwnAllocation:
                return;
            case BaseObject::Kind::EntryParameter:
                m_summary.parameters[object.entryParameter].memory.merge(memory);
                return;
            case BaseObject::Kind::Untracked: {
                Effects effects = Effects::none();
                effects.reads = memory.reads;
                effects.writes = memory.writes;
                addToExits(block, effects, mayThrow ? effects : Effects::none());
                return;
            }
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    }

    void addBaseDependency(BlockID block, ValueID base, SummaryNode calleeParameter)
    {
        forEachBase(base, [&](const BaseObject& object) {
            switch (object.kind) {
            case BaseObject::Kind::OwnAllocation:
                addDependencyToParametersInMemory(calleeParameter);
                return;
            case BaseObject::Kind::EntryParameter:
                addDependencyToParameter(calleeParameter, object.entryParameter);
                return;
            case BaseObject::Kind::Untracked:
                addDependencyToExits(block, calleeParameter, calleeParameter);
                return;
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    }

    void forEachEntryParameterOf(ValueID base, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        if (!mayBeReference(m_context.program.typeOf(m_function, base)))
            return;
        forEachBase(base, [&](const BaseObject& object) {
            if (object.kind == BaseObject::Kind::EntryParameter)
                functor(object.entryParameter);
        });
    }

    void addMemoryOfOperands(const auto& entry, const EffectDescription& description)
    {
        bool writesObjects = description.own.normal.writes.containsAny(perObjectHeaps) || description.own.handler.writes.containsAny(perObjectHeaps);
        if (!writesObjects && !description.callsUnknownCode && description.functions.isEmpty())
            return;
        Effects own = description.own.onAnyEdge();
        OperandMemory memory { own.reads.restrictedTo(perObjectHeaps), own.writes.restrictedTo(perObjectHeaps) };
        entry.forEachOperand([&](ValueID operand, uint32_t) {
            forEachEntryParameterOf(operand, [&](uint32_t entryParameter) {
                m_summary.parameters[entryParameter].memory.merge(memory);
                if (!isFirstToRunAt(placeOfParameter(entryParameter), description))
                    return;
                if (description.callsUnknownCode)
                    addDependencyToParameter(SummaryNode::unknownCode(), entryParameter);
                for (FunctionID id : description.functions) {
                    uint32_t numberOfParameters = m_context.program.at(id).numberOfEntryParameters();
                    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter)
                        addDependencyToParameter(SummaryNode::parameterOf(id, parameter), entryParameter);
                }
            });
        });
    }

    void addArgumentWithoutParameter(ValueID argument, FunctionID callee)
    {
        forEachEntryParameterOf(argument, [&](uint32_t entryParameter) {
            addDependencyToParameter(SummaryNode::onReturn(callee), entryParameter);
            addDependencyToParameter(SummaryNode::onThrow(callee), entryParameter);
        });
    }

    static bool runsOnlyCallees(const EffectDescription& description, std::span<const FunctionID> callees)
    {
        if (description.callsUnknownCode)
            return false;
        if (std::ranges::is_sorted(callees))
            return std::ranges::includes(callees, description.functions);
        return std::ranges::all_of(description.functions, [&](FunctionID function) {
            return std::ranges::contains(callees, function);
        });
    }

    void summarizeCall(BlockID block, const Value& value, ProvenValue<std::span<const FunctionID>> callees, const EffectDescription& description)
    {
        addToExits(block, description.own.normal, description.own.handler);
        for (FunctionID id : callees.value()) {
            const Function& callee = m_context.program.at(id);
            uint32_t numberOfParameters = callee.numberOfEntryParameters();
            addDependencyToExits(block, SummaryNode::onReturn(id), SummaryNode::onThrow(id));
            BitVector mapped(numberOfParameters);
            value.forEachOperand([&](ValueID operand, uint32_t index) {
                std::optional<uint32_t> parameter = mappedEntryParameter(callee, value, index);
                if (!parameter || *parameter >= numberOfParameters) {
                    if (value.variadicOperandIndex(index))
                        addArgumentWithoutParameter(operand, id);
                    return;
                }
                mapped.quickSet(*parameter);
                addBaseDependency(block, operand, SummaryNode::parameterOf(id, *parameter));
            });
            for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
                if (!mapped.quickGet(parameter))
                    addDependencyToExits(block, SummaryNode::parameterOf(id, parameter), SummaryNode::parameterOf(id, parameter));
            }
        }
    }

    // A callee that accepts more kinds of values than it is passed here does what its operations do to values of the passed types.
    // So does the generic version in which this body restarts with its own parameters, and code outlined from that version.
    bool isReadWithOperandTypes(BlockID block, const Value& value) const
    {
        if (m_readAs == ReadAs::Itself || value.opcode != Opcode::CallDirect)
            return false;
        const Program& program = m_context.program;
        const Function& callee = program.at(value.function());
        std::span<const ValueID> operands = value.variadicOperands();
        if (callee.blocks.isEmpty() || operands.size() != callee.numberOfEntryParameters() || operands.size() != value.numberOfOperands())
            return false;
        switch (m_readAs) {
        case ReadAs::Itself:
            return false;
        case ReadAs::ItselfAndCalleesByOperandTypes:
            return value.function() == m_genericVersionEntered && !m_function.at(block).handler && m_function.at(block).terminator.kind == TerminatorKind::ReturnCall;
        case ReadAs::GenericVersionGivenTypes:
            return callee.role == FunctionRole::OutlinedBody;
        case ReadAs::OutlinedBodyGivenTypes:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void addDependencyOnNormalEdge(BlockID block, SummaryNode source)
    {
        addDependencyToParametersInMemoryAnd(source, exitBitsOf(block));
    }

    void addDependencyOnHandlerEdge(BlockID block, SummaryNode source)
    {
        const std::optional<Target>& handler = m_function.at(block).handler;
        addDependencyToParametersInMemoryAnd(source, handler ? exitBitsOf(handler->block) : bitOfThrow);
    }

    void summarizeCalleeWithOperandTypes(BlockID block, ValueID call, const EffectDescription& description)
    {
        const Program& program = m_context.program;
        const Value& value = m_function.at(call);
        std::span<const ValueID> operands = value.variadicOperands();
        Function callee(program.at(value.function()));
        Vector<ValueID, 4> narrowedParameters;
        for (uint32_t index = 0; index < operands.size(); ++index) {
            ValueID parameter = callee.at(entryBlock).parameters[index];
            TypeID given = m_function.at(operands[index]).type;
            if (given == callee.at(parameter).type || !program.at(given).isSubsetOf(program.at(callee.at(parameter).type), program))
                continue;
            Value& narrowed = callee.edit(parameter);
            narrowed.type = given;
            narrowed.setExpected({ });
            if (DeclaredType* declared = callee.declaredEntryParameter(index))
                declared->establish(Proven::byPosition(given));
            narrowedParameters.append(parameter);
        }
        AnalysisContext context = m_analyses.context();
        context.id = value.function();
        FunctionAnalyses analyses(context, callee);
        // Values derived from a parameter follow it: a narrowed copy after a test, a join of two paths.
        ScratchTypeArena types(program.types);
        if (!narrowedParameters.isEmpty())
            narrowValueTypesBehindChanges(TypePropagationInputs { program, value.function(), callee, analyses, types, context.build }, narrowedParameters.span());
        BodySummary entered = BodySummarizer(m_context, value.function(), analyses, nullptr, m_readAs == ReadAs::ItselfAndCalleesByOperandTypes ? ReadAs::GenericVersionGivenTypes : ReadAs::OutlinedBodyGivenTypes).run();

        addToExits(block, description.own.normal, description.own.handler);
        addToExits(block, entered.onReturn, entered.onThrow);
        m_summary.followsFieldsOfOperands |= entered.followsFieldsOfOperands;
        if (entered.unknownCodeWitness && !m_summary.unknownCodeWitness)
            m_summary.unknownCodeWitness = { m_id, call };
        for (uint32_t index = 0; index < operands.size(); ++index)
            addMemoryOfBase(block, operands[index], entered.parameters[index].memory, true);
        for (const SummaryDependency& dependency : entered.dependencies) {
            switch (dependency.target.part) {
            case SummaryPart::OnReturn:
                addDependencyOnNormalEdge(block, dependency.source);
                break;
            case SummaryPart::OnThrow:
                addDependencyOnHandlerEdge(block, dependency.source);
                break;
            case SummaryPart::Parameter:
                addBaseDependency(block, operands[dependency.target.parameter], dependency.source);
                break;
            case SummaryPart::UnknownCode:
                RELEASE_ASSERT_NOT_REACHED();
            }
        }
    }

    bool addCalledCode(BlockID block, const EffectDescription& description, ValueID entry)
    {
        if (!isFirstToRunAt(placeOfExits(block), description))
            return true;
        if (description.callsUnknownCode) {
            addDependencyToExits(block, SummaryNode::unknownCode(), SummaryNode::unknownCode());
            if (!m_summary.unknownCodeWitness)
                m_summary.unknownCodeWitness = { m_id, entry };
        }
        for (FunctionID id : description.functions) {
            addDependencyToExits(block, SummaryNode::onReturn(id), SummaryNode::onThrow(id));
            uint32_t numberOfParameters = m_context.program.at(id).numberOfEntryParameters();
            for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter)
                addDependencyToExits(block, SummaryNode::parameterOf(id, parameter), SummaryNode::parameterOf(id, parameter));
        }
        return description.callsUnknownCode || !description.functions.isEmpty();
    }

    void addConfinedEffects(BlockID block, const Value& value, const EffectsOnEdges& own)
    {
        EffectsOnEdges beyond = own;
        beyond.normal.writes.remove(perObjectHeaps);
        beyond.handler.writes.remove(perObjectHeaps);
        addToExits(block, beyond.normal, beyond.handler);

        Effects onAnyEdge = own.onAnyEdge();
        OperandMemory memory { onAnyEdge.reads.restrictedTo(perObjectHeaps), onAnyEdge.writes.restrictedTo(perObjectHeaps) };
        uint32_t numberOfNamedOperands = numberOfNamedOperandsOf(value.opcode);
        value.forEachOperand([&](ValueID operand, uint32_t index) {
            if (!isWrittenOperand(value.opcode, std::min(index, numberOfNamedOperands)))
                return;
            if (!mayBeReference(m_context.program.typeOf(m_function, operand)))
                return;
            addMemoryOfBase(block, operand, memory, own.mayThrow());
        });
    }

    void summarizeValue(BlockID block, ValueID id)
    {
        const Program& program = m_context.program;
        const Value& value = m_function.at(id);
        if (hasEmptyDescription(value.opcode)) {
            RELEASE_ASSERT(!m_analyses.context().build.options().verifiesAfterEveryPass || isEmpty(describeEffects(program, m_function, value)));
            if (m_references)
                m_references->read(value, m_emptyDescription);
            return;
        }
        bool isGivenTypes = m_readAs == ReadAs::GenericVersionGivenTypes || m_readAs == ReadAs::OutlinedBodyGivenTypes;
        bool isUnmatchedCall = value.opcode == Opcode::CallBuiltin && value.builtinCallForm() == BuiltinCallForm::AnyReceiver;
        AOT_THRESHOLD(size_t, mostFunctionsFollowed, 64);
        // More callees than are followed count as unknown code, whatever they do. n calls of one set of n callees do not go through it to find that out.
        CalleeSetID longSet = longCalleeSetRunBy(program, value);
        bool hasTooManyCallees = longSet && program.at(longSet).written().functions.size() > mostFunctionsFollowed;
        auto describe = [&] {
            return isGivenTypes && isUnmatchedCall ? describeUnmatchedCallByOperandTypes(program, m_function, value) : describeEffects(program, m_function, value);
        };
        EffectDescription description = hasTooManyCallees ? describeCodeRunBesideLongCalleeSet(program, value, longSet) : describe();
        m_summary.followsFieldsOfOperands |= description.followsFieldsOfOperands;
        if (m_references)
            m_references->read(value, description);
        if (value.opcode == Opcode::Check && !program.typeOf(m_function, value.valueOperand()).isSubsetOf(program.at(value.expectedType().predicted()), program))
            description.callsUnknownCode = true;
        auto countAsUnknownCode = [](EffectDescription& description) {
            description.functions.clear();
            description.callsUnknownCode = true;
            description.leavesOutOwnEffects = false;
            description.addOwn(Effects::unknownCode());
        };
        if (hasTooManyCallees || description.functions.size() > mostFunctionsFollowed)
            countAsUnknownCode(description);
        if (hasTooManyCallees && m_analyses.context().build.options().verifiesAfterEveryPass) [[unlikely]] {
            EffectDescription whole = describe();
            RELEASE_ASSERT(whole.functions.size() > mostFunctionsFollowed);
            countAsUnknownCode(whole);
            RELEASE_ASSERT(whole == description);
        }
        if (isReadWithOperandTypes(block, value)) [[unlikely]] {
            // The generic version is the same function, entered before any effect with what this one was entered with: it does what the code did that stood here.
            // Every rewrite of that code was checked to add no effect, so the row that the callers read still bounds it. The text of the generic version says more: it calls by name what is resolved here.
            if (m_readAs == ReadAs::ItselfAndCalleesByOperandTypes && m_function.effectsBeforeRestart) {
                addToExits(block, description.own.normal, description.own.handler);
                addToExits(block, m_function.effectsBeforeRestart->normal, m_function.effectsBeforeRestart->handler);
                return;
            }
            summarizeCalleeWithOperandTypes(block, id, description);
            return;
        }
        if (effectRuleOf(value.opcode) == EffectRule::Call && !maySuspend(value.opcode) && !hasTooManyCallees) {
            CalleeList callees = calleesThatBoundEffectsOf(program, value);
            if (std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.functionsAlone(); functions && runsOnlyCallees(description, *functions)) {
                summarizeCall(block, value, *functions, description);
                return;
            }
        }
        bool runsOtherCode = addCalledCode(block, description, id);
        const EffectsOnEdges& own = description.own;
        if (runsOtherCode || confinementOf(value.opcode) != OperandConfinement::WritesOnlyNamedOperands) {
            addToExits(block, own.normal, own.handler);
            addMemoryOfOperands(value, description);
            return;
        }
        if (!isPure(own.normal) || !isPure(own.handler))
            addConfinedEffects(block, value, own);
    }

    void summarizeTerminator(BlockID block)
    {
        const Terminator& terminator = m_function.at(block).terminator;
        if (hasEmptyDescription(terminator.kind)) {
            RELEASE_ASSERT(!m_analyses.context().build.options().verifiesAfterEveryPass || isEmpty(describeEffects(m_context.program, m_function, terminator)));
            if (m_references)
                m_references->read(terminator, m_emptyDescription);
            return;
        }
        EffectDescription description = describeEffects(m_context.program, m_function, terminator);
        m_summary.followsFieldsOfOperands |= description.followsFieldsOfOperands;
        if (m_references)
            m_references->read(terminator, description);
        addCalledCode(block, description, ValueID());
        addToExits(block, description.own.normal, description.own.handler);
        addMemoryOfOperands(terminator, description);
    }

    const EffectContext& m_context;
    FunctionID m_id;
    FunctionAnalyses& m_analyses;
    BodyReferenceCollector* m_references;
    const Function& m_function;
    struct CodeRunAtPlace {
        uint32_t place;
        bool callsUnknownCode;
        Vector<FunctionID> functions;
    };

    ExitReachability m_exits;
    bool m_maySuspend;
    ReadAs m_readAs;
    FunctionID m_genericVersionEntered;
    const EffectDescription m_emptyDescription;
    BodySummary m_summary;
    Vector<uint32_t, 4> m_parametersInMemory;
    std::array<Effects, 4> m_gatheredForExits { Effects::none(), Effects::none(), Effects::none(), Effects::none() };
    Effects m_gatheredForThrowAlone { Effects::none() };
    const uint32_t m_wordsInRow;
    Vector<uint64_t, 1> m_parametersInMemoryAsTargets;
    Vector<SummaryNode, 32> m_sources;
    Vector<uint64_t, 32> m_targets;
    uint32_t m_positionOfLatestSource { 0 };
    HashedIndex m_indexOfSources;
    Vector<CodeRunAtPlace> m_codeRunAtPlaces;
    HashedIndex m_indexOfCodeRunAtPlaces;
};

} // anonymous namespace

static BodySummary summarizeDescription(const Program& program, FunctionID id, const Function& function, const EffectDescription& description)
{
    BodySummary summary;
    summary.onReturn = description.own.normal;
    summary.onThrow = description.own.handler;
    summary.followsFieldsOfOperands = description.followsFieldsOfOperands;
    summary.onReturn.reads.merge(LocationMasks { perObjectHeaps });
    if (description.own.mayThrow())
        summary.onThrow.reads.merge(LocationMasks { perObjectHeaps });

    uint32_t numberOfParameters = function.numberOfEntryParameters();
    ParameterSummary parameter;
    parameter.memory.writes = description.own.onAnyEdge().writes.restrictedTo(perObjectHeaps);
    summary.parameters.fill(parameter, numberOfParameters);

    auto addSource = [&](SummaryNode source) {
        summary.dependencies.append(SummaryDependency { source, SummaryNode::onReturn(id) });
        summary.dependencies.append(SummaryDependency { source, SummaryNode::onThrow(id) });
        for (uint32_t index = 0; index < numberOfParameters; ++index)
            summary.dependencies.append(SummaryDependency { source, SummaryNode::parameterOf(id, index) });
    };
    if (description.callsUnknownCode) {
        addSource(SummaryNode::unknownCode());
        summary.unknownCodeWitness = { id, ValueID() };
    }
    for (FunctionID callee : description.functions) {
        addSource(SummaryNode::onReturn(callee));
        addSource(SummaryNode::onThrow(callee));
        uint32_t numberOfCalleeParameters = program.at(callee).numberOfEntryParameters();
        for (uint32_t index = 0; index < numberOfCalleeParameters; ++index)
            addSource(SummaryNode::parameterOf(callee, index));
    }
    return summary;
}

BodySummary summarizeBodyLocally(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses, BodyReferenceCollector* references, CalleesAreRead calleesAreRead)
{
    const Function& function = analyses.function();
    if (isSummarizedByRole(function.role)) {
        EffectDescription description;
        if (describeByRole(context.program, function, description))
            return summarizeDescription(context.program, id, function, description);
    }
    if (function.hasBody())
        return BodySummarizer(context, id, analyses, references, calleesAreRead == CalleesAreRead::AsTheyAre ? BodySummarizer::ReadAs::Itself : BodySummarizer::ReadAs::ItselfAndCalleesByOperandTypes).run();

    BodySummary summary;
    summary.onReturn = Effects::unknownCode();
    summary.onThrow = Effects::unknownCode();
    summary.parameters = Vector<ParameterSummary>(function.numberOfEntryParameters());
    summary.unknownCodeWitness = { id, ValueID() };
    return summary;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
