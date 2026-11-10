/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTModuleGraph.h"
#include "AOTModuleHost.h"
#include "AOTModuleLists.h"
#include "AOTPhaseTimes.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <utility>
#include <wtf/MathExtras.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

namespace {


struct ModuleListedByPath {
    std::span<const uint8_t> path;
    uint32_t module { 0 };
};

struct ModulesListedInChunk {
    Vector<ModuleListedByPath> modulesByPath;
    Vector<std::pair<uint32_t, uint32_t>> entriesByHashOfPath;
    Vector<uint32_t> hashesOfDirectories;
    Vector<CompiledRuntimeModuleData> runtimeModules;
    Vector<uint32_t> modulesRejectingUncompiledCode;
    Vector<uint32_t> modulesWithReadOnlyBuiltins;
    Vector<uint32_t> importedModuleAliases;
    Vector<uint32_t> modulesThatDependOnMoreThanSource;
};

struct FunctionsListedInRange {
    Vector<StateOfLibraryModuleData> stateOfLibraryModules;
    Vector<uint32_t> asyncFunctionsWithoutAwait;
    Vector<uint32_t> functionsImportingOwnOriginal;
    Vector<RequestedOriginalData> requestedOriginals;
};

struct RequestsThatWalksFollow {
    Vector<uint32_t> linkRequests;
    Vector<uint32_t> evaluationRequests;
    Vector<ImportOfHeldFileData> importsOfHeldFiles;
};

struct StartsOfModules {
    Vector<uint32_t> firstSteps;
    Vector<ModuleStartStepData> steps;
};

struct TablesOfModulesByPath {
    Vector<uint16_t> modulesByPath;
    Vector<uint32_t> modulesByHashOfPath;
    Vector<uint16_t> firstModulesByHashOfPath;
    Vector<uint32_t> directoriesOfModulesByPath;
};

} // anonymous namespace

template<typename Part, typename T>
static Vector<T> joinListsForModuleTables(const Vector<Part>& parts, Vector<T> Part::* list)
{
    size_t size = 0;
    for (const Part& part : parts)
        size += (part.*list).size();
    Vector<T> result;
    result.reserveInitialCapacity(size);
    for (const Part& part : parts)
        result.appendVector(part.*list);
    return result;
}

static std::span<const ModuleRequestData> requestsInModuleTables(const ModuleLists& lists, uint32_t module)
{
    uint32_t first = lists.modules[module].firstRequest;
    return lists.requests.span().subspan(first, lists.modules[module + 1].firstRequest - first);
}

static Vector<uint16_t> encodeNumbersForModuleTables(const Typed::Program& program, const Vector<uint32_t>& numbers)
{
    bool needsTwoWords = ModuleNumbers::needsTwoWords(program.modules.size() + 1);
    Vector<uint16_t> words;
    words.reserveInitialCapacity(needsTwoWords ? 2 * numbers.size() : numbers.size());
    for (uint32_t number : numbers) {
        words.append(static_cast<uint16_t>(number));
        if (needsTwoWords)
            words.append(static_cast<uint16_t>(number >> 16));
    }
    return words;
}

template<typename NumberOfString>
static Vector<UncompiledModuleData> listModulesOfUncompiledCode(const Typed::Program& program, const AnswersAboutModules& answers, const NumberOfString& numberOfString)
{
    return answers.modulesOfUncompiledCode.map([&](uint32_t index) {
        StringID typeAttribute = program.modules[index].typeAttribute;
        return UncompiledModuleData { index, typeAttribute ? numberOfString(typeAttribute) : UncompiledModuleData::noTypeAttribute };
    });
}

template<typename NumberOfString>
static Vector<ModuleFailureData> listFailuresForModuleTables(const Typed::Program& program, const NumberOfString& numberOfString)
{
    Vector<ModuleFailureData> failures;
    Vector<ModuleFailureData> failedLoads;
    failures.reserveInitialCapacity(program.moduleFailures.size());
    for (const Typed::ModuleFailure& failure : program.moduleFailures) {
        if (failure.failedLoadRoot)
            failedLoads.append(ModuleFailureData::ofLoad(failure.failedLoadRoot, ModuleFailureID(failures.size())));
        uint32_t specifier = numberOfString(failure.specifier);
        if (failure.exportSpecifier)
            failures.append(ModuleFailureData::ofExportStatement(failure.importer, numberOfString(failure.exportSpecifier), specifier));
        else if (failure.exporter)
            failures.append(ModuleFailureData::ofAbsentExport(failure.importer, failure.exporter, specifier));
        else
            failures.append(ModuleFailureData { failure.importer.index(), specifier, failure.importer.index() });
    }
    AOT::sort(failedLoads, { }, &ModuleFailureData::importer);
    failures.appendVector(failedLoads);
    return failures;
}

template<typename NumberOfString>
static Vector<NameOfStatementData> listNamesOfStatements(const Typed::Program& program, const NumberOfString& numberOfString)
{
    Vector<NameOfStatementData> names;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        for (StringID name : program.modules[index].namesOfStatements)
            names.append({ index, numberOfString(name) });
    }
    return names;
}

