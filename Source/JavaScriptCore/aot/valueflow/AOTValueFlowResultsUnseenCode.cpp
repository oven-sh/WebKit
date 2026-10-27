/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsUnseenCode.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTValueFlowRulesModules.h"

namespace JSC { namespace AOT { namespace Typed {

Vector<FunctionID> functionsHeldButNotCalled(const ValueFlowSolution& solution)
{
    const ValueSets& sets = solution.sets();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueSetID held = solver.possibleValuesOf(solution.bodies().nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
    ValueSetID escaped = solver.possibleValuesOf(ValueFlowProgramNodes::escaped());

    BitVector isHeld(solution.program().functions.size());
    auto addClosuresIn = [&](ValueSetID objects) {
        sets.forEachMember(objects, [&](ValueFlowObjectID object) {
            FunctionID function = universe.closureFunction(object);
            if (function && !sets.contains(escaped, object))
                isHeld.quickSet(function.index());
        });
    };
    addClosuresIn(held);
    if (sets.contains(held, ValueFlowUniverse::untrackedObject()))
        addClosuresIn(solver.possibleValuesOf(ValueFlowProgramNodes::lost()));

    Vector<FunctionID> functions;
    for (size_t index : isHeld)
        functions.append(FunctionID(static_cast<uint32_t>(index)));
    return functions;
}

static bool isInSeenCode(const ValueFlowSolution& solution, FunctionID id)
{
    const Function& function = solution.program().at(id);
    if (function.role != FunctionRole::Library && function.role != FunctionRole::Model)
        return true;
    const ValueFlowBodies& bodies = solution.bodies();
    if (solution.sets().contains(solution.solver().possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)), bodies.universe().closureOf(id)))
        return true;
    return function.module && solution.program().at(function.module).body == id
        && solution.sets().contains(solution.solver().possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode)), bodies.universe().namespaceOf(function.module));
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
    const ParallelLoops& loops = solution.solver().input().build->parallelLoops();

    ValueSetMakers makers(solution.makerPool(), loops);
    Vector<ValueFlowObjectID, 32> functionsWhoseCallIsEvent;
    solution.sets().forEachMember(universe.builtinFunctions(), [&](ValueFlowObjectID object) {
        if (eventOfCalling(builtins, builtins.canonicalID(universe.builtinOf(object)), solution.solver().input().refusedCode))
            functionsWhoseCallIsEvent.append(object);
    });
    ValueSetID eventFunctions = makers.of(0).of(functionsWhoseCallIsEvent.span());
    struct Loader {
        BuiltinID builtin;
        ValueSetID objects;
    };
    Vector<Loader, 8> loaders;
    ValueSetID objectsOfLoaders = ValueSets::nothing;
    for (const BuiltinLoaderOfMachineCode& loader : builtins.loadersOfMachineCode()) {
        Vector<ValueFlowObjectID, 32> objects;
        universe.appendObjectsOf(loader.callee, objects);
        loaders.append(Loader { loader.named, makers.of(0).of(objects.span()) });
        objectsOfLoaders = makers.of(0).unite(objectsOfLoaders, loaders.last().objects);
    }

