/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowResultsUnseenCode.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTFlowIDs.h"
#include "AOTSort.h"
#include "AOTValueFlowResultsInternal.h"
#include "AOTValueFlowRulesModules.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static bool isRaised(const ValueFlowSolution& solution, ValueFlowHostNode flag)
{
    return solution.sets().contains(solution.solver().valueOf(solution.bodies().nodes().ofHost(flag)), ValueFlowUniverse::unknownObject());
}

bool unseenCodeCanRun(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::UnseenCodeRuns);
}

bool unseenCodeChangesRealm(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::UnseenCodeChangesRealm);
}

bool unseenCodeHasLoader(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::UnseenCodeHasLoader);
}

bool modulesAreLoadedByName(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::ModulesAreRequiredByName) || isRaised(solution, ValueFlowHostNode::ModulesAreImportedByName) || isRaised(solution, ValueFlowHostNode::LibraryModulesAreRequiredByName);
}

bool filesOfProgramAreLoadedByName(const ValueFlowSolution& solution)
{
    return isRaised(solution, ValueFlowHostNode::ModulesAreRequiredByName) || isRaised(solution, ValueFlowHostNode::ModulesAreImportedByName);
}

Vector<FunctionID> functionsHeldButNotCalled(const ValueFlowSolution& solution)
{
    const ValueSets& sets = solution.sets();
    const ValueFlowSolver& solver = solution.solver();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    ValueSetID held = solver.valueOf(solution.bodies().nodes().ofHost(ValueFlowHostNode::HeldByNativeCode));
    ValueSetID escaped = solver.valueOf(ValueFlowProgramNodes::escaped());

    BitVector isHeld(solution.program().functions.size());
    auto addClosuresIn = [&](ValueSetID objects) {
        sets.forEachMember(objects, [&](ValueFlowObjectID object) {
            FunctionID function = universe.closureFunction(object);
            if (function && !sets.contains(escaped, object))
                isHeld.quickSet(function.index());
        });
    };
    addClosuresIn(held);
    // The set of what an object with many members holds is given up. The untracked object stands for those members here, and they are among what the analysis has stopped following.
    if (sets.contains(held, ValueFlowUniverse::untrackedObject()))
        addClosuresIn(solver.valueOf(ValueFlowProgramNodes::lost()));

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
    if (solution.sets().contains(solution.solver().valueOf(bodies.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)), bodies.universe().closureOf(id)))
        return true;
    return function.module && solution.program().at(function.module).body == id
        && solution.sets().contains(solution.solver().valueOf(bodies.nodes().ofHost(ValueFlowHostNode::ModulesOfSeenCode)), bodies.universe().namespaceOf(function.module));
}

static bool opcodeCallsValue(Opcode opcode)
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

// No operation spells out the call where a built-in applies its callback or a protocol finds a method.
static void appendRecordsOfApplicationsWithoutOperation(const ValueFlowSolution& solution, ValueSetMaker& maker, Vector<UnseenCodeRecord>& records)
{
    const BuiltinTable& builtins = solution.program().builtins();
    BitVector isListed(builtins.builtinCount());
    for (const UnseenCodeRecord& record : records) {
        if (record.builtin != Wire::none)
            isListed.quickSet(record.builtin);
    }
    ValueSetID applied = maker.intersect(solution.solver().valueOf(solution.bodies().nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode)), solution.bodies().universe().builtinFunctions());
    size_t first = records.size();
    solution.sets().forEachMember(applied, [&](ValueFlowObjectID object) {
        BuiltinID builtin = builtins.canonicalID(solution.bodies().universe().builtinOf(object));
        std::optional<UnseenCodeCause> cause = eventOfCalling(builtins, builtin, solution.solver().input().refusedCode);
        if (!cause && builtins.unseenCodeOf(builtin))
            cause = UnseenCodeCause::RunsUnseenCode;
        if (cause && !isListed.quickSet(builtin.index()))
            records.append(UnseenCodeRecord { *cause, builtin.index(), noWitness });
    });
    AOT::sort(records.mutableSpan().subspan(first), { }, &UnseenCodeRecord::builtin);
}