static Vector<StarAsWrittenData> listStarsAsWritten(const Typed::Program& program, const AnswersAboutModules& answers)
{
    Vector<StarAsWrittenData> stars;
    for (uint32_t index = 0; index < program.modules.size(); ++index) {
        size_t first = stars.size();
        for (const Typed::ModuleRequest& request : program.modules[index].requests) {
            if (request.starPlace && !answers.ofModules[request.target.index()].containsAll({ AnswerAboutModule::IsNotWorthVisiting, AnswerAboutModule::IsNeverEvaluated }))
                stars.append({ index, request.target.index(), *request.starPlace, request.isDynamicStarExport });
        }
        AOT::sort(stars.mutableSpan().subspan(first), [](const StarAsWrittenData& a, const StarAsWrittenData& b) {
            return a.place < b.place;
        });
    }
    return stars;
}

void nameWhatModuleTablesName(const TableInputs& inputs, const GatheredMentions&, NamesOfTables& names)
{
    RELEASE_ASSERT(inputs.build);
    const ParallelLoops& loops = inputs.build->parallelLoops();
    StepTimes times(*inputs.build, "image: module tables"_s);
    const AnswersAboutModules& answers = inputs.answersAboutModules.emplace(askAboutModules(inputs, loops));
    times.didFinish("ask about the modules"_s);

    auto noteString = [&](StringID id) -> uint32_t {
        names.strings.set(id.index());
        return 0;
    };
    nameWhatModuleListsName(inputs, answers, loops, names);
    listModulesOfUncompiledCode(inputs.program, answers, noteString);
    listFailuresForModuleTables(inputs.program, noteString);
    listNamesOfStatements(inputs.program, noteString);
    for (const Typed::Function& function : inputs.program.functions) {
        for (StringID specifier : function.requestedOriginals)
            noteString(specifier);
    }
    for (uint32_t place : answers.compiledRuntimeFunctions)
        names.functionsWithClosureCell.set(inputs.program.runtimeFunctions[place].function.index());
}

static bool evaluationVisitsModule(const ModuleData& module)
{
    return module.needsEvaluation() && (module.hasExportTable() || module.kind == ModuleKind::CommonJs || module.isUncompiledCode());
}

static void appendModulesThatEvaluationVisitsNext(const ModuleLists& lists, uint32_t module, BitVector& isListed, Vector<uint32_t>& targets)
{
    if (!evaluationVisitsModule(lists.modules[module]) || !lists.modules[module].hasExportTable() || lists.modules[module].hasDeferredRequest())
        return;
    size_t first = targets.size();
    for (const ModuleRequestData& request : requestsInModuleTables(lists, module)) {
        uint32_t target = request.target();
        if (target == module || request.isRequire)
            continue;
        if (!evaluationVisitsModule(lists.modules[target]) || isListed.quickSet(target))
            continue;
        targets.append(target);
    }
    for (uint32_t target : targets.span().subspan(first))
        isListed.quickClear(target);
}

static void appendImportsOfHeldFiles(const ModuleLists& lists, uint32_t importer, Vector<ImportOfHeldFileData>& result)
{
    if (lists.modules[importer].kind != ModuleKind::SourceText)
        return;
    for (const ModuleRequestData& request : requestsInModuleTables(lists, importer)) {
        if (request.isRequire)
            continue;
        const ModuleData& target = lists.modules[request.target()];
        uint32_t file = target.isFacade() ? lists.requests[target.firstRequest].target() : request.target();
        const ModuleData& held = lists.modules[file];
        bool isOfLibraryAndMayBeMocked = (held.kind == ModuleKind::RuntimeSource || held.kind == ModuleKind::Runtime) && held.mayBeMocked();
        if (held.requireResult == RequireResult::NamespaceIfImported || held.isImportableDuringEvaluation() || isOfLibraryAndMayBeMocked)
            result.append(ImportOfHeldFileData { file, importer });
    }
}

