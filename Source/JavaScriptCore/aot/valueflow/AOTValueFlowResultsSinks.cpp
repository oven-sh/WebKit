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
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

void computeSinkOperandResults(const ValueFlowResultInputs& inputs, const ValueFlowStampedCalls& calls, FlowResultTables& tables)
{
    const Program& program = inputs.program;
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowUniverse& universe = inputs.universe();
    const ValueSets& sets = inputs.sets();

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
            bool mayCallOne = (callee.constant && sets.meet(callee.constant, callees)) || (callee.node && sets.meet(inputs.solver().possibleValuesOf(callee.node), callees));
            if (!mayCallOne)
                continue;

            Vector<uint32_t, 2> arguments;
            ValueSetID held = inputs.possibleValuesOf(maker, callee);
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

    AOT_THRESHOLD(unsigned, maxValuesFollowed, 4096);
    struct DirectCall {
        FunctionID callee;
        FunctionID caller;
        ValueID call;

        auto operator<=>(const DirectCall&) const = default;
    };
    SinkCandidates candidates;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> visited;
    Vector<std::pair<FunctionID, ValueID>, 16> worklist;
    bool hasIndexOfDirectCalls = false;
    Vector<DirectCall> directCalls;
    HashMap<uint32_t, std::unique_ptr<CFG>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> cfgs;

    auto visit = [&](FunctionID function, ValueID value) {
        if (!value) {
            candidates.mayBeUndefined = true;
            return;
        }
        if (visited.size() >= maxValuesFollowed) {
            candidates.isComplete = false;
            return;
        }
        if (visited.add(static_cast<uint64_t>(function.index()) << 32 | value.index()).isNewEntry)
            worklist.append({ function, value });
    };
    auto followEntryParameter = [&](FunctionID id, const Function& function, uint32_t entryParameter) {
        bool isEnteredByDirectCallsAlone = tables.functions.functions[id.index()].callers == CallerState::AllKnown
            && !calls.functionsInStamps.get(id.index()) && !calls.functionsEnteredWithoutStamp.get(id.index()) && !calls.functionsNamedByUnsolvedCode.get(id.index());
        if (function.entryParameterKind(entryParameter) != EntryParameter::Parameter || !isEnteredByDirectCallsAlone)
            candidates.isComplete = false;
        if (function.entryParameterKind(entryParameter) != EntryParameter::Parameter)
            return;

        if (!std::exchange(hasIndexOfDirectCalls, true)) {
            directCalls = collectInParallel<DirectCall>(inputs.loops(), program.functions.size(), [&](unsigned index, Vector<DirectCall>& found) {
                if (!inputs.isReached(FunctionID(index)) || !inputs.bodies().holdsAnyOf(FunctionID(index), notedOperations({ ValueFlowNotedOperation::CallDirect })))
                    return;
                const Function& caller = program.functions[index];
                for (uint32_t value = 0; value < caller.values.size(); ++value) {
                    if (caller.values[value].opcode == Opcode::CallDirect)
                        found.append({ caller.values[value].function(), FunctionID(index), ValueID(value) });
                }
            });
            parallelSort(inputs.loops(), directCalls.mutableSpan(), std::less<DirectCall> { });
        }
        for (const DirectCall& call : std::ranges::equal_range(directCalls, id, { }, &DirectCall::callee)) {
            std::span<const ValueID> passed = program.at(call.caller).at(call.call).variadicOperands();
            if (entryParameter < passed.size())
                visit(call.caller, passed[entryParameter]);
            else
                candidates.isComplete = false;
        }
    };
    auto followParameter = [&](FunctionID id, const Function& function, ValueID parameter) {
        for (uint32_t block = 0; block < function.blocks.size(); ++block) {
            size_t index = function.blocks[block].parameters.find(parameter);
            if (index == notFound)
                continue;
            if (BlockID(block) == entryBlock) {
                followEntryParameter(id, function, static_cast<uint32_t>(index));
                return;
            }
            const CFG& cfg = *cfgs.ensure(id.index(), [&] {
                return makeUnique<CFG>(function);
            }).iterator->value;
            for (Edge edge : cfg.incomingEdges(BlockID(block))) {
                IncomingValue incoming = incomingValueOf(function, edge, static_cast<uint32_t>(index));
                if (incoming.kind == IncomingValue::Kind::Argument)
                    visit(id, incoming.argument);
                else
                    candidates.isComplete = false;
            }
            return;
        }
        candidates.isComplete = false;
    };
    auto follow = [&](FunctionID id, ValueID value) {
        const Function& function = program.at(id);
        const Value& definition = function.at(value);
        switch (definition.opcode) {
        case Opcode::String:
            candidates.strings.append(definition.string());
            return;
        case Opcode::Undefined:
            candidates.mayBeUndefined = true;
            return;
        case Opcode::Parameter:
            followParameter(id, function, value);
            return;
        default:
            if (isCast(definition.opcode))
                visit(id, definition.operand(0));
            else
                candidates.isComplete = false;
            return;
        }
    };
    for (const Operand& operand : operands) {
        candidates = SinkCandidates { .strings = { }, .mayBeUndefined = false, .isComplete = true };
        visited.clear();
        worklist.shrink(0);
        visit(operand.function, operand.passed);
        while (!worklist.isEmpty()) {
            auto [function, value] = worklist.takeLast();
            follow(function, value);
        }
        AOT::sort(candidates.strings);
        removeRepeatedElements(candidates.strings);
        tables.sites.sinkOperands.append({ operand.call, operand.operandIndex, WTF::move(candidates) });
    }
    AOT::sort(tables.sites.sinkOperands, { }, [](const FlowSinkOperand& operand) {
        return std::tuple(operand.call, operand.operandIndex);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
