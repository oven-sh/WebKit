/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTModuleGraph.h"
#include "AOTSlotPlanSteps.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include <algorithm>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ModuleLists {
    Vector<ModuleData> modules;
    Vector<ModuleDirectoryData> directories;
    // By the directory above and the name.
    HashMap<uint64_t, uint32_t> numbersOfDirectories;
    Vector<ModuleDisplayPathData> displayPaths;
    Vector<ModuleRequestData> requests;
    Vector<ModuleExportData> exports;
    Vector<IndirectExportData> indirectExports;
    Vector<SyntheticExportData> syntheticExports;
    Vector<RequiredNameData> requiredNames;
};

struct RequestTargets {
    const Vector<ModuleRequest>& requests;

    size_t size() const { return requests.size(); }
    ModuleID operator[](size_t index) const { return requests[index].target; }
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
    entry.firstRequest = Number24(lists.requests.size());
    entry.firstExport = Number24(lists.exports.size());
    entry.firstSyntheticExport = Number24(lists.syntheticExports.size());
    entry.firstRequiredName = Number24(lists.requiredNames.size());
}

// The loader ignores what the body of a file of the program returns, unless it awaits at the top level.
static bool onlyReturns(const Function& body)
{
    if (body.blocks.size() != 1 || body.has(FunctionFlags::IsAsync))
        return false;
    const Block& block = body.blocks.first();
    return block.terminator.kind == TerminatorKind::Return && std::ranges::all_of(block.values, [&](ValueID id) {
        return body.at(id).opcode == Opcode::Undefined;
    });
}

BitVector findModuleBodiesThatOnlyReturn(const Program& program)
{
    BitVector result(program.functions.size());
    for (const Module& module : program.modules) {
        if (module.body && onlyReturns(program.at(module.body)))
            result.quickSet(module.body.index());
    }
    return result;
}

static uint32_t bodyEntrypointOf(const TableEncoder& encoder, const Module& module)
{
    if (!module.body)
        return UINT32_MAX;
    const Function& body = encoder.program.at(module.body);
    bool isNeverEvaluated = !body.hasBody();
    bool ignoresResult = module.kind == ModuleKind::SourceText || module.kind == ModuleKind::CommonJs;
    bool doesNothing = ignoresResult && encoder.inputs.bodyFacts.functionsThatOnlyReturn.get(module.body.index());
    return isNeverEvaluated || doesNothing ? UINT32_MAX : encoder.entrypointOf(module.body);
}

// The name of a module of the library and of one outside the image is no path of a file.
static bool mayHavePathInPieces(const Module& module)
{
    return module.kind == ModuleKind::SourceText || module.kind == ModuleKind::Data;
}

// Each directory with its slash, then what stands behind the last slash. Nothing for a path without a slash or without a name behind the last one.
static void forEachPieceOfPath(std::span<const uint8_t> path, NOESCAPE const Invocable<void(std::span<const uint8_t>)> auto& functor)
{
    if (path.empty() || path.back() == '/' || !std::ranges::contains(path, '/'))
        return;
    while (!path.empty()) {
        auto slash = std::ranges::find(path, '/');
        size_t length = slash == path.end() ? path.size() : static_cast<size_t>(slash - path.begin()) + 1;
        functor(path.first(length));
        path = path.subspan(length);
    }
}

void internPiecesOfModulePaths(Program& program)
{
    for (const Module& module : program.modules) {
        if (!mayHavePathInPieces(module))
            continue;
        forEachPieceOfPath(program.string(module.path), [&](std::span<const uint8_t> piece) {
            program.strings.intern(piece);
        });
    }
}