static RequestsThatWalksFollow listRequestsThatWalksFollow(const ParallelLoops& loops, ModuleLists& lists, const Typed::StronglyConnectedComponents& componentsOfRequests)
{
    uint32_t numberOfModules = static_cast<uint32_t>(lists.modules.size() - 1);
    ChunksOfIndices chunks = divideIntoChunks(numberOfModules);
    Vector<RequestsThatWalksFollow> followedInChunks(chunks.size());
    Vector<uint32_t> placesInChunks(numberOfModules);
    LinkRequestsOfModules linkRequests;
    forEachIndexOfLoopsInParallel(loops, {
        { 1, [&](unsigned) { linkRequests = contractLinkRequests(lists.modules.mutableSpan(), lists.requests.span(), componentsOfRequests); } },
        { chunks.size(), [&](unsigned chunk) {
            RequestsThatWalksFollow& followed = followedInChunks[chunk];
            BitVector isListed(numberOfModules);
            for (uint32_t module = chunks.firstIndexOf[chunk]; module < chunks.firstIndexOf[chunk + 1]; ++module) {
                placesInChunks[module] = static_cast<uint32_t>(followed.evaluationRequests.size());
                appendModulesThatEvaluationVisitsNext(lists, module, isListed, followed.evaluationRequests);
                appendImportsOfHeldFiles(lists, module, followed.importsOfHeldFiles);
            }
        } },
    });
    layLinkRequests(lists.modules.mutableSpan(), linkRequests);

    uint32_t firstEvaluationRequest = 0;
    for (unsigned chunk = 0; chunk < chunks.size(); ++chunk) {
        for (uint32_t module = chunks.firstIndexOf[chunk]; module < chunks.firstIndexOf[chunk + 1]; ++module)
            lists.modules[module].firstEvaluationRequest = Number24(firstEvaluationRequest + placesInChunks[module]);
        firstEvaluationRequest += static_cast<uint32_t>(followedInChunks[chunk].evaluationRequests.size());
    }
    lists.modules[numberOfModules].firstEvaluationRequest = Number24(firstEvaluationRequest);

    RequestsThatWalksFollow result {
        WTF::move(linkRequests.requests),
        joinListsForModuleTables(followedInChunks, &RequestsThatWalksFollow::evaluationRequests),
        joinListsForModuleTables(followedInChunks, &RequestsThatWalksFollow::importsOfHeldFiles),
    };
    AOT::sort(result.importsOfHeldFiles, { }, &ImportOfHeldFileData::key);
    result.importsOfHeldFiles.shrink(std::ranges::unique(result.importsOfHeldFiles, { }, &ImportOfHeldFileData::key).begin() - result.importsOfHeldFiles.begin());
    return result;
}

static void appendModuleAliases(const Typed::Module& module, uint32_t index, Vector<uint32_t>& result)
{
    using Typed::ModuleFlags;
    constexpr std::pair<ModuleFlags, ModuleAliasKind> otherNames[] = {
        { ModuleFlags::IsImportedWithoutExtension, ModuleAliasKind::WithoutExtension },
        { ModuleFlags::IsImportedWithOtherExtension, ModuleAliasKind::WithOtherExtension },
        { ModuleFlags::IsImportedAsDirectory, ModuleAliasKind::AsDirectory },
    };
    if (!module.flags.contains(ModuleFlags::MayBeMocked))
        return;
    for (auto [flag, name] : otherNames) {
        if (module.flags.contains(flag))
            result.append(encodeModuleAlias(index, name));
    }
}

static void appendHashesOfModulePath(const Typed::Program& program, uint32_t index, ModulesListedInChunk& listed)
{
    String path = String::fromUTF8(program.string(program.modules[index].path));
    ModuleByHashOfPath sought(program.modules.size() + 1, StringView(path).hash());
    listed.entriesByHashOfPath.append({ sought.hash(), sought.entryFor(index) });
    uint32_t hash = HashOfDirectory::ofNoText;
    for (char16_t character : StringView(path).codeUnits()) {
        hash = HashOfDirectory::with(hash, character);
        if (character == '/')
            listed.hashesOfDirectories.append(hash);
    }
}

static void listModulesOfChunkForModuleTables(const TableInputs& inputs, const AnswersAboutModules& answers, uint32_t first, uint32_t end, ModulesListedInChunk& listed)
{
    using Typed::ModuleFlags;
    const Typed::Program& program = inputs.program;
    for (uint32_t index = first; index < end; ++index) {
        ModuleID id(index);
        const Typed::Module& module = program.modules[index];
        if (module.flags.contains(ModuleFlags::RejectsUncompiledCode))
            listed.modulesRejectingUncompiledCode.append(index);
        if (module.flags.contains(ModuleFlags::HasReadOnlyBuiltins))
            listed.modulesWithReadOnlyBuiltins.append(index);
        appendModuleAliases(module, index, listed.importedModuleAliases);
        if (module.flags.contains(ModuleFlags::DependsOnMoreThanSource))
            listed.modulesThatDependOnMoreThanSource.append(index);

        bool isFoundOnlyWithCode = module.kind == ModuleKind::RuntimeSource || module.kind == ModuleKind::Data;
        if (!Typed::answersToPath(program, module) || (isFoundOnlyWithCode && !answers.has(id, AnswerAboutModule::HasCompiledBody)))
            continue;
        if (module.kind == ModuleKind::RuntimeSource && module.runtimeModule)
            listed.runtimeModules.append(CompiledRuntimeModuleData { module.runtimeModule.index(), index, inputs.facts.linked().modulesWhoseValueStaysIn.get(index), inputs.facts.linked().modulesWhoseRecordIsHandedOutFrozen.get(index) });
        if (answers.has(id, AnswerAboutModule::IsNeverEvaluated))
            continue;
        if (Typed::isBundlerHelperModule(program, module) && !module.flags.contains(ModuleFlags::MayBeMocked) && !Typed::isBundlerHelperNamedByProgram(program, id))
            continue;
        listed.modulesByPath.append({ program.string(module.path), index });
        appendHashesOfModulePath(program, index, listed);
    }
    radixSortKeepingEqualKeysInOrder<32>(listed.hashesOfDirectories, [](uint32_t hash) {
        return hash;
    });
    removeRepeatedElements(listed.hashesOfDirectories);
}

