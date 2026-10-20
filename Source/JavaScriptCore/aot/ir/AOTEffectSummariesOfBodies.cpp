/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectSummariesInternal.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIR.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

using PredecessorLists = Vector<Vector<unsigned, 2>>;

template<typename Entry>
static bool rowMayThrow(Entry entry, const Effects& bound)
{
    return bound.mayThrow || effectRuleOf(entry) != EffectRule::Declared || mayRunOperandCode(entry);
}

static BitVector findBlocksReaching(const PredecessorLists& predecessors, Vector<unsigned>&& worklist)
{
    BitVector result(predecessors.size());
    for (unsigned block : worklist)
        result.quickSet(block);
    while (!worklist.isEmpty()) {
        for (unsigned predecessor : predecessors[worklist.takeLast()]) {
            if (!result.quickSet(predecessor))
                worklist.append(predecessor);
        }
    }
    return result;
}

ExitReachability computeExitReachability(const Function& function)
{
    unsigned numberOfBlocks = function.blocks.size();
    PredecessorLists predecessors(numberOfBlocks);
    Vector<unsigned> returningBlocks;
    Vector<unsigned> throwingBlocks;
    for (unsigned index = 0; index < numberOfBlocks; ++index) {
        const Block& block = function.blocks[index];
        bool reachesEnd = true;
        bool mayThrowInside = false;
        bool returns = false;
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            Effects bound = declaredBoundOf(value.opcode);
            reachesEnd = reachesEnd && value.type != TypeTable::never;
            mayThrowInside = mayThrowInside || rowMayThrow(value.opcode, bound);
            returns = returns || bound.maySuspend;
        }
        if (reachesEnd) {
            TerminatorKind kind = block.terminator.kind;
            mayThrowInside = mayThrowInside || rowMayThrow(kind, declaredBoundOf(kind));
            returns = returns || returnsToCaller(kind);
            for (const Target& target : block.terminator.targets)
                predecessors[target.block.index()].append(index);
        }
        if (returns)
            returningBlocks.append(index);
        if (mayThrowInside && block.handler)
            predecessors[block.handler->block.index()].append(index);
        if (mayThrowInside && !block.handler)
            throwingBlocks.append(index);
    }
    return {
        findBlocksReaching(predecessors, WTF::move(returningBlocks)),
        findBlocksReaching(predecessors, WTF::move(throwingBlocks)),
    };
}

static bool maySuspend(const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return maySuspend(function.at(id).opcode);
        });
    });
}

static std::tuple<SummaryPart, FunctionID, uint32_t> sortKeyOf(const SummaryNode& node)
{
    SummaryPart part = node.part;
    uint32_t parameter = node.parameter;
    return { part, node.function, parameter };
}

