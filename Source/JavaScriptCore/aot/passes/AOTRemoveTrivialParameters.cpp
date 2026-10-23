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
#include "AOTKnownValue.h"
#include "AOTResultRules.h"
#include "AOTParameterReplacement.h"
#include "AOTVector.h"
#include <optional>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PendingUpcast {
    ValueID parameter;
    ValueID argument;
    TypeID type;
    TypeID expected;
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
        , m_positionOfParameter(FillWith { }, context.function.values.size(), noPosition)
    {
        for (uint32_t index = 0; index < m_parameters.size(); ++index)
            m_positionOfParameter[m_parameters[index].parameter.index()] = index;
    }

    size_t run()
    {
        bool hasCandidates = findCandidates();
        return hasCandidates || hasParameterWithOneValue() ? replaceParameters() : 0;
    }

private:
    KnownValue onlyValueOf(const Block& block, ValueID parameter) const
    {
        if (block.handler || block.subject == parameter)
            return { };
        KnownValue value = knownValueOfType(m_context.program, m_context.types.at(m_function.at(parameter).type));
        switch (value.kind()) {
        case KnownValue::Kind::Undefined:
        case KnownValue::Kind::Null:
        case KnownValue::Kind::Boolean:
        case KnownValue::Kind::Integer:
            return value;
        default:
            return { };
        }
    }

    bool hasParameterWithOneValue() const
    {
        return std::ranges::any_of(m_parameters, [&](const ParameterPosition& position) {
            return onlyValueOf(m_function.at(position.block), position.parameter).isKnown();
        });
    }

    void becomeConstant(ValueID id, KnownValue constant)
    {
        Value& value = m_function.edit(id);
        switch (constant.kind()) {
        case KnownValue::Kind::Undefined:
            value.become(Opcode::Undefined);
            break;
        case KnownValue::Kind::Null:
            value.become(Opcode::Null);
            break;
        case KnownValue::Kind::Boolean:
            value.become(Opcode::Boolean);
            value.setBoolean(constant.asBoolean());
            break;
        case KnownValue::Kind::Integer:
            value.become(Opcode::Integer);
            value.setInteger(constant.asInteger());
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        value.type = resultTypeOf(m_context.program, m_function, value, m_context.types);
        value.setExpected({ });
    }

    ValueID leaderOf(ValueID value) const { return m_leaders[m_candidates.resolve(value).index()]; }

    ValueID computeLeader(const ParameterPosition& position) const
    {
        ValueID only;
        bool receivesEdgeResult = false;
        for (Edge edge : m_cfg.incomingEdges(position.block)) {
            const Arguments& arguments = targetOf(m_function, edge).arguments;
            bool isExplicitArgument = position.index < arguments.size();
            ValueID passed = isExplicitArgument ? arguments[position.index] : valuePassedOn(m_function, edge, position.index);
            if (!passed)
                return position.parameter;
            receivesEdgeResult |= !isExplicitArgument;

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
        if (value == position.parameter)
            return value;
        const Type& joined = m_context.types.at(m_function.at(position.parameter).type);
        auto fitsJoin = [&](ValueID candidate) {
            return m_context.types.at(m_function.at(candidate).type).isSubsetOf(joined, m_context.program);
        };
        if (fitsJoin(value))
            return value;
        ValueID narrowed = findNarrowedValueOfEveryEdge(position);
        return narrowed && fitsJoin(narrowed) ? narrowed : position.parameter;
    }

    ValueID sourceOfNarrowedValue(ValueID value) const
    {
        const Value& definition = m_function.at(value);
        if (isCast(definition.opcode))
            return m_candidates.resolve(definition.operand(0));
        uint32_t index = m_positionOfParameter[value.index()];
        if (index == noPosition)
            return { };
        const ParameterPosition& position = m_parameters[index];
        std::optional<Edge> edge = m_cfg.singleIncomingEdge(position.block);
        if (!edge)
            return { };
        ValueID passed = valuePassedOn(m_function, *edge, position.index);
        return passed ? m_candidates.resolve(passed) : ValueID();
    }

    ValueID findNarrowedValueOfEveryEdge(const ParameterPosition& position) const
    {
        Vector<ValueID, 8> common;
        bool hasCommon = false;
        for (Edge edge : m_cfg.incomingEdges(position.block)) {
            Vector<ValueID, 8> sources;
            for (ValueID value = m_candidates.resolve(valuePassedOn(m_function, edge, position.index)); value; value = sourceOfNarrowedValue(value))
                sources.append(value);
            if (sources.contains(position.parameter))
                continue;
            if (!std::exchange(hasCommon, true)) {
                common = WTF::move(sources);
                continue;
            }
            common.removeAllMatching([&](ValueID value) {
                return !sources.contains(value);
            });
        }
        return common.isEmpty() ? ValueID() : common.first();
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
        if (numberOfRemovedParameters)
            m_analyses.didChangeValues();
        return numberOfRemovedParameters;
    }

    size_t replaceParametersOf(BlockID id, IRBuilder& builder, ValueSubstitution& replacements)
    {
        const Block& block = m_function.at(id);
        BitVector removedIndexes;
        Vector<PendingUpcast, 4> upcasts;
        Vector<std::pair<ValueID, KnownValue>, 4> constants;
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            ValueID parameter = block.parameters[index];
            ValueID argument = m_candidates.resolve(parameter);
            if (argument == parameter) {
                if (KnownValue constant = onlyValueOf(block, parameter); constant.isKnown() && m_positionOfParameter[parameter.index()] != noPosition) {
                    removedIndexes.set(index);
                    constants.append({ parameter, constant });
                }
                continue;
            }
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
            upcasts.append({ parameter, argument, value.type, value.expected() });
        }
        builder.removeParameters(id, removedIndexes);
        if (constants.isEmpty() && upcasts.isEmpty())
            return removedIndexes.bitCount();

        uint32_t index = indexAfterCaughtValues(m_function, block);
        BlockValues& values = m_function.edit(id, BlockEdit::Values).values;
        for (auto [parameter, constant] : constants) {
            becomeConstant(parameter, constant);
            values.insert(index++, parameter);
        }
        for (const PendingUpcast& upcast : upcasts) {
            Value& value = m_function.edit(upcast.parameter, ValueEdit::Type);
            value.type = upcast.type;
            value.setExpected(upcast.expected);
            becomeUpcast(m_function, upcast.parameter, upcast.argument);
            values.insert(index++, upcast.parameter);
            // The subject is named at the head of the block, above the Upcast.
            if (block.subject == upcast.parameter)
                m_function.edit(id, BlockEdit::Rest).subject = upcast.argument;
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
    static constexpr uint32_t noPosition = UINT32_MAX;
    Vector<uint32_t> m_positionOfParameter;
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

size_t removeTrivialParametersAmong(FunctionPassContext& context, Vector<ParameterPosition>&& candidatesInReversePostOrder)
{
    if (candidatesInReversePostOrder.isEmpty())
        return 0;
    return TrivialParameterRemover(context, WTF::move(candidatesInReversePostOrder)).run();
}

PassOutcome removeTrivialParameters(FunctionPassContext& context)
{
    size_t numberOfRemovedParameters = removeTrivialParametersAmong(context, collectMergePointParameters(context.function, context.analyses.cfg()));
    if (!numberOfRemovedParameters)
        return PassOutcome::Unchanged;
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