static TablesOfModulesByPath makeTablesOfModulesByPath(const Typed::Program& program, const ParallelLoops& loops, const Vector<ModulesListedInChunk>& listedInChunks)
{
    RELEASE_ASSERT(program.modules.size() < Number24::max);
    Vector<ModuleListedByPath> modulesByPath = joinListsForModuleTables(listedInChunks, &ModulesListedInChunk::modulesByPath);
    parallelSort(loops, modulesByPath.mutableSpan(), [](const ModuleListedByPath& left, const ModuleListedByPath& right) {
        return std::ranges::lexicographical_compare(left.path, right.path);
    });

    Vector<uint32_t> hashesOfDirectories = joinListsForModuleTables(listedInChunks, &ModulesListedInChunk::hashesOfDirectories);
    sortAndRemoveDuplicates(loops, hashesOfDirectories);
    Vector<uint32_t> directoriesOfModulesByPath;
    if (!hashesOfDirectories.isEmpty()) {
        size_t numberOfBits = WTF::roundUpToPowerOfTwo(std::max<size_t>(hashesOfDirectories.size() * HashOfDirectory::bitsPerDirectory, 32));
        directoriesOfModulesByPath.fill(0, numberOfBits / 32);
        for (uint32_t hash : hashesOfDirectories) {
            size_t bit = HashOfDirectory::bit(hash, numberOfBits);
            directoriesOfModulesByPath[bit / 32] |= 1u << (bit % 32);
        }
    }

    Vector<std::pair<uint32_t, uint32_t>> entriesByHash = joinListsForModuleTables(listedInChunks, &ModulesListedInChunk::entriesByHashOfPath);
    parallelSort(loops, entriesByHash.mutableSpan(), std::less<std::pair<uint32_t, uint32_t>> { });
    size_t numberOfBuckets = ModuleByHashOfPath::numberOfBuckets(entriesByHash.size());
    Vector<uint32_t> firstModulesByHashOfPath;
    for (size_t place = 0; place < entriesByHash.size(); ++place) {
        while (firstModulesByHashOfPath.size() <= static_cast<uint64_t>(entriesByHash[place].first) * numberOfBuckets >> 32)
            firstModulesByHashOfPath.append(static_cast<uint32_t>(place));
    }
    while (numberOfBuckets && firstModulesByHashOfPath.size() <= numberOfBuckets)
        firstModulesByHashOfPath.append(static_cast<uint32_t>(entriesByHash.size()));

    return {
        encodeNumbersForModuleTables(program, modulesByPath.map([](const ModuleListedByPath& listed) { return listed.module; })),
        entriesByHash.map([](const auto& entry) { return entry.second; }),
        encodeNumbersForModuleTables(program, firstModulesByHashOfPath),
        WTF::move(directoriesOfModulesByPath),
    };
}

static Vector<CompiledRuntimeFunctionData> listCompiledRuntimeFunctions(const TableEncoder& encoder, const AnswersAboutModules& answers)
{
    return answers.compiledRuntimeFunctions.map([&](uint32_t place) {
        const Typed::RuntimeFunctionEntry& entry = encoder.program.runtimeFunctions[place];
        if (!encoder.numbers.closureCellNumber(entry.function))
            encoder.fail("FunctionThatHostCallsWithoutClosureCell"_s, entry.function);
        return CompiledRuntimeFunctionData { entry.id.index(), encoder.entrypointOf(entry.function, "a function of the run time that the host calls"_s), static_cast<uint32_t>(entry.visibility) };
    });
}

static bool initialWordMayBeRead(const Typed::LocationFacts& location, const PlacedVariable& placed)
{
    if (placed.plan.encoding != Typed::VariableEncoding::Plain || location.initialization == Typed::InitializationState::Unknown)
        return true;
    SlotTraits traits = traitsOf(placed.plan.representation);
    return traits.hasAbsent && placed.initialWord == traits.absentBits;
}

static ModuleID moduleThatStartsFromWord(const TableEncoder& encoder, VariableID id, const PlacedVariable& placed)
{
    const Typed::Program& program = encoder.program;
    const Typed::Plan& plan = encoder.plan;
    if (const Typed::PromotedVariable* promoted = plan.promotedEnvironments().find(id))
        return initialWordMayBeRead(promoted->location, placed) ? promoted->module : ModuleID();
    const Typed::Variable& variable = program.at(id);
    if (variable.owner || !initialWordMayBeRead(encoder.facts.variable(id).location, placed))
        return { };
    return variable.module;
}