static void setPublicPath(TableEncoder& encoder, const Module& module, ModuleLists& lists, ModuleData& entry)
{
    Vector<StringID, 16> pieces;
    bool everyPieceIsString = true;
    // A module that is not simple gets a record when it is linked, and the key of the record is its path.
    if (mayHavePathInPieces(module) && entry.isSimple() && encoder.program.strings.size() <= Number24::max) {
        forEachPieceOfPath(encoder.program.string(module.path), [&](std::span<const uint8_t> piece) {
            pieces.append(encoder.program.strings.find(piece));
            everyPieceIsString &= !!pieces.last();
        });
    }
    // Not every piece: a program that comes in behind the step that makes them.
    if (pieces.isEmpty() || !everyPieceIsString) {
        entry.publicPathOrName = encoder.shownStringNumber(module.path);
        entry.directory = Number24(Number24::max);
        return;
    }

    uint32_t directory = Number24::max;
    for (StringID name : pieces.span().first(pieces.size() - 1)) {
        directory = lists.numbersOfDirectories.ensure((static_cast<uint64_t>(directory) << 32 | name.index()) + 1, [&] {
            lists.directories.append({ Number24(directory), Number24(encoder.shownStringNumber(name)) });
            return static_cast<uint32_t>(lists.directories.size() - 1);
        }).iterator->value;
    }
    RELEASE_ASSERT(directory < Number24::max);
    entry.directory = Number24(directory);
    entry.publicPathOrName = encoder.shownStringNumber(pieces.last());
}

static void setDisplayPath(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists, ModuleData& entry)
{
    std::span<const uint8_t> displayPath = module.displayPath ? encoder.program.string(module.displayPath) : std::span<const uint8_t>();
    entry.charactersInFrontOfDisplayPath = ModuleData::hasNoDisplayPath;
    if (encoder.obfuscates() || displayPath.empty())
        return;
    std::span<const uint8_t> publicPath = encoder.program.string(module.path);
    if (publicPath.size() >= displayPath.size()) {
        std::span<const uint8_t> inFront = publicPath.first(publicPath.size() - displayPath.size());
        // One byte is one character.
        bool isCounted = inFront.size() < ModuleData::hasNoDisplayPath && std::ranges::all_of(inFront, isASCII<uint8_t>);
        if (isCounted && std::ranges::equal(publicPath.subspan(inFront.size()), displayPath)) {
            entry.charactersInFrontOfDisplayPath = static_cast<uint8_t>(inFront.size());
            return;
        }
    }
    entry.charactersInFrontOfDisplayPath = ModuleData::displayPathIsListed;
    uint32_t number = encoder.shownStringNumber(module.displayPath);
    // The names of functions report the limit.
    lists.displayPaths.append({ Number24(id.index()), Number24(std::min(number, Number24::max)) });
}

