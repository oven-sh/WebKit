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
#include "AOTIR.h"
#include "AOTSort.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
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

static void removeRepeatedDependencies(Vector<SummaryDependency>& dependencies)
{
    AOT::sort(dependencies, [](const SummaryDependency& a, const SummaryDependency& b) {
        return std::pair(sortKeyOf(a.source), sortKeyOf(a.target)) < std::pair(sortKeyOf(b.source), sortKeyOf(b.target));
    });
    removeRepeatedElements(dependencies, [](const SummaryDependency& a, const SummaryDependency& b) {
        return a.source == b.source && a.target == b.target;
    });
}

static std::optional<uint32_t> mappedEntryParameter(const Function& callee, const Value& call, uint32_t operandIndex)
{
    if (Value::opcodesWithCallees.contains(call.opcode))
        return entryParameterOf(callee, call, operandIndex);
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

static bool isEmpty(const EffectDescription& description)
{
    return description.own == EffectsOnEdges::none() && description.functions.isEmpty() && description.slowPathFunctions.isEmpty() && !description.callsUnknownCode && !description.followsFieldsOfOperands;
}

namespace {

class BodySummarizer {
public:
    BodySummarizer(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses, BodyReferenceCollector* references)
        : m_context(context)
        , m_id(id)
        , m_analyses(analyses)
        , m_references(references)
        , m_function(analyses.function())
        , m_exits(computeExitReachability(m_function))
        , m_maySuspend(maySuspend(analyses))
    {
    }

    BodySummary run()
    {
        uint32_t numberOfParameters = m_function.numberOfEntryParameters();
        m_summary.parameters.reserveInitialCapacity(numberOfParameters);
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter) {
            m_summary.parameters.append(ParameterSummary { summarizeEntryParameter(m_analyses, parameter), OperandMemory::none() });
            if (mayBeReadOutOfMemory(parameter))
                m_parametersInMemory.append(parameter);
        }
        if (m_function.isUnsplitCoroutine())
            addDependency(SummaryNode::onThrow(m_id), SummaryNode::onReturn(m_id));
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
            if (m_summary.onReturn.mayThrow || m_summary.onThrow.mayThrow || !m_summary.dependencies.isEmpty())
                m_summary.onReturn.merge(declaredBoundOf(Opcode::RejectPromise));
            addDependency(SummaryNode::onThrow(m_id), SummaryNode::onReturn(m_id));
        }
        removeRepeatedDependencies(m_summary.dependencies);
        moveToExactBuffer(m_summary.dependencies);
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
        if (handler.isPure())
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

    void addDependency(SummaryNode source, SummaryNode target)
    {
        SummaryDependency dependency { source, target };
        uint64_t mixed = (sortKeyOf(source) * 0x9e3779b97f4a7c15ull ^ sortKeyOf(target)) * 0xff51afd7ed558ccdull;
        uint32_t hash = static_cast<uint32_t>(mixed >> 32);
        bool isKnown = !!m_indexOfDependencies.find(hash, [&](uint32_t position) {
            return m_summary.dependencies[position] == dependency;
        });
        if (isKnown)
            return;
        m_indexOfDependencies.add(hash, m_summary.dependencies.size());
        m_summary.dependencies.append(dependency);
    }

    void addDependencyToExitsOf(BlockID block, SummaryNode source)
    {
        bool reachesThrow = m_exits.reachesThrow.get(block.index());
        if (m_exits.reachesReturn.get(block.index()) || !reachesThrow)
            addDependency(source, SummaryNode::onReturn(m_id));
        if (reachesThrow)
            addDependency(source, SummaryNode::onThrow(m_id));
    }

    void addDependencyToParametersInMemory(SummaryNode source)
    {
        for (uint32_t parameter : m_parametersInMemory)
            addDependency(source, SummaryNode::parameterOf(m_id, parameter));
    }

    void addDependencyToExits(BlockID block, SummaryNode normalSource, SummaryNode handlerSource)
    {
        addDependencyToParametersInMemory(normalSource);
        addDependencyToParametersInMemory(handlerSource);
        addDependencyToExitsOf(block, normalSource);
        if (const std::optional<Target>& target = m_function.at(block).handler)
            addDependencyToExitsOf(target->block, handlerSource);
        else
            addDependency(handlerSource, SummaryNode::onThrow(m_id));
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
                addDependency(calleeParameter, SummaryNode::parameterOf(m_id, object.entryParameter));
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
                SummaryNode target = SummaryNode::parameterOf(m_id, entryParameter);
                m_summary.parameters[entryParameter].memory.merge(memory);
                if (!isFirstToRunAt(placeOfParameter(entryParameter), description))
                    return;
                if (description.callsUnknownCode)
                    addDependency(SummaryNode::unknownCode(), target);
                for (FunctionID id : description.functions) {
                    uint32_t numberOfParameters = m_context.program.at(id).numberOfEntryParameters();
                    for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter)
                        addDependency(SummaryNode::parameterOf(id, parameter), target);
                }
            });
        });
    }

    void addArgumentWithoutParameter(ValueID argument, FunctionID callee)
    {
        forEachEntryParameterOf(argument, [&](uint32_t entryParameter) {
            addDependency(SummaryNode::onReturn(callee), SummaryNode::parameterOf(m_id, entryParameter));
            addDependency(SummaryNode::onThrow(callee), SummaryNode::parameterOf(m_id, entryParameter));
        });
    }

    static bool runsOnlyCallees(const EffectDescription& description, const CalleeList& callees)
    {
        if (description.callsUnknownCode)
            return false;
        if (std::ranges::is_sorted(callees.functions))
            return std::ranges::includes(callees.functions, description.functions);
        return std::ranges::all_of(description.functions, [&](FunctionID function) {
            return callees.functions.contains(function);
        });
    }

    void summarizeCall(BlockID block, const Value& value, const CalleeList& callees, const EffectDescription& description)
    {
        addToExits(block, description.own.normal, description.own.handler);
        for (FunctionID id : callees.functions) {
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
        EffectDescription description = describeEffects(program, m_function, value);
        m_summary.followsFieldsOfOperands |= description.followsFieldsOfOperands;
        if (m_references)
            m_references->read(value, description);
        if (value.opcode == Opcode::Check && !program.typeOf(m_function, value.valueOperand()).isSubsetOf(program.at(value.castType()), program))
            description.callsUnknownCode = true;
        AOT_THRESHOLD(size_t, mostFunctionsFollowed, 64);
        if (description.functions.size() > mostFunctionsFollowed) {
            description.functions.clear();
            description.callsUnknownCode = true;
            description.addOwn(Effects::unknownCode());
        }
        if (effectRuleOf(value.opcode) == EffectRule::Call && !maySuspend(value.opcode)) {
            CalleeList callees = calleesThatBoundEffectsOf(program, value);
            if (callees.isComplete && callees.builtins.isEmpty() && runsOnlyCallees(description, callees)) {
                summarizeCall(block, value, callees, description);
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
        if (own != EffectsOnEdges::none())
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
    const EffectDescription m_emptyDescription;
    BodySummary m_summary;
    Vector<uint32_t, 4> m_parametersInMemory;
    std::array<Effects, 4> m_gatheredForExits { Effects::none(), Effects::none(), Effects::none(), Effects::none() };
    Effects m_gatheredForThrowAlone { Effects::none() };
    HashedIndex m_indexOfDependencies;
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

BodySummary summarizeBodyLocally(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses, BodyReferenceCollector* references)
{
    const Function& function = analyses.function();
    if (isSummarizedByRole(function.role)) {
        EffectDescription description;
        if (describeByRole(context.program, function, description))
            return summarizeDescription(context.program, id, function, description);
    }
    if (function.hasBody())
        return BodySummarizer(context, id, analyses, references).run();

    BodySummary summary;
    summary.onReturn = Effects::unknownCode();
    summary.onThrow = Effects::unknownCode();
    summary.parameters = Vector<ParameterSummary>(function.numberOfEntryParameters());
    summary.unknownCodeWitness = { id, ValueID() };
    return summary;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