static void removeRepeatedDependencies(Vector<SummaryDependency>& dependencies)
{
    std::ranges::sort(dependencies, [](const SummaryDependency& a, const SummaryDependency& b) {
        return std::tuple(sortKeyOf(a.source), sortKeyOf(a.target)) < std::tuple(sortKeyOf(b.source), sortKeyOf(b.target));
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

namespace {

class BodySummarizer {
public:
    BodySummarizer(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses)
        : m_context(context)
        , m_id(id)
        , m_analyses(analyses)
        , m_function(analyses.function())
        , m_exits(computeExitReachability(m_function))
        , m_maySuspend(maySuspend(m_function))
    {
    }

    BodySummary run()
    {
        uint32_t numberOfParameters = m_function.numberOfEntryParameters();
        m_summary.parameters.reserveInitialCapacity(numberOfParameters);
        for (uint32_t parameter = 0; parameter < numberOfParameters; ++parameter)
            m_summary.parameters.append(ParameterSummary { summarizeEntryParameter(m_analyses, parameter), OperandMemory::none() });
        if (m_function.isUnsplitCoroutine())
            addDependency(SummaryNode::onThrow(m_id), SummaryNode::onReturn(m_id));
        for (unsigned index = 0; index < m_function.blocks.size(); ++index) {
            BlockID block(index);
            for (ValueID id : m_function.at(block).values)
                summarizeValue(block, id);
            summarizeTerminator(block);
        }
        removeRepeatedDependencies(m_summary.dependencies);
        return WTF::move(m_summary);
    }

private:
    void addToExitsOf(BlockID block, const Effects& effects)
    {
        if (m_exits.reachesReturn.get(block.index()))
            m_summary.onReturn.merge(effects);
        if (m_exits.reachesThrow.get(block.index()))
            m_summary.onThrow.merge(effects);
        m_summary.onReturn.mayThrow |= effects.mayThrow;
        m_summary.onReturn.mayCollect |= effects.mayCollect;
        m_summary.onReturn.mayBeTerminated |= effects.mayBeTerminated;
    }

    void addToExits(BlockID block, const Effects& normal, const Effects& handler)
    {
        addToExitsOf(block, normal);
        if (handler.isPure())
            return;
        if (const std::optional<Target>& target = m_function.at(block).handler)
            addToExitsOf(target->block, handler);
        else
            m_summary.onThrow.merge(handler);
    }

    void addDependency(SummaryNode source, SummaryNode target)
    {
        m_summary.dependencies.append(SummaryDependency { source, target });
    }

    void addDependencyToExitsOf(BlockID block, SummaryNode source)
    {
        bool reachesThrow = m_exits.reachesThrow.get(block.index());
        if (m_exits.reachesReturn.get(block.index()) || !reachesThrow)
            addDependency(source, SummaryNode::onReturn(m_id));
        if (reachesThrow)
            addDependency(source, SummaryNode::onThrow(m_id));
    }

    void addDependencyToExits(BlockID block, SummaryNode normalSource, SummaryNode handlerSource)
    {
        addDependencyToExitsOf(block, normalSource);
        if (const std::optional<Target>& target = m_function.at(block).handler)
            addDependencyToExitsOf(target->block, handlerSource);
        else
            addDependency(handlerSource, SummaryNode::onThrow(m_id));
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

    // The exits hold memory reached other than through an entry parameter, so a caller that alone holds an object it passes looks only at the memory of the parameter.
    // Nothing says which operand an unconfined entry writes, or which parameter of the code it runs takes which operand.
    void addMemoryOfOperands(const auto& entry, const EffectDescription& description)
    {
        Effects own = description.own.onAnyEdge();
        OperandMemory memory { own.reads.restrictedTo(perObjectHeaps), own.writes.restrictedTo(perObjectHeaps) };
        if (memory.writes.isEmpty() && !description.callsUnknownCode && description.functions.isEmpty())
            return;
        entry.forEachOperand([&](ValueID operand, uint32_t) {
            forEachEntryParameterOf(operand, [&](uint32_t entryParameter) {
                SummaryNode target = SummaryNode::parameterOf(m_id, entryParameter);
                m_summary.parameters[entryParameter].memory.merge(memory);
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

    // The callee reaches such an argument through its arguments object or its rest parameter.
    void addArgumentWithoutParameter(ValueID argument, FunctionID callee)
    {
        forEachEntryParameterOf(argument, [&](uint32_t entryParameter) {
            addDependency(SummaryNode::onReturn(callee), SummaryNode::parameterOf(m_id, entryParameter));
            addDependency(SummaryNode::onThrow(callee), SummaryNode::parameterOf(m_id, entryParameter));
        });
    }

    // A construction reads the prototype of new.target before it enters a callee, which may run a getter or a trap.
    static bool runsOnlyCallees(const EffectDescription& description, const CalleeList& callees)
    {
        return !description.callsUnknownCode && std::ranges::all_of(description.functions, [&](FunctionID function) {
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
        EffectDescription description = describeEffects(program, m_function, value);
        if (effectRuleOf(value.opcode) == EffectRule::Call && !maySuspend(value.opcode)) {
            CalleeList callees = calleesOfCall(program, value);
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
        EffectDescription description = describeEffects(m_context.program, m_function, terminator);
        addCalledCode(block, description, ValueID());
        addToExits(block, description.own.normal, description.own.handler);
        addMemoryOfOperands(terminator, description);
    }

    const EffectContext& m_context;
    FunctionID m_id;
    FunctionAnalyses& m_analyses;
    const Function& m_function;
    ExitReachability m_exits;
    bool m_maySuspend;
    BodySummary m_summary;
};

} // anonymous namespace

static BodySummary summarizeDescription(const Program& program, FunctionID id, const Function& function, const EffectDescription& description)
{
    BodySummary summary;
    summary.onReturn = description.own.normal;
    summary.onThrow = description.own.handler;
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

BodySummary summarizeBodyLocally(const EffectContext& context, FunctionID id, FunctionAnalyses& analyses)
{
    const Function& function = analyses.function();
    if (isSummarizedByRole(function.role)) {
        EffectDescription description;
        if (describeByRole(context.program, function, description))
            return summarizeDescription(context.program, id, function, description);
    }
    if (function.hasBody())
        return BodySummarizer(context, id, analyses).run();

    BodySummary summary;
    summary.onReturn = Effects::unknownCode();
    summary.onThrow = Effects::unknownCode();
    summary.parameters.grow(function.numberOfEntryParameters());
    summary.unknownCodeWitness = { id, ValueID() };
    return summary;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