static bool isLibraryModuleWithBodyForModuleTables(const Typed::Program& program, ModuleID module)
{
    return module && program.at(module).kind == ModuleKind::RuntimeSource;
}

static void listStateOfLibraryModulesInVariables(const TableEncoder& encoder, size_t first, size_t end, Vector<StateOfLibraryModuleData>& found)
{
    const Vector<PlacedVariable>& placedVariables = encoder.numbers.statics().variables;
    for (size_t index = first; index < end; ++index) {
        const PlacedVariable& placed = placedVariables[index];
        if (!placed.hasWordInStatics())
            continue;
        ModuleID module = moduleThatStartsFromWord(encoder, VariableID(static_cast<uint32_t>(index)), placed);
        if (isLibraryModuleWithBodyForModuleTables(encoder.program, module))
            found.append(StateOfLibraryModuleData::create(module.index(), StateOfLibraryModuleData::Kind::Variables, placed.offset, widthOf(placed.plan.representation)));
    }
}

static void listFunctionsForModuleTables(const TableEncoder& encoder, size_t first, size_t end, FunctionsListedInRange& listed)
{
    const Typed::Program& program = encoder.program;
    for (size_t index = first; index < end; ++index) {
        const Typed::Function& function = program.functions[index];
        if (Typed::isAsyncFunctionWithoutAwait(program, function))
            listed.asyncFunctionsWithoutAwait.append(static_cast<uint32_t>(index));
        if (function.syntax.contains(Typed::FunctionSyntax::MayImportOwnOriginal))
            listed.functionsImportingOwnOriginal.append(static_cast<uint32_t>(index));
        for (StringID specifier : function.requestedOriginals)
            listed.requestedOriginals.append({ static_cast<uint32_t>(index), encoder.stringNumber(specifier) });
        uint32_t cell = encoder.numbers.closureCellNumber(FunctionID(static_cast<uint32_t>(index)));
        if (cell && isLibraryModuleWithBodyForModuleTables(program, function.module))
            listed.stateOfLibraryModules.append(StateOfLibraryModuleData::create(function.module.index(), StateOfLibraryModuleData::Kind::Closures, cell, 1));
    }
}

static void listStateOfLibraryModulesInClasses(const TableEncoder& encoder, size_t first, size_t end, Vector<StateOfLibraryModuleData>& found)
{
    const Typed::Program& program = encoder.program;
    for (size_t index = first; index < end; ++index) {
        FunctionID constructor = encoder.plan.isInUse(LayoutID(static_cast<uint32_t>(index))) ? program.layouts[index].constructor : FunctionID();
        ModuleID module = constructor ? program.at(constructor).module : ModuleID();
        if (isLibraryModuleWithBodyForModuleTables(program, module))
            found.append(StateOfLibraryModuleData::create(module.index(), StateOfLibraryModuleData::Kind::Classes, static_cast<uint32_t>(index), 1));
    }
}

static Vector<StateOfLibraryModuleData> uniteStateOfLibraryModules(Vector<StateOfLibraryModuleData>&& found)
{
    AOT::sort(found, { }, [](const StateOfLibraryModuleData& state) {
        return std::pair(state.moduleAndKind, state.first);
    });
    Vector<StateOfLibraryModuleData> ranges;
    for (const StateOfLibraryModuleData& state : found) {
        if (!ranges.isEmpty() && ranges.last().moduleAndKind == state.moduleAndKind && ranges.last().first + ranges.last().count == state.first)
            ranges.last().count += state.count;
        else
            ranges.append(state);
    }
    return ranges;
}

static std::optional<uint32_t> findBodyForStartsOfModules(const TableEncoder& encoder, FunctionID body, ModuleID owner)
{
    const Typed::CallingConventions& conventions = encoder.plan.conventions();
    CodeUnit unit = conventions.directEntryOf(body);
    std::optional<uint32_t> position = conventions.exists(unit) ? encoder.inputs.functions.positionOfUnit(unit, conventions.indexOf(unit)) : std::nullopt;
    if (!position) {
        encoder.fail("StartOfModuleCallsBodyWithoutCode"_s, owner, makeString("the body is f"_s, body.index()));
        return std::nullopt;
    }
    Typed::ArgumentLocations locations = conventions.locationsOf(encoder.program, unit);
    bool takesOnlyBoxedThis = locations.convention == Typed::EntryConvention::Direct && !conventions.stackWordsReservedByCallersOf(encoder.program, unit) && std::ranges::all_of(locations.parameters, [](const Typed::ArgumentLocation& parameter) {
        if (parameter.kind == Typed::ArgumentLocation::Kind::None)
            return true;
        return parameter.kind == Typed::ArgumentLocation::Kind::GPR && parameter.reg == Reg(directArgumentGPR(0)) && parameter.representation == Typed::Representation::JSValue;
    });
    if (!takesOnlyBoxedThis || encoder.inputs.functions[*position].body.primaryEntryOffset() || *position > ModuleStartStepData::maxOperand) {
        encoder.fail("StartOfModuleCannotEnterBody"_s, owner, makeString("the body is f"_s, body.index()));
        return std::nullopt;
    }
    return position;
}