    Vector<Vector<UnseenCodeRecord>> recordsOfFunction(program.functions.size());
    forEachIndexInParallel(loops, program.functions.size(), [&](unsigned index, unsigned worker) {
        FunctionID id(index);
        if (!solution.solver().isReached(id))
            return;

        const Function& function = program.at(id);
        bool isSeenCode = isInSeenCode(solution, id);
        auto add = [&](UnseenCodeCause cause, BuiltinID builtin, ValueID site) {
            Witness witness = site ? Witness { index, site.index(), function.writtenLocationOf(site) } : Witness { index, Wire::none, function.declarationLocation() };
            recordsOfFunction[index].append(UnseenCodeRecord { cause, builtin ? builtin.index() : Wire::none, witness });
        };

        for (const ValueFlowStatement& statement : bodies.statementsOf(index)) {
            if (statement.kind == ValueFlowStatementKind::ModuleEvaluation) {
                for (const ModuleRequest& request : program.at(universe.moduleWithBody(id)).requests) {
                    if (staysOutsideImage(program, request.target))
                        add(UnseenCodeCause::ImportsUncompiledModule, { }, { });
                }
                continue;
            }
            if (statement.kind != ValueFlowStatementKind::Operation)
                continue;

            switch (statement.opcode) {
            case Opcode::DirectEval:
                if (isSeenCode && !solution.solver().input().refusedCode.evaluatedText)
                    add(UnseenCodeCause::EvaluatesText, { }, statement.site);
                continue;
            case Opcode::ImportDynamic:
            case Opcode::RequireDynamic:
                if (isWrittenInProgram(function))
                    add(solution.solver().input().refusedCode.codeOutsideImage ? UnseenCodeCause::LoadsModuleByName : UnseenCodeCause::LoadsCodeByName, { }, statement.site);
                continue;
            case Opcode::ModuleNamespace:
            case Opcode::ImportModule:
            case Opcode::RequireModule:
                if (staysOutsideImage(program, function.at(statement.site).module()))
                    add(UnseenCodeCause::ImportsUncompiledModule, { }, statement.site);
                continue;
            case Opcode::Call:
            case Opcode::CallList:
            case Opcode::Construct:
            case Opcode::ConstructList:
            case Opcode::SuperConstruct:
            case Opcode::SuperConstructList:
                break;
            default:
                continue;
            }
            if (!isSeenCode)
                continue;
            ValueSetMaker& maker = makers.of(worker);
            ValueSetID callees = ValueFlowRuleContext::valueShownToRules(bodies.operandsOf(statement)[0], solution.solver(), maker);
            if (solution.sets().meet(callees, objectsOfLoaders)) {
                for (const Loader& loader : loaders) {
                    if (!solution.sets().meet(callees, loader.objects))
                        continue;
                    if (isWrittenInProgram(function) || hasFlag(solution.solver().input().build->options().reports, ReportSet::Flow))
                        add(UnseenCodeCause::RunsMachineCode, loader.builtin, statement.site);
                    else
                        recordsOfFunction[index].append(UnseenCodeRecord { UnseenCodeCause::RunsMachineCode, loader.builtin.index(), noWitness });
                }
            }
            if (!solution.sets().meet(callees, eventFunctions))
                continue;
            bool isFiled = false;
            solution.sets().forEachMember(maker.intersect(callees, eventFunctions), [&](ValueFlowObjectID callee) {
                BuiltinID builtin = builtins.canonicalID(universe.builtinOf(callee));
                if (!std::exchange(isFiled, true))
                    add(*eventOfCalling(builtins, builtin, solution.solver().input().refusedCode), builtin, statement.site);
            });
        }
    });

    Vector<UnseenCodeRecord> records;
    for (Vector<UnseenCodeRecord>& recordsOfOne : recordsOfFunction)
        records.appendVector(WTF::move(recordsOfOne));

    BitVector isListed(builtins.builtinCount());
    for (const UnseenCodeRecord& record : records) {
        if (record.builtin != Wire::none)
            isListed.quickSet(record.builtin);
    }
    ValueSetID applied = solution.solver().possibleValuesOf(bodies.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode));
    size_t first = records.size();
    solution.sets().forEachMember(makers.of(0).intersect(applied, universe.builtinFunctions()), [&](ValueFlowObjectID object) {
        BuiltinID builtin = builtins.canonicalID(universe.builtinOf(object));
        std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin, solution.solver().input().refusedCode);
        if (!cause && builtins.unseenCodeOf(builtin))
            cause = UnseenCodeCause::RunsUnseenCode;
        if (cause && !isListed.quickSet(builtin.index()))
            records.append(UnseenCodeRecord { *cause, builtin.index(), noWitness });
    });
    AOT::sort(records.mutableSpan().subspan(first), { }, &UnseenCodeRecord::builtin);
    for (const Loader& loader : loaders) {
        bool hasRecord = std::ranges::any_of(records, [&](const UnseenCodeRecord& record) {
            return record.cause == UnseenCodeCause::RunsMachineCode && record.builtin == loader.builtin.index();
        });
        if (!hasRecord && solution.sets().meet(applied, loader.objects))
            records.append(UnseenCodeRecord { UnseenCodeCause::RunsMachineCode, loader.builtin.index(), noWitness });
    }
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
