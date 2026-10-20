/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTModuleGraph.h"
#include "AOTResolveCallsCallees.h"
#include "AOTSlotPlanSteps.h"
#include <algorithm>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ModuleLists {
    Vector<ModuleData> modules;
    Vector<ModuleRequestData> requests;
    Vector<ModuleExportData> exports;
    Vector<IndirectExportData> indirectExports;
    Vector<SyntheticExportData> syntheticExports;
    Vector<RequiredNameData> requiredNames;
};

} // anonymous namespace

template<typename ID>
static uint32_t indexOrNone(ID id)
{
    return id ? id.index() : UINT32_MAX;
}

// The closure table is indexed by generic entrypoint. PlacedVariable::function is the function as declared.
static SlotData closureSlotOf(const TableEncoder& encoder, FunctionID function)
{
    uint32_t entrypoint = encoder.entrypointOf(function);
    if (entrypoint == UINT32_MAX)
        return { };
    return SlotData::create(encoder.checkedOffset(entrypoint), SlotRepresentation::DeferredClosure);
}

SlotData placedVariableSlot(const TableEncoder& encoder, VariableID id)
{
    const PlacedVariable& placed = encoder.placement.variable(id);
    RELEASE_ASSERT(placed.encoding != VariableEncoding::UndefinedAsZero);
    switch (placed.storage) {
    case VariableStorage::Statics:
        return SlotData::create(encoder.checkedOffset(placed.offset), placed.representation);
    case VariableStorage::ClosureTable:
        return closureSlotOf(encoder, placed.function);
    case VariableStorage::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void setListStarts(ModuleData& entry, const ModuleLists& lists)
{
    entry.firstRequest = lists.requests.size();
    entry.firstExport = lists.exports.size();
    entry.firstSyntheticExport = lists.syntheticExports.size();
    entry.firstRequiredName = lists.requiredNames.size();
}

// The loader ignores what the body of an ES module without top-level await returns.
static bool onlyReturns(const Function& body)
{
    if (body.blocks.size() != 1 || body.has(FunctionFlags::IsAsync))
        return false;
    const Block& block = body.blocks.first();
    return block.terminator.kind == TerminatorKind::Return && std::ranges::all_of(block.values, [&](ValueID id) {
        return body.at(id).opcode == Opcode::Undefined;
    });
}

static uint32_t bodyEntrypointOf(const TableEncoder& encoder, const Module& module)
{
    if (!module.body)
        return UINT32_MAX;
    const Function& body = encoder.program.at(module.body);
    bool isNeverEvaluated = !body.hasBody();
    bool doesNothing = module.kind == ModuleKind::SourceText && onlyReturns(body);
    return isNeverEvaluated || doesNothing ? UINT32_MAX : encoder.entrypointOf(module.body);
}

static void appendModule(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists)
{
    ModuleData entry { };
    entry.publicPath = encoder.stringNumber(module.path);
    entry.displayPath = encoder.stringNumber(encoder.obfuscates() ? StringID() : module.displayPath);
    entry.kind = module.kind;
    entry.externalKind = module.externalKind;
    entry.exportRule = module.exportRule;
    entry.requireResult = module.requireResult;
    if (module.flags.contains(ModuleFlags::HasTopLevelAwait))
        entry.add(ModuleData::Flag::HasTopLevelAwait);
    entry.body = bodyEntrypointOf(encoder, module);
    entry.referrer = indexOrNone(module.referrer);
    entry.runtimeModule = indexOrNone(module.runtimeModule);
    entry.value = SlotData::create(encoder.checkedOffset(encoder.placement.moduleValueOffset(id)), SlotRepresentation::JSValue);
    setListStarts(entry, lists);
    lists.modules.append(entry);
}

static bool isNeverEvaluated(const Program& program, const Module& module)
{
    return module.body && !program.at(module.body).hasBody();
}

static bool runsNothingItself(const TableEncoder& encoder, const Module& module)
{
    switch (module.kind) {
    case ModuleKind::SourceText:
        return bodyEntrypointOf(encoder, module) == UINT32_MAX && (module.requiredNames.isEmpty() || isNeverEvaluated(encoder.program, module));
    case ModuleKind::CommonJs:
        return isNeverEvaluated(encoder.program, module);
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An importer has no reason to visit a module of which nothing runs and which imports only such modules.
static BitVector findModulesNotWorthVisiting(const TableEncoder& encoder)
{
    const Program& program = encoder.program;
    BitVector modules(program.modules.size());
    for (size_t index = 0; index < program.modules.size(); ++index) {
        if (runsNothingItself(encoder, program.modules[index]))
            modules.quickSet(index);
    }

    bool didChange = true;
    while (didChange) {
        didChange = false;
        for (size_t index = 0; index < program.modules.size(); ++index) {
            if (!modules.quickGet(index))
                continue;
            bool importsOnlySuchModules = std::ranges::all_of(program.modules[index].requests, [&](const ModuleRequest& request) {
                return modules.quickGet(request.target.index());
            });
            if (!importsOnlySuchModules) {
                modules.quickClear(index);
                didChange = true;
            }
        }
    }
    return modules;
}

static void appendRequests(const Module& module, const BitVector& modulesNotWorthVisiting, ModuleLists& lists)
{
    for (const ModuleRequest& request : module.requests) {
        if (modulesNotWorthVisiting.quickGet(request.target.index()))
            continue;
        ModuleRequestData entry { };
        entry.target = request.target.index();
        entry.isDeferred = request.phase == ImportPhase::Defer;
        entry.isDynamicStarExport = request.isDynamicStarExport;
        lists.requests.append(entry);
    }
}

static void appendExports(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists)
{
    for (const ModuleExport& moduleExport : module.exports) {
        if (!isDescribedToRunTime(encoder.program, encoder.flow, id, moduleExport))
            continue;
        ModuleExportData entry { };
        entry.name = encoder.identifierNumberOfText(moduleExport.name);
        entry.kindBits = static_cast<uint32_t>(moduleExport.kind);
        entry.isDeferred = moduleExport.phase == ImportPhase::Defer;
        entry.isFromStarExport = moduleExport.isFromStarExport;
        switch (moduleExport.kind) {
        case ModuleExportKind::Variable:
            entry.variable = placedVariableSlot(encoder, moduleExport.variable);
            break;
        case ModuleExportKind::Namespace:
            entry.module = moduleExport.module.index();
            break;
        case ModuleExportKind::Indirect:
            entry.indirectExport = static_cast<uint32_t>(lists.indirectExports.size());
            lists.indirectExports.append({ moduleExport.module.index(), encoder.identifierNumberOfText(moduleExport.importName) });
            break;
        }
        lists.exports.append(entry);
    }
}

static void appendSyntheticExports(TableEncoder& encoder, const Module& module, ModuleLists& lists)
{
    for (const SyntheticExport& syntheticExport : module.syntheticExports) {
        SyntheticExportData entry { };
        entry.name = encoder.identifierNumberOfText(syntheticExport.name);
        entry.absentIsUndefined = syntheticExport.isKnownMember;
        entry.hasVariable = !!syntheticExport.variable;
        entry.variable = syntheticExport.variable ? placedVariableSlot(encoder, syntheticExport.variable) : SlotData { };
        lists.syntheticExports.append(entry);
    }
}

static void appendRequiredNames(TableEncoder& encoder, const Module& module, ModuleLists& lists)
{
    if (isNeverEvaluated(encoder.program, module))
        return;
    for (const RequiredName& requiredName : module.requiredNames)
        lists.requiredNames.append({ requiredName.module.index(), requiredName.mayBeAbsent, encoder.identifierNumberOfText(requiredName.name) });
}

static ModuleLists collectModuleLists(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    ModuleLists lists;
    BitVector modulesNotWorthVisiting = findModulesNotWorthVisiting(encoder);
    lists.modules.reserveInitialCapacity(program.modules.size() + 1);
    for (size_t index = 0; index < program.modules.size(); ++index) {
        ModuleID id(index);
        const Module& module = program.modules[index];
        appendModule(encoder, id, module, lists);
        if (module.kind == ModuleKind::CommonJs && module.body && encoder.flow.runsAtMostOnce(module.body))
            lists.modules.last().add(ModuleData::Flag::IsEvaluatedAtMostOnce);
        appendRequests(module, modulesNotWorthVisiting, lists);
        appendExports(encoder, id, module, lists);
        appendSyntheticExports(encoder, module, lists);
        appendRequiredNames(encoder, module, lists);
    }
    for (const Entry& entry : program.entries)
        lists.modules[entry.module.index()].add(ModuleData::Flag::IsEntryPoint);
    ModuleData endEntry { };
    setListStarts(endEntry, lists);
    lists.modules.append(endEntry);
    return lists;
}

static bool mayOverrideRequire(const FlowResults& flow)
{
    static constexpr BuiltinID locations[] = {
        Builtins::NodeModulePrototypeRequire,
        Builtins::NodeModule_resolveFilename,
        Builtins::NodeModuleWrap,
        Builtins::NodeModuleWrapper,
    };
    return std::ranges::any_of(locations, [&](BuiltinID location) {
        return flow.builtinLocation(location) != BuiltinLocationState::Constant;
    });
}

static void appendRequireEdges(TableEncoder& encoder)
{
    std::span<const RequireEdge> edges = encoder.placement.requireEdges();
    std::span<const StringID> specifiers = encoder.placement.requireEdgeSpecifiers();
    RELEASE_ASSERT(edges.size() == specifiers.size());
    bool needsSpecifiers = mayOverrideRequire(encoder.flow);
    Vector<RequireEdgeData> requireEdges;
    requireEdges.reserveInitialCapacity(edges.size());
    for (size_t index = 0; index < edges.size(); ++index) {
        uint32_t specifier = needsSpecifiers ? encoder.stringNumber(specifiers[index]) : UINT32_MAX;
        requireEdges.append({ edges[index].referrer.index(), edges[index].target.index(), specifier });
    }
    encoder.setTable(ProgramTableIndex::requireEdges, requireEdges);
}

// The host creates the module object, its exports and its require function for each run.
static void appendResetSlots(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<ModuleResetSlotData> slots;
    for (const ModuleBuiltinCell& cell : encoder.placement.statics().moduleBuiltinCells) {
        if (program.at(cell.module).kind == ModuleKind::CommonJs)
            slots.append({ cell.module.index(), SlotData::create(encoder.checkedOffset(cell.offset), SlotRepresentation::JSValue) });
    }
    std::ranges::sort(slots, { }, [](const ModuleResetSlotData& entry) {
        return std::pair { entry.module, static_cast<uint32_t>(entry.slot.offset) };
    });
    encoder.setTable(ProgramTableIndex::moduleResetSlots, slots);
}

static void appendModuleFailures(TableEncoder& encoder)
{
    Vector<ModuleFailureData> failures;
    for (const ModuleFailure& failure : encoder.program.moduleFailures)
        failures.append({ failure.importer.index(), encoder.stringNumber(failure.specifier) });
    encoder.setTable(ProgramTableIndex::moduleFailures, failures);
}

static bool isFoundByPath(ModuleKind kind)
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

// The generic entrypoint of a function without a body ends in Unreachable.
static bool hasCompiledBody(const TableEncoder& encoder, const Module& module)
{
    return module.body && encoder.program.at(versionToEnter(encoder.program, encoder.flow, module.body, { })).hasBody();
}

static void appendModulesByPath(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<uint32_t> modules;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (!isFoundByPath(module.kind))
            continue;
        // Without an entry here, the host answers for the name of its module.
        if (module.kind == ModuleKind::RuntimeSource && !hasCompiledBody(encoder, module))
            continue;
        // No code of the module is in the image, so a load by its name fails as that of a file that was not compiled.
        if (isNeverEvaluated(program, module))
            continue;
        modules.append(index);
    }
    std::ranges::sort(modules, [&](uint32_t left, uint32_t right) {
        return std::ranges::lexicographical_compare(program.string(program.modules[left].path), program.string(program.modules[right].path));
    });
    encoder.setTable(ProgramTableIndex::modulesByPath, modules);
}

static void appendCompiledRuntimeModules(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    Vector<CompiledRuntimeModuleData> runtimeModules;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        const Module& module = program.modules[index];
        if (module.kind != ModuleKind::RuntimeSource || !module.runtimeModule)
            continue;
        // Without an entry here, the host loads its ordinary module.
        if (!hasCompiledBody(encoder, module))
            continue;
        runtimeModules.append({ module.runtimeModule.index(), index });
    }
    std::ranges::sort(runtimeModules, { }, &CompiledRuntimeModuleData::registryID);
    encoder.setTable(ProgramTableIndex::compiledRuntimeModules, runtimeModules);
}

static void appendCompiledRuntimeFunctions(TableEncoder& encoder)
{
    const Program& program = encoder.program;
    BitVector isCalledByHost = functionsCalledByHost(program);
    Vector<CompiledRuntimeFunctionData> runtimeFunctions;
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        if (!entry.function || !isCalledByHost.get(entry.function.index()))
            continue;
        // The generic entrypoint of a function without a body ends in Unreachable. Without an entry here, the host creates its ordinary built-in.
        if (!program.at(versionToEnter(program, encoder.flow, entry.function, { })).hasBody())
            continue;
        runtimeFunctions.append({ entry.id.index(), encoder.entrypointOf(entry.function), static_cast<uint32_t>(entry.visibility) });
    }
    encoder.setTable(ProgramTableIndex::compiledRuntimeFunctions, runtimeFunctions);
}

void encodeModuleTables(TableEncoder& encoder)
{
    ModuleLists lists = collectModuleLists(encoder);
    computeModuleFlags(lists.modules.mutableSpan(), lists.requests.span());
    encoder.setTable(ProgramTableIndex::modules, lists.modules);
    encoder.setTable(ProgramTableIndex::moduleRequests, lists.requests);
    encoder.setTable(ProgramTableIndex::moduleExports, lists.exports);
    encoder.setTable(ProgramTableIndex::indirectExports, lists.indirectExports);
    encoder.setTable(ProgramTableIndex::syntheticExports, lists.syntheticExports);
    encoder.setTable(ProgramTableIndex::requiredNames, lists.requiredNames);
    appendRequireEdges(encoder);
    appendResetSlots(encoder);
    appendModuleFailures(encoder);
    appendModulesByPath(encoder);
    appendCompiledRuntimeModules(encoder);
    appendCompiledRuntimeFunctions(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