static StartsOfModules listStartsOfModules(const TableEncoder& encoder)
{
    using Operation = ModuleStartStepData::Operation;
    using Kind = Typed::ModuleStartOperation::Kind;
    StartsOfModules result;
    const Typed::Program& program = encoder.program;
    const Typed::ModuleEvaluationPlan& evaluation = encoder.plan.moduleEvaluation();
    if (evaluation.starts().empty())
        return result;

    result.firstSteps.fill(0, program.modules.size());
    result.steps.append(ModuleStartStepData::create(Operation::Trap));
    for (const Typed::ModuleStart& start : evaluation.starts()) {
        if (!encoder.plan.conventions().exists({ start.body, Typed::CodeRole::ModuleStart }))
            continue;
        result.firstSteps[evaluation.numberOf(start)] = static_cast<uint32_t>(result.steps.size());
        bool returns = true;
        for (const Typed::ModuleStartOperation& operation : evaluation.operationsOf(start)) {
            switch (operation.kind) {
            case Kind::CallStart: {
                const Typed::ModuleStart* called = evaluation.findStartOfBody(operation.callee);
                if (!called || !encoder.plan.conventions().exists({ operation.callee, Typed::CodeRole::ModuleStart })) {
                    encoder.fail("StartOfModuleCallsStartThatNothingReaches"_s, start.module, makeString("the body is f"_s, operation.callee.index()));
                    break;
                }
                result.steps.append(ModuleStartStepData::create(Operation::CallStart, evaluation.numberOf(*called)));
                break;
            }
            case Kind::CallBody:
                if (std::optional<uint32_t> position = findBodyForStartsOfModules(encoder, operation.callee, start.module))
                    result.steps.append(ModuleStartStepData::create(Operation::CallBody, *position));
                break;
            case Kind::Begin:
                result.steps.append(ModuleStartStepData::create(Operation::Begin, operation.module.index(), Typed::ModuleEvaluationPlan::beginAsksRunTime(program, operation.module)));
                break;
            case Kind::BeginOrReturn:
                result.steps.append(ModuleStartStepData::create(Operation::BeginOrReturn, operation.module.index(), Typed::ModuleEvaluationPlan::beginAsksRunTime(program, operation.module)));
                break;
            case Kind::Finish:
                result.steps.append(ModuleStartStepData::create(Operation::Finish, operation.module.index(), evaluation.finishLeavesStatusInRecord(program, operation.module)));
                break;
            case Kind::ReturnIfLoadingStops:
                result.steps.append(ModuleStartStepData::create(Operation::ReturnIfLoadingStops, operation.module.index()));
                break;
            case Kind::AbandonOnThrow:
                result.steps.append(ModuleStartStepData::create(Operation::AbandonOnThrow, operation.module.index()));
                break;
            case Kind::NeverComesBack:
                returns = false;
                break;
            }
        }
        result.steps.append(ModuleStartStepData::create(returns ? Operation::Return : Operation::Trap));
    }
    return result;
}

