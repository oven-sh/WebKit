/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowOrderInternal.h"

#if ENABLE(AOT)

#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static bool readsModuleValue(Opcode opcode, ModuleReferenceKind kind)
{
    if (opcode == Opcode::EvaluateModule || opcode == Opcode::RecordRequire)
        return false;
    switch (kind) {
    case ModuleReferenceKind::Target:
        return true;
    case ModuleReferenceKind::Referrer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void markLayoutOpaque(FlowScopeIndex& index, LayoutID layout)
{
    uint32_t end = index.numbering.firstField[layout.index() + 1];
    for (uint32_t location = index.numbering.firstField[layout.index()]; location < end; ++location)
        index.opaqueLocations.set(location);
}

static void markEvalBindingsOpaque(FlowScopeIndex& index, const EvalScope& scope)
{
    for (const EvalBinding& binding : scope.bindings) {
        if (binding.variable)
            index.opaqueLocations.set(binding.variable.index());
        if (!binding.layout)
            continue;
        if (!binding.field) {
            markLayoutOpaque(index, binding.layout);
            continue;
        }
        if (auto location = index.numbering.locationOf(binding.layout, *binding.field))
            index.opaqueLocations.set(*location);
    }
}

static void collectAccesses(FlowScopeIndex& index)
{
    enum class Kind : uint8_t { Read, Store, Allocation, ModuleValueRead, Mention, OpaqueVariable, OpaqueLayout, EvalScope };
    struct Row {
        Kind kind;
        FunctionReferenceKind reference;
        uint32_t subject;
        FunctionID function;
        ValueID value;
    };
    const Program& program = index.program();
    const FlowOrderInputs& inputs = index.inputs;
    const ParallelLoops& loops = inputs.build.parallelLoops();
    Vector<Row> rows = collectInParallel<Row>(loops, program.functions.size(), [&](unsigned functionIndex, Vector<Row>& rows) {
        if (!index.bodies.quickGet(functionIndex))
            return;
        const Function& function = program.functions[functionIndex];
        for (const Block& block : function.blocks) {
            for (ValueID id : block.values) {
                const Value& value = function.at(id);
                auto add = [&](Kind kind, uint32_t subject, FunctionReferenceKind reference = FunctionReferenceKind::Call) {
                    rows.append({ kind, reference, subject, FunctionID(functionIndex), id });
                };
                forEachVariable(value, [&](VariableID variable) {
                    if (variable)
                        add(value.opcode == Opcode::GetModuleVariable ? Kind::Read : value.opcode == Opcode::PutModuleVariable ? Kind::Store : Kind::OpaqueVariable, variable.index());
                });
                forEachLayout(value, [&](LayoutID layout) {
                    if (!layout || program.at(layout).kind != LayoutKind::Environment)
                        return;
                    if (value.opcode == Opcode::GetEnvironmentField || value.opcode == Opcode::PutEnvironmentField)
                        add(value.opcode == Opcode::GetEnvironmentField ? Kind::Read : Kind::Store, *index.numbering.locationOf(layout, value.field()));
                    else
                        add(value.opcode == Opcode::NewObject ? Kind::Allocation : Kind::OpaqueLayout, layout.index());
                });
                forEachFunctionReference(value, [&](FunctionID referenced, FunctionReferenceKind kind) {
                    if (referenced)
                        add(Kind::Mention, referenced.index(), kind);
                });
                forEachModuleReference(value, [&](ModuleID module, ModuleReferenceKind kind) {
                    if (module && readsModuleValue(value.opcode, kind))
                        add(Kind::ModuleValueRead, module.index());
                });
                forEachEvalScope(value, [&](EvalScopeID scope) {
                    if (scope)
                        add(Kind::EvalScope, scope.index());
                });
            }
        }
    });

    index.firstMention.fill(0, program.functions.size() + 1);
    for (const Row& row : rows) {
        switch (row.kind) {
        case Kind::Read:
            index.reads.append({ row.subject, row.function, row.value });
            break;
        case Kind::Store:
            index.stores.append({ row.subject, row.function, row.value });
            break;
        case Kind::Allocation:
            index.allocations.append({ LayoutID(row.subject), row.function, row.value });
            break;
        case Kind::ModuleValueRead:
            index.moduleValueReads.append({ ModuleID(row.subject), row.function, row.value });
            break;
        case Kind::Mention:
            index.mentions.append({ FunctionID(row.subject), row.reference, row.value });
            ++index.firstMention[row.function.index() + 1];
            break;
        case Kind::OpaqueVariable:
            index.opaqueLocations.set(row.subject);
            break;
        case Kind::OpaqueLayout:
            markLayoutOpaque(index, LayoutID(row.subject));
            break;
        case Kind::EvalScope:
            markEvalBindingsOpaque(index, program.at(EvalScopeID(row.subject)));
            break;
        }
    }
    for (size_t function = 0; function < program.functions.size(); ++function)
        index.firstMention[function + 1] += index.firstMention[function];

    for (const FlowStaticFieldAccess& access : inputs.staticFieldAccesses) {
        FlowScopeLocation location = access.variable.index();
        if (!access.function) {
            index.opaqueLocations.set(location);
            continue;
        }
        index.accessesThroughClassObjects.append({ location, access.function, access.origin });
        if (access.isStore)
            index.storesThroughClassObjects.append({ location, access.function, access.origin });
    }
    for (Vector<FlowScopeLocationAccess>* list : { &index.reads, &index.stores, &index.accessesThroughClassObjects, &index.storesThroughClassObjects })
        parallelSort(loops, list->mutableSpan(), std::less<FlowScopeLocationAccess>());
    parallelSort(loops, index.allocations.mutableSpan(), std::less<FlowEnvironmentAllocation>());
    parallelSort(loops, index.moduleValueReads.mutableSpan(), std::less<FlowModuleValueRead>());
}

static bool canOwnVariables(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FunctionID moduleBodyOwnerOf(const FlowScopeIndex& index, ModuleID moduleID)
{
    const Program& program = index.program();
    if (!moduleID || moduleID.index() >= program.modules.size() || !index.inputs.evaluatedModules.get(moduleID.index()))
        return { };
    const Module& module = program.at(moduleID);
    if (!canOwnVariables(module.kind) || !module.body || !index.bodies.quickGet(module.body.index()))
        return { };
    ModuleID file = program.at(module.body).module;
    if (index.modulesOfFunctionsEnteredWithoutMention.quickGet(moduleID.index()) || (file && index.modulesOfFunctionsEnteredWithoutMention.quickGet(file.index())))
        return { };
    return module.body;
}

static void findVariableOwners(FlowScopeIndex& index)
{
    const Program& program = index.program();
    for (const Module& module : program.modules) {
        for (const SyntheticExport& syntheticExport : module.syntheticExports) {
            if (syntheticExport.variable)
                index.syntheticVariables.set(syntheticExport.variable.index());
        }
    }
    for (unsigned variable = 0; variable < program.variables.size(); ++variable) {
        if (index.syntheticVariables.get(variable))
            continue;
        index.locationOwners[variable] = moduleBodyOwnerOf(index, program.variables[variable].module);
        if (!index.locationOwners[variable])
            index.opaqueLocations.set(variable);
    }
}

static BitVector findFunctionsEnteredFromOutside(const FlowScopeIndex& index)
{
    const Program& program = index.program();
    BitVector functions(program.functions.size());
    for (unsigned function = 0; function < program.functions.size(); ++function) {
        FunctionRole role = program.functions[function].role;
        bool isFoundThroughRegistration = role == FunctionRole::Library || role == FunctionRole::Model;
        if (index.bodies.quickGet(function) && program.functions[function].mayBeCalledByHost() && !isFoundThroughRegistration)
            functions.quickSet(function);
    }
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (entry.function && index.bodies.quickGet(entry.function.index()))
            functions.quickSet(entry.function.index());
    }
    return functions;
}

static void findFunctionsEnteredWithoutMention(FlowScopeIndex& index, const BitVector& functionsEnteredFromOutside)
{
    BitVector& functions = index.functionsEnteredWithoutMention;
    functions = functionsEnteredFromOutside;
    Vector<uint32_t> worklist;
    for (size_t function : functions)
        worklist.append(static_cast<uint32_t>(function));
    while (!worklist.isEmpty()) {
        for (const FlowFunctionMention& mention : index.mentionsIn(FunctionID(worklist.takeLast()))) {
            if (index.bodies.quickGet(mention.function.index()) && !functions.quickSet(mention.function.index()))
                worklist.append(mention.function.index());
        }
    }
    for (size_t function : functions) {
        if (ModuleID module = index.program().functions[function].module)
            index.modulesOfFunctionsEnteredWithoutMention.quickSet(module.index());
    }
}

static void findSoleCallSites(FlowScopeIndex& index, const BitVector& functionsEnteredFromOutside)
{
    const Program& program = index.program();
    for (size_t caller : index.bodies) {
        for (const FlowFunctionMention& mention : index.mentionsIn(FunctionID(static_cast<uint32_t>(caller)))) {
            ++index.calls.numberOfMentions[mention.function.index()];
            if (program.functions[caller].at(mention.value).opcode == Opcode::CallDirect)
                index.calls.soleCallSites[mention.function.index()] = { FunctionID(static_cast<uint32_t>(caller)), mention.value };
        }
    }
    for (const Variable& variable : program.variables) {
        if (variable.function)
            ++index.calls.numberOfMentions[variable.function.index()];
    }
    for (unsigned function = 0; function < program.functions.size(); ++function) {
        bool canRunOnlyDuringCall = !program.functions[function].isModuleBody() && !program.functions[function].isUnsplitCoroutine();
        if (index.calls.numberOfMentions[function] != 1 || !index.bodies.quickGet(function) || functionsEnteredFromOutside.quickGet(function) || !canRunOnlyDuringCall)
            index.calls.soleCallSites[function] = { };
    }
    for (const FlowCall& link : index.inputs.calls) {
        FlowCallSite& site = index.calls.soleCallSites[link.callee.index()];
        if (site != FlowCallSite { link.caller, link.site })
            site = { };
    }
}

static void collectCalls(FlowScopeIndex& index)
{
    const FlowOrderInputs& inputs = index.inputs;
    FlowScopeCalls& calls = index.calls;
    BitVector isCalled(index.program().functions.size());
    for (const FlowCall& link : inputs.calls) {
        isCalled.set(link.callee.index());
        if (link.caller == inputs.host() || !link.site)
            calls.functionsEnteredByHost.set(link.callee.index());
        else
            calls.byCaller.append({ link.caller, link.callee, link.site, { } });
    }
    for (unsigned function = 0; function < index.program().functions.size(); ++function) {
        if (!isCalled.get(function))
            calls.functionsEnteredByHost.set(function);
    }
    calls.functionsEnteredByHost.merge(index.functionsEnteredWithoutMention);

    const ParallelLoops& loops = inputs.build.parallelLoops();
    sortAndRemoveDuplicates(loops, calls.byCaller);
    forEachIndexInParallel(loops, calls.byCaller.size(), [&](unsigned call) {
        FlowScopeCall& record = calls.byCaller[call];
        record.subject = variableBehindSubjectOf(index.program().at(record.caller), record.site);
    });
    calls.byCallee = calls.byCaller;
    parallelSort(loops, calls.byCallee.mutableSpan(), [](const FlowScopeCall& a, const FlowScopeCall& b) {
        return std::tuple { a.callee, a.caller, a.site } < std::tuple { b.callee, b.caller, b.site };
    });
    auto findFirstCalls = [&](const Vector<FlowScopeCall>& sorted, FunctionID FlowScopeCall::* function) {
        Vector<uint32_t> first(FillWith { }, index.program().functions.size() + 1, 0u);
        for (const FlowScopeCall& call : sorted)
            ++first[(call.*function).index() + 1];
        for (size_t position = 1; position < first.size(); ++position)
            first[position] += first[position - 1];
        return first;
    };
    calls.firstByCaller = findFirstCalls(calls.byCaller, &FlowScopeCall::caller);
    calls.firstByCallee = findFirstCalls(calls.byCallee, &FlowScopeCall::callee);
}

static void findEnvironmentOwners(FlowScopeIndex& index)
{
    for (unsigned layoutIndex = 0; layoutIndex < index.program().layouts.size(); ++layoutIndex) {
        LayoutID layout(layoutIndex);
        auto sites = operationsOn(index.allocations, layout);
        if (sites.empty() || sites.front().function != sites.back().function)
            continue;
        index.environmentOwners[layoutIndex] = sites.front().function;
        uint32_t end = index.numbering.firstField[layoutIndex + 1];
        for (uint32_t location = index.numbering.firstField[layoutIndex]; location < end; ++location)
            index.locationOwners[location] = sites.front().function;
    }
}

static FlowScopeOwner& ownerFor(FlowScopeIndex& index, FunctionID function)
{
    auto owner = std::ranges::lower_bound(index.owners, function, { }, &FlowScopeOwner::function);
    RELEASE_ASSERT(owner != index.owners.end() && owner->function == function);
    return *owner;
}

static void assignExport(FlowScopeIndex& index, VariableID variable, ModuleID exporter)
{
    FunctionID function = variable ? index.locationOwners[variable.index()] : FunctionID();
    if (!function)
        return;
    FlowScopeOwner& owner = ownerFor(index, function);
    owner.exports.append({ variable, exporter });
    if (owner.exporters.isEmpty() || owner.exporters.last() != exporter)
        owner.exporters.append(exporter);
}

static void assignExportsOfNamespace(FlowScopeIndex& index, ModuleID exportedNamespace, ModuleID exporter)
{
    const Program& program = index.program();
    BitVector visitedModules(program.modules.size());
    Vector<ModuleID, 8> pending { exportedNamespace };
    while (!pending.isEmpty()) {
        ModuleID module = pending.takeLast();
        if (!module || visitedModules.set(module.index()))
            continue;
        for (const ModuleExport& moduleExport : program.at(module).exports) {
            switch (moduleExport.kind) {
            case ModuleExportKind::Variable:
                assignExport(index, moduleExport.variable, exporter);
                break;
            case ModuleExportKind::Namespace:
                pending.append(moduleExport.module);
                break;
            case ModuleExportKind::Indirect:
                break;
            }
        }
    }
}

static void createOwners(FlowScopeIndex& index)
{
    const Program& program = index.program();
    Vector<FunctionID> functions;
    for (const Vector<FunctionID>* list : { &index.locationOwners, &index.environmentOwners }) {
        for (FunctionID owner : *list) {
            if (owner)
                functions.append(owner);
        }
    }
    sortAndRemoveDuplicates(index.inputs.build.parallelLoops(), functions);
    index.owners.reserveInitialCapacity(functions.size());
    for (FunctionID function : functions)
        index.owners.append(FlowScopeOwner { function, { }, { }, { }, { }, { } });

    for (unsigned variable = 0; variable < program.variables.size(); ++variable) {
        FunctionID function = index.locationOwners[variable];
        if (!function)
            continue;
        FlowScopeOwner& owner = ownerFor(index, function);
        owner.module = program.variables[variable].module;
        owner.variables.append(VariableID(variable));
    }
    for (unsigned layout = 0; layout < program.layouts.size(); ++layout) {
        if (FunctionID function = index.environmentOwners[layout])
            ownerFor(index, function).environments.append(LayoutID(layout));
    }
    for (unsigned moduleIndex = 0; moduleIndex < program.modules.size(); ++moduleIndex) {
        assignExport(index, program.modules[moduleIndex].exportsVariable, ModuleID(moduleIndex));
        if (ModuleID captured = commonJsModuleCapturedBy(program, program.modules[moduleIndex]))
            assignExport(index, program.at(captured).exportsVariable, ModuleID(moduleIndex));
        for (const ModuleExport& moduleExport : program.modules[moduleIndex].exports) {
            switch (moduleExport.kind) {
            case ModuleExportKind::Variable:
                assignExport(index, moduleExport.variable, ModuleID(moduleIndex));
                break;
            case ModuleExportKind::Namespace:
                assignExportsOfNamespace(index, moduleExport.module, ModuleID(moduleIndex));
                break;
            case ModuleExportKind::Indirect:
                break;
            }
        }
    }
    for (FlowScopeOwner& owner : index.owners)
        AOT::sort(owner.exports);
}

FlowScopeIndex createScopeIndex(const FlowOrderInputs& inputs, const FlowModuleOrder& moduleOrder)
{
    const Program& program = inputs.program;
    FlowScopeIndex index { inputs, moduleOrder };
    FlowScopeLocationNumbering numbering;
    numbering.numberOfVariables = program.variables.size();
    numbering.firstField.reserveInitialCapacity(program.layouts.size() + 1);
    uint32_t numberOfLocations = numbering.numberOfVariables;
    for (const Layout& layout : program.layouts) {
        numbering.firstField.append(numberOfLocations);
        if (layout.kind == LayoutKind::Environment)
            numberOfLocations += layout.fields.size();
    }
    numbering.firstField.append(numberOfLocations);
    index.numbering = WTF::move(numbering);
    index.bodies = inputs.reachedFunctions;
    for (size_t function : inputs.reachedFunctions) {
        if (!program.functions[function].hasBody())
            index.bodies.quickClear(function);
    }
    index.opaqueLocations = BitVector(numberOfLocations);
    index.syntheticVariables = BitVector(program.variables.size());
    index.modulesOfFunctionsEnteredWithoutMention = BitVector(program.modules.size());
    index.locationOwners.grow(numberOfLocations);
    index.environmentOwners.grow(program.layouts.size());
    index.calls.numberOfMentions.fill(0, program.functions.size());
    index.calls.soleCallSites.grow(program.functions.size());
    index.calls.functionsEnteredByHost = BitVector(program.functions.size());

    BitVector functionsEnteredFromOutside = findFunctionsEnteredFromOutside(index);
    collectAccesses(index);
    findFunctionsEnteredWithoutMention(index, functionsEnteredFromOutside);
    findVariableOwners(index);
    findSoleCallSites(index, functionsEnteredFromOutside);
    collectCalls(index);
    findEnvironmentOwners(index);
    createOwners(index);
    return index;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
