/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTParameterReplacement.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PendingUpcast {
    ValueID parameter;
    ValueID argument;
    TypeID type;
    TypeID written;
    TypeID prediction;
};

class TrivialParameterRemover {
public:
    TrivialParameterRemover(FunctionPassContext& context, Vector<ParameterPosition>&& parameters)
        : m_context(context)
        , m_function(context.function)
        , m_analyses(context.analyses)
        , m_cfg(context.analyses.cfg())
        , m_parameters(WTF::move(parameters))
        , m_leaders(context.function.values.size(), [](size_t index) { return ValueID(index); })
        , m_candidates(context.function)
    {
    }

    size_t run()
    {
        return findCandidates() ? replaceParameters() : 0;
    }

private:
    ValueID leaderOf(ValueID value) const { return m_leaders[m_candidates.resolve(value).index()]; }

    ValueID computeLeader(const ParameterPosition& position) const
    {
        ValueID only;
        bool receivesEdgeResult = false;
        for (Edge edge : m_cfg.incomingEdges(position.block)) {
            ValueID passed = valuePassedOn(m_function, edge, position.index);
            if (!passed)
                return position.parameter;
            receivesEdgeResult |= position.index >= numberOfExplicitArguments(m_function, edge);

            ValueID leader = leaderOf(passed);
            if (!leader || leader == position.parameter || leader == only || isCastOf(m_function, m_candidates, leader, position.parameter))
                continue;
            if (only)
                return valueBehindCasts(position);
            only = leader;
        }
        if (receivesEdgeResult && only && !canReplaceParameter(m_function, only, position.parameter))
            return position.parameter;
        return only;
    }

    // The edges pass one value, some of them under a stronger type from a cast or a test, and the join has forgotten what those proved.
    ValueID valueBehindCasts(const ParameterPosition& position) const
    {
        ValueID value = m_analyses.identityOf(position.parameter);
        const Type& joined = m_context.types.at(m_function.at(position.parameter).type);
        return m_context.types.at(m_function.at(value).type).isSubsetOf(joined, m_context.program) ? value : position.parameter;
    }

    bool tryAssignLeaders(Vector<ParameterPosition>& undecided)
    {
        for (const ParameterPosition& position : undecided)
            m_leaders[position.parameter.index()] = ValueID();
        for (const ParameterPosition& position : undecided) {
            ValueID leader = computeLeader(position);
            m_leaders[position.parameter.index()] = leader ? leader : position.parameter;
        }

        Vector<ValueID, 8> refutedParameters;
        for (const ParameterPosition& position : undecided) {
            ValueID leader = m_leaders[position.parameter.index()];
            if (leader != position.parameter && computeLeader(position) != leader)
                refutedParameters.append(position.parameter);
        }
        for (ValueID parameter : refutedParameters)
            m_leaders[parameter.index()] = parameter;

        undecided.removeAllMatching([&](const ParameterPosition& position) {
            return m_leaders[position.parameter.index()] == position.parameter;
        });
        return refutedParameters.isEmpty();
    }

    bool findCandidates()
    {
        Vector<ParameterPosition> undecided = m_parameters;
        for (;;) {
            while (!tryAssignLeaders(undecided)) { }
            if (undecided.isEmpty())
                return !m_candidates.isEmpty();

            for (const ParameterPosition& position : undecided)
                m_candidates.replace(position.parameter, m_leaders[position.parameter.index()]);
            undecided = m_parameters;
            undecided.removeAllMatching([&](const ParameterPosition& position) {
                return m_candidates.resolve(position.parameter) != position.parameter;
            });
        }
    }

    size_t replaceParameters()
    {
        IRBuilder builder(m_analyses.context(), m_function, m_context.types, &m_analyses);
        ValueSubstitution replacements(m_function);
        size_t numberOfRemovedParameters = 0;
        for (BlockID block : m_cfg.reversePostOrder()) {
            if (block != m_cfg.root())
                numberOfRemovedParameters += replaceParametersOf(block, builder, replacements);
        }
        replacements.apply(m_function, &m_analyses);
        m_analyses.didChangeValues();
        return numberOfRemovedParameters;
    }

    size_t replaceParametersOf(BlockID id, IRBuilder& builder, ValueSubstitution& replacements)
    {
        Block& block = m_function.at(id);
        BitVector removedIndexes;
        Vector<PendingUpcast, 4> upcasts;
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            ValueID parameter = block.parameters[index];
            ValueID argument = m_candidates.resolve(parameter);
            if (argument == parameter)
                continue;
            bool canReplace = canReplaceParameter(m_function, argument, parameter);
            // A parameter dominates the handler of its block. An Upcast among the values of the block does not.
            if (!canReplace && block.handler)
                continue;
            removedIndexes.set(index);
            if (canReplace) {
                replacements.replace(parameter, argument);
                continue;
            }
            const Value& value = m_function.at(parameter);
            upcasts.append({ parameter, argument, value.type, value.written(), value.prediction() });
        }
        builder.removeParameters(id, removedIndexes);

        uint32_t index = indexAfterCaughtValues(m_function, block);
        for (const PendingUpcast& upcast : upcasts) {
            Value& value = m_function.at(upcast.parameter);
            value.type = upcast.type;
            value.setWritten(upcast.written);
            value.setPrediction(upcast.prediction);
            becomeUpcast(m_function, upcast.parameter, upcast.argument);
            block.values.insert(index++, upcast.parameter);
        }
        return removedIndexes.bitCount();
    }

    FunctionPassContext& m_context;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    Vector<ParameterPosition> m_parameters;
    Vector<ValueID> m_leaders;
    ValueSubstitution m_candidates;
};

} // anonymous namespace

static Vector<ParameterPosition> collectMergePointParameters(const Function& function, const CFG& cfg)
{
    Vector<ParameterPosition> parameters;
    for (BlockID block : cfg.reversePostOrder()) {
        if (block == cfg.root())
            continue;
        const CompactList<ValueID>& blockParameters = function.at(block).parameters;
        for (uint32_t index = 0; index < blockParameters.size(); ++index)
            parameters.append({ blockParameters[index], block, index });
    }
    return parameters;
}

size_t removeTrivialParameters(FunctionPassContext& context, Vector<ParameterPosition>&& candidatesInReversePostOrder)
{
    if (candidatesInReversePostOrder.isEmpty())
        return 0;
    return TrivialParameterRemover(context, WTF::move(candidatesInReversePostOrder)).run();
}

PassOutcome removeTrivialParameters(FunctionPassContext& context)
{
    size_t numberOfRemovedParameters = removeTrivialParameters(context, collectMergePointParameters(context.function, context.analyses.cfg()));
    return numberOfRemovedParameters ? PassOutcome::Changed : PassOutcome::Unchanged;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
