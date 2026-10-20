/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsUnseenCode.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

static bool isRaised(const ValueFlowSolution& solution, ValueFlowHostNode flag)
{
    return solution.sets().contains(solution.solver().valueOf(solution.bodies().nodes().ofHost(flag)), ValueFlowUniverse::unknownObject());
}

bool unseenCodeCanRun(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::UnseenCodeRuns);
}

bool modulesAreLoadedByName(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::ModulesAreRequiredByName) || isRaised(solution, ValueFlowHostNode::ModulesAreImportedByName);
}

static bool isInSeenCode(const ValueFlowSolution& solution, FunctionID id)
{
    const Function& function = solution.program().at(id);
    if (function.role != FunctionRole::Library && function.role != FunctionRole::Model)
        return true;
    const ValueFlowBodies& bodies = solution.bodies();
    if (solution.sets().contains(solution.solver().valueOf(bodies.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)), bodies.universe().closureOf(id)))
        return true;
    return function.module && solution.sets().contains(solution.solver().valueOf(bodies.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode)), bodies.universe().namespaceOf(function.module));
}

static bool callsValue(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Call:
    case Opcode::CallList:
    case Opcode::Construct:
    case Opcode::ConstructList:
    case Opcode::SuperConstruct:
    case Opcode::SuperConstructList:
        return true;
    default:
        return false;
    }
}

static bool staysOutsideImage(const Program& program, ModuleID module)
{
    return program.at(module).kind == ModuleKind::External && program.at(module).externalKind == ExternalKind::UncompiledCode;
}

Vector<UnseenCodeRecord> unseenCodeRecordsOf(ValueFlowSolution& solution)
{
    const Program& program = solution.program();
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    Vector<UnseenCodeRecord> records;

    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        FunctionID id(index);
        if (!solution.solver().isReached(id))
            continue;

        const Function& function = program.at(id);
        bool isSeenCode = isInSeenCode(solution, id);
        auto add = [&](UnseenCodeCause cause, BuiltinID builtin, ValueID site) {
            Witness witness = site ? Witness { index, site.index(), function.locationOf(site) } : Witness { index, Wire::none, function.declarationLocation() };
            records.append(UnseenCodeRecord { cause, builtin ? builtin.index() : Wire::none, witness });
        };

        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind == ValueFlowStatementKind::ModuleEvaluation) {
                for (const ModuleRequest& request : program.at(function.module).requests) {
                    if (staysOutsideImage(program, request.target))
                        add(UnseenCodeCause::ImportsUncompiledModule, { }, { });
                }
                continue;
            }
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;

            switch (statement.opcode) {
            case Opcode::DirectEval:
                if (isSeenCode)
                    add(UnseenCodeCause::EvaluatesText, { }, statement.site);
                continue;
            case Opcode::ImportDynamic:
            case Opcode::RequireDynamic:
                if (function.role == FunctionRole::Source)
                    add(UnseenCodeCause::LoadsModuleByName, { }, statement.site);
                continue;
            case Opcode::ModuleNamespace:
            case Opcode::ImportModule:
            case Opcode::RequireModule:
                if (staysOutsideImage(program, function.at(statement.site).module()))
                    add(UnseenCodeCause::ImportsUncompiledModule, { }, statement.site);
                continue;
            default:
                break;
            }
            if (!callsValue(statement.opcode) || !isSeenCode)
                continue;
            solution.sets().forEachMember(solution.valueOf(bodies.operandsOf(statement)[0]), [&](ValueFlowObjectID callee) {
                // What a built-in has made also knows that built-in.
                if (!solution.sets().contains(universe.builtinFunctions(), callee))
                    return;
                BuiltinID builtin = builtins.canonicalID(universe.builtinOf(callee));
                if (std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin))
                    add(*cause, builtin, statement.site);
            });
        }
    }
    // Whoever asks whether a require with a constant name still answers its module looks for the cache in a record.
    if (isRaised(solution, ValueFlowHostNode::AnswersOfReplacedLoader))
        records.append(UnseenCodeRecord { UnseenCodeCause::LoadsModuleByName, Builtins::ModuleCache.index(), { } });
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