void encodeModuleTables(TableEncoder& encoder)
{
    const Typed::Program& program = encoder.program;
    const ParallelLoops& loops = encoder.build.parallelLoops();
    TablePart& part = encoder.part;
    StepTimes times(encoder.build, "image: module tables"_s);
    auto numberOfString = [&](StringID id) {
        return encoder.stringNumber(id);
    };

    std::optional<AnswersAboutModules> answersOfNamer = std::exchange(encoder.inputs.answersAboutModules, std::nullopt);
    AnswersAboutModules answers = answersOfNamer ? WTF::move(*answersOfNamer) : askAboutModules(encoder.inputs, loops);
    ModuleLists lists = collectModuleLists(encoder, answers);
    times.didFinish("records and lists of the modules"_s);

    const Vector<PlacedVariable>& placedVariables = encoder.numbers.statics().variables;
    ChunksOfIndices chunksOfModules = divideIntoChunks(static_cast<unsigned>(program.modules.size()));
    Vector<ModulesListedInChunk> modulesListed(chunksOfModules.size());
    Vector<Vector<StateOfLibraryModuleData>> stateInVariables(numberOfRangesOfIndices(placedVariables.size()));
    Vector<FunctionsListedInRange> functionsListed(numberOfRangesOfIndices(program.functions.size()));
    Vector<Vector<StateOfLibraryModuleData>> stateInClasses(numberOfRangesOfIndices(program.layouts.size()));
    StartsOfModules startsOfModules;
    BitVector esModulesOfProgram = findESModulesOfProgram(lists.modules.span());
    Vector<uint16_t> groupsLoadedWithGroups;
    Typed::StronglyConnectedComponents componentsOfRequests;
    forEachIndexOfLoopsInParallel(loops, {
        { 1, [&](unsigned) { componentsOfRequests = computeModuleFlags(lists.modules.mutableSpan(), lists.requests.span()); } },
        { 1, [&](unsigned) { groupsLoadedWithGroups = encodeNumbersForModuleTables(program, groupModulesLoadedTogether(lists.modules.mutableSpan(), lists.requests.span(), esModulesOfProgram)); } },
        { 1, [&](unsigned) { startsOfModules = listStartsOfModules(encoder); } },
        { chunksOfModules.size(), [&](unsigned chunk) {
            listModulesOfChunkForModuleTables(encoder.inputs, answers, chunksOfModules.firstIndexOf[chunk], chunksOfModules.firstIndexOf[chunk + 1], modulesListed[chunk]);
        } },
        { static_cast<unsigned>(stateInVariables.size()), [&](unsigned range) {
            auto [first, end] = boundsOfRangeOfIndices(range, placedVariables.size());
            listStateOfLibraryModulesInVariables(encoder, first, end, stateInVariables[range]);
        } },
        { static_cast<unsigned>(functionsListed.size()), [&](unsigned range) {
            auto [first, end] = boundsOfRangeOfIndices(range, program.functions.size());
            listFunctionsForModuleTables(encoder, first, end, functionsListed[range]);
        } },
        { static_cast<unsigned>(stateInClasses.size()), [&](unsigned range) {
            auto [first, end] = boundsOfRangeOfIndices(range, program.layouts.size());
            listStateOfLibraryModulesInClasses(encoder, first, end, stateInClasses[range]);
        } },
    });
    times.didFinish("flags of the modules, beside what the tables list of modules, variables, functions and classes"_s);

    RequestsThatWalksFollow followed = listRequestsThatWalksFollow(loops, lists, componentsOfRequests);
    Vector<LinkPlanStepData> entryPointLinkPlan;
    if (!program.entries.isEmpty())
        entryPointLinkPlan = planLinkOfEntryPoint(lists.modules.mutableSpan(), lists.requests.span(), program.entries.first().module);
    times.didFinish("requests that walks follow, link plan"_s);

    TablesOfModulesByPath byPath = makeTablesOfModulesByPath(program, loops, modulesListed);
    times.didFinish("modules by path"_s);

    Vector<uint16_t> linkRequests = encodeNumbersForModuleTables(program, followed.linkRequests);
    Vector<uint16_t> evaluationRequests = encodeNumbersForModuleTables(program, followed.evaluationRequests);
    Vector<ModuleFailureData> moduleFailures = listFailuresForModuleTables(program, numberOfString);
    Vector<NameOfStatementData> namesOfStatements = listNamesOfStatements(program, numberOfString);
    Vector<StarAsWrittenData> starsAsWritten = listStarsAsWritten(program, answers);
    Vector<WriterOfExportData> writersOfExports = listWritersOfExports(encoder.inputs);
    Vector<UncompiledModuleData> modulesOfUncompiledCode = listModulesOfUncompiledCode(program, answers, numberOfString);
    Vector<uint32_t> modulesRejectingUncompiledCode = joinListsForModuleTables(modulesListed, &ModulesListedInChunk::modulesRejectingUncompiledCode);
    Vector<uint32_t> modulesWithReadOnlyBuiltins = joinListsForModuleTables(modulesListed, &ModulesListedInChunk::modulesWithReadOnlyBuiltins);
    Vector<uint32_t> importedModuleAliases = joinListsForModuleTables(modulesListed, &ModulesListedInChunk::importedModuleAliases);
    Vector<uint32_t> modulesThatDependOnMoreThanSource = joinListsForModuleTables(modulesListed, &ModulesListedInChunk::modulesThatDependOnMoreThanSource);
    Vector<uint32_t> asyncFunctionsWithoutAwait = joinListsForModuleTables(functionsListed, &FunctionsListedInRange::asyncFunctionsWithoutAwait);
    Vector<uint32_t> functionsImportingOwnOriginal = joinListsForModuleTables(functionsListed, &FunctionsListedInRange::functionsImportingOwnOriginal);
    Vector<RequestedOriginalData> requestedOriginals = joinListsForModuleTables(functionsListed, &FunctionsListedInRange::requestedOriginals);
    Vector<CompiledRuntimeModuleData> compiledRuntimeModules = joinListsForModuleTables(modulesListed, &ModulesListedInChunk::runtimeModules);
    AOT::sort(compiledRuntimeModules, { }, &CompiledRuntimeModuleData::registryID);
    Vector<CompiledRuntimeFunctionData> compiledRuntimeFunctions = listCompiledRuntimeFunctions(encoder, answers);
    Vector<StateOfLibraryModuleData> stateFound;
    for (const Vector<StateOfLibraryModuleData>& found : stateInVariables)
        stateFound.appendVector(found);
    stateFound.appendVector(joinListsForModuleTables(functionsListed, &FunctionsListedInRange::stateOfLibraryModules));
    for (const Vector<StateOfLibraryModuleData>& found : stateInClasses)
        stateFound.appendVector(found);
    Vector<StateOfLibraryModuleData> stateOfLibraryModules = uniteStateOfLibraryModules(WTF::move(stateFound));

    auto roomFor = [](const auto&... tables) {
        return (... + (tables.sizeInBytes() + sizeof(uint64_t)));
    };
    part.reserveRoomFor(roomFor(lists.modules, lists.directories, lists.displayPaths, lists.requests, linkRequests, evaluationRequests, groupsLoadedWithGroups, entryPointLinkPlan,
        lists.exports, lists.indirectExports, lists.syntheticExports, lists.requiredNames, lists.exportStatements, lists.ambiguousExports, lists.sourceStamps,
        moduleFailures, modulesOfUncompiledCode, modulesRejectingUncompiledCode, modulesWithReadOnlyBuiltins, importedModuleAliases, modulesThatDependOnMoreThanSource, followed.importsOfHeldFiles, asyncFunctionsWithoutAwait, functionsImportingOwnOriginal, requestedOriginals,
        byPath.modulesByPath, byPath.modulesByHashOfPath, byPath.firstModulesByHashOfPath, byPath.directoriesOfModulesByPath, compiledRuntimeModules, compiledRuntimeFunctions, stateOfLibraryModules,
        startsOfModules.firstSteps, startsOfModules.steps, namesOfStatements, starsAsWritten, writersOfExports));
    part.setTable(ProgramTableIndex::modules, lists.modules);
    part.setTable(ProgramTableIndex::moduleDirectories, lists.directories);
    part.setTable(ProgramTableIndex::moduleDisplayPaths, lists.displayPaths);
    part.setTable(ProgramTableIndex::moduleRequests, lists.requests);
    part.setTable(ProgramTableIndex::linkRequests, linkRequests);
    part.setTable(ProgramTableIndex::evaluationRequests, evaluationRequests);
    part.setTable(ProgramTableIndex::groupsLoadedWithGroups, groupsLoadedWithGroups);
    part.setTable(ProgramTableIndex::entryPointLinkPlan, entryPointLinkPlan);
    part.setTable(ProgramTableIndex::moduleExports, lists.exports);
    part.setTable(ProgramTableIndex::indirectExports, lists.indirectExports);
    part.setTable(ProgramTableIndex::syntheticExports, lists.syntheticExports);
    part.setTable(ProgramTableIndex::requiredNames, lists.requiredNames);
    part.setTable(ProgramTableIndex::exportStatements, lists.exportStatements);
    part.setTable(ProgramTableIndex::ambiguousExports, lists.ambiguousExports);
    part.setTable(ProgramTableIndex::sourceStamps, lists.sourceStamps);
    part.setTable(ProgramTableIndex::moduleFailures, moduleFailures);
    part.setTable(ProgramTableIndex::namesOfStatements, namesOfStatements);
    part.setTable(ProgramTableIndex::starsAsWritten, starsAsWritten);
    part.setTable(ProgramTableIndex::writersOfExports, writersOfExports);
    part.setTable(ProgramTableIndex::modulesOfUncompiledCode, modulesOfUncompiledCode);
    part.setTable(ProgramTableIndex::modulesRejectingUncompiledCode, modulesRejectingUncompiledCode);
    part.setTable(ProgramTableIndex::modulesWithReadOnlyBuiltins, modulesWithReadOnlyBuiltins);
    part.setTable(ProgramTableIndex::importedModuleAliases, importedModuleAliases);
    part.setTable(ProgramTableIndex::modulesThatDependOnMoreThanSource, modulesThatDependOnMoreThanSource);
    part.setTable(ProgramTableIndex::importsOfHeldFiles, followed.importsOfHeldFiles);
    part.setTable(ProgramTableIndex::asyncFunctionsWithoutAwait, asyncFunctionsWithoutAwait);
    part.setTable(ProgramTableIndex::functionsImportingOwnOriginal, functionsImportingOwnOriginal);
    part.setTable(ProgramTableIndex::requestedOriginals, requestedOriginals);
    part.setTable(ProgramTableIndex::modulesByPath, byPath.modulesByPath);
    part.setTable(ProgramTableIndex::modulesByHashOfPath, byPath.modulesByHashOfPath);
    part.setTable(ProgramTableIndex::firstModulesByHashOfPath, byPath.firstModulesByHashOfPath);
    part.setTable(ProgramTableIndex::directoriesOfModulesByPath, byPath.directoriesOfModulesByPath);
    part.setTable(ProgramTableIndex::compiledRuntimeModules, compiledRuntimeModules);
    part.setTable(ProgramTableIndex::compiledRuntimeFunctions, compiledRuntimeFunctions);
    part.setTable(ProgramTableIndex::stateOfLibraryModules, stateOfLibraryModules);
    part.setTable(ProgramTableIndex::moduleStarts, startsOfModules.firstSteps);
    part.setTable(ProgramTableIndex::moduleStartSteps, startsOfModules.steps);
    times.didFinish("tables"_s);

    destroyInBackground(encoder.build.backgroundJobs(), std::pair(WTF::move(lists), WTF::move(answers)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