static void appendModule(TableEncoder& encoder, ModuleID id, const Module& module, ModuleLists& lists)
{
    ModuleData entry { };
    setDisplayPath(encoder, id, module, lists, entry);
    entry.hasInstalledPositions = module.flags.contains(ModuleFlags::HasInstalledPositions);
    entry.kind = module.kind;
    entry.externalKind = module.externalKind;
    entry.exportRule = module.exportRule;
    if (module.kind == ModuleKind::CommonJs && module.syntheticExports.isEmpty())
        entry.add(ModuleData::Flag::CapturesOnDemand);
    entry.requireResult = module.requireResult;
    if (module.flags.contains(ModuleFlags::HasTopLevelAwait))
        entry.add(ModuleData::Flag::HasTopLevelAwait);
    // Only a CommonJS module can go on as evaluated behind a body that threw.
    if (module.kind != ModuleKind::CommonJs || module.flags.contains(ModuleFlags::KeepsError))
        entry.add(ModuleData::Flag::KeepsError);
    entry.body = optionalNumber24(bodyEntrypointOf(encoder, module));
    if (module.flags.contains(ModuleFlags::EvaluatesItself) && entry.bodyFunction())
        entry.add(ModuleData::Flag::EvaluatesItself);
    RELEASE_ASSERT(module.kind != ModuleKind::External || !module.runtimeModule);
    entry.referrerOrRuntimeModule = optionalNumber24(module.kind == ModuleKind::External ? indexOrNone(module.referrer) : indexOrNone(module.runtimeModule));
    if (module.exportsVariable) {
        entry.value = placedVariableSlot(encoder, module.exportsVariable);
        entry.add(ModuleData::Flag::HasValue);
    } else if (encoder.placement.hasModuleValue(id)) {
        entry.value = SlotData::create(encoder.checkedOffset(encoder.placement.moduleValueOffset(id)), SlotRepresentation::JSValue);
        entry.add(ModuleData::Flag::HasValue);
    }
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

// No walk has a reason to visit a module of which nothing runs and which imports only such modules.
static BitVector findModulesNotWorthVisiting(const TableEncoder& encoder)
{
    const Vector<Module>& modules = encoder.program.modules;
    StronglyConnectedComponents components = computeStronglyConnectedComponents(modules.size(), [&](unsigned index) {
        return RequestTargets { modules[index].requests };
    });
    const Vector<uint32_t>& componentOf = components.componentOf;

    // A component has a higher number than every component it requests.
    BitVector componentsWorthVisiting(components.numberOfComponents);
    for (uint32_t index : components.members) {
        bool isWorthVisiting = !runsNothingItself(encoder, modules[index]) || std::ranges::any_of(modules[index].requests, [&](const ModuleRequest& request) {
            return componentsWorthVisiting.quickGet(componentOf[request.target.index()]);
        });
        if (isWorthVisiting)
            componentsWorthVisiting.quickSet(componentOf[index]);
    }

    BitVector modulesNotWorthVisiting(modules.size());
    for (size_t index = 0; index < modules.size(); ++index) {
        if (!componentsWorthVisiting.quickGet(componentOf[index]))
            modulesNotWorthVisiting.quickSet(index);
    }
    return modulesNotWorthVisiting;
}

// The code of the requesting module evaluates the target by itself, so nobody links the target at that point: it is linked with the requesting module.
static bool isAnsweredByCode(const TableEncoder& encoder, ModuleID requester, const ModuleRequest& request)
{
    switch (encoder.program.at(request.target).kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
        return !encoder.placement.requireEdge(requester, request.target);
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A module that is evaluated and has nothing to run stays: its importer's evaluation is what makes it an evaluated module, and the lists that the walks go by leave it out.
static void appendRequests(const TableEncoder& encoder, ModuleID id, const Module& module, const BitVector& modulesNotWorthVisiting, ModuleLists& lists)
{
    const Program& program = encoder.program;
    for (const ModuleRequest& request : module.requests) {
        if (modulesNotWorthVisiting.quickGet(request.target.index()) && isNeverEvaluated(program, program.at(request.target)))
            continue;
        bool isRequire = request.kind == ModuleRequestKind::Require;
        if (isRequire && !isAnsweredByCode(encoder, id, request))
            continue;
        lists.requests.append(ModuleRequestData::create(request.target.index(), request.phase == ImportPhase::Defer, request.isDynamicStarExport, isRequire));
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

static void markExportsBoundByRequiredName(ModuleLists& lists)
{
    for (const RequiredNameData& requiredName : lists.requiredNames) {
        const ModuleData& exporter = lists.modules[requiredName.exporter];
        if (exporter.exportRule != SyntheticExportRule::RuntimeModule)
            continue;
        uint32_t end = lists.modules[requiredName.exporter + 1].firstSyntheticExport;
        for (uint32_t index = exporter.firstSyntheticExport; index < end; ++index) {
            if (lists.syntheticExports[index].name == requiredName.name)
                lists.syntheticExports[index].isBoundByRequiredName = true;
        }
    }
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
        if (isBundlerHelperModule(program, module))
            lists.modules.last().add(ModuleData::Flag::IsLibraryCodeWithFilePath);
        if (module.kind == ModuleKind::RuntimeSource || module.flags.contains(ModuleFlags::IsLibrary) || isBundlerHelperModule(program, module))
            lists.modules.last().add(ModuleData::Flag::IsOfLibrary);
        appendRequests(encoder, id, module, modulesNotWorthVisiting, lists);
        appendExports(encoder, id, module, lists);
        appendSyntheticExports(encoder, module, lists);
        appendRequiredNames(encoder, module, lists);
    }
    for (const Entry& entry : program.entries)
        lists.modules[entry.module.index()].add(ModuleData::Flag::IsEntryPoint);
    ModuleData endEntry { };
    setListStarts(endEntry, lists);
    lists.modules.append(endEntry);
    markExportsBoundByRequiredName(lists);
    return lists;
}

static void appendModuleFailures(TableEncoder& encoder)
{
    Vector<ModuleFailureData> failures;
    for (const ModuleFailure& failure : encoder.program.moduleFailures)
        failures.append(failure.exporter ? ModuleFailureData::ofAbsentExport(failure.exporter, encoder.shownStringNumber(failure.specifier)) : ModuleFailureData { failure.importer.index(), encoder.shownStringNumber(failure.specifier) });
    encoder.setTable(ProgramTableIndex::moduleFailures, failures);
}

static Vector<uint16_t> encodeModuleNumbers(const TableEncoder& encoder, std::span<const uint32_t> modules)
{
    // The table of modules ends with one more record.
    bool needsTwoWords = ModuleNumbers::needsTwoWords(encoder.program.modules.size() + 1);
    Vector<uint16_t> words;
    words.reserveInitialCapacity(needsTwoWords ? 2 * modules.size() : modules.size());
    for (uint32_t module : modules) {
        words.append(static_cast<uint16_t>(module));
        if (needsTwoWords)
            words.append(static_cast<uint16_t>(module >> 16));
    }
    return words;
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

// The generic entrypoint of a function without a body only throws.
static bool hasCompiledBody(const TableEncoder& encoder, const Module& module)
{
    return module.body && encoder.program.at(module.body).hasBody();
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
        if (isNeverEvaluated(program, module) || (isBundlerHelperModule(program, module) && !isBundlerHelperNamedByProgram(program, ModuleID(index))))
            continue;
        modules.append(index);
    }
    std::ranges::sort(modules, [&](uint32_t left, uint32_t right) {
        return std::ranges::lexicographical_compare(program.string(program.modules[left].path), program.string(program.modules[right].path));
    });
    encoder.setTable(ProgramTableIndex::modulesByPath, encodeModuleNumbers(encoder, modules.span()));
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
        // The generic entrypoint of a function without a body only throws. Without an entry here, the host creates its ordinary built-in.
        if (!program.at(entry.function).hasBody())
            continue;
        runtimeFunctions.append({ entry.id.index(), encoder.entrypointOf(entry.function), static_cast<uint32_t>(entry.visibility) });
    }
    encoder.setTable(ProgramTableIndex::compiledRuntimeFunctions, runtimeFunctions);
}

static ContractedModuleRequests contractRequestLists(TableEncoder& encoder, ModuleLists& lists)
{
    BitVector modulesNamedByRequire(encoder.program.modules.size());
    for (const RequireEdge& edge : encoder.placement.requireEdges())
        modulesNamedByRequire.quickSet(edge.target.index());
    for (const ModuleRequestData& request : lists.requests) {
        if (request.isRequire)
            modulesNamedByRequire.quickSet(request.target());
    }

    uint32_t maxContractedRequests = static_cast<uint32_t>(std::min<uint64_t>(encoder.build.limit(LimitID::MaxContractedModuleRequests), UINT32_MAX));
    ContractedModuleRequests contracted = contractModuleRequests(lists.modules.mutableSpan(), lists.requests.span(), modulesNamedByRequire, maxContractedRequests);
    for (uint32_t module : contracted.modulesOverLimit)
        encoder.build.gaveUp(LimitID::MaxContractedModuleRequests, { .function = encoder.program.modules[module].body });
    return contracted;
}

Vector<ModuleImportSite> findImportSites(const Program& program, const ParallelLoops& loops)
{
    auto namesModule = [](const Value& value) {
        return value.opcode == Opcode::ImportModule;
    };
    Vector<uint32_t> importers = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return std::ranges::any_of(program.functions[index].values, namesModule);
    });
    Vector<ModuleImportSite> sites;
    for (uint32_t importer : importers) {
        for (const Value& value : program.functions[importer].values) {
            if (namesModule(value))
                sites.append(ModuleImportSite { value.referrer().index(), value.module().index() });
        }
    }
    return sites;
}

// The process starts at the first entry.
static Vector<LinkPlanStepData> planLinkOfFirstEntry(const TableEncoder& encoder, ModuleLists& lists)
{
    if (encoder.program.entries.isEmpty())
        return { };
    return planLinkOfEntryPoint(lists.modules.mutableSpan(), lists.requests.span(), encoder.program.entries.first().module);
}

void encodeModuleTables(TableEncoder& encoder)
{
    ModuleLists lists = collectModuleLists(encoder);
    computeModuleFlags(lists.modules.mutableSpan(), lists.requests.span());
    for (size_t index = 0; index < encoder.program.modules.size(); ++index)
        setPublicPath(encoder, encoder.program.modules[index], lists, lists.modules[index]);
    ContractedModuleRequests contracted = contractRequestLists(encoder, lists);
    Vector<LinkPlanStepData> entryPointLinkPlan = planLinkOfFirstEntry(encoder, lists);
    std::optional<ModuleID> plannedEntryPoint = encoder.program.entries.isEmpty() ? std::nullopt : std::optional(encoder.program.entries.first().module);
    LinkPlansOfRoots linkPlansOfRoots = planLinksOfRoots(lists.modules.mutableSpan(), lists.requests.span(), encoder.inputs.bodyFacts.importSites.span(), plannedEntryPoint);
    Vector<uint32_t> modulesOfUncompiledCode;
    for (uint32_t index = 0; index + 1 < lists.modules.size(); ++index) {
        if (lists.modules[index].isUncompiledCode())
            modulesOfUncompiledCode.append(index);
    }
    encoder.setTable(ProgramTableIndex::modulesOfUncompiledCode, modulesOfUncompiledCode);
    encoder.setTable(ProgramTableIndex::modules, lists.modules);
    encoder.setTable(ProgramTableIndex::moduleDirectories, lists.directories);
    encoder.setTable(ProgramTableIndex::moduleDisplayPaths, lists.displayPaths);
    encoder.setTable(ProgramTableIndex::moduleRequests, lists.requests);
    encoder.setTable(ProgramTableIndex::linkRequests, encodeModuleNumbers(encoder, contracted.linkRequests.span()));
    encoder.setTable(ProgramTableIndex::evaluationRequests, encodeModuleNumbers(encoder, contracted.evaluationRequests.span()));
    encoder.setTable(ProgramTableIndex::entryPointLinkPlan, entryPointLinkPlan);
    encoder.setTable(ProgramTableIndex::linkPlanRoots, linkPlansOfRoots.roots);
    encoder.setTable(ProgramTableIndex::linkPlanStepsOfRoots, linkPlansOfRoots.steps);
    encoder.setTable(ProgramTableIndex::moduleExports, lists.exports);
    encoder.setTable(ProgramTableIndex::indirectExports, lists.indirectExports);
    encoder.setTable(ProgramTableIndex::syntheticExports, lists.syntheticExports);
    encoder.setTable(ProgramTableIndex::requiredNames, lists.requiredNames);
    appendModuleFailures(encoder);
    appendModulesByPath(encoder);
    appendCompiledRuntimeModules(encoder);
    appendCompiledRuntimeFunctions(encoder);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
