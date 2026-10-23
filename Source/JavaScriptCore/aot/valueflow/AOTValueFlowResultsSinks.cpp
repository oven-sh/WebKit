/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsInternal.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTFlowStamps.h"
#include "AOTSort.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// A set knows a string by its kind alone unless the text is a key.
class TextsThatArrive {
    WTF_MAKE_NONCOPYABLE(TextsThatArrive);
public:
    TextsThatArrive(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, const FlowResultTables& tables)
        : m_inputs(inputs)
        , m_calls(calls)
        , m_tables(tables)
    {
    }

    // `value` is none for an argument that the call does not pass.
    SinkCandidates at(FunctionID function, ValueID value)
    {
        m_candidates = SinkCandidates { .strings = { }, .mayBeUndefined = false, .isComplete = true };
        m_visited.clear();
        m_worklist.shrink(0);
        visit(function, value);
        while (!m_worklist.isEmpty()) {
            auto [currentFunction, currentValue] = m_worklist.takeLast();
            follow(currentFunction, currentValue);
        }
        AOT::sort(m_candidates.strings);
        removeRepeatedElements(m_candidates.strings);
        return WTF::move(m_candidates);
    }

private:
    static constexpr unsigned maxValuesFollowed = 4096;

    struct DirectCall {
        FunctionID callee;
        FunctionID caller;
        ValueID call;

        friend auto operator<=>(const DirectCall&, const DirectCall&) = default;
    };

    void visit(FunctionID function, ValueID value)
    {
        if (!value) {
            m_candidates.mayBeUndefined = true;
            return;
        }
        if (m_visited.size() >= maxValuesFollowed) {
            m_candidates.isComplete = false;
            return;
        }
        if (m_visited.add(static_cast<uint64_t>(function.index()) << 32 | value.index()).isNewEntry)
            m_worklist.append({ function, value });
    }

    void follow(FunctionID id, ValueID value)
    {
        const Function& function = m_inputs.program.at(id);
        const Value& definition = function.at(value);
        switch (definition.opcode) {
        case Opcode::String:
            m_candidates.strings.append(definition.string());
            return;
        case Opcode::Undefined:
            m_candidates.mayBeUndefined = true;
            return;
        case Opcode::Parameter:
            followParameter(id, function, value);
            return;
        default:
            if (isCast(definition.opcode))
                visit(id, definition.operand(0));
            else
                m_candidates.isComplete = false;
            return;
        }
    }

    void followParameter(FunctionID id, const Function& function, ValueID parameter)
    {
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            size_t index = function.blocks[block].parameters.find(parameter);
            if (index == notFound)
                continue;
            if (BlockID(block) == entryBlock) {
                followEntryParameter(id, function, static_cast<uint32_t>(index));
                return;
            }
            const CFG& cfg = *m_cfgs.ensure(id.index(), [&] {
                return makeUnique<CFG>(function);
            }).iterator->value;
            for (Edge edge : cfg.incomingEdges(BlockID(block))) {
                IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(index));
                if (incoming.kind == IncomingValue::Kind::Argument)
                    visit(id, incoming.argument);
                else
                    m_candidates.isComplete = false;
            }
            return;
        }
        m_candidates.isComplete = false;
    }

    // Every entry into the function is a CallDirect of solved code, which spells out each entry parameter.
    bool isEnteredByDirectCallsAlone(FunctionID function) const
    {
        return m_tables.functions.functions[function.index()].callers == CallerState::AllKnown
            && !m_calls.functionsInStamps.get(function.index()) && !m_calls.functionsEnteredWithoutStamp.get(function.index()) && !m_calls.functionsNamedByUnsolvedCode.get(function.index());
    }

    void followEntryParameter(FunctionID id, const Function& function, uint32_t entryParameter)
    {
        if (function.entryParameterKind(entryParameter) != EntryParameter::Parameter || !isEnteredByDirectCallsAlone(id))
            m_candidates.isComplete = false;
        if (function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
            return;

        indexDirectCalls();
        auto calls = std::ranges::equal_range(m_directCalls, id, { }, &DirectCall::callee);
        for (const DirectCall& call : calls) {
            std::span<const ValueID> operands = m_inputs.program.at(call.caller).at(call.call).variadicOperands();
            if (entryParameter < operands.size())
                visit(call.caller, operands[entryParameter]);
            else
                m_candidates.isComplete = false;
        }
    }

    void indexDirectCalls()
    {
        if (m_hasIndexOfDirectCalls)
            return;
        m_hasIndexOfDirectCalls = true;
        const Program& program = m_inputs.program;
        m_directCalls = collectInParallel<DirectCall>(m_inputs.loops(), program.functions.size(), [&](unsigned index, Vector<DirectCall>& calls) {
            if (!m_inputs.isReached(FunctionID(index)) || !m_inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::CallDirect })))
                return;
            const Function& function = program.functions[index];
            for (uint32_t value = 0; value < function.values.size(); ++value) {
                if (function.values[value].opcode == Opcode::CallDirect)
                    calls.append({ function.values[value].function(), FunctionID(index), ValueID(value) });
            }
        });
        parallelSort(m_inputs.loops(), m_directCalls.mutableSpan(), std::less<DirectCall> { });
    }

    const ValueFlowResultInputs& m_inputs;
    const ValueFlowStampedCalls& m_calls;
    const FlowResultTables& m_tables;
    SinkCandidates m_candidates;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_visited;
    Vector<std::pair<FunctionID, ValueID>, 16> m_worklist;
    bool m_hasIndexOfDirectCalls { false };
    Vector<DirectCall> m_directCalls;
    HashMap<uint32_t, std::unique_ptr<CFG>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_cfgs;
};

} // anonymous namespace

void computeSinkOperandResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();

    // By built-in: the arguments whose text decides what the call makes. Ascending.
    Vector<Vector<uint32_t, 2>> argumentsOfBuiltin(builtins.builtinCount());
    Vector<ValueFlowObjectID> builtinsWithSuchArguments;
    for (uint32_t index = 0; index < builtins.builtinCount(); ++index) {
        BuiltinID builtin(index);
        if (builtins.canonicalID(builtin) != builtin)
            continue;
        Vector<uint32_t, 2>& arguments = argumentsOfBuiltin[index];
        for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
            for (const BuiltinParameter& parameter : overload.parameters) {
                if (parameter.roles.contains(BuiltinOperandRole::ContentKey) && parameter.operand.kind() == BuiltinOperand::Kind::Argument)
                    arguments.appendIfNotContains(parameter.operand.argumentIndex());
            }
        }
        AOT::sort(arguments);
        if (!arguments.isEmpty())
            builtinsWithSuchArguments.append(universe.objectOf(builtin));
    }
    if (builtinsWithSuchArguments.isEmpty())
        return;

    struct Operand {
        AccessSiteID call;
        uint32_t operandIndex;
        FunctionID function;
        ValueID passed;
    };
    ValueSetMakers makers(inputs);
    ValueSetID callees = makers.of(0).of(builtinsWithSuchArguments.span());
    Vector<Operand> operands = collectWithMakers<Operand>(inputs, makers, program.functions.size(), [&](unsigned index, ValueSetMaker& maker, Vector<Operand>& found) {
        FunctionID id(index);
        if (!inputs.isReached(id))
            return;
        const Function& function = program.functions[index];
        Vector<AccessSiteID> accessSites;
        for (const ValueFlowStatement& statement : inputs.bodies().statementsOf(index)) {
            if (statement.kind != ValueFlowStatementKind::Operation || (statement.opcode != Opcode::Call && statement.opcode != Opcode::Construct))
                continue;
            const ValueFlowOperand& callee = inputs.bodies().operandsOf(statement)[0];
            bool mayCallOne = (callee.constant && sets.meet(callee.constant, callees)) || (callee.node && sets.meet(inputs.solver().valueOf(callee.node), callees));
            if (!mayCallOne)
                continue;

            // A callee that nobody knows lists thousands of members, and a handful of built-ins have such arguments.
            Vector<uint32_t, 2> arguments;
            ValueSetID held = inputs.valueOf(maker, callee);
            if (!ValueSets::isComplement(held)) {
                sets.forEachMember(maker.intersect(held, callees), [&](ValueFlowObjectID member) {
                    for (uint32_t argument : argumentsOfBuiltin[universe.builtinOf(member).index()])
                        arguments.appendIfNotContains(argument);
                });
            }
            if (accessSites.isEmpty())
                accessSites = accessSitesByValue(function, tables.application->accessSites, id);
            AccessSiteID accessSite = accessSites[statement.site.index()];
            if (!accessSite)
                continue;
            std::span<const ValueID> passed = function.at(statement.site).variadicOperands();
            for (uint32_t argument : arguments)
                found.append({ accessSite, argument, id, argument < passed.size() ? passed[argument] : ValueID() });
        }
    });
    TextsThatArrive texts(inputs, calls, tables);
    for (const Operand& operand : operands)
        tables.sites.sinkOperands.append({ operand.call, operand.operandIndex, texts.at(operand.function, operand.passed) });
    AOT::sort(tables.sites.sinkOperands, { }, [](const FlowSinkOperand& operand) {
        return std::tuple(operand.call, operand.operandIndex);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