static ValueSetID functionsWhoseCallIsEvent(const ValueFlowSolution& solution, ValueSetMaker& maker)
{
    const BuiltinTable& builtins = solution.program().builtins();
    const ValueFlowUniverse& universe = solution.bodies().universe();
    Vector<ValueFlowObjectID, 32> objects;
    solution.sets().forEachMember(universe.builtinFunctions(), [&](ValueFlowObjectID object) {
        if (eventOfCalling(builtins, builtins.canonicalID(universe.builtinOf(object)), solution.solver().input().refusedCode))
            objects.append(object);
    });
    return maker.of(objects.span());
}

Vector<UnseenCodeRecord> unseenCodeRecordsOf(ValueFlowSolution& solution)
{
    const Program& program = solution.program();
    const BuiltinTable& builtins = program.builtins();
    const ValueFlowBodies& bodies = solution.bodies();
    const ValueFlowUniverse& universe = bodies.universe();
    const ParallelLoops& loops = solution.solver().input().build->parallelLoops();

    ValueSetMakers makers(solution.makerPool(), loops);
    ValueSetID eventFunctions = functionsWhoseCallIsEvent(solution, makers.of(0));
    struct Loader {
        BuiltinID builtin;
        ValueSetID objects;
    };
    Vector<Loader, 8> loaders;
    ValueSetID objectsOfLoaders = ValueSets::nothing;
    for (const BuiltinLoaderOfMachineCode& loader : program.builtins().loadersOfMachineCode()) {
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
            Witness witness = site ? Witness { index, site.index(), function.locationOf(site) } : Witness { index, Wire::none, function.declarationLocation() };
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
            default:
                break;
            }
            if (!opcodeCallsValue(statement.opcode) || !isSeenCode)
                continue;
            ValueSetMaker& maker = makers.of(worker);
            // As the rule of the call sees them: beside a placeholder, without what the placeholder covers.
            ValueSetID callees = ValueFlowRuleContext::valueShownToRules(bodies.operandsOf(statement)[0], solution.solver(), maker);
            if (solution.sets().meet(callees, objectsOfLoaders)) {
                for (const Loader& loader : loaders) {
                    if (!solution.sets().meet(callees, loader.objects))
                        continue;
                    // A place in the library says nothing to whoever reads it. A report finds the statement of the program that leads there.
                    if (isWrittenInProgram(function) || hasFlag(solution.solver().input().build->options().reports, ReportSet::Flow))
                        add(UnseenCodeCause::RunsMachineCode, loader.builtin, statement.site);
                    else
                        recordsOfFunction[index].append(UnseenCodeRecord { UnseenCodeCause::RunsMachineCode, loader.builtin.index(), noWitness });
                }
            }
            if (!solution.sets().meet(callees, eventFunctions))
                continue;
            // One record for a site, and only where the solve has raised the event: applyEventsOfCall().
            bool isFiled = false;
            solution.sets().forEachMember(maker.intersect(callees, eventFunctions), [&](ValueFlowObjectID callee) {
                BuiltinID builtin = builtins.canonicalID(universe.builtinOf(callee));
                if (isFiled)
                    return;
                isFiled = true;
                add(*eventOfCalling(builtins, builtin, solution.solver().input().refusedCode), builtin, statement.site);
            });
        }
    });

    Vector<UnseenCodeRecord> records;
    for (Vector<UnseenCodeRecord>& recordsOfOne : recordsOfFunction)
        records.appendVector(WTF::move(recordsOfOne));
    appendRecordsOfApplicationsWithoutOperation(solution, makers.of(0), records);
    // Likewise where a built-in applies the loader: Reflect.apply(process.dlopen, ..).
    ValueSetID applied = solution.solver().valueOf(bodies.nodes().ofHost(ValueFlowHostNode::FunctionsOfSeenCode));
    for (const Loader& loader : loaders) {
        bool isListed = std::ranges::any_of(records, [&](const UnseenCodeRecord& record) {
            return record.cause == UnseenCodeCause::RunsMachineCode && record.builtin == loader.builtin.index();
        });
        if (!isListed && solution.sets().meet(applied, loader.objects))
            records.append(UnseenCodeRecord { UnseenCodeCause::RunsMachineCode, loader.builtin.index(), noWitness });
    }
    return records;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
