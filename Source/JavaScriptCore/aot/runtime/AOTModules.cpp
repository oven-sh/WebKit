/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTBuiltinCells.h"
#include "AOTBuiltinTable.h"
#include "AOTErrorMessages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleLinkWalk.h"
#include "AOTModuleMicrotasks.h"
#include "AOTModuleNamespace.h"
#include "AOTOperationArguments.h"
#include "AOTOperationScope.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStatics.h"
#include "AOTProgramStrings.h"
#include "AOTRealmStartup.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "DeferGCInlines.h"
#include "DeferTermination.h"
#include "DeferredWorkTimer.h"
#include "DisallowVMEntry.h"
#include "ErrorInstance.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleRegistryEntry.h"
#include "ObjectConstructor.h"
#include "ScriptFetchParameters.h"
#include "SlotVisitorInlines.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "TypeError.h"
#include "VMInlines.h"
#include <wtf/ASCIICType.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StackPointer.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Modules);

static bool isAbsoluteFileSystemPath(StringView path)
{
#if OS(WINDOWS)
    auto isSeparator = [&](unsigned index) {
        return path[index] == '/' || path[index] == '\\';
    };
    return !path.isEmpty() && (isSeparator(0) || (path.length() >= 3 && isASCIIAlpha(path[0]) && path[1] == ':' && isSeparator(2)));
#else
    return path.startsWith('/');
#endif
}

static AbstractModuleRecord::ModulePhase targetPhaseOf(const ModuleExportData& moduleExport)
{
    return moduleExport.isDeferred ? AbstractModuleRecord::ModulePhase::Defer : AbstractModuleRecord::ModulePhase::Evaluation;
}

static CyclicModuleRecord::Status recordStatusFor(ModuleStatus status)
{
    RELEASE_ASSERT(status != ModuleStatus::Failed);
    return static_cast<CyclicModuleRecord::Status>(status);
}

static ModuleStatus statusOf(const ModuleRecord& record)
{
    if (record.status() == CyclicModuleRecord::Status::New)
        return ModuleStatus::Unlinked;
    if (record.status() == CyclicModuleRecord::Status::Evaluated && record.evaluationError())
        return ModuleStatus::Failed;
    return static_cast<ModuleStatus>(record.status());
}

template<typename Visitor>
void Modules::visitRoots(Visitor& visitor)
{
    m_storedReferences.forEachRegionToVisit(visitor, [&](size_t, size_t) {
        for (ModuleRecord* record : m_records.values())
            visitor.appendUnbarriered(record);
        for (auto& loaded : m_loaded.values()) {
#define AOT_VISIT_REFERENCE(type, name) visitor.appendUnbarriered(loaded->name.get());
            FOR_EACH_REFERENCE_OF_LOADED_MODULE(AOT_VISIT_REFERENCE)
#undef AOT_VISIT_REFERENCE
        }
        visitor.appendUnbarriered(m_namespaceStructure);
        visitor.appendUnbarriered(m_extensibleNamespaceStructure);
    });
}

template void Modules::visitRoots(SlotVisitor&);
template void Modules::visitRoots(AbstractSlotVisitor&);

Modules::Modules(State& state)
    : m_state(state)
    , m_instance(state.instance)
    , m_table(state.tables)
    , m_admitsCodeOutsideImage(hasFlag(state.instance.options().admitted(), AdmittedCode::Inspector) || state.instance.vm->aotState().image->loadsCodeOutsideImage())
    , m_admitsInspector(hasFlag(state.instance.options().admitted(), AdmittedCode::Inspector))
    , m_showsWaysBetweenModuleObjects(state.instance.vm->aotState().image->showsWaysBetweenModuleObjects())
    , m_storesPathOfMainModule(state.instance.vm->aotState().image->storesPathOfMainModule())
    , m_loadsModulesByName(state.instance.vm->aotState().image->loadsModulesByName())
    , m_refusesOriginalsBesideMocks(state.instance.vm->aotState().image->refusesOriginalsBesideMocks())
    , m_showsEntriesOfRequireCache(state.instance.vm->aotState().image->showsEntriesOfRequireCache())
    , m_mockOperations(host().mockOperations ? host().mockOperations() : nullptr)
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.evaluatingMark = static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    m_instance.modules.runsImportedFilesAlone = 0;
    m_modulesSettledForLinks.fill(0, m_table.size() / 64 + 1);
    m_loadedGroups.fill(0, m_table.size() / 64 + 1);
    m_goesModuleByModule = isTestRun();
    if (m_goesModuleByModule) {
        m_momentsOfLoads.fill(0, m_table.size());
        m_loadedInPreload.ensureSize(m_table.size());
    }
    state.caches->add(CacheKind::StrongRoots, "modules"_s, *this, RootVolatility::GreyedByExecution);
}

Modules::~Modules()
{
    forgetUnfinishedLoads();
}

void Modules::forgetUnfinishedLoads()
{
    m_synchronousLoadsHandedToLoader.clear();
    m_evaluationOfImportOfMainModule.clear();
    m_capabilityOfMainModule.clear();
    m_loadsInFlight.clear();
    m_loadsThatHoldModules.clear();
    m_levelsOfLoads.clear();
    m_deferredRequests.clear();
    m_deferredImports.clear();
    m_graphsHeldBehindEarlierCalls.clear();
    m_textsOnTheirWay.clear();
    forgetTextsThatWait();
}

void forgetUnfinishedLoadsOfRetiredRealms(VM& vm)
{
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        if (!instance->globalObject || instance->globalObject->microtaskRunnability() != QueuedTaskResult::Discard)
            continue;
        if (instance->state && instance->state->modules) {
            instance->state->modules->noteTextsThatHostKeeps();
            instance->state->modules->forgetUnfinishedLoads();
        }
    }
}

Modules& modulesOf(Instance& instance)
{
    return *instance.state->modules;
}

Modules* modulesOf(JSGlobalObject* realm)
{
    Instance* instance = Instance::of(realm);
    return instance ? instance->state->modules.get() : nullptr;
}

void Modules::noteFirstRequestFromHost(ModuleID module)
{
    m_firstModuleRequestedByHost = module;
}

const ModuleHost& Modules::host() const
{
    return moduleHostOf(m_instance.globalObject);
}

Identifier Modules::identifier(uint32_t number) const
{
    return identifiersOf(m_instance).identifier(number);
}

JSModuleLoader* Modules::loader() const
{
    return m_instance.globalObject->moduleLoader();
}

ModuleStatus Modules::statusInRecord(ModuleID module) const
{
    ModuleRecord* record = m_records.get(module);
    if (!record && m_filesToLoadAgain.get(module.index()))
        return ModuleStatus::Linked;
    RELEASE_ASSERT(record);
    ModuleStatus status = statusOf(*record);
    return status == ModuleStatus::Unlinked && !data(module).needsLink() ? ModuleStatus::Linked : status;
}

bool Modules::wouldBeEvicted(ModuleID module)
{
    ModuleID file = fileOf(module);
    return (status(file) == ModuleStatus::Evaluating && data(file).kind == ModuleKind::CommonJs) || hasRun(file);
}

void Modules::keepObjectMadeByRequire(ModuleID module, JSObject* moduleObject)
{
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.objectMadeByRequire || loaded.isBeingRequired)
        return;
    loaded.objectMadeByRequire.set(*this, moduleObject);
    JSValue exports = moduleObject->getDirect(*m_instance.vm, m_instance.vm->propertyNames->exports);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, exports);
    loaded.exportsWhileRequired.set(*this, exports && exports.isObject() ? asObject(exports) : nullptr);
}

void Modules::forgetObjectMadeByRequire(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->isKeptByRequire)
        return;
    loaded->isKeptByRequire = false;
    loaded->objectMadeByRequire.set(*this, nullptr);
}

bool Modules::wasLoadedByImport(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (data(file).kind == ModuleKind::SourceText)
        return true;
    ModuleID imported = moduleWithExportsOf(file);
    if (imported != file && status(imported) >= ModuleStatus::Evaluating)
        return true;
    BitVector reached(m_table.size());
    Vector<ModuleID, 64> toFollow;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        if (status(ModuleID(index)) >= ModuleStatus::Evaluating)
            toFollow.append(ModuleID(index));
    }
    while (!toFollow.isEmpty()) {
        ModuleID importer = toFollow.takeLast();
        if (standsForWholeModule(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (request.isDeferred || request.isRequire || request.isHoisted)
                continue;
            if (request.targetModule() == imported)
                return true;
            if (data(request.targetModule()).isUncompiledCode()) {
                LoadedModule* linked = loadedIfExists(request.targetModule());
                if (linked && linked->uncompiledRecord && linked->keyOfLatestLoad.string() == m_table.pathText(file))
                    return true;
                continue;
            }
            if (!reached.set(request.target()))
                toFollow.append(request.targetModule());
        }
    }
    return false;
}

void Modules::takeOutOfLoader(ModuleID module)
{
    ModuleID file = fileOf(module);
    for (ModuleID known : { file, moduleWithExportsOf(file) }) {
        if (ModuleRecord* record = m_records.get(known))
            loader()->removeEntryOfRecord(record);
    }
}

void Modules::evict(ModuleID module, bool isUndoneForKeptFiles)
{
    ModuleID file = fileOf(module);
    bool hasThrown = status(file) == ModuleStatus::Failed || status(moduleWithExportsOf(file)) == ModuleStatus::Failed;
    if (!wouldBeEvicted(file) && !hasThrown)
        return;
    if (isUndoneForKeptFiles && !isEvicted(file) && m_keptFiles.get(file.index()))
        m_filesToBringBack.set(file.index());
    leaveToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    if (status(file) == ModuleStatus::Evaluated && !m_filesToBringBack.get(file.index()))
        layValueInPlaceOfExportsVariable(file);
    m_filesToLoadAgain.set(file.index());
    setStoredStatus(file, StoredModuleStatus::InRecord);
}

bool Modules::holdsCodeThatMayBeEvicted()
{
    if (!m_holdsCodeThatMayBeEvicted) {
        m_holdsCodeThatMayBeEvicted = false;
        for (unsigned index = 0; index < m_table.size() && !*m_holdsCodeThatMayBeEvicted; ++index) {
            const ModuleData& file = data(ModuleID(index));
            m_holdsCodeThatMayBeEvicted = !file.isOfLibrary() && (file.kind == ModuleKind::SourceText || file.kind == ModuleKind::CommonJs) && file.mayBeEvicted();
        }
    }
    return *m_holdsCodeThatMayBeEvicted;
}

BitVector Modules::modulesThatHaveRun()
{
    followImportsOfModulesThatHaveRun();
    BitVector modules = m_modulesSeenToHaveRun;
    modules.merge(m_modulesThatRanWithImporters);
    return modules;
}

void Modules::willRunCodeThatKeepsFiles()
{
    if (holdsCodeThatMayBeEvicted())
        m_modulesThatRanBeforeCodeThatKeepsFiles = modulesThatHaveRun();
}

void Modules::keepFilesThatHaveRun()
{
    if (!holdsCodeThatMayBeEvicted())
        return;
    BitVector modules = modulesThatHaveRun();
    modules.exclude(std::exchange(m_modulesThatRanBeforeCodeThatKeepsFiles, { }));
    for (size_t index : modules) {
        ModuleID module(index);
        if (!data(module).isOfLibrary() && fileOf(module) == module && mayBeEvicted(module) && !isEvicted(module))
            m_keptFiles.set(index);
    }
}

void Modules::bringBackKeptFiles(const WTF::Function<void(const String&)>& functor)
{
    if (m_filesToBringBack.isEmpty())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ModuleID, 16> back;
    for (size_t index : std::exchange(m_filesToBringBack, { }))
        back.append(ModuleID(index));
    for (ModuleID file : back)
        m_evictedFiles.clear(file.index());
    BitVector evictedWithImports = std::exchange(m_filesEvictedWithImports, { });
    m_evictedFiles.exclude(evictedWithImports);
    m_filesAboveEvictedFile.clearAll();
    m_filesAboveNoEvictedFile.clearAll();
    ++m_linkEpoch;
    for (size_t index : evictedWithImports)
        isLeftToHostAsFarAsIsKnown(ModuleID(index));
    for (ModuleID file : back) {
        functor(m_table.pathText(file));
        if (data(file).kind != ModuleKind::CommonJs)
            continue;
        bool wasLoadedAgain = !m_filesToLoadAgain.get(file.index());
        m_filesToLoadAgain.clear(file.index());
        if (!recordIfExists(file))
            setStoredStatus(file, StoredModuleStatus::Evaluated);
        if (wasLoadedAgain) {
            captureExportsAgain(file);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

void Modules::forgetEvaluation(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    if (!m_instance.program->runsEntryPointsAgain() || !moduleData.isEntryPoint() || !moduleData.isESModuleOfProgram() || moduleData.isFaceOfFile())
        return;
    if (isEvicted(module) || isMocked(module))
        return;
    ModuleStatus current = status(module);
    if (current != ModuleStatus::Evaluated && current != ModuleStatus::Failed)
        return;
    RELEASE_ASSERT(!m_innermostEvaluation);
    if (ModuleRecord* record = m_records.get(module)) {
        loader()->removeEntryOfRecord(record);
        Locker locker { m_state.caches->lock() };
        m_records.remove(module);
    }
    bool importsByLoader = false;
    for (const ModuleRequestData& request : m_table.imports(module)) {
        if (!data(request.targetModule()).isUncompiledCode())
            continue;
        importsByLoader = true;
        if (LoadedModule* external = loadedIfExists(request.targetModule()))
            external->uncompiledRecord.set(*this, nullptr);
    }
    setStoredStatus(module, importsByLoader ? StoredModuleStatus::Unlinked : StoredModuleStatus::Linked);
    if (LoadedModule* loaded = loadedIfExists(module))
        loaded->importMeta.set(*this, nullptr);
    if (m_evictedFiles.isEmpty())
        return;
    WalkForEvictedFiles walk;
    walk.visitOrder.add(module.index(), 0);
    uint32_t earliestVisitReached = 0;
    for (const ModuleRequestData& request : m_table.imports(module)) {
        if (importsEvictedFile(request.targetModule(), walk, earliestVisitReached))
            return leaveToHost(module);
    }
}

void Modules::leaveToHostUnlessItHasRun(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (status(file) >= ModuleStatus::Evaluating || isEvicted(file) || hasRun(file))
        return;
    leaveToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    m_filesToLoadAgain.set(file.index());
    setStoredStatus(file, StoredModuleStatus::InRecord);
}

void Modules::leaveToHost(ModuleID file)
{
    m_evictedFiles.set(file.index());
    m_filesAskedAbout.clear(file.index());
    m_filesWhoseGraphHasBegun.clear(file.index());
    m_filesAboveNoEvictedFile.clearAll();
    m_runsCopyFromDisk = true;
}

bool Modules::isChangedOnDisk(ModuleID file)
{
    std::optional<uint64_t> stampOfBuild = m_table.sourceStamp(file);
    if (!stampOfBuild || !host().stampOfFile)
        return false;
    countEvent(*m_instance.vm, RuntimeCounterID::FilesComparedWithDisk);
    ModuleTable::PathInUTF8 path = m_table.pathInUTF8(file);
    path.append('\0');
    return host().stampOfFile(path.span().first(path.size() - 1)) != *stampOfBuild;
}

bool Modules::writesNameThatHostMayAnswerFor(ModuleID file, WalkForEvictedFiles& walk)
{
    if (m_table.namesOfStatements(file).empty() || !host().hasPluginsThatAnswerForNames || !mayBeEvicted(file))
        return false;
    if (!walk.pluginsAnswerForNames)
        walk.pluginsAnswerForNames = host().hasPluginsThatAnswerForNames(m_instance.globalObject);
    return *walk.pluginsAnswerForNames;
}

Modules::AnswersForNamesOfFile Modules::askHostAboutNamesWrittenBy(ModuleID file)
{
    AnswersForNamesOfFile answers;
    String importer = m_table.pathText(file);
    std::span<const NameOfStatementData> names = m_table.namesOfStatements(file);
    auto textOf = [&](const NameOfStatementData& written) {
        return ProgramStrings::text(m_table.programTables(), written.nameString());
    };
    auto fileAt = [&](const String& path) -> std::optional<uint32_t> {
        std::optional<ModuleID> linked = path.isNull() ? std::nullopt : m_table.findByPath(path);
        return linked ? std::optional(fileOf(*linked).index()) : std::nullopt;
    };
    Vector<uint32_t, 16> namesDeclined;
    for (uint32_t index = 0; index < names.size(); ++index) {
        String foundWithoutPlugins;
        AnswerForName answer = host().answersForName(m_instance.globalObject, textOf(names[index]), importer, foundWithoutPlugins);
        if (answer == AnswerForName::Answered) {
            answers.answer = answer;
            if (std::optional<uint32_t> linked = fileAt(foundWithoutPlugins))
                answers.filesThatAnswersLeadAwayFrom.append(*linked);
            continue;
        }
        if (answer == AnswerForName::Declined) {
            namesDeclined.append(index);
            continue;
        }
        answers.answer = answer;
        for (uint32_t inFront : namesDeclined) {
            if (std::optional<uint32_t> linked = fileAt(host().findsWithoutPlugins(m_instance.globalObject, textOf(names[inFront]), importer)))
                answers.filesInFrontOfThrow.append(*linked);
        }
        break;
    }
    return answers;
}

bool Modules::isChangedOutOfSightOfBuild(StringView path) const
{
    if (m_filesChangedOutOfSightOfBuild.isEmpty())
        return false;
    std::optional<ModuleID> found = m_table.findByPath(path);
    return found && m_filesChangedOutOfSightOfBuild.get(fileOf(*found).index());
}

bool Modules::walkForEvictedFiles(ModuleID module, WalkForEvictedFiles& walk)
{
    uint32_t earliestVisitReached = 0;
    bool isAboveEvictedFile = importsEvictedFile(module, walk, earliestVisitReached);
    if (isAboveEvictedFile) {
        for (uint32_t file : walk.unsettledFiles)
            m_filesAboveEvictedFile.set(file);
    }
    return isAboveEvictedFile;
}

bool Modules::askHostLevelByLevel(ModuleID root, WalkForEvictedFiles& walk, BitVector& filesAsked)
{
    if (!m_table.listsNamesOfStatements() || !host().hasPluginsThatAnswerForNames)
        return false;
    if (!walk.pluginsAnswerForNames)
        walk.pluginsAnswerForNames = host().hasPluginsThatAnswerForNames(m_instance.globalObject);
    if (!*walk.pluginsAnswerForNames)
        return false;
    bool didAsk = false;
    BitVector isReached;
    Deque<ModuleID, 32> toFollow;
    isReached.set(fileOf(root).index());
    toFollow.append(fileOf(root));
    while (!toFollow.isEmpty()) {
        ModuleID file = toFollow.takeFirst();
        if (m_filesAskedAbout.get(file.index()))
            continue;
        if (isEvicted(file) ? loader()->registryEntry(moduleKey(file)) || isChangedOnDisk(file) : m_filesWhoseGraphHasBegun.get(file.index()) || status(file) >= ModuleStatus::Evaluating || hasRun(file, walk.importsAreFollowed))
            continue;
        if (isMocked(file) || mockInPlace(file) || m_modulesWithMockToRun.get(file.index()))
            continue;
        AnswersForNamesOfFile answers;
        if (!filesAsked.get(file.index()) && mayBeAskedAbout(file) && writesNameThatHostMayAnswerFor(file, walk)) {
            if (filesAsked.isEmpty())
                host().askingAboutNames(m_instance.globalObject, nullptr);
            filesAsked.set(file.index());
            didAsk = true;
            uint64_t numberOfGraphsBegun = m_numberOfGraphsBegun;
            answers = askHostAboutNamesWrittenBy(file);
            if (m_numberOfGraphsBegun != numberOfGraphsBegun)
                walk.importsAreFollowed = false;
            if (answers.answer != AnswerForName::Declined)
                leaveToHostUnlessItHasRun(file);
            m_filesAskedAbout.set(file.index());
        }
        auto takeUp = [&](ModuleID target) {
            if (!m_filesAboveNoEvictedFile.get(target.index()) && !answers.filesThatAnswersLeadAwayFrom.contains(target.index()) && !isReached.set(target.index()))
                toFollow.append(target);
        };
        if (!isLoadedWithRequests(file))
            continue;
        for (const ModuleRequestData& request : m_table.imports(file)) {
            ModuleID target = fileOf(request.targetModule());
            if (answers.answer != AnswerForName::Thrown || answers.filesInFrontOfThrow.contains(target.index()))
                takeUp(target);
        }
    }
    return didAsk;
}

bool Modules::importsEvictedFile(ModuleID module)
{
    BitVector filesAsked;
    bool isAboveEvictedFile = false;
    auto tellHostWhoLeaves = makeScopeExit([&] {
        if (filesAsked.isEmpty())
            return;
        auto isLeaving = [&](const String& importer) {
            std::optional<ModuleID> found = isAboveEvictedFile ? m_table.findByPath(importer) : std::nullopt;
            if (!found)
                return false;
            WalkForEvictedFiles walk;
            return walkForEvictedFiles(*found, walk);
        };
        ScopedLambda<bool(const String&)> leaves = isLeaving;
        host().askingAboutNames(m_instance.globalObject, &leaves);
    });
    while (true) {
        WalkForEvictedFiles walk;
        isAboveEvictedFile = walkForEvictedFiles(module, walk);
        if (m_listsWithoutAsking || !askHostLevelByLevel(module, walk, filesAsked))
            return isAboveEvictedFile;
    }
}

bool Modules::importsEvictedFile(ModuleID module, WalkForEvictedFiles& walk, uint32_t& earliestVisitReached)
{
    ModuleID file = fileOf(module);
    if (m_evictedFiles.get(file.index()))
        return true;
    if (m_filesAboveNoEvictedFile.get(file.index()))
        return false;
    uint32_t visit = walk.visitOrder.size();
    if (auto added = walk.visitOrder.add(file.index(), visit); !added.isNewEntry) {
        earliestVisitReached = std::min(earliestVisitReached, added.iterator->value);
        return false;
    }
    if (status(file) >= ModuleStatus::Evaluating || hasRun(file, walk.importsAreFollowed)) {
        m_filesAboveNoEvictedFile.set(file.index());
        return false;
    }
    if (isMocked(file) || mockInPlace(file) || m_modulesWithMockToRun.get(file.index()))
        return false;
    if (m_filesAboveEvictedFile.get(file.index()))
        return true;
    if (isChangedOnDisk(file)) [[unlikely]] {
        if (!mayBeEvicted(file))
            m_filesChangedOutOfSightOfBuild.set(file.index());
        leaveToHostUnlessItHasRun(file);
        return true;
    }
    uint32_t numberOfFilesToAskHostAbout = walk.numberOfFilesToAskHostAbout;
    if (mayBeAskedAbout(file) && writesNameThatHostMayAnswerFor(file, walk)) [[unlikely]]
        ++walk.numberOfFilesToAskHostAbout;
    size_t numberOfUnsettledFiles = walk.unsettledFiles.size();
    uint32_t earliestVisitReachedFromHere = visit;
    if (isLoadedWithRequests(file)) {
        for (const ModuleRequestData& request : m_table.imports(file)) {
            if (importsEvictedFile(request.targetModule(), walk, earliestVisitReachedFromHere)) {
                m_filesAboveEvictedFile.set(file.index());
                return true;
            }
        }
    }
    if (earliestVisitReachedFromHere < visit) {
        walk.unsettledFiles.append(file.index());
        earliestVisitReached = std::min(earliestVisitReached, earliestVisitReachedFromHere);
        return false;
    }
    if (walk.numberOfFilesToAskHostAbout == numberOfFilesToAskHostAbout) {
        m_filesAboveNoEvictedFile.set(file.index());
        for (uint32_t settled : walk.unsettledFiles.span().subspan(numberOfUnsettledFiles))
            m_filesAboveNoEvictedFile.set(settled);
    }
    walk.unsettledFiles.shrink(numberOfUnsettledFiles);
    return false;
}

bool Modules::isLeftToHostAsFarAsIsKnown(ModuleID module)
{
    SetForScope listsWithoutAsking(m_listsWithoutAsking, true);
    return isLeftToHost(module);
}

bool Modules::isLeftToHost(ModuleID module)
{
    if (m_evictedFiles.isEmpty() && !m_table.hasSourceStamps())
        return false;
    if (data(module).isUncompiledCode())
        return false;
    if (!importsEvictedFile(module))
        return false;
    if (!m_evictedFiles.set(fileOf(module).index()))
        m_filesEvictedWithImports.set(fileOf(module).index());
    m_runsCopyFromDisk = true;
    return true;
}

void Modules::setStatus(ModuleID module, ModuleStatus status)
{
    if (ModuleRecord* record = recordIfExists(module)) {
        record->setStatus(recordStatusFor(status));
        didChangeStatusInRecord(module);
        if (status == ModuleStatus::Evaluated && !record->cycleRoot())
            record->setCycleRoot(*m_instance.vm, record);
        return;
    }
    switch (status) {
    case ModuleStatus::Unlinked:
        return setStoredStatus(module, StoredModuleStatus::Unlinked);
    case ModuleStatus::Linked:
        return setStoredStatus(module, StoredModuleStatus::Linked);
    case ModuleStatus::Evaluating:
        return setStoredStatus(module, StoredModuleStatus::Evaluating);
    case ModuleStatus::Evaluated:
        return setStoredStatus(module, StoredModuleStatus::Evaluated);
    case ModuleStatus::Linking:
    case ModuleStatus::EvaluatingAsync:
    case ModuleStatus::Failed:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ModuleRecord* Modules::ensureRecordWithoutStatus(ModuleID module)
{
    if (ModuleRecord* record = m_records.get(module))
        return record;

    ModuleRecord* record = ModuleRecord::create(m_instance, m_instance.vm->aotState().moduleRecordStructure.get(), loader(), moduleKey(module), module);
    record->setHasTLA(data(module).hasTopLevelAwait());
    if (JSObject* mock = mockInPlace(module)) [[unlikely]]
        letRecordAnswerByMock(record, mock);
    Locker locker { m_state.caches->lock() };
    m_records.add(module, record);
    m_storedReferences.recordAll();
    didChangeStatusInRecord(module);
    return record;
}

ModuleRecord* Modules::ensureRecord(ModuleID module)
{
    ModuleRecord* record = ensureRecordWithoutStatus(module);
    if (record->holdsStatus())
        return record;

    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Evaluating && (data(module).evaluatesItself() || !data(module).bodyFunction())) {
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        return record;
    }
    record->setHoldsStatus();
    if (data(module).onlyPassesLinkOn())
        didGetStateOfItsOwn(module);
    record->setStatus((moduleStatus == ModuleStatus::Unlinked && data(module).reachesUncompiledCode()) || isNotLoaded(module) ? CyclicModuleRecord::Status::New : recordStatusFor(moduleStatus));
    if (moduleStatus == ModuleStatus::Evaluated)
        record->setCycleRoot(*m_instance.vm, record);
    setStoredStatus(module, StoredModuleStatus::InRecord);
    return record;
}

void Modules::didLinkEntryPoint()
{
    m_entryPointIsLinked = true;
    ++m_linkEpoch;
    for (ModuleRecord* record : m_records.values()) {
        if (data(record->module()).isLinkedWithEntryPoint() && statusOf(*record) == ModuleStatus::Unlinked)
            record->setStatus(CyclicModuleRecord::Status::Linked);
    }
}

void Modules::retireRecord(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(record->status() == CyclicModuleRecord::Status::Evaluated);
    ASSERT(!record->evaluationError());
    ASSERT(!record->asyncEvaluationOrder().hasOrder());
    setStoredStatus(record->module(), StoredModuleStatus::Evaluated);
    loader()->removeEntryOfRecord(record);
    Locker locker { m_state.caches->lock() };
    m_records.remove(record->module());
}

void Modules::retireRecordOfModuleThatHasNotBegun(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(status(record->module()) == ModuleStatus::Linked);
    ASSERT(!record->evaluationError());
    setStoredStatus(record->module(), StoredModuleStatus::Linked);
    loader()->removeEntryOfRecord(record);
    Locker locker { m_state.caches->lock() };
    m_records.remove(record->module());
}

bool Modules::hasRunToItsEndWithItsCycle(const ModuleRecord& record) const
{
    auto hasRunToItsEnd = [](const CyclicModuleRecord& module) {
        return module.status() == CyclicModuleRecord::Status::Evaluated && !module.evaluationError() && !module.asyncEvaluationOrder().hasOrder();
    };
    const CyclicModuleRecord* root = record.cycleRoot();
    return hasRunToItsEnd(record) && (!root || hasRunToItsEnd(*root));
}

bool Modules::isEvaluatingAsynchronouslyWithItsCycle(const ModuleRecord& record)
{
    const CyclicModuleRecord* root = record.cycleRoot();
    return record.status() == CyclicModuleRecord::Status::EvaluatingAsync || (root && root->status() == CyclicModuleRecord::Status::EvaluatingAsync);
}

void Modules::retireRecordsOfSettledLoads()
{
    m_levelsOfLoads.removeIf([&](auto& entry) {
        return hasGraphRun(*entry.value);
    });
    if (m_instance.vm->synchronousModuleQueue() || m_innermostEvaluation || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty() || !m_textsOnTheirWay.isEmpty())
        return;
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return;
    if (m_loadsInFlight.hasPendingPromise())
        return;
    m_loadsInFlight.clear();
    m_loadsThatHoldModules.clear();
    for (ModuleID module : std::exchange(m_modulesWhoseRecordsLoaderHolds, { })) {
        ModuleRecord* record = m_records.get(module);
        if (!record)
            continue;
        record->setIsHeldByLoader(false);
        if (record->isRetained() || !isLoadedWithRequests(module))
            continue;
        record->takeStatus();
        if (!record->holdsStatus())
            continue;
        if (record->status() == CyclicModuleRecord::Status::New)
            continue;
        if (isLeftToStartsOfImporters(module))
            retireRecordOfModuleThatHasNotBegun(record);
        else if (hasRunToItsEndWithItsCycle(*record))
            retireRecord(record);
        else if (isEvaluatingAsynchronouslyWithItsCycle(*record))
            m_modulesToRetireAfterAsyncEvaluation.append(module);
    }
    m_modulesToRetireAfterAsyncEvaluation.removeAllMatching([&](ModuleID module) {
        ModuleRecord* record = recordIfExists(module);
        if (!record || record->isRetained())
            return true;
        if (isEvaluatingAsynchronouslyWithItsCycle(*record))
            return false;
        if (hasRunToItsEndWithItsCycle(*record))
            retireRecord(record);
        return true;
    });
}

void didSettleLoad(JSPromise* settled)
{
    JSGlobalObject* realm = settled->realm();
    if (Modules* modules = modulesOf(realm)) {
        modules->didSettlePromiseOfImport(settled);
        modules->letGoGraphsHeldBehindEarlierCalls();
        modules->retireRecordsOfSettledLoads();
    }
}

void Modules::setFailed(ModuleID module, JSValue error)
{
    SuspendExceptionScope errorStillThrown(*m_instance.vm);
    DeferTerminationForAWhile noTerminationArrives(*m_instance.vm);
    ModuleRecord* record = ensureRecord(module);
    setStoredStatus(module, StoredModuleStatus::InRecord);
    record->setHoldsStatus();
    record->retain();
    record->setEvaluationError(*m_instance.vm, error);
    record->setStatus(CyclicModuleRecord::Status::Evaluated);
}

void Modules::throwEvaluationError(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = recordIfExists(module);
    RELEASE_ASSERT(record && record->evaluationError());
    if (JSPromise* capability = record->topLevelCapability())
        capability->markAsHandled();
    throwException(m_instance.globalObject, scope, record->evaluationError());
}

JSValue Modules::value(ModuleID module) const
{
    return data(module).hasValue() ? ProgramStatics::readVariableWithoutAllocating(m_instance, data(module).value) : JSValue();
}

void Modules::setValue(ModuleID module, JSValue value)
{
    RELEASE_ASSERT(data(module).hasValue());
    ProgramStatics::writeVariable(m_instance, data(module).value, value);
}

auto Modules::ensureLoaded(ModuleID module) -> LoadedModule&
{
    if (LoadedModule* loaded = loadedIfExists(module))
        return *loaded;
    RELEASE_ASSERT(module.index() < m_table.size());
    Locker locker { m_state.caches->lock() };
    return *m_loaded.add(module, makeUnique<LoadedModule>()).iterator->value;
}

static bool isNamedByNumber(StringView path, ModuleID module)
{
    return path.startsWith('#') && parseInteger<uint32_t>(path.substring(1)) == module.index();
}

Identifier Modules::moduleKey(ModuleID module)
{
    if (ModuleRecord* record = m_records.get(module))
        return record->moduleKey();
    String path = m_table.pathText(module);
    return Identifier::fromString(*m_instance.vm, path);
}

std::optional<ModuleID> Modules::findByKey(const Identifier& key) const
{
    return m_table.findByPath(key.string());
}

Identifier Modules::keyOfRequest(ModuleID target)
{
    return ensureRecordWithoutStatus(target)->moduleKey();
}

JSString* Modules::pathString(ModuleID module)
{
    if (data(module).directory == Number24::max)
        return m_state.strings->string(*m_instance.vm, StringID(data(module).publicPathOrName));
    return jsString(*m_instance.vm, m_table.pathText(module));
}

String Modules::pathOfExporterAsLoaderNamesIt(const RequiredNameData& requiredName)
{
    ModuleID exporter = m_table.fileBehindFace(requiredName.exporterModule()).value_or(requiredName.exporterModule());
    return data(exporter).isUncompiledCode() ? resolvedKey(exporter).string() : m_table.pathText(exporter);
}

void Modules::throwExportNotFound(const RequiredNameData& requiredName)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Identifier name = identifier(requiredName.name);
    bool isDefault = name == m_instance.vm->propertyNames->defaultKeyword;
    if (const ExportStatementData* statement = m_table.findExportStatement(requiredName)) {
        if (isDefault)
            throwSyntaxError(realm, scope, "export default cannot be used with export *"_s);
        else
            throwException(realm, scope, createExportStatementFindsNothingError(realm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()), ProgramStrings::text(m_table.programTables(), statement->specifierString())));
        return;
    }
    String path = pathOfExporterAsLoaderNamesIt(requiredName);
    RETURN_IF_EXCEPTION(scope, void());
    throwException(realm, scope, isDefault ? createMissingDefaultExportError(realm, path) : createExportNotFoundError(realm, name.string(), path));
}

void Modules::throwExportAmbiguous(const RequiredNameData& requiredName)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (const ExportStatementData* statement = m_table.findExportStatement(requiredName)) {
        throwException(realm, scope, createExportStatementFindsSeveralError(realm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()), ProgramStrings::text(m_table.programTables(), statement->specifierString())));
        return;
    }
    String path = pathOfExporterAsLoaderNamesIt(requiredName);
    RETURN_IF_EXCEPTION(scope, void());
    throwException(realm, scope, createAmbiguousExportError(realm, identifier(requiredName.name).string(), path));
}

JSPromise* Modules::rejectedPromiseWithPendingException(JSGlobalObject* realm, ThrowScope& scope)
{
    if (JSPromise* promise = JSPromise::rejectedPromiseWithCaughtException(realm, scope))
        return promise;
    return JSPromise::create(realm->vm(), realm->promiseStructure());
}

JSValue Modules::readExportIfInitialized(ModuleID module, unsigned index) const
{
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    if (someMockIsSeen() && (mockInPlace(module) || mockThatAnswersFor(module, moduleExport))) [[unlikely]]
        return { };
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        return ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable);
    case ModuleExportKind::Namespace: {
        if (handedOnInPlaceOfNamespace(moduleExport)) [[unlikely]]
            return { };
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (isLeftAloneByMock(moduleExport.targetModule()) || indexOfEndedMockSeenBy(fileOf(moduleExport.targetModule()), module)) [[unlikely]]
            return { };
        JSObject* namespaceObject = namespaceIfCreated(moduleExport.targetModule(), targetPhaseOf(moduleExport));
        return namespaceObject ? JSValue(namespaceObject) : JSValue();
    }
    case ModuleExportKind::Indirect:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Modules::readExport(ModuleID module, unsigned index, MockOfModule mockOfModule)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    if (JSObject* mock = mockOfModule == MockOfModule::IsReadPast ? nullptr : mockInPlace(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(realm, identifier(moduleExport.name)));
    if (status(module) == ModuleStatus::Linked) [[unlikely]]
        recordEvaluationIfNothingIsLeftToRun(module);
    if (someMockIsSeen()) [[unlikely]] {
        if (std::optional<AnswerOfMock> answer = mockThatAnswersFor(module, moduleExport, mockOfModule)) {
            bool hasExport = answer->mock->hasProperty(realm, answer->name);
            RETURN_IF_EXCEPTION(scope, { });
            if (!hasExport)
                return jsUndefined();
            RELEASE_AND_RETURN(scope, answer->mock->get(realm, answer->name));
        }
    }
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable: {
        JSValue value = ProgramStatics::readVariable(m_instance, moduleExport.variable);
        if (!value) [[unlikely]] {
            DeferGCForAWhile deferGC(vm);
            throwException(realm, scope, createTDZError(realm, StringView(identifier(moduleExport.name).string())));
        }
        return value;
    }
    case ModuleExportKind::Namespace:
        if (JSValue ofMock = handedOnInPlaceOfNamespace(moduleExport)) [[unlikely]]
            return ofMock;
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (targetPhaseOf(moduleExport) == ModulePhase::Evaluation && !seesMockOf(moduleExport.targetModule(), module)) [[unlikely]]
            RELEASE_AND_RETURN(scope, namespaceOfModuleItself(moduleExport.targetModule()));
        if (targetPhaseOf(moduleExport) == ModulePhase::Evaluation)
            RELEASE_AND_RETURN(scope, namespaceSeenBy(moduleExport.targetModule(), module));
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        if (m_mocks) [[unlikely]] {
            AbstractModuleRecord* record = recordForResolution(indirectExport.targetModule());
            RETURN_IF_EXCEPTION(scope, { });
            AbstractModuleRecord::Resolution resolution = record->resolveExport(realm, identifier(indirectExport.importName));
            RETURN_IF_EXCEPTION(scope, { });
            if (resolution.isLiveExport(vm))
                RELEASE_AND_RETURN(scope, resolution.moduleRecord->getReexportedLiveExport(realm, resolution.localName));
        }
        JSObject* namespaceObject = moduleNamespace(indirectExport.targetModule(), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, namespaceObject->get(realm, identifier(indirectExport.importName)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::fillExportsOfBodyThatNeverRuns(ModuleID module)
{
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> variablesOfOtherModules;
    BitVector seen(m_table.size());
    seen.quickSet(module.index());
    Vector<ModuleID, 32> work { module };
    while (!work.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(work.takeLast())) {
            if (request.isRequire || seen.quickSet(request.target()))
                continue;
            work.append(request.targetModule());
            for (const ModuleExportData& moduleExport : m_table.exports(request.targetModule())) {
                if (moduleExport.kind() == ModuleExportKind::Variable)
                    variablesOfOtherModules.add(std::bit_cast<uint32_t>(moduleExport.variable));
            }
        }
    }
    for (const ModuleExportData& moduleExport : m_table.exports(module)) {
        if (moduleExport.kind() != ModuleExportKind::Variable || !moduleExport.isReplaceable || variablesOfOtherModules.contains(std::bit_cast<uint32_t>(moduleExport.variable)))
            continue;
        if (!ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable))
            ProgramStatics::writeVariable(m_instance, moduleExport.variable, jsUndefined());
    }
}

bool Modules::wasLoadedWithImporter(ModuleID module) const
{
    BitVector seen(m_table.size());
    Vector<ModuleID, 32> work;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        if (ModuleID(index) != module && status(ModuleID(index)) > ModuleStatus::Linked) {
            seen.quickSet(index);
            work.append(ModuleID(index));
        }
    }
    while (!work.isEmpty()) {
        ModuleID importer = work.takeLast();
        if (isMocked(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (request.isHoisted || request.isRequire || request.isDeferred)
                continue;
            if (request.targetModule() == module)
                return true;
            if (!seen.quickSet(request.target()))
                work.append(request.targetModule());
        }
    }
    return false;
}

bool Modules::tryReplaceExport(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    BitVector seenBehindStars;
    return tryReplaceExport(module, name, value, seenBehindStars);
}

bool Modules::tryReplaceExport(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    for (unsigned steps = 0; steps <= m_table.size(); ++steps) {
        std::optional<unsigned> index = m_table.findExport(module, name);
        if (!index) {
            auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
            if (data(module).hasDynamicStarExport() && !standsForWholeModule(module)) {
                bool isReplaced = tryReplaceExportBehindStars(module, name, value, seenBehindStars);
                RETURN_IF_EXCEPTION(scope, false);
                if (isReplaced)
                    return true;
            }
            RELEASE_AND_RETURN(scope, tryReplaceCapturedExport(module, name, value));
        }
        const ModuleExportData& moduleExport = m_table.exports(module)[*index];
        switch (moduleExport.kind()) {
        case ModuleExportKind::Variable:
            if (!moduleExport.isReplaceable)
                return false;
            if (const SyntheticExportPlace* origin = syntheticExportKeptIn(moduleExport.variable))
                return tryReplaceCapturedExport(origin->module, identifier(origin->name).impl(), value);
            if (m_moduleWithMockBeingLaid) {
                LoadedModule& mocked = ensureLoaded(*m_moduleWithMockBeingLaid);
                if (!mocked.variablesBeforeMock)
                    mocked.variablesBeforeMock.set(*this, createTableOfExports());
                Identifier place = Identifier::from(*m_instance.vm, moduleExport.variable.encoded());
                JSValue replaced = ProgramStatics::readVariable(m_instance, moduleExport.variable);
                if (replaced && !storedValue(m_instance.globalObject, mocked.variablesBeforeMock, place))
                    storeValue(m_instance.globalObject, mocked.variablesBeforeMock.get(), moduleExport.variable.encoded(), replaced);
            }
            ProgramStatics::writeVariable(m_instance, moduleExport.variable, value);
            return true;
        case ModuleExportKind::Namespace:
            return tryReplaceNamespaceHandedOn(moduleExport.targetModule(), value);
        case ModuleExportKind::Indirect: {
            const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
            module = moduleLoadedUnder(indirectExport.targetModule());
            name = identifier(indirectExport.importName).impl();
            break;
        }
        }
    }
    return false;
}

bool Modules::tryReplaceExportBehindStars(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    if (name == m_instance.vm->propertyNames->defaultKeyword.impl() || seenBehindStars.set(module.index()))
        return false;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (const ModuleRequestData& request : m_table.requests(module)) {
        if (!request.isDynamicStarExport)
            continue;
        ModuleID target = request.targetModule();
        bool hasName = true;
        if (data(target).capturesExports())
            hasName = hasCapturedExport(target, Identifier::fromUid(vm, name));
        else if (data(target).isUncompiledCode()) {
            JSObject* namespaceObject = uncompiledNamespace(target, ModulePhase::Evaluation, NamespaceUse::CarriesBindings);
            RETURN_IF_EXCEPTION(scope, false);
            hasName = namespaceObject->hasProperty(m_instance.globalObject, Identifier::fromUid(vm, name));
        }
        RETURN_IF_EXCEPTION(scope, false);
        if (!hasName)
            continue;
        bool isReplaced = tryReplaceExport(target, name, value, seenBehindStars);
        RETURN_IF_EXCEPTION(scope, false);
        if (isReplaced)
            return true;
    }
    return false;
}

bool Modules::hasBegunToLoad(ModuleID module) const
{
    if (hasMockThatEnded(module) || hasRunBesideMockAlone(module)) [[unlikely]]
        return false;
    ModuleStatus current = status(module);
    if (data(module).kind == ModuleKind::CommonJs)
        return !mayBeMocked(module) || (current != ModuleStatus::Unlinked && current != ModuleStatus::Linked);
    if ((current != ModuleStatus::Unlinked && current != ModuleStatus::Linked) || didCaptureExports(module))
        return true;
    if (m_modulesOwedToImportersThatRan.get(module.index()) || (wasLetGoByLoader(module) && data(module).needsEvaluation()))
        return false;
    return data(module).isESModuleOfProgram() && wasLoadedWithGraph(module);
}

std::optional<ModuleID> Modules::formThatMockFinds(StringView path, std::optional<ModuleID> file) const
{
    Vector<ModuleID, 2> forms = m_table.formsThatAttributesAskFor(path);
    size_t numberLoaded = std::ranges::count_if(forms, [&](ModuleID form) { return hasBegunToLoad(form); }) + (file && hasBegunToLoad(*file));
    if (numberLoaded > 1)
        return std::nullopt;
    if (file && hasBegunToLoad(*file))
        return file;
    std::optional<ModuleID> json;
    for (ModuleID form : forms) {
        if (!hasBegunToLoad(form))
            continue;
        if (!m_table.pathText(form).endsWith("?json"_s))
            return form;
        json = form;
    }
    if (json || file || forms.isEmpty())
        return json ? json : file;
    return forms[0];
}

void Modules::willMockLibraryModule(ModuleID module)
{
    if (m_libraryModulesWithMock.get(module.index()))
        return;
    m_libraryModulesWithMock.set(module.index());
    m_libraryModulesThatMockReplaces.set(module.index(), !isHeldForImport(module));
}

bool Modules::letMockStandInPlaceOfLibraryModule(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(module).kind != ModuleKind::RuntimeSource || !m_libraryModulesThatMockReplaces.get(module.index()))
        return false;
    if (m_modulesWithMockToRun.get(module.index())) {
        runMockThatWaits(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (exportsWhatMockNamesAlone(module))
        return true;
    exportWhatMockNamesAlone(module, true);
    if (!exportsWhatMockNamesAlone(module))
        return false;
    fillCapturedExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

void Modules::leaveOutWhatMockLacks(ModuleID module, JSObject* exportsOfMock)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::span<const ModuleExportData> entries = m_table.exports(module);
    BitVector leftOut(entries.size());
    for (unsigned index = 0; index < entries.size(); ++index) {
        bool isAnswered = exportsOfMock->hasOwnProperty(m_instance.globalObject, identifier(entries[index].name));
        RETURN_IF_EXCEPTION(scope, void());
        if (!isAnswered)
            leftOut.quickSet(index);
    }
    m_exportsLeftOut.set(module, WTF::move(leftOut));
}

void Modules::takeWhatMockAdds(ModuleID module, bool comesAheadOfLoad)
{
    if (!comesAheadOfLoad) {
        if (LoadedModule* loaded = loadedIfExists(module))
            loaded->takesNamesThatMockAdds = false;
        return;
    }
    LoadedModule& loaded = ensureLoaded(module);
    loaded.takesNamesThatMockAdds = true;
    loaded.exportsAddedByMock.set(*this, createTableOfExports());
}

bool Modules::isAddedByMock(ModuleID module, const Identifier& name)
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->exportsAddedByMock && !!storedValue(m_instance.globalObject, loaded->exportsAddedByMock, name);
}

bool Modules::standsForWholeModule(ModuleID module) const
{
    if (!isMocked(module) && !mockInPlace(module))
        return false;
    const LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->exportsAddedByMock;
}

void Modules::appendNamesAddedByMock(ModuleID module, WTF::Vector<Identifier>& names)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->exportsAddedByMock)
        return;
    PropertyNameArrayBuilder added(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    JSObject::getOwnPropertyNames(loaded->exportsAddedByMock, m_instance.globalObject, added, DontEnumPropertiesMode::Include);
    RETURN_IF_EXCEPTION(scope, void());
    for (const Identifier& name : added)
        names.append(name);
}

bool Modules::tryReplaceExportAddedByMock(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);
    JSObject* added = loaded.exportsAddedByMock;
    if (!added)
        return false;
    Identifier key = Identifier::fromUid(vm, name);
    if (!loaded.takesNamesThatMockAdds && !storedValue(realm, added, key))
        return false;
    added->putDirectMayBeIndex(realm, key, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSObject* namespaceObject = namespaceIfCreated(module, ModulePhase::Evaluation)) {
        uncheckedDowncast<ModuleNamespace>(namespaceObject)->replaceCapturedValue(realm, key, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

bool Modules::dropExportThatFindsNothing(ModuleID importer, const RequiredNameData& requiredName)
{
    const ExportStatementData* statement = m_table.findExportStatement(requiredName);
    if (!statement || !statement->dropsExportIfAbsent)
        return false;
    Identifier exportName = Identifier::fromString(*m_instance.vm, ProgramStrings::text(m_table.programTables(), statement->exportNameString()));
    if (std::optional<unsigned> index = m_table.findExport(importer, exportName.impl())) {
        m_exportsLeftOut.ensure(importer, [&] {
            return BitVector(m_table.exports(importer).size());
        }).iterator->value.quickSet(*index);
    }
    return true;
}

bool Modules::isVariableOfExportEmpty(ModuleID module, unsigned index) const
{
    return ProgramStatics::isVariableEmpty(m_instance, m_table.exports(module)[index].variable);
}

static bool exportSameBinding(const ModuleTable& table, const ModuleExportData& a, const ModuleExportData& b)
{
    if (a.kind() != b.kind())
        return false;
    switch (a.kind()) {
    case ModuleExportKind::Variable:
        return a.variable == b.variable;
    case ModuleExportKind::Namespace:
        return a.module == b.module;
    case ModuleExportKind::Indirect:
        return table.indirectExport(a).module == table.indirectExport(b).module && table.indirectExport(a).importName == table.indirectExport(b).importName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Modules::namesBehindStarsAreInTables(ModuleID module, const SearchBehindStars& search) const
{
    BitVector isSeen(m_table.size());
    for (ModuleID onPath : search.path)
        isSeen.quickSet(onPath.index());
    Vector<ModuleID, 8> toAsk { module };
    while (!toAsk.isEmpty()) {
        ModuleID asked = toAsk.takeLast();
        if (!data(asked).hasDynamicStarExport() || isSeen.quickSet(asked.index()) || standsForWholeModule(asked))
            continue;
        for (const ModuleRequestData& request : m_table.requests(asked)) {
            if (!request.isDynamicStarExport)
                continue;
            const ModuleData& source = data(request.targetModule());
            if (source.capturesExports() || source.isUncompiledCode() || !source.hasExportTable())
                return false;
            toAsk.append(request.targetModule());
        }
    }
    return true;
}

bool Modules::isLeftOut(ModuleID module, unsigned index, TablesBehindStars tables, SearchBehindStars& search) const
{
    if (auto found = m_exportsLeftOut.find(module); found != m_exportsLeftOut.end())
        return found->value.get(index);
    const ModuleExportData& entry = m_table.exports(module)[index];
    if (!entry.isFromStarExport)
        return false;
    size_t place = module.index() * 2 + (tables == TablesBehindStars::HaveName);
    if (search.isAnswered.get(place))
        return search.isLeftOut.get(place);
    unsigned ringsMet = search.ringsMet;
    if (data(module).hasDynamicStarExport()) {
        if (!namesBehindStarsAreInTables(module, search))
            return false;
        tables = TablesBehindStars::HaveName;
    }
    Identifier name = identifier(entry.name);
    bool isMissing = false;
    bool isAnyAsked = false;
    bool comesRoundInCircle = false;
    search.path.append(module);
    for (const ModuleRequestData& request : m_table.requests(module)) {
        ModuleID target = request.targetModule();
        if (request.isRequire || !data(target).hasExportTable())
            continue;
        std::optional<unsigned> handedOn = m_table.findExport(target, name.impl());
        bool handsOnBinding = handedOn && exportSameBinding(m_table, entry, m_table.exports(target)[*handedOn]);
        if (search.path.contains(target)) {
            ++search.ringsMet;
            comesRoundInCircle |= handsOnBinding;
            continue;
        }
        if (handsOnBinding) {
            std::optional<bool> inViewOfModule = isLeftOutInViewOf(module, target, *handedOn);
            isMissing = inViewOfModule ? *inViewOfModule : isLeftOut(target, *handedOn, tables, search);
        }
        else if (!handedOn && tables == TablesBehindStars::MayLackName)
            isMissing = false;
        else
            continue;
        isAnyAsked = true;
        if (!isMissing)
            break;
    }
    search.path.removeLast();
    bool answer = isAnyAsked ? isMissing : comesRoundInCircle;
    if (ringsMet == search.ringsMet) {
        search.isAnswered.set(place);
        search.isLeftOut.set(place, answer);
    }
    return answer;
}

void Modules::setExportsOfMockAreOfCommonJSModule(ModuleID module, bool areOfCommonJSModule)
{
    ensureLoaded(module).exportsOfMockAreOfCommonJSModule = areOfCommonJSModule;
}

void Modules::mockWhatRequireAnswered(ModuleID module, JSObject* exports)
{
    LoadedModule* loaded = &ensureLoaded(module);
    loaded->exportsOfMock.set(*this, exports);
    if (!loaded->requiredValue)
        return;
    loaded->requiredValue.set(*this, exports);
    if (JSObject* entry = loaded->objectMadeByRequire) {
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, exports);
        loaded->exportsGivenToObjectMadeByRequire.set(*this, exports);
    }
}

void Modules::mockWhatRequireCacheHolds(ModuleID module, JSObject* exports)
{
    LoadedModule& loaded = ensureLoaded(module);
    bool isFirstMock = !loaded.exportsOfMock;
    loaded.exportsOfMock.set(*this, exports);
    if (!loaded.valueInRequireCache)
        return;
    if (isFirstMock)
        loaded.valueInRequireCacheBeforeMock.set(*this, loaded.valueInRequireCache.get());
    loaded.valueInRequireCache.set(*this, exports);
    if (JSObject* entry = loaded.objectMadeByRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, exports);
}

void Modules::letGoOfMockInRequireCache(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->valueInRequireCache)
        return;
    JSValue before = loaded->valueInRequireCacheBeforeMock;
    loaded->valueInRequireCacheBeforeMock.set(*this, JSValue());
    loaded->valueInRequireCache.set(*this, before);
    setRequireCacheValue(module, before);
    if (!before) {
        loaded->objectMadeByRequire.set(*this, nullptr);
        forgetFirstRequirerOf(module);
        loaded->firstRequirerOutsideImage.set(*this, nullptr);
        m_libraryModulesInRequireCache.removeFirst(module);
        return;
    }
    if (JSObject* entry = loaded->objectMadeByRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, before);
}

auto Modules::mockThatAnswersForSlow(ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) const -> std::optional<AnswerOfMock>
{
    if (!someMockIsSeen() || passedOn.kind() == ModuleExportKind::Indirect)
        return std::nullopt;
    BitVector modulesWithMock = m_modulesWithMockInPlace;
    for (ModuleID mocked : m_endedMocks.keys())
        modulesWithMock.set(mocked.index());
    auto mockSeenOf = [&](ModuleID mocked) -> JSObject* {
        if (mocked != passingOn && moduleWithExportsOf(mocked) != passingOn)
            return mockSeenBy(mocked, passingOn);
        JSObject* standing = ofPassingOn == MockOfModule::IsReadPast ? nullptr : mockInPlace(mocked);
        return standing && seesMockOf(mocked, passingOn) ? standing : nullptr;
    };
    auto mockedModuleFor = [&](ModuleID module) -> std::optional<ModuleID> {
        for (size_t index : modulesWithMock) {
            if ((ModuleID(index) == module || moduleWithExportsOf(ModuleID(index)) == module) && mockSeenOf(ModuleID(index)))
                return ModuleID(index);
        }
        return std::nullopt;
    };
    auto indexOfExport = [&](ModuleID module, uint32_t name) -> std::optional<unsigned> {
        return data(module).hasExportTable() ? m_table.findExport(module, identifier(name).impl()) : std::nullopt;
    };
    auto wasLeftOutByMock = [&](ModuleID module, uint32_t name) {
        auto leftOut = m_exportsLeftOut.find(module);
        std::optional<unsigned> index = leftOut == m_exportsLeftOut.end() ? std::nullopt : indexOfExport(module, name);
        return index && leftOut->value.get(*index);
    };
    struct Asked {
        ModuleID module;
        uint32_t name;
        bool isAskedByStar;
    };
    UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> seen;
    Vector<Asked, 8> toAsk { Asked { passingOn, passedOn.name, false } };
    std::optional<AnswerOfMock> mockThatLacksName;
    while (!toAsk.isEmpty()) {
        Asked asked = toAsk.takeLast();
        if (!seen.add(static_cast<uint64_t>(asked.module.index()) << 32 | asked.name).isNewEntry)
            continue;
        if (std::optional<ModuleID> mocked = mockedModuleFor(asked.module)) {
            AnswerOfMock answer { mockSeenOf(*mocked), identifier(asked.name), *mocked };
            if (!asked.isAskedByStar || !wasLeftOutByMock(asked.module, asked.name))
                return answer;
            if (!mockThatLacksName)
                mockThatLacksName = answer;
            continue;
        }
        std::optional<unsigned> index = indexOfExport(asked.module, asked.name);
        bool hasNameByStar = index && m_table.exports(asked.module)[*index].isFromStarExport;
        size_t countBefore = toAsk.size();
        String exportName = identifier(asked.name).string();
        for (const RequiredNameData& requiredName : m_table.requiredNames(asked.module)) {
            const ExportStatementData* statement = m_table.findExportStatement(requiredName);
            if (statement && ProgramStrings::text(m_table.programTables(), statement->exportNameString()) == exportName)
                toAsk.append(Asked { requiredName.exporterModule(), requiredName.name, hasNameByStar });
        }
        if (toAsk.size() == countBefore) {
            mockThatLacksName = std::nullopt;
            break;
        }
        std::ranges::reverse(toAsk.mutableSpan().subspan(countBefore));
    }
    if (mockThatLacksName)
        return mockThatLacksName;
    if (passedOn.kind() != ModuleExportKind::Variable)
        return std::nullopt;
    for (size_t index : modulesWithMock) {
        JSObject* mock = mockSeenOf(ModuleID(index));
        if (!mock)
            continue;
        for (const SyntheticExportData& entry : m_table.syntheticExports(moduleWithExportsOf(ModuleID(index)))) {
            if (entry.hasVariable && entry.variable == passedOn.variable)
                return AnswerOfMock { mock, identifier(entry.name), ModuleID(index) };
        }
    }
    return std::nullopt;
}

void Modules::letRecordAnswerByMock(ModuleRecord* record, JSObject* mock)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (!record->hasLiveExports()) {
        record->setHasLiveExports();
        ModuleID module = moduleWithExportsOf(record->module());
        auto declare = [&](const Identifier& name) {
            record->addExportEntry(AbstractModuleRecord::ExportEntry::createLocal(name, name));
        };
        std::span<const ModuleExportData> entries = m_table.exports(module);
        for (unsigned index = 0; index < entries.size(); ++index) {
            if (!isLeftOut(module, index, TablesBehindStars::HaveName))
                declare(identifier(entries[index].name));
        }
        LoadedModule& loaded = ensureLoaded(module);
        if (JSObject* table = loaded.exportsWhatMockNamesAlone ? loaded.capturedExports.get() : loaded.exportsAddedByMock.get()) {
            PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            JSObject::getOwnPropertyNames(table, m_instance.globalObject, names, DontEnumPropertiesMode::Include);
            scope.assertNoExceptionExceptTermination();
            for (const Identifier& name : names)
                declare(name);
        }
    }
    record->setLiveExportsSource(vm, mock);
}

void Modules::letGoOfRecordThatAnswersByMock(ModuleID module)
{
    ModuleRecord* record = m_records.get(module);
    if (!record || !record->hasLiveExports())
        return;
    if (storedStatus(module) == StoredModuleStatus::InRecord)
        setStoredStatus(module, StoredModuleStatus::Unlinked);
    loader()->removeEntryOfRecord(record);
    Locker locker { m_state.caches->lock() };
    m_records.remove(module);
}

void Modules::takeNamesOfMockInPlace(ModuleID module, JSObject* mock)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);
    bool hasTable = data(module).hasExportTable() && !data(module).capturesExports();
    loaded.wasImportedUnderMockInPlace = false;
    if (!mock) {
        m_exportsLeftOut.remove(module);
        loaded.exportsAddedByMock.set(*this, nullptr);
        if (loaded.exportsWhatMockNamesAlone)
            loaded.capturedExports.set(*this, nullptr);
        loaded.exportsWhatMockNamesAlone = false;
        loaded.wasLoadedBehindMock = false;
        loaded.wasRequiredBehindMock = false;
        return;
    }
    PropertyNameArrayBuilder names(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
    mock->methodTable()->getOwnPropertyNames(mock, realm, names, DontEnumPropertiesMode::Exclude);
    RETURN_IF_EXCEPTION(scope, void());
    JSObject* table = createTableOfExports();
    BitVector leftOut(hasTable ? m_table.exports(module).size() : 0);
    for (unsigned index = 0; hasTable && index < m_table.exports(module).size(); ++index)
        leftOut.quickSet(index);
    for (const Identifier& name : names) {
        if (std::optional<unsigned> index = hasTable ? m_table.findExport(module, name.impl()) : std::nullopt) {
            leftOut.quickClear(*index);
            continue;
        }
        table->putDirectMayBeIndex(realm, name, jsUndefined());
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (hasTable) {
        m_exportsLeftOut.set(module, WTF::move(leftOut));
        loaded.exportsAddedByMock.set(*this, table);
        loaded.takesNamesThatMockAdds = false;
        return;
    }
    loaded.capturedExports.set(*this, table);
    loaded.exportsWhatMockNamesAlone = true;
    RELEASE_AND_RETURN(scope, fillCapturedExports(module));
}

bool Modules::setMockInPlaceOfLibraryModule(ModuleID module, JSObject* mock, JSValue whatRequireAnswers)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool stoodInPlace = !!mockInPlace(module);
    if (mock) {
        willMockLibraryModule(module);
        if (!m_libraryModulesThatMockReplaces.get(module.index()))
            return false;
        takeNamesOfMockInPlace(module, mock);
        RETURN_IF_EXCEPTION(scope, false);
        if (whatRequireAnswers.isObject())
            mockWhatRequireCacheHolds(module, asObject(whatRequireAnswers));
        m_modulesWithMockToRun.clear(module.index());
    } else if (!stoodInPlace)
        return false;
    m_numberOfMocksInPlace += !!mock - stoodInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.set(module.index(), !!mock);
    ensureLoaded(module).mockInPlace.set(*this, mock);
    if (!mock)
        letGoOfRecordThatAnswersByMock(module);
    else if (ModuleRecord* record = m_records.get(module))
        letRecordAnswerByMock(record, mock);
    if (std::optional<SlotData> word = m_table.wordOfMock(module))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : JSValue());
    return true;
}

bool Modules::setMockInPlace(ModuleID fileOrModule, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(fileOrModule).isOfLibrary() || !m_table.hasFilePath(fileOrModule))
        RELEASE_AND_RETURN(scope, setMockInPlaceOfLibraryModule(fileOrModule, mock, whatRequireAnswers));
    if (mock && hasMockThatEnded(fileOrModule)) [[unlikely]] {
        setMockInPlace(fileOrModule, nullptr, { });
        RETURN_IF_EXCEPTION(scope, false);
    }
    ModuleID module = moduleWithExportsOf(fileOrModule);
    bool isFile = data(fileOrModule).kind == ModuleKind::CommonJs;
    bool stoodInPlace = !!mockInPlace(fileOrModule);
    m_modulesWithMockToLetGo.clear(fileOrModule.index());
    m_modulesThatOwnFactoryMayLoad.clear(fileOrModule.index());
    if (!mock && !stoodInPlace)
        return false;
    JSObject* endedMock = mock ? nullptr : mockInPlace(fileOrModule);
    BitVector leftOutByEndedMock = endedMock ? m_exportsLeftOut.get(module) : BitVector();
    if (mock)
        noteMomentMockComesInPlace(fileOrModule, mock, stoodInPlace, leavesEarlierMockToItsImporters);
    if (!mock || leavesEarlierMockToItsImporters || !ensureLoaded(module).wasImportedUnderMockInPlace)
        takeNamesOfMockInPlace(module, mock);
    RETURN_IF_EXCEPTION(scope, false);
    m_numberOfMocksInPlace += !!mock - stoodInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.set(fileOrModule.index(), !!mock);
    if (mock)
        willMock(fileOrModule);
    if (mock && m_modulesThatRanBesideMock.get(module.index())) [[unlikely]]
        setAsideErrorOfModuleThatRanBesideMock(module);
    ensureLoaded(fileOrModule).mockInPlace.set(*this, mock);
    ensureLoaded(module).mockInPlace.set(*this, mock);
    for (ModuleID each : { fileOrModule, module }) {
        if (!mock)
            letGoOfRecordThatAnswersByMock(each);
        else if (ModuleRecord* record = m_records.get(each))
            letRecordAnswerByMock(record, mock);
    }
    if (endedMock)
        keepMockThatStoodInPlace(fileOrModule, module, endedMock, WTF::move(leftOutByEndedMock));
    didReplaceNamespaceHandedOn(fileOrModule);
    if (module != fileOrModule)
        didReplaceNamespaceHandedOn(module);
    if (std::optional<SlotData> word = m_table.wordOfMock(fileOrModule))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : wordWhereNoMockStands(fileOrModule));

    LoadedModule& loaded = ensureLoaded(fileOrModule);
    bool originalRuns = isFile && m_filesWhoseOriginalRuns.get(fileOrModule.index());
    if (mock && loaded.mockLaidInPreload.get() == mock) {
        loaded.whatRequireAnswersUnderMockLaidInPreload.set(*this, whatRequireAnswers);
        ensureLoaded(module).wasImportedUnderMockInPlace |= m_loadedInPreload.get(module.index());
    }
    if (mock) {
        if (originalRuns) [[unlikely]]
            loaded.answerOfMockWhileOriginalRuns.set(*this, whatRequireAnswers);
        else if (isFile) {
            markEvaluated(fileOrModule);
            setCommonJSExports(fileOrModule, whatRequireAnswers);
        } else
            loaded.requiredValue.set(*this, whatRequireAnswers);
        if (!isFile && loaded.mockLaidInPreload.get() == mock && m_loadedInPreload.get(fileOrModule.index())) {
            for (ModuleID each : { fileOrModule, module }) {
                if (status(each) == ModuleStatus::Unlinked || status(each) == ModuleStatus::Linked)
                    setStatus(each, ModuleStatus::Evaluated);
            }
        }
        if (JSObject* entry = loaded.objectMadeByRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, whatRequireAnswers);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, whatRequireAnswers);
        }
        return true;
    }

    didEndMock(fileOrModule);
    forgetWhoLoadedItUnderMockInPlace(fileOrModule);
    bringBackErrorOfModuleThatRanBesideMock(module);
    loaded.wasRequiredUnderMockInPlace = false;
    JSValue original = isFile ? loaded.exportsOfOriginalBesideMock.get() : JSValue();
    loaded.exportsOfOriginalBesideMock.set(*this, JSValue());
    bool ranBesideMock = m_modulesThatRanBesideMock.get(module.index());
    m_modulesThatRanBesideMock.clear(module.index());
    m_modulesOwedBesideMock.clear(module.index());
    goModuleByModuleFromNowOn();
    for (ModuleID each : { fileOrModule, module }) {
        if (ranBesideMock) [[unlikely]] {
            if (status(each) < ModuleStatus::Evaluating)
                setStatus(each, ModuleStatus::Evaluated);
            m_modulesThatRanBesideMockAlone.set(each.index());
            m_modulesLoadedBesideGroups.clear(each.index());
            continue;
        }
        if (status(each) == ModuleStatus::Evaluated && (!original || each != fileOrModule))
            setStatus(each, ModuleStatus::Unlinked);
        m_modulesLoadedBesideGroups.clear(each.index());
        m_modulesOwedToImportersThatRan.set(each.index());
        m_modulesWithNothingOwedBelow.clearAll();
        m_mockedModulesPassedOverByWalks.clear();
        m_modulesSettledForLinks[each.index() / 64] &= ~(1ull << (each.index() % 64));
    }
    followImportsAnewAtNextQuestion();
    ++m_linkEpoch;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == fileOrModule || each == module || data(each).hasDynamicStarExport())
            letGoOfNamespaceMadeUnderMock(each);
    }
    if (JSObject* own = ensureLoaded(module).namespaceBeforeMockInPlace.get()) [[unlikely]] {
        uncheckedDowncast<ModuleNamespace>(own)->setKeepsToModule(false);
        ensureLoaded(module).namespaceBeforeMockInPlace.set(*this, nullptr);
        setNamespace(module, ModulePhase::Evaluation, own);
    }
    if (originalRuns) [[unlikely]]
        loaded.answerOfMockWhileOriginalRuns.set(*this, JSValue());
    else if (isFile)
        setCommonJSExports(fileOrModule, original ? original : jsUndefined());
    else
        loaded.requiredValue.set(*this, JSValue());
    loaded.objectMadeByRequire.set(*this, nullptr);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    return true;
}

uint32_t Modules::momentOfFirstMockInTestFile(ModuleID module)
{
    VMState& state = m_instance.vm->aotState();
    auto result = m_firstMocksInTestFile.add(module, FirstMockInTestFile { 0, 0 });
    if (result.isNewEntry || result.iterator->value.momentOfTestFile != state.momentOfTestFile)
        result.iterator->value = FirstMockInTestFile { ++state.momentOfLoads, state.momentOfTestFile };
    return result.iterator->value.moment;
}

void Modules::noteEndOfLoadedMock(ModuleID module)
{
    if (auto first = m_firstMocksInTestFile.find(module); first != m_firstMocksInTestFile.end())
        first->value.hasEnded = true;
}

void Modules::countMocksAnewBehindLoad(ModuleID module)
{
    auto first = m_firstMocksInTestFile.find(module);
    if (first == m_firstMocksInTestFile.end() || !std::exchange(first->value.hasEnded, false))
        return;
    if (!hasMockThatEnded(module) && !wasLetGoByLoader(module) && !m_modulesOwedToImportersThatRan.get(module.index()))
        m_firstMocksInTestFile.remove(first);
}

bool Modules::hasMockLaidOver(ModuleID module) const
{
    const LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->hasMockLaidOver;
}

bool Modules::keepsStatusOfRunBeforeLoadThatRuns(ModuleID module) const
{
    auto anew = m_momentsLoadedAnew.find(module);
    return anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == m_instance.vm->aotState().momentOfTestFile && status(module) == ModuleStatus::Evaluated && status(anew->value.rootOfLoad) == ModuleStatus::Evaluating;
}

BitVector Modules::modulesThatWerePassedWithoutStatus() const
{
    BitVector passed(m_table.size());
    Vector<ModuleID, 32> toFollow;
    auto pass = [&](ModuleID imported) {
        if ((!data(imported).needsEvaluation() || keepsStatusOfRunBeforeLoadThatRuns(imported)) && !passed.quickSet(imported.index()))
            toFollow.append(imported);
    };
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (!data(importer).isESModuleOfProgram() || !data(importer).needsEvaluation() || status(importer) < ModuleStatus::Evaluating || m_importersOfMockThatEnded.get(index) || keepsStatusOfRunBeforeLoadThatRuns(importer))
            continue;
        bool isEntered = status(importer) == ModuleStatus::Evaluating;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID imported = moduleLoadedBy(request);
            if (isEntered && data(imported).needsEvaluation() && status(imported) == ModuleStatus::Evaluating)
                break;
            pass(imported);
        }
    }
    while (!toFollow.isEmpty()) {
        for (const ModuleRequestData& request : m_table.imports(toFollow.takeLast()))
            pass(moduleLoadedBy(request));
    }
    return passed;
}

bool Modules::hasLinkedImporterYetToRun(ModuleID module, std::optional<ModuleID> asking) const
{
    std::optional<BitVector> passed;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (!data(importer).isESModuleOfProgram() || data(importer).isFaceOfFile() || !wasLoadedWithGraph(importer))
            continue;
        bool keepsStatus = keepsStatusOfRunBeforeLoadThatRuns(importer);
        if (status(importer) > ModuleStatus::Evaluating && !keepsStatus)
            continue;
        if (asking && importer == *asking)
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (moduleLoadedBy(request) != module && moduleLoadedBy(request) != moduleWithExportsOf(module))
                continue;
            if (data(importer).needsEvaluation() && !keepsStatus)
                return true;
            if (!passed)
                passed = modulesThatWerePassedWithoutStatus();
            if (!passed->quickGet(index))
                return true;
            break;
        }
    }
    return false;
}

bool Modules::willLeaveModuleAlone(ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking)
{
    if (auto waiting = m_mocksOverModulesThatHaveRun.find(module); waiting != m_mocksOverModulesThatHaveRun.end() && !waiting->value.isInPlace && wasLetGoByLoader(module))
        return true;
    const ModuleData& moduleData = data(module);
    bool isFile = moduleData.kind == ModuleKind::CommonJs && !moduleData.isOfLibrary();
    bool isData = moduleData.kind == ModuleKind::Data && !moduleData.isOfLibrary();
    if ((!isFile && !isData && (!moduleData.isESModuleOfProgram() || moduleData.isFaceOfFile())) || !m_table.hasFilePath(module) || !m_table.wordOfMock(module))
        return false;
    if (!hasRun(module) || isMocked(module) || mockInPlace(module) || isEvicted(module) || m_momentsOfLoads.isEmpty())
        return false;
    ModuleID withExports = moduleWithExportsOf(module);
    if (ModuleStatus face = status(withExports); isFile && !onlyIfLoadedInPreload && withExports != module && (face == ModuleStatus::Unlinked || face == ModuleStatus::Linked)) {
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        evaluateSynchronously(withExports, SynchronousEvaluationReason::Require);
        RETURN_IF_EXCEPTION(scope, false);
        m_facesThatTookCopiesForMock.set(withExports.index());
    }
    if (isFile && (withExports == module || status(withExports) != ModuleStatus::Evaluated || !didCaptureExports(withExports)))
        return false;
    if (onlyIfLoadedInPreload && !m_loadedInPreload.get(withExports.index()))
        return false;
    if (LoadedModule* loaded = loadedIfExists(module); loaded && loaded->objectMadeByRequire && !isFile)
        return false;
    if (isGraphRunning() && hasLinkedImporterYetToRun(module, asking))
        return false;
    if (!wasLoadedInThisTestFile(withExports))
        letGoOfImportersOf(module, ImportersLetGo::All);
    VMState& state = m_instance.vm->aotState();
    auto laid = m_mocksOverModulesThatHaveRun.set(module, MockOverModuleThatHasRun { momentOfFirstMockInTestFile(module), state.momentOfTestFile });
    laid.iterator->value.momentThisWasLaid = ++state.momentOfLoads;
    auto setAsideRecord = [&](ModuleID each) {
        ModuleRecord* record = m_records.get(each);
        if (!record)
            return;
        loader()->removeEntryOfRecord(record);
        if (storedStatus(each) == StoredModuleStatus::InRecord)
            setStoredStatus(each, StoredModuleStatus::Evaluated);
        ensureLoaded(each).recordBeforeMockInPlace.set(*this, record);
        Locker locker { m_state.caches->lock() };
        m_records.remove(each);
    };
    setAsideRecord(module);
    if (isFile)
        setAsideRecord(withExports);
    return true;
}

bool Modules::setMockOverModuleThatHasRun(ModuleID module, JSObject* mock, JSValue whatRequireAnswers)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleID withExports = moduleWithExportsOf(module);
    bool isFile = withExports != module;
    LoadedModule& exporting = ensureLoaded(withExports);
    JSObject* ownNamespace = exporting.namespaceBeforeMockInPlace.get();
    if (!ownNamespace)
        ownNamespace = namespaceIfCreated(withExports, ModulePhase::Evaluation);
    bool namespaceIsHeld = !!ownNamespace;
    if (!ownNamespace) {
        ownNamespace = createNamespace(withExports, ModulePhase::Evaluation, true);
        RETURN_IF_EXCEPTION(scope, false);
        setNamespace(withExports, ModulePhase::Evaluation, nullptr);
    }
    std::optional<BitVector> exportsLeftOutBefore;
    if (auto leftOut = m_exportsLeftOut.find(withExports); leftOut != m_exportsLeftOut.end())
        exportsLeftOutBefore = leftOut->value;
    BitVector exportsHiddenBefore(m_table.exports(withExports).size());
    for (unsigned index = 0; index < m_table.exports(withExports).size(); ++index)
        exportsHiddenBefore.quickSet(index, (m_table.exports(withExports)[index].isFromStarExport && isExportAmbiguous(withExports, index)) || isLeftOut(withExports, index, TablesBehindStars::MayLackName));
    if (isFile && !ensureLoaded(withExports).capturedExportsBeforeMockInPlace) {
        JSObject* own = capturedExports(withExports);
        RETURN_IF_EXCEPTION(scope, false);
        ensureLoaded(withExports).capturedExportsBeforeMockInPlace.set(*this, own);
    }
    takeNamesOfMockInPlace(withExports, mock);
    RETURN_IF_EXCEPTION(scope, false);
    auto found = m_mocksOverModulesThatHaveRun.find(module);
    if (found == m_mocksOverModulesThatHaveRun.end())
        return false;
    MockOverModuleThatHasRun& kept = found->value;
    LoadedModule& loaded = ensureLoaded(module);
    loaded.mockInPlace.set(*this, mock);
    ensureLoaded(withExports).mockInPlace.set(*this, mock);
    ProgramStatics::writeVariable(m_instance, *m_table.wordOfMock(module), mock);
    if (!kept.isInPlace) {
        JSObject* askedFor = exporting.namespaceBeforeMockInPlace.get();
        if (!askedFor)
            askedFor = namespaceIfCreated(withExports, ModulePhase::Evaluation);
        if (askedFor) {
            ownNamespace = askedFor;
            namespaceIsHeld = true;
        }
        uncheckedDowncast<ModuleNamespace>(ownNamespace)->setKeepsToModule(true);
        exporting.namespaceBeforeMockInPlace.set(*this, ownNamespace);
        kept.namespaceIsHeld = namespaceIsHeld;
    }
    setNamespace(withExports, ModulePhase::Evaluation, nullptr);
    auto handsOnNamespace = [&](const ModuleExportData& entry) {
        return entry.kind() == ModuleExportKind::Namespace && moduleWithExportsOf(fileOf(entry.targetModule())) == withExports;
    };
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == withExports)
            continue;
        JSObject* made = namespaceIfCreated(each, ModulePhase::Evaluation);
        LoadedModule* other = loadedIfExists(each);
        JSObject* setAside = other ? other->namespaceBeforeMockInPlace.get() : nullptr;
        if ((made || setAside) && std::ranges::any_of(m_table.exports(each), handsOnNamespace)) {
            for (JSObject* object : { made, setAside }) {
                if (auto* filed = dynamicDowncast<ModuleNamespace>(object))
                    filed->leaveStructureThatCachesKnow(*m_instance.vm);
            }
        }
        if (!made || !data(each).hasDynamicStarExport())
            continue;
        LoadedModule& behindStars = ensureLoaded(each);
        if (!behindStars.namespaceBeforeMockInPlace)
            behindStars.namespaceBeforeMockInPlace.set(*this, made);
        letGoOfNamespaceMadeUnderMock(each);
    }
    if (ModuleRecord* record = m_records.get(module))
        letRecordAnswerByMock(record, mock);
    if (ModuleRecord* record = isFile ? m_records.get(withExports) : nullptr)
        letRecordAnswerByMock(record, mock);
    auto answerRequire = [&] {
        loaded.requiredValue.set(*this, whatRequireAnswers);
        if (!isFile)
            return;
        setCommonJSExports(module, whatRequireAnswers);
        if (JSObject* entry = loaded.objectMadeByRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, whatRequireAnswers);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, whatRequireAnswers);
        }
    };
    if (kept.isInPlace) {
        answerRequire();
        return true;
    }
    kept.isInPlace = true;
    kept.exportsLeftOutBefore = WTF::move(exportsLeftOutBefore);
    kept.exportsHiddenBefore = WTF::move(exportsHiddenBefore);
    ++m_numberOfMocksInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.set(module.index());
    loaded.requiredValueBeforeMockInPlace.set(*this, loaded.requiredValue.get());
    if (isFile)
        loaded.exportsOfOriginalBesideMock.set(*this, commonJSExports(module));
    answerRequire();
    return true;
}

void Modules::endMockOverModuleThatHasRun(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockOverModuleThatHasRun kept = m_mocksOverModulesThatHaveRun.take(module);
    m_modulesWithMockToRun.clear(module.index());
    m_modulesWithMockThatWaited.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didEndMock(module);
    ModuleID withExports = moduleWithExportsOf(module);
    bool isFile = withExports != module;
    LoadedModule& loaded = ensureLoaded(module);
    auto bringBackRecord = [&](ModuleID each) {
        if (ModuleRecord* recordOfMock = m_records.get(each)) {
            loader()->removeEntryOfRecord(recordOfMock);
            if (storedStatus(each) == StoredModuleStatus::InRecord)
                setStoredStatus(each, StoredModuleStatus::Evaluated);
            Locker locker { m_state.caches->lock() };
            m_records.remove(each);
        }
        LoadedModule& held = ensureLoaded(each);
        JSObject* before = held.recordBeforeMockInPlace.get();
        if (!before)
            return;
        auto* record = uncheckedDowncast<ModuleRecord>(before);
        held.recordBeforeMockInPlace.set(*this, nullptr);
        {
            Locker locker { m_state.caches->lock() };
            m_records.add(each, record);
            m_storedReferences.recordAll();
        }
        didChangeStatusInRecord(each);
        if (record->holdsStatus())
            setStoredStatus(each, StoredModuleStatus::InRecord);
    };
    bringBackRecord(module);
    if (isFile)
        bringBackRecord(withExports);
    bool takesCopiesAnew = isFile && m_facesThatTookCopiesForMock.clear(withExports.index());
    if (!kept.isInPlace) {
        if (takesCopiesAnew)
            RELEASE_AND_RETURN(scope, captureExportsAgain(withExports));
        return;
    }
    LoadedModule& exporting = ensureLoaded(withExports);
    BitVector leftOutByEndedMock = m_exportsLeftOut.get(withExports);
    takeNamesOfMockInPlace(withExports, nullptr);
    scope.assertNoExceptionExceptTermination();
    if (kept.exportsLeftOutBefore)
        m_exportsLeftOut.set(withExports, WTF::move(*kept.exportsLeftOutBefore));
    if (JSObject* own = exporting.capturedExportsBeforeMockInPlace.get()) {
        exporting.capturedExports.set(*this, own);
        exporting.capturedExportsBeforeMockInPlace.set(*this, nullptr);
    }
    m_momentsWaitingMocksWereLaid.remove(module);
    keepNamespacesMadeUnder(module, withExports, loaded.mockInPlace.get(), keepEndedMock(module, loaded.mockInPlace.get(), { kept.momentThisWasLaid, kept.momentOfTestFile }, 0, WTF::move(leftOutByEndedMock)));
    loaded.mockInPlace.set(*this, nullptr);
    exporting.mockInPlace.set(*this, nullptr);
    --m_numberOfMocksInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithMockInPlace.clear(module.index());
    ProgramStatics::writeVariable(m_instance, *m_table.wordOfMock(module), wordWhereNoMockStands(module));
    loaded.requiredValue.set(*this, loaded.requiredValueBeforeMockInPlace.get());
    loaded.requiredValueBeforeMockInPlace.set(*this, JSValue());
    if (isFile) {
        JSValue original = loaded.exportsOfOriginalBesideMock.get();
        loaded.exportsOfOriginalBesideMock.set(*this, JSValue());
        setCommonJSExports(module, original);
        if (JSObject* entry = loaded.objectMadeByRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, original);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, original);
        }
    } else {
        loaded.objectMadeByRequire.set(*this, nullptr);
        loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    }
    bool anotherMockStands = false;
    for (const MockOverModuleThatHasRun& other : m_mocksOverModulesThatHaveRun.values())
        anotherMockStands |= other.isInPlace;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == withExports || data(each).hasDynamicStarExport())
            letGoOfNamespaceMadeUnderMock(each);
        if (each == withExports || !data(each).hasDynamicStarExport() || anotherMockStands)
            continue;
        LoadedModule* behindStars = loadedIfExists(each);
        if (JSObject* before = behindStars ? behindStars->namespaceBeforeMockInPlace.get() : nullptr) {
            behindStars->namespaceBeforeMockInPlace.set(*this, nullptr);
            setNamespace(each, ModulePhase::Evaluation, before);
        }
    }
    if (JSObject* own = exporting.namespaceBeforeMockInPlace.get()) {
        uncheckedDowncast<ModuleNamespace>(own)->setKeepsToModule(false);
        if (!kept.namespaceIsHeld || !data(withExports).hasDynamicStarExport() || !anotherMockStands)
            exporting.namespaceBeforeMockInPlace.set(*this, nullptr);
        if (kept.namespaceIsHeld)
            setNamespace(withExports, ModulePhase::Evaluation, own);
    }
    if (takesCopiesAnew)
        RELEASE_AND_RETURN(scope, captureExportsAgain(withExports));
}

JSObject* Modules::namespaceOfModuleItself(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto mock = m_mocksOverModulesThatHaveRun.find(fileOf(module));
    if (mock == m_mocksOverModulesThatHaveRun.end() && hasRunItself(module) && isMocked(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, namespaceOfModuleThatRanBesideMock(module));
    if (mock == m_mocksOverModulesThatHaveRun.end())
        RELEASE_AND_RETURN(scope, moduleNamespace(module, ModulePhase::Evaluation));
    if (!mock->value.isInPlace) {
        if (JSObject* own = namespaceIfCreated(module, ModulePhase::Evaluation))
            return own;
        RELEASE_AND_RETURN(scope, createNamespace(module, ModulePhase::Evaluation, true));
    }
    JSObject* own = ensureLoaded(moduleWithExportsOf(mock->key)).namespaceBeforeMockInPlace.get();
    RELEASE_ASSERT(own);
    mock->value.namespaceIsHeld = true;
    return own;
}

bool Modules::isHiddenInModuleItself(ModuleID module, unsigned index) const
{
    auto mock = m_mocksOverModulesThatHaveRun.find(fileOf(module));
    if (mock != m_mocksOverModulesThatHaveRun.end() && mock->value.isInPlace)
        return mock->value.exportsHiddenBefore.get(index);
    if (hasRunItself(module) && mockInPlace(module)) [[unlikely]]
        return m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index);
    return (m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index)) || isLeftOut(module, index, TablesBehindStars::MayLackName);
}

JSValue Modules::readExportOfModuleItself(ModuleID module, unsigned index)
{
    return readExport(module, index, MockOfModule::IsReadPast);
}






const MockOperations& Modules::mockOperations()
{
    static constexpr MockOperations operations {
        .runMocksBelow = [](Modules& modules, ModuleID root, RootOfLoad rootOfLoad) { return modules.runMocksBelowSlow(root, rootOfLoad); },
        .runMocksBelowModuleThatLoaderFetches = [](Modules& modules, ModuleID module) { return modules.runMocksBelowModuleThatLoaderFetchesSlow(module); },
        .letGoOfMockThatEnded = [](Modules& modules, ModuleID module) { modules.letGoOfMockThatEndedSlow(module); },
        .runMockThatWaits = [](Modules& modules, ModuleID module) { return modules.runMockThatWaitsSlow(module); },
        .mockThatAnswersFor = [](const Modules& modules, ModuleID passingOn, const ModuleExportData& passedOn, MockOfModule ofPassingOn) { return modules.mockThatAnswersForSlow(passingOn, passedOn, ofPassingOn); },
        .isMockedInItsImage = [](Modules& modules, ModuleID module, MocksAskedFor which) { return modules.isMockedInItsImageSlow(module, which); },
    };
    return operations;
}

const MockOperations* mockOperations()
{
    return &Modules::mockOperations();
}

auto Modules::syntheticExportKeptIn(SlotData variable) -> const SyntheticExportPlace*
{
    if (!m_syntheticExportsByVariable) {
        m_syntheticExportsByVariable.emplace();
        for (unsigned index = 0; index < m_table.size(); ++index) {
            for (const SyntheticExportData& syntheticExport : m_table.syntheticExports(ModuleID(index))) {
                if (syntheticExport.hasVariable && syntheticExport.isReplaceable)
                    m_syntheticExportsByVariable->add(syntheticExport.variable.encoded(), SyntheticExportPlace { ModuleID(index), syntheticExport.name });
            }
        }
    }
    auto found = m_syntheticExportsByVariable->find(variable.encoded());
    return found == m_syntheticExportsByVariable->end() ? nullptr : &found->value;
}

ModuleID Modules::moduleWithNamesOf(ModuleID module) const
{
    ModuleID withExports = moduleWithExportsOf(module);
    return data(withExports).capturesExports() || exportsWhatMockNamesAlone(withExports) ? withExports : module;
}

ModuleID Modules::moduleWithExportsOf(ModuleID module) const
{
    std::optional<ModuleID> face = data(module).kind == ModuleKind::CommonJs ? m_table.faceOfFile(module) : std::nullopt;
    return face.value_or(module);
}

bool Modules::mayBeMocked(ModuleID module) const
{
    return data(module).mayBeMocked();
}

bool Modules::tryReplaceCapturedExport(ModuleID module, UniquedStringImpl* name, JSValue value)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isNamed = [&](const SyntheticExportData& syntheticExport) {
        return identifier(syntheticExport.name).impl() == name;
    };
    std::span<const SyntheticExportData> syntheticExports = m_table.syntheticExports(module);
    auto found = std::ranges::find_if(syntheticExports, isNamed);
    bool isListed = found != syntheticExports.end();
    if (isListed ? !found->isReplaceable : !data(m_table.fileBehindFace(module).value_or(module)).mayBeMocked())
        return false;
    if (!isListed && data(module).hasExportTable() && !m_table.fileBehindFace(module))
        RELEASE_AND_RETURN(scope, tryReplaceExportAddedByMock(module, name, value));

    Identifier key = Identifier::fromUid(vm, name);
    JSObject* captured = capturedExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    LoadedModule& loaded = ensureLoaded(module);
    bool hasItsNames = loaded.exportsWhatMockNamesAlone ? loaded.wasLoadedBehindMock : !m_libraryModulesThatMockReplaces.get(module.index());
    if (hasItsNames && !storedValue(realm, captured, key))
        return false;
    if ((m_libraryModulesWithMock.get(module.index()) || m_moduleWithMockBeingLaid) && !loaded.exportsWhatMockNamesAlone) {
        if (!loaded.exportsBeforeMock)
            loaded.exportsBeforeMock.set(*this, createTableOfExports());
        JSObject* before = loaded.exportsBeforeMock;
        JSValue replaced = storedValue(realm, captured, key);
        if (replaced && !storedValue(realm, before, key)) {
            before->putDirectMayBeIndex(realm, key, replaced == JSValue(captured) ? JSValue(before) : replaced);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    captured->putDirectMayBeIndex(realm, key, value);
    RETURN_IF_EXCEPTION(scope, false);
    if (JSObject* namespaceObject = namespaceIfCreated(module, ModulePhase::Evaluation)) {
        uncheckedDowncast<ModuleNamespace>(namespaceObject)->replaceCapturedValue(realm, key, value);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (isListed && found->hasVariable)
        ProgramStatics::writeVariable(m_instance, found->variable, value);
    return true;
}

JSObject* Modules::capturedExports(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.capturedExports && loaded.capturedFields)
        makeTableOfCapturedFields(loaded);
    RELEASE_ASSERT(loaded.capturedExports);
    return loaded.capturedExports;
}

JSValue Modules::capturedValue(ModuleID module, const Identifier& name)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, { });
    LoadedModule& loaded = ensureLoaded(module);
    JSObject* captured = loaded.capturedExports;
    if (!captured && loaded.capturedFields)
        return capturedField(loaded, name.impl());
    RELEASE_ASSERT(captured);
    JSValue value = storedValue(realm, captured, name);
    RETURN_IF_EXCEPTION(scope, { });
    if (value != JSValue(captured))
        return value;
    value = loaded.deferredExportsSource->get(realm, name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue filledMeanwhile = storedValue(realm, captured, name);
    if (filledMeanwhile != JSValue(captured))
        return filledMeanwhile;
    captured->putDirectMayBeIndex(realm, name, value);
    RETURN_IF_EXCEPTION(scope, { });
    return value;
}

bool Modules::hasCapturedExport(ModuleID module, const Identifier& name)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    captureExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.capturedExports && loaded.capturedFields)
        return !!capturedField(loaded, name.impl());
    RELEASE_ASSERT(loaded.capturedExports);
    return !!storedValue(m_instance.globalObject, loaded.capturedExports, name);
}

bool Modules::isExportAmbiguous(ModuleID module, unsigned index) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->ambiguousExports.get(index);
}

JSObject* Modules::createTableOfExports()
{
    VM& vm = *m_instance.vm;
    JSObject* table = constructEmptyObject(vm, m_instance.globalObject->nullPrototypeObjectStructure());
    table->convertToDictionary(vm);
    return table;
}

JSObject* Modules::namespaceThatCompiledCodeHolds(ModuleID module, ModulePhase phase) const
{
    if (data(module).kind == ModuleKind::CommonJs) {
        if (std::optional<ModuleID> face = m_table.faceOfFile(module); face && data(*face).capturesExports())
            module = *face;
    }
    if (phase != ModulePhase::Evaluation || !m_modulesWithEarlierNamespaceInValue.get(module.index()))
        return nullptr;
    JSValue held = value(module);
    return held ? asObject(held) : nullptr;
}

JSObject* Modules::namespaceIfCreated(ModuleID module, ModulePhase phase) const
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index())) {
        JSValue held = value(module);
        return held ? asObject(held) : nullptr;
    }
    LoadedModule* loaded = loadedIfExists(module);
    return loaded ? loaded->namespaceObjectFor(phase).get() : nullptr;
}

void Modules::letGoOfNamespaceMadeUnderMock(ModuleID module)
{
    JSObject* made = namespaceIfCreated(module, ModulePhase::Evaluation);
    if (!made)
        return;
    setNamespace(module, ModulePhase::Evaluation, nullptr);
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || loaded->requiredValue.get() != JSValue(made))
        return;
    loaded->requiredValue.set(*this, JSValue());
    loaded->objectMadeByRequire.set(*this, nullptr);
    loaded->exportsGivenToObjectMadeByRequire.set(*this, JSValue());
}

void Modules::setNamespace(ModuleID module, ModulePhase phase, JSObject* namespaceObject)
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index()))
        setValue(module, namespaceObject);
    else
        ensureLoaded(module).namespaceObjectFor(phase).set(*this, namespaceObject);
}

JSObject* Modules::createNamespace(ModuleID module, ModulePhase phase, bool shouldPreventExtensions)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleData& moduleData = data(module);

    if (moduleData.isUncompiledCode()) {
        JSObject* loaderNamespace = uncompiledNamespace(module, phase, NamespaceUse::IsShownToProgram, shouldPreventExtensions);
        RETURN_IF_EXCEPTION(scope, nullptr);
        setNamespace(module, phase, loaderNamespace);
        return loaderNamespace;
    }
    if (moduleData.kind == ModuleKind::CommonJs) {
        if (std::optional<ModuleID> face = m_table.faceOfFile(module); face && data(*face).capturesExports())
            RELEASE_AND_RETURN(scope, moduleNamespace(*face, phase, shouldPreventExtensions));
    }
    if (std::optional<ModuleID> file = m_table.fileBehindFace(module); file && !moduleData.capturesExports() && m_table.exports(module).empty()) {
        JSObject* namespaceOfFile = moduleNamespace(*file, phase, shouldPreventExtensions);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (auto* compiled = dynamicDowncast<ModuleNamespace>(namespaceOfFile))
            compiled->setFace(module);
        return namespaceOfFile;
    }

    bool ranCode = false;
    if (status(module) == ModuleStatus::Unlinked) {
        link(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ranCode = true;
    }
    if ((moduleData.capturesExports() || moduleData.kind == ModuleKind::CommonJs) && !didCaptureExports(module)) {
        captureExports(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        ranCode = true;
    }
    if (JSObject* createdMeanwhile = ranCode ? namespaceIfCreated(module, phase) : nullptr)
        return createdMeanwhile;

    bool isRequired = phase == ModulePhase::Evaluation && m_modulesWithMarkOfRequire.get(module.index());
    if (phase == ModulePhase::Evaluation && m_modulesWithExtensibleNamespace.get(module.index()))
        shouldPreventExtensions = false;
    ModuleNamespace* namespaceObject = ModuleNamespace::create(vm, namespaceStructure(shouldPreventExtensions), module, phase, { });
    if (phase == ModulePhase::Evaluation)
        m_namespacesBeingMade.append({ module, namespaceObject });
    ModuleNamespace::DynamicExports exportObjects;
    createDynamicExports(module, exportObjects.names, exportObjects.capturedValues, exportObjects.liveSources);
    if (phase == ModulePhase::Evaluation)
        m_namespacesBeingMade.removeLast();
    RETURN_IF_EXCEPTION(scope, nullptr);
    namespaceObject->replaceDynamicExports(vm, exportObjects);
    if (isRequired && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl()))
        namespaceObject->setESModuleMarker(true);
    if (JSObject* mock = phase == ModulePhase::Evaluation && isLeftAloneByMock(module) ? mockInPlace(module) : nullptr) [[unlikely]]
        namespaceObject->setMockOfView(vm, mock);
    setNamespace(module, phase, namespaceObject);
    return namespaceObject;
}

void Modules::createDynamicExports(ModuleID module, JSCellButterfly*& names, JSObject*& capturedValues, JSObject*& liveSources)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    Vector<DynamicExport> dynamicExports = collectDynamicExports(module);
    RETURN_IF_EXCEPTION(scope, void());
    if (!dynamicExports.isEmpty()) {
        capturedValues = createTableOfExports();
        names = JSCellButterfly::create(vm, CopyOnWriteArrayWithContiguous, dynamicExports.size());
    }
    for (unsigned i = 0; i < dynamicExports.size(); ++i) {
        const DynamicExport& dynamicExport = dynamicExports[i];
        if (dynamicExport.liveSource) {
            if (!liveSources)
                liveSources = createTableOfExports();
            liveSources->putDirectMayBeIndex(realm, dynamicExport.name, dynamicExport.liveSource);
        } else if (dynamicExport.capturedValue)
            capturedValues->putDirectMayBeIndex(realm, dynamicExport.name, dynamicExport.capturedValue);
        RETURN_IF_EXCEPTION(scope, void());
        JSString* text = jsString(vm, dynamicExport.name.string());
        text->toIdentifier(realm);
        RETURN_IF_EXCEPTION(scope, void());
        names->setIndex(vm, i, text);
    }
}

Structure* Modules::namespaceStructure(bool shouldPreventExtensions)
{
    JSGlobalObject* realm = m_instance.globalObject;
    Structure*& structure = shouldPreventExtensions ? m_namespaceStructure : m_extensibleNamespaceStructure;
    if (!structure) {
        structure = ModuleNamespace::createStructure(*m_instance.vm, realm, realm->moduleNamespaceObjectStructure()->storedPrototype(), shouldPreventExtensions);
        m_storedReferences.recordAll();
    }
    return structure;
}

bool Modules::mayExport(ModuleID module, UniquedStringImpl* name) const
{
    const ModuleData& moduleData = data(module);
    if (moduleData.kind != ModuleKind::SourceText || moduleData.capturesExports() || moduleData.hasDynamicStarExport())
        return true;
    return !!m_table.findExport(module, name);
}

JSObject* Modules::moduleNamespace(ModuleID module, ModulePhase phase, bool shouldPreventExtensions)
{
    if (m_mocks) [[unlikely]]
        noteLoadBehindMock(module, false);
    if (m_numberOfMocksInPlace && mockInPlace(module)) [[unlikely]]
        noteImportUnderMockInPlace(module);
    if (JSObject* namespaceObject = namespaceIfCreated(module, phase))
        return namespaceObject;
    return createNamespace(module, phase, shouldPreventExtensions);
}

JSObject* Modules::importMeta(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.importMeta)
        return loaded.importMeta;
    JSObject* meta = loader()->createImportMetaProperties(m_instance.globalObject, pathString(module), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_ASSERT(meta);
    loaded.importMeta.set(*this, meta);
    return meta;
}

JSCell* Modules::builtinCell(BuiltinID builtin, ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    const Builtin& entry = BuiltinTable::singleton().at(builtin);
    RELEASE_ASSERT(entry.scope == BuiltinScope::Module);

    if (entry.holder) {
        JSCell* holder = builtinCell(entry.holder, module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!holder || !holder->isObject())
            return nullptr;
        JSCell* member = m_state.builtinCells->readMember(m_instance.globalObject, asObject(holder), builtin);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (builtin != Builtins::ModuleRequireResolve || !host().resolveFunctionOf)
            return member;
        RELEASE_AND_RETURN(scope, host().resolveFunctionOf(m_instance.globalObject, ensureLoaded(module).requirer, member));
    }
    if (builtin == Builtins::ImportMeta)
        RELEASE_AND_RETURN(scope, importMeta(module));
    JSObject* requirer = moduleObject(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (builtin == Builtins::ModuleObject)
        return requirer;
    RELEASE_ASSERT(builtin == Builtins::ModuleRequire);
    JSValue function = ensureLoaded(module).requireFunction;
    RELEASE_ASSERT(function.isCell());
    return function.asCell();
}

JSString* Modules::resolve(ModuleID module, ResolvedSpecifierForm form)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isURL = form == ResolvedSpecifierForm::URL;
    if (m_table.hasFilePath(module) && isURL)
        return jsString(vm, URL::fileURLWithFileSystemPath(m_table.pathText(module)).string());
    JSString* specifier = pathString(module);
    if (data(module).kind != ModuleKind::External)
        return specifier;
    if (!isURL && data(module).isUncompiledCode()) {
        JSValue resolved = resolveDynamic(specifier, *data(module).referrerModule(), form);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return resolved.isString() ? asString(resolved) : specifier;
    }

    Identifier key;
    {
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(m_instance.globalObject, specifier, pathString(*data(module).referrerModule()), nullptr, true);
    }
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, specifier);
        if (!data(module).isUncompiledCode())
            return specifier;
        JSValue resolved = resolveDynamic(specifier, *data(module).referrerModule(), form);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return resolved.isString() ? asString(resolved) : specifier;
    }
    if (isURL && isAbsoluteFileSystemPath(key.string()))
        return jsString(vm, URL::fileURLWithFileSystemPath(key.string()).string());
    return jsString(vm, key.string());
}

const SourceOrigin& Modules::sourceOrigin(ModuleID module)
{
    if (std::optional<ModuleID> evaluator = data(module).evaluatorModule())
        return sourceOrigin(*evaluator);
    LoadedModule& loaded = ensureLoaded(module);
    if (!loaded.sourceOrigin) {
        String path = m_table.pathText(module);
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(m_table.hasFilePath(module) ? URL::fileURLWithFileSystemPath(path) : URL(path));
    }
    return *loaded.sourceOrigin;
}

void Modules::throwFailure(ModuleFailureID id, bool isForRequire)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    if (failure.isAbsentExport())
        RELEASE_AND_RETURN(scope, throwNewFailure(id, isForRequire));
    ModuleID importer = failure.importerModule();
    std::optional<ModuleFailureID> endOfLoad = m_table.loadFailure(importer);
    if (!endOfLoad || m_table.failure(*endOfLoad).importer != failure.importer || m_table.failure(*endOfLoad).specifier != failure.specifier)
        RELEASE_AND_RETURN(scope, throwNewFailure(id, isForRequire));
    if (JSValue error = ensureLoaded(importer).errorKeptByLoader) {
        throwException(m_instance.globalObject, scope, error);
        return;
    }
    throwNewFailure(id, isForRequire);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception))
        ensureLoaded(importer).errorKeptByLoader.set(*this, exception->value());
}

void Modules::throwNewFailure(ModuleFailureID id, bool isForRequire)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    String specifier = ProgramStrings::text(m_table.programTables(), failure.specifierString());
    if (failure.isAbsentExport()) {
        JSObject* error = failure.isAskedByExportStatement()
            ? createExportStatementFindsNothingError(realm, specifier, ProgramStrings::text(m_table.programTables(), failure.exportSpecifierString()))
            : createExportNotFoundError(realm, specifier, m_table.pathText(failure.importerModule()));
        uncheckedDowncast<ErrorInstance>(error)->setStackFrames(vm, { });
        throwException(realm, scope, error);
        return;
    }
    String importer = m_table.pathText(failure.importerModule());
    Identifier key;
    {
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(realm, m_state.strings->string(vm, failure.specifierString()), pathString(failure.importerModule()), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, void());
    if (!isForRequire) {
        DisallowVMEntry disallowVMEntry(vm);
        key = loader()->resolve(realm, jsString(vm, key.string()), pathString(failure.importerModule()), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, void());
    if (host().createLoadError) {
        JSValue error = host().createLoadError(realm, key.string());
        RETURN_IF_EXCEPTION(scope, void());
        if (error) {
            throwException(realm, scope, error);
            return;
        }
    }
    if (host().createModuleNotFoundError) {
        JSValue error = host().createModuleNotFoundError(realm, specifier, importer, isForRequire);
        RETURN_IF_EXCEPTION(scope, void());
        throwException(realm, scope, error);
        return;
    }
    throwException(realm, scope, createModuleMissingAtBuildError(realm, specifier, importer, isForRequire ? "MODULE_NOT_FOUND"_s : "ERR_MODULE_NOT_FOUND"_s));
}

static JSValue moduleForJob(ModuleID module)
{
    return jsNumber(module.index());
}

static void withModuleOfJob(JSGlobalObject* realm, JSValue moduleForJob, const ScopedLambda<void(Modules&, ModuleID)>&);
static void continueImportOfJob(Modules&, JSPromise*, ModuleID, JSValue referrerAsyncOrder);

static void queueImportJob(JSGlobalObject* realm, JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    constexpr uint8_t unusedPayload = 0;
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImport, unusedPayload, importPromise, moduleForJob(module), referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
}

void runCompiledModuleImport(JSGlobalObject*, JSValue importPromise, JSValue module, JSValue referrerAsyncOrder)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    withModuleOfJob(promise->realm(), module, [&](Modules& modules, ModuleID imported) {
        continueImportOfJob(modules, promise, imported, referrerAsyncOrder);
    });
}

static void continueImportOfJob(Modules& modules, JSPromise* promise, ModuleID imported, JSValue referrerAsyncOrder)
{
    modules.continueImport(promise, imported, referrerAsyncOrder.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(referrerAsyncOrder.asNumber())));
}

static constexpr InternalMicrotask jobsOfImportOfEvaluatedModule[] = {
    InternalMicrotask::ModuleLoadTopSettled,
    InternalMicrotask::DynamicImportLoadSettled,
    InternalMicrotask::DynamicImportEvaluateSettled,
    InternalMicrotask::ModuleLoadCombinedStateSettled,
    InternalMicrotask::PromiseFulfillWithoutHandlerJob,
    InternalMicrotask::ModuleLoadTopRejected,
    InternalMicrotask::ImportModuleNamespace,
};

static constexpr size_t numberOfJobsBehindEvaluation = std::size(jobsOfImportOfEvaluatedModule) - 2;

static void queueStepOfImport(JSPromise* importPromise, JSValue module, uint8_t stepsLeft, JSValue error = { })
{
    JSGlobalObject* realm = importPromise->realm();
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImportStep, stepsLeft, importPromise, module, error);
}

void runCompiledModuleImportStep(JSValue importPromise, JSValue module, JSValue error, uint8_t stepsLeft)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    if (--stepsLeft)
        queueStepOfImport(promise, module, stepsLeft, error);
    else
        withModuleOfJob(promise->realm(), module, [&](Modules& modules, ModuleID imported) {
            modules.finishImportOfEvaluatedModule(promise, imported, error);
        });
}

void runCompiledModuleImportSettled(JSGlobalObject*, JSValue importPromise, JSValue resolution, JSValue module, JSPromise::Status status)
{
    auto* promise = uncheckedDowncast<JSPromise>(importPromise);
    RELEASE_ASSERT(status != JSPromise::Status::Pending);
    withModuleOfJob(promise->realm(), module, [&](Modules& modules, ModuleID imported) {
        if (JSValue error = modules.errorOfLoadEndedByMock(imported)) [[unlikely]]
            promise->reject(promise->vm(), error);
        else if (modules.isImportedByLoader(imported))
            queueStepOfImport(promise, module, numberOfJobsBehindEvaluation - 1, status == JSPromise::Status::Rejected ? resolution : JSValue());
        else if (status == JSPromise::Status::Rejected)
            promise->reject(promise->vm(), resolution);
        else
            modules.resolveWithNamespace(promise, imported);
    });
}

std::optional<int64_t> Modules::asyncEvaluationOrder(ModuleID module) const
{
    ModuleRecord* record = recordIfExists(module);
    if (!record || !record->asyncEvaluationOrder().hasOrder())
        return std::nullopt;
    return record->asyncEvaluationOrder().order();
}

static void settleAsLoaderDoes(JSGlobalObject* realm, JSPromise* promiseOfProgram, JSPromise* loaded)
{
    VM& vm = realm->vm();
    if (loaded->status() == JSPromise::Status::Fulfilled)
        promiseOfProgram->fulfill(vm, loaded->result());
    else
        promiseOfProgram->resolve(realm, vm, loaded);
}

static JSPromise* createPromiseOfProgram(JSGlobalObject* realm, JSPromise* loaded)
{
    JSPromise* promise = JSPromise::create(realm->vm(), realm->promiseStructure());
    settleAsLoaderDoes(realm, promise, loaded);
    return promise;
}

static JSPromise* importPromiseRejectedByLoader(JSGlobalObject* realm, ThrowScope& scope)
{
    JSPromise* rejected = JSPromise::rejectedPromiseWithCaughtException(realm, scope);
    if (!rejected)
        return JSPromise::create(realm->vm(), realm->promiseStructure());
    return createPromiseOfProgram(realm, rejected);
}

JSPromise* Modules::rejectImportWithErrorOfLoad(ModuleID module, ImportCaller caller)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwException(realm, scope, errorOfLoadEndedByMock(module));
    return caller == ImportCaller::CompiledCode ? importPromiseRejectedByLoader(realm, scope) : rejectedPromiseWithPendingException(realm, scope);
}

static JSC_DECLARE_HOST_FUNCTION(importBehindFactories);
static JSC_DECLARE_HOST_FUNCTION(settleCallsBehindEarlierCall);
static JSC_DECLARE_HOST_FUNCTION(settleCallBehindOwnLoad);
static constexpr unsigned firstCallInLine = 2;
static JSC_DECLARE_HOST_FUNCTION(importByKeyBehindFactories);

static JSC_DECLARE_HOST_FUNCTION(continueImportWithModuleItself);

JSPromise* Modules::import(ModuleID module, ModuleID referrer, JSValue options)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    bool isMadeByOwnFactory = isAskedForByOwnFactory(module);
    if (m_mocks) [[unlikely]]
        noteLoadBegunByFactories(module);
    ModuleID walkedBelow = module;
    bool walkStopped = false;
    auto start = [&](ModuleID started) -> JSPromise* {
        JSPromise* promise = startImport(started, asyncEvaluationOrder(referrer), ImportCaller::CompiledCode, TextOfImport::MayComeLater, nullptr, JSValue(), started == walkedBelow ? std::optional(walkStopped) : std::nullopt);
        RETURN_IF_EXCEPTION(scope, promise);
        ModuleID file = fileOf(started);
        if (isMadeByOwnFactory || importsOriginalBesideMock(started, referrer)) [[unlikely]] {
            VM& vm = *m_instance.vm;
            JSPromise* ofModuleItself = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), continueImportWithModuleItself, ImplementationVisibility::Private);
            promise->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), ofModuleItself, jsNumber(started.index()));
            promise = ofModuleItself;
        }
        scope.release();
        tellHostOfImport(promise, data(file).mayBeMocked() ? keyWithoutForm(file).string() : String());
        return promise;
    };

    if (m_modulesWithLoadEndedByMock.get(module.index())) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(module, ImportCaller::CompiledCode));
    if (m_modulesWhoseFactoryRuns.get(module.index())) [[unlikely]]
        m_importsMadeByOwnFactory.append(module);
    else if (!m_importsMadeByOwnFactory.contains(module)) [[likely]] {
        dropErrorOfMockKeptByLoader(module);
        m_modulesOwedBesideMock.clear(module.index());
    }
    {
        SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, m_modulesWhoseFactoryRuns.get(module.index()) ? std::nullopt : std::optional(module));
        SetForScope moduleThatImportAsksFor(m_moduleThatImportAsksFor, module);
        SetForScope importThatAsksHasWaited(m_importThatAsksHasWaited, false);
        walkStopped = runMocksBelow(module);
    }
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    if (isLeftToHostAsFarAsIsKnown(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, importUncompiled(keyWithoutForm(fileOf(module)), referrer, WTF::move(*fetchParameters)));
    if (hostAnswersInPlaceOf(module, LoadByName::Import)) [[unlikely]] {
        Identifier key;
        {
            SetForScope isResolvingImportCall(m_instance.vm->aotState().isResolvingImportCall, true);
            key = loader()->resolve(realm, pathString(module), pathString(referrer), nullptr, true);
        }
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
    }
    if (!data(module).isUncompiledCode()) {
        if (const ScriptFetchParameters* attributes = fetchParameters->get()) {
            ModuleID file = fileOf(module);
            Identifier key = keyWithoutForm(file);
            if (registryEntry(key, attributes)) [[unlikely]]
                RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
            std::optional<ModuleID> form = findWithAttributes(key, attributes);
            RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
            if (form)
                module = m_table.faceOfFile(*form).value_or(*form);
            else
                noteFormAnsweredFirst(key, *attributes, file);
        }
        return start(module);
    }

    const LoadedModule* loadedBefore = loadedIfExists(module);
    bool hasAskedLoader = loadedBefore && !loadedBefore->resolvedKey.isNull();
    Identifier key = resolvedKey(module);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    if (hasAskedLoader) {
        key = loader()->resolve(realm, pathString(module), pathString(*data(module).referrerModule()), nullptr, true);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        willAnswerNameLeftToHostWith(key);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    }
    std::optional<ModuleID> found = findWithAttributes(key, fetchParameters->get(), FormWithoutAttributes::IsAnotherModule);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    if (found)
        return start(*found);
    if (m_uncompiledNamespacesNotShownYet.get(module.index())) [[unlikely]] {
        uncompiledNamespace(module, ModulePhase::Evaluation, NamespaceUse::IsShownToProgram);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, *data(module).referrerModule(), WTF::move(*fetchParameters)));
}

bool Modules::hostAnswersInPlaceOf(ModuleID module, LoadByName way)
{
    const ModuleData& replaced = data(module);
    bool isNative = replaced.kind == ModuleKind::Runtime;
    bool isCodeOfLibrary = replaced.kind == ModuleKind::RuntimeSource || (replaced.kind == ModuleKind::SourceText && replaced.isOfLibrary());
    if (!isNative && (!isCodeOfLibrary || !replaced.mayBeMocked()))
        return false;
    String name = m_table.pathText(module);
    if (!host().replacesNativeModule || !host().replacesNativeModule(m_instance.globalObject, name))
        return false;
    if (way == LoadByName::Require && (name == "bun"_s || isInRequireCache(module)))
        return false;
    return m_libraryModulesThatMockReplaces.get(module.index());
}

bool Modules::isKeptInRequireCache(ModuleID module) const
{
    if (!data(module).isOfLibrary())
        return false;
    if (!m_libraryModulesWithCheckedName.get(module.index())) {
        String name = m_table.pathText(module);
        m_libraryModulesWithCheckedName.set(module.index());
        m_libraryModulesKeptInRequireCache.set(module.index(), !name.startsWith("node:"_s) && name != "bun"_s && name != "bun:test"_s);
    }
    return m_libraryModulesKeptInRequireCache.get(module.index());
}

bool Modules::isInRequireCache(ModuleID module) const
{
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->valueInRequireCache;
}

void Modules::takeOutOfRequireCache(ModuleID module)
{
    if (isInRequireCache(module)) {
        LoadedModule& loaded = ensureLoaded(module);
        loaded.valueInRequireCache.set(*this, JSValue());
        loaded.objectMadeByRequire.set(*this, nullptr);
        forgetFirstRequirerOf(module);
        loaded.firstRequirerOutsideImage.set(*this, nullptr);
        setRequireCacheValue(module, JSValue());
        m_libraryModulesInRequireCache.removeFirst(module);
    }
    m_libraryModulesTakenFromLoader.set(module.index());
    if (m_libraryModulesWithMock.get(module.index()))
        m_libraryModulesThatMockReplaces.set(module.index());
}

Vector<ModuleID> Modules::libraryModulesInRequireCache()
{
    Vector<ModuleID> listed = m_libraryModulesInRequireCache;
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 64> toFollow;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        if (data(module).isOfLibrary() || data(module).kind != ModuleKind::SourceText)
            continue;
        if (hasBegunToLoad(module) || isRunningFilesOf(module)) {
            isSeen.quickSet(index);
            toFollow.append(module);
        }
    }
    for (size_t next = 0; next < toFollow.size(); ++next) {
        if (standsForWholeModule(toFollow[next]))
            continue;
        for (const ModuleRequestData& request : m_table.requests(toFollow[next])) {
            ModuleID target = moduleLoadedBy(request);
            if (request.isDeferred || request.isRequire || isSeen.quickSet(target.index()))
                continue;
            if (!data(target).isOfLibrary()) {
                if (data(target).kind == ModuleKind::SourceText)
                    toFollow.append(target);
                continue;
            }
            m_libraryModulesThatStatementsReached.set(target.index());
            if (isListedOnceStatementReaches(target))
                listed.append(target);
        }
    }
    for (size_t index : m_libraryModulesThatStatementsReached) {
        if (!isSeen.get(index) && isListedOnceStatementReaches(ModuleID(index)))
            listed.append(ModuleID(index));
    }
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesForRequireCache, m_table.size() + toFollow.size());
    return listed;
}

bool Modules::isListedOnceStatementReaches(ModuleID module) const
{
    return isKeptInRequireCache(module) && !isInRequireCache(module) && !m_libraryModulesTakenFromLoader.get(module.index()) && !m_table.pathText(module).startsWith("bun:"_s);
}

bool Modules::isLibraryModuleInRequireCache(ModuleID module)
{
    if (m_libraryModulesInRequireCache.contains(module))
        return true;
    if (!isListedOnceStatementReaches(module))
        return false;
    if (!m_libraryModulesThatStatementsReached.get(module.index()))
        libraryModulesInRequireCache();
    return m_libraryModulesThatStatementsReached.get(module.index());
}

void Modules::addToRequireCache(ModuleID module, JSValue exports)
{
    ensureLoaded(module).valueInRequireCache.set(*this, exports);
    setRequireCacheValue(module, exports);
    m_libraryModulesInRequireCache.append(module);
}

void Modules::setRequireCacheValue(ModuleID module, JSValue exports)
{
    if (data(module).hasRequireCacheValue())
        ProgramStatics::writeVariable(m_instance, data(module).requireCacheValue(), exports);
}

JSObject* Modules::requireCacheEntry(ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data(module).isOfLibrary() && !data(module).isEntryPoint() && !m_showsEntriesOfRequireCache) {
        throwException(m_instance.globalObject, scope, createEntryOfRequireCacheRefusedError(m_instance.globalObject, m_table.pathText(module)));
        return nullptr;
    }
    if (!data(module).isOfLibrary())
        RELEASE_AND_RETURN(scope, moduleObjectMadeByRequire(module));
    if (valueOfLibraryFileStaysWithCompiledCode(module)) [[unlikely]] {
        dataLogLn("AOT: require.cache hands out all that the library module ", m_table.pathText(module), " gives, and the build has kept it for compiled code alone: what it has proven of it no longer holds.");
        CRASH();
    }
    if (JSObject* entry = ensureLoaded(module).objectMadeByRequire)
        return entry;
    if (!isInRequireCache(module)) {
        JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, nullptr);
        addToRequireCache(module, namespaceObject);
    }
    OptionSet<ModuleObjectTrait> traits { ModuleObjectTrait::IsNoCommonJSFile };
    if (data(module).kind == ModuleKind::SourceText)
        traits.add(ModuleObjectTrait::IsESModule);
    ModuleObjects objects;
    host().createModuleObject(m_instance.globalObject, pathString(module), module, traits, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);
    LoadedModule& loaded = ensureLoaded(module);
    objects.module->putDirect(vm, vm.propertyNames->exports, loaded.valueInRequireCache.get());
    loaded.objectMadeByRequire.set(*this, objects.module);
    setRequireCacheValue(module, JSValue());
    return objects.module;
}

std::optional<ModuleID> Modules::findByName(const Identifier& key)
{
    std::optional<ModuleID> found = m_table.findByPath(key.string());
    if (found && hostAnswersInPlaceOf(*found, LoadByName::Import))
        return std::nullopt;
    if (found && !data(*found).isOfLibrary() && host().loadsFileItself && host().loadsFileItself(m_instance.globalObject, key.string()) && !hasRun(*found) && !namespaceIfCreated(*found, ModulePhase::Evaluation))
        return std::nullopt;
    return found;
}

Identifier Modules::keyWithoutForm(ModuleID file)
{
    if (data(file).kind != ModuleKind::Data)
        return moduleKey(file);
    String path = m_table.pathText(file);
    size_t mark = path.reverseFind('?');
    return mark == notFound ? moduleKey(file) : Identifier::fromString(*m_instance.vm, path.left(mark));
}

std::optional<ModuleID> Modules::findFormThatStatementLoaded(const Identifier& key, const ScriptFetchParameters& fetchParameters, std::optional<ModuleID> asked) const
{
    if (fetchParameters.type() != ScriptFetchParameters::Type::HostDefined)
        return std::nullopt;
    Vector<ModuleID, 2> forms = m_table.formsThatAttributesAskFor(key.string());
    forms.removeAllMatching([&](ModuleID form) {
        return data(form).kind != ModuleKind::Data || m_table.pathText(form).endsWith("?json"_s);
    });
    if (forms.isEmpty() || (forms.size() == 1 && asked == forms[0]))
        return std::nullopt;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (data(importer).kind != ModuleKind::SourceText || standsForWholeModule(importer))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (forms.contains(request.targetModule()) && wasImporterLoaded(importer))
                return request.targetModule();
        }
    }
    return std::nullopt;
}

void Modules::noteFormAnsweredFirst(const Identifier& key, const ScriptFetchParameters& fetchParameters, ModuleID file)
{
    ModuleMapKey keyOfLoader { key.impl(), fetchParameters.type() };
    if (data(file).kind == ModuleKind::Data && !loader()->moduleMap().contains(keyOfLoader))
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, file });
}

void Modules::giveStatementsFormsThatLoaderHas(ModuleID importer)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ModuleID, 8> forms;
    Vector<ModuleID, 8> importers { importer };
    BitVector seen;
    seen.set(importer.index());
    while (!importers.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(importers.takeLast())) {
            ModuleID target = request.targetModule();
            if (request.isRequire || request.isDeferred || seen.set(target.index()) || !hasNotBegun(target))
                continue;
            if (data(target).kind == ModuleKind::SourceText)
                importers.append(target);
            else if (data(target).kind == ModuleKind::Data && !m_table.pathText(target).endsWith("?json"_s))
                forms.append(target);
        }
    }
    for (ModuleID form : forms) {
        bool hasTaken = takeWhatFormAnsweredFirstHolds(form);
        RETURN_IF_EXCEPTION(scope, void());
        if (hasTaken)
            markEvaluated(form);
    }
}

bool Modules::takeWhatFormAnsweredFirstHolds(ModuleID form)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    String path = m_table.pathText(form);
    if (!path.contains('?') || path.endsWith("?json"_s))
        return false;
    Identifier key = keyWithoutForm(form);
    auto first = m_formsAnsweredFirst.find(ModuleMapKey { key.impl(), ScriptFetchParameters::Type::HostDefined });
    if (first == m_formsAnsweredFirst.end() || first->value.module == form)
        return false;
    ModuleID other = first->value.module;
    if (data(other).kind != ModuleKind::Data)
        return false;
    if (hasNotBegun(other)) {
        evaluateSynchronously(other, SynchronousEvaluationReason::Require);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (status(other) != ModuleStatus::Evaluated)
        return false;
    for (const ModuleExportData& moduleExport : m_table.exports(form)) {
        if (moduleExport.kind() != ModuleExportKind::Variable || !moduleExport.isReplaceable)
            continue;
        std::optional<unsigned> index = m_table.findExport(other, identifier(moduleExport.name).impl());
        JSValue value;
        if (index && m_table.exports(other)[*index].kind() == ModuleExportKind::Variable)
            value = ProgramStatics::readVariable(m_instance, m_table.exports(other)[*index].variable);
        ProgramStatics::writeVariable(m_instance, moduleExport.variable, value ? value : jsUndefined());
    }
    return true;
}

void Modules::forgetFormAnsweredFirst(ModuleID file)
{
    if (m_formsAnsweredFirst.isEmpty())
        return;
    Identifier key = keyWithoutForm(file);
    m_formsAnsweredFirst.removeIf([&](auto& entry) {
        return entry.value.name == key;
    });
}

std::optional<ModuleID> Modules::findWithAttributes(const Identifier& key, const ScriptFetchParameters* fetchParameters, FormWithoutAttributes formWithoutAttributes)
{
    if (!fetchParameters)
        return std::nullopt;
    if (std::optional<ModuleID> module = findByName(key); module && data(*module).registryID())
        return module;
    if (!host().findModuleWithAttributes)
        return std::nullopt;
    ModuleMapKey keyOfLoader { key.impl(), fetchParameters->type() };
    if (loader()->moduleMap().contains(keyOfLoader))
        return std::nullopt;
    if (auto first = m_formsAnsweredFirst.find(keyOfLoader); first != m_formsAnsweredFirst.end())
        return first->value.module;
    std::optional<ModuleID> module = host().findModuleWithAttributes(m_instance.globalObject, key.string(), *fetchParameters);
    if (module && formWithoutAttributes == FormWithoutAttributes::IsAnotherModule && m_table.pathText(*module) == key.string())
        module = std::nullopt;
    if (std::optional<ModuleID> loaded = findFormThatStatementLoaded(key, *fetchParameters, module))
        module = loaded;
    if (module)
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, *module });
    return module;
}

bool Modules::doOtherJobsWait() const
{
    MicrotaskQueue& queue = m_instance.vm->defaultMicrotaskQueue();
    if (!m_isJobOfRoundQueued)
        return !queue.isEmpty();
    return !queue.allOf([&](const QueuedTask& task) {
        return task.arguments()[0] == JSValue(m_promiseOfJobOfRound.get());
    });
}

bool Modules::hasFileOfGraphBeingLoadedBegun() const
{
    return m_innermostFilesBeingRun || m_rootWhoseFilesRanAhead || m_loadsInFlight.hasSeenFileRunWhileBeingLoaded();
}

bool Modules::isTextLeftToRead(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!isFileOfProgram(file) || m_instance.vm->synchronousModuleQueue() || isFetchedInsideCall(file))
        return false;
    if (loader()->registryEntry(moduleKey(module)) || loader()->registryEntry(moduleKey(file)))
        return false;
    if (!isCommonJSFileOrFace(module))
        return isNotLoaded(module);
    if (hasNotBegunAsFile(module) && hasNotBegunAsFile(file))
        return true;
    return !isTestRun() && !isEvaluatedByLoader(file) && !wasLoadedByImport(file);
}

void Modules::noteThatTextHasCome(ModuleID module)
{
    m_turnsInWhichTextsCame.set(fileOf(module), ++m_turnOfTexts);
}

void Modules::holdText(JSPromise* promise, ModuleID module, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, uint32_t turnOfRequest)
{
    VM& vm = *m_instance.vm;
    putCallsThatWaitApartInLine();
    uint64_t place = static_cast<uint64_t>(turnOfRequest) << 32 | m_numberOfTextsAskedFor++;
    m_textsOnTheirWay.appendAndBubble(TextOnItsWay { place, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder, m_stepsOfLoader }, [&](const TextOnItsWay& inLine) {
        return inLine.place > place;
    });
    if (host().callInLaterTurn && isTestRun()) {
        host().callInLaterTurn(m_instance.globalObject);
        return;
    }
    auto ticket = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::ImminentlyScheduled, vm, promise, { });
    vm.deferredWorkTimer->scheduleWorkSoonIfActive(ticket, [](DeferredWorkTimer::Ticket& ticket) {
        if (Modules* modules = modulesOf(ticket.target()->realm()))
            modules->deliverFirstTextOnItsWay();
    });
}

void Modules::deliverFirstTextOnItsWay()
{
    if (m_textsOnTheirWay.isEmpty())
        return;
    TextOnItsWay text = m_textsOnTheirWay.takeFirst();
    noteThatTextHasCome(text.module);
    if (text.requested) {
        text.promise->fulfill(*m_instance.vm, text.requested.get());
        return;
    }
    Vector<TextOnItsWay, 2> callsMadeWhileLoaderTookNoStep;
    while (!m_textsOnTheirWay.isEmpty() && !m_textsOnTheirWay.first().requested && m_textsOnTheirWay.first().stepsOfLoaderAtCall == text.stepsOfLoaderAtCall)
        callsMadeWhileLoaderTookNoStep.append(m_textsOnTheirWay.takeFirst());
    SetForScope stepsOfLoaderAtCall(m_stepsOfLoaderAtCallWhoseTextHasCome, text.stepsOfLoaderAtCall);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    startImportWhoseTextHasCome(text.promise.get(), text.module, text.referrerAsyncOrder);
    RETURN_IF_EXCEPTION(scope, void());
    for (TextOnItsWay& call : callsMadeWhileLoaderTookNoStep) {
        noteThatTextHasCome(call.module);
        startImportWhoseTextHasCome(call.promise.get(), call.module, call.referrerAsyncOrder);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool Modules::holdTextOfRequest(JSPromise* fetched, ModuleRecord* requested, ModuleID requester)
{
    ModuleID file = fileOf(requested->module());
    bool canProgramTellWhenTextsCome = !m_textsOnTheirWay.isEmpty() || m_canJobsOfFileTellWhenTextsCome || data(requester).isLoadedByLoader();
    if (!canProgramTellWhenTextsCome || !isFileOfProgram(file) || m_instance.vm->synchronousModuleQueue() || isFetchedInsideCall(file)) {
        noteThatTextHasCome(file);
        return false;
    }
    holdText(fetched, requested->module(), requested, std::nullopt, turnInWhichTextCame(requester));
    return true;
}

JSPromise* Modules::startImportInLaterTurn(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    JSPromise* importPromise = JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure());
    m_modulesOfQueuedImports.set(module.index());
    std::optional<ModuleID> fileThatCalls = m_innermostFilesBeingRun ? fileOrFaceEvaluatedIn(*m_innermostFilesBeingRun) : std::nullopt;
    holdText(importPromise, module, nullptr, referrerAsyncOrder, fileThatCalls ? turnInWhichTextCame(*fileThatCalls) : m_turnOfTexts);
    return importPromise;
}

void Modules::startImportWhoseTextHasCome(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_modulesOfQueuedImports.clear(module.index());
    JSPromise* loaded = startImport(module, referrerAsyncOrder, ImportCaller::Host, TextOfImport::HasCome);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    settleAsLoaderDoes(m_instance.globalObject, importPromise, loaded);
}

JSPromise* Modules::startImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller, TextOfImport text, JSPromise* promiseOfCaller, JSValue stateOfWait, std::optional<bool> answerOfWalkAlreadyMade)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto promiseForCaller = [&] {
        return promiseOfCaller ? promiseOfCaller : JSPromise::create(*m_instance.vm, realm->promiseStructure());
    };
    runDataFileAgainForImport(module, caller == ImportCaller::Host ? ImportAnsweredBy::Loader : ImportAnsweredBy::Build);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (isCommonJSFileOrFace(module)) {
        if (ModuleID file = fileOf(module); data(file).runsAgainAfterThrow() && !isAwaitedByImport(file)) [[unlikely]]
            ensureLoaded(file).isAwaitedByCallThatComesInLaterJob = status(file) == ModuleStatus::Evaluating || !m_instance.vm->synchronousModuleQueue();
    }

    std::optional<SetForScope<ModuleID>> moduleThatImportAsksFor;
    std::optional<SetForScope<bool>> importThatAsksHasWaited;
    moduleThatImportAsksFor.emplace(m_moduleThatImportAsksFor, module);
    importThatAsksHasWaited.emplace(m_importThatAsksHasWaited, !!stateOfWait);
    bool walkStopped = answerOfWalkAlreadyMade ? *answerOfWalkAlreadyMade : runMocksBelow(module);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (m_innermostFilesBeingRun) [[unlikely]] {
        handGraphsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (m_innermostFilesBeingRun->isAheadOfStepsOfLoader) {
            handSynchronousLoadToLoader(*m_innermostFilesBeingRun);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        }
    }
    FactoryAskedForBy askedBy = text == TextOfImport::MayComeLater || stateOfWait ? FactoryAskedForBy::ImportCall : FactoryAskedForBy::ImportCallBehindItsText;
    JSPromise* factories = factoryThatWaitsBelow(module, askedBy, stateOfWait);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    while (!factories && walkStopped) [[unlikely]] {
        walkStopped = runMocksBelow(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories = factoryThatWaitsBelow(module, askedBy, stateOfWait);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    if (factories) [[unlikely]] {
        VM& vm = *m_instance.vm;
        JSPromise* promise = promiseForCaller();
        JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importBehindFactories, ImplementationVisibility::Private);
        MarkedArgumentBuffer call;
        call.append(jsNumber(module.index()));
        call.append(referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
        call.append(jsBoolean(caller == ImportCaller::Host));
        call.append(jsNumber(static_cast<uint32_t>(text)));
        call.append(stateOfWait);
        JSArray* heldCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, heldCall);
        return promise;
    }
    importThatAsksHasWaited.reset();
    moduleThatImportAsksFor.reset();
    if (text == TextOfImport::MayComeLater && (!m_textsOnTheirWay.isEmpty() || hasFileOfGraphBeingLoadedBegun() || data(module).isLoadedByLoader()) && isTextLeftToRead(module)) [[unlikely]]
        return startImportInLaterTurn(module, referrerAsyncOrder);
    if (text == TextOfImport::MayComeLater && !hasTextAtHand() && textOfImportComesInLaterTurn(module, AskedBy::Import)) [[unlikely]] {
        JSPromise* promise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
        if (caller == ImportCaller::Host)
            promise->markAsHandled();
        holdTextOfCall(promise, module, nullptr, referrerAsyncOrder, caller);
        return promise;
    }
    bool isFile = isImportedByLoader(module);
    bool isBuiltinModule = !isFile && isBuiltinModuleOfLoader(module);
    noteRequestByLoader(module);
    noteLoadOfGraph(module);
    noteCallThatComesToModuleItself(module);
    if (isFile || isBuiltinModule) {
        VM& vm = *m_instance.vm;
        bool isBuiltinModuleThatLoaderHas = isBuiltinModule && !isBuiltinModuleNewToLoader(module);
        bool isCalledByProgram = caller == ImportCaller::CompiledCode;
        bool hasBegun = status(module) >= ModuleStatus::Evaluating || status(fileOf(module)) >= ModuleStatus::Evaluating;
        bool isNoPartOfLoad = isCalledByProgram && !(isCommonJSFileOrFace(module) && (hasBegun || data(fileOf(module)).mayBeImportedWhileItRuns()));
        SetForScope noSynchronousLoad(vm.m_synchronousModuleQueueSetAside, isNoPartOfLoad ? vm.m_synchronousModuleQueue : vm.m_synchronousModuleQueueSetAside);
        if (isCalledByProgram && (!isBuiltinModule || isBuiltinModuleThatLoaderHas) && loaderOnlyHandsOverNamespace(module)) {
            JSPromise* promise = promiseForCaller();
            if (promise->isThenFastAndNonObservable()) [[likely]] {
                queueStepOfImport(promise, moduleForJob(module), std::size(jobsOfImportOfEvaluatedModule));
                return promise;
            }
        }
        bool isCalledUnderRequire = vm.synchronousModuleQueue() && isCommonJSFileOrFace(module) && isEvaluatedByLoader(fileOf(module));
        if (isCalledUnderRequire || (asyncLoadMustWaitFor(module) && !loader()->registryEntry(moduleKey(module)))) [[unlikely]] {
            JSPromise* promise = promiseForCaller();
            m_deferredImports.append(DeferredImport { fileOf(module), Strong<JSPromise>(vm, promise) });
            return promise;
        }
        if (isBuiltinModuleThatLoaderHas || (data(module).kind == ModuleKind::SourceText && !isNotLoaded(module))) {
            ModuleRecord* record = registerWithLoader(module, RecordHolder::Loader);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            if (ModuleRegistryEntry* entry = loader()->registryEntry(record->moduleKey()); entry && entry->record() == record)
                entry->markLoaded();
        }
        LoadInFlight* earlierLoad = isCalledByProgram && isFile && !m_innermostFilesBeingRun && m_textsOnTheirWay.isEmpty() ? earlierLoadToSettleFirst(module) : nullptr;
        JSPromise* loaded = askLoaderForImport(module, referrerAsyncOrder, isCalledByProgram ? nullptr : promiseOfCaller);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (!isCalledByProgram)
            return loaded;
        if (earlierLoad) [[unlikely]] {
            JSPromise* promise = promiseForCaller();
            JSArray* line = earlierLoad->callsThatSettleBehind.get();
            if (!line) {
                MarkedArgumentBuffer head;
                head.append(jsBoolean(false));
                head.append(jsNumber(firstCallInLine));
                line = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), head);
                RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
                earlierLoad->callsThatSettleBehind.set(vm, line);
                JSFunction* behindEarlierCall = JSFunction::create(vm, realm, 2, String(), settleCallsBehindEarlierCall, ImplementationVisibility::Private);
                earlierLoad->promise->performPromiseThenWithContext(vm, realm, behindEarlierCall, behindEarlierCall, jsUndefined(), line);
            }
            MarkedArgumentBuffer parts;
            parts.append(promise);
            parts.append(loaded);
            parts.append(jsBoolean(false));
            parts.append(line);
            JSArray* call = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), parts);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            line->push(realm, call);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            JSFunction* behindOwnLoad = JSFunction::create(vm, realm, 2, String(), settleCallBehindOwnLoad, ImplementationVisibility::Private);
            loaded->performPromiseThenWithContext(vm, realm, behindOwnLoad, behindOwnLoad, jsUndefined(), call);
            return promise;
        }
        if (promiseOfCaller) {
            scope.release();
            settleAsLoaderDoes(realm, promiseOfCaller, loaded);
            return promiseOfCaller;
        }
        RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
    }

    if (data(module).reachesUncompiledCode() && status(module) == ModuleStatus::Unlinked) {
        ModuleRecord* record = registerWithLoader(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        SetForScope rootThatCompiledCodeLoads(m_rootThatCompiledCodeLoads, std::optional(module));
        JSPromise* promise = loader()->requestImportModuleForKey(realm, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        didStartLoad(promise, module);
        return promise;
    }

    JSPromise* importPromise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
    m_modulesOfQueuedImports.set(module.index());
    queueImportJob(realm, importPromise, module, referrerAsyncOrder);
    return importPromise;
}

bool Modules::leadsToModuleLeftToRun(const ModuleRequestData& request) const
{
    return !request.isHoisted && !request.isRequire && status(request.targetModule()) < ModuleStatus::Evaluating;
}

bool Modules::leadsTo(ModuleID from, ModuleID to)
{
    if (from == to)
        return true;
    const uint32_t isSeen = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    placesSeen[from.index()] = isSeen;
    Vector<ModuleID, 16> modules { from };
    while (!modules.isEmpty()) {
        for (const ModuleRequestData& request : m_table.requests(modules.takeLast())) {
            if (!leadsToModuleLeftToRun(request) || std::exchange(placesSeen[request.target()], isSeen) == isSeen)
                continue;
            if (request.targetModule() == to)
                return true;
            modules.append(request.targetModule());
        }
    }
    return false;
}

bool Modules::asksForLastTextBelow(const LoadInFlight& load, ModuleID module)
{
    const uint32_t isAskedForByLoad = takeMarkOfSearch();
    const uint32_t isAskedForByCall = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    placesSeen[load.root.index()] = isAskedForByLoad;
    placesSeen[module.index()] = isAskedForByCall;
    Vector<ModuleID, 32> inOrder { load.root, module };
    for (size_t next = 0; next < inOrder.size(); ++next) {
        uint32_t asker = placesSeen[inOrder[next].index()];
        for (const ModuleRequestData& request : m_table.requests(inOrder[next])) {
            if (!leadsToModuleLeftToRun(request))
                continue;
            uint32_t& seen = placesSeen[request.target()];
            if (!next && request.targetModule() == module)
                seen = isAskedForByLoad;
            if (seen == isAskedForByLoad || seen == isAskedForByCall)
                continue;
            seen = asker;
            inOrder.append(request.targetModule());
        }
    }
    return placesSeen[inOrder.last().index()] == isAskedForByLoad && leadsTo(module, inOrder.last());
}

Modules::LoadInFlight* Modules::loadBeingLoadedThatHolds(ModuleID module) const
{
    uint64_t holder = m_loadsThatHoldModules.get(module);
    LoadInFlight* load = holder ? m_loadsInFlight.withNumber(holder - 1) : nullptr;
    return load && m_loadsInFlight.isBeingLoaded(*load) ? load : nullptr;
}

void Modules::goThroughLoadsOfEarlierJobs()
{
    for (uint64_t end = m_loadsInFlight.numberBehindLast(); m_numberOfNextLoadToGoThrough < end; ++m_numberOfNextLoadToGoThrough) {
        LoadInFlight* load = m_loadsInFlight.withNumber(m_numberOfNextLoadToGoThrough);
        if (!load || !m_loadsInFlight.isBeingLoaded(*load) || data(load->root).reachesUncompiledCode())
            continue;
        if (load->stepsOfLoaderAtCall == m_stepsOfLoader)
            return;
        Vector<ModuleID, 16> modules;
        auto take = [&](ModuleID module) {
            if (loadBeingLoadedThatHolds(module))
                return;
            m_loadsThatHoldModules.set(module, load->number + 1);
            modules.append(module);
        };
        if (status(load->root) < ModuleStatus::Evaluating)
            take(load->root);
        while (!modules.isEmpty()) {
            for (const ModuleRequestData& request : m_table.requests(modules.takeLast())) {
                if (leadsToModuleLeftToRun(request))
                    take(request.targetModule());
            }
        }
    }
}

bool Modules::runsFirstBelow(ModuleID root, ModuleID module)
{
    const uint32_t isOnWay = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    for (ModuleID next = root; next != module;) {
        placesSeen[next.index()] = isOnWay;
        std::span<const ModuleRequestData> requests = m_table.requests(next);
        auto down = std::ranges::find_if(requests, [&](const ModuleRequestData& request) {
            return leadsToModuleLeftToRun(request) && placesSeen[request.target()] != isOnWay;
        });
        if (down == requests.end())
            return false;
        next = down->targetModule();
    }
    return true;
}

Vector<ModuleID, 2> Modules::modulesThatCloseCyclesBelow(ModuleID root)
{
    Vector<ModuleID, 2> closeCycles;
    const uint32_t isBeingGoneThrough = takeMarkOfSearch();
    const uint32_t isGoneThrough = takeMarkOfSearch();
    std::span<uint32_t> placesSeen = m_placesSeenByPlans.mutableSpan();
    struct Visit {
        ModuleID module;
        uint32_t nextRequest;
    };
    Vector<Visit, 16> visits { Visit { root, 0 } };
    placesSeen[root.index()] = isBeingGoneThrough;
    while (!visits.isEmpty()) {
        Visit& visit = visits.last();
        std::span<const ModuleRequestData> requests = m_table.requests(visit.module);
        if (visit.nextRequest == requests.size()) {
            placesSeen[visit.module.index()] = isGoneThrough;
            visits.removeLast();
            continue;
        }
        const ModuleRequestData& request = requests[visit.nextRequest++];
        if (!leadsToModuleLeftToRun(request))
            continue;
        uint32_t seen = placesSeen[request.target()];
        if (seen == isBeingGoneThrough) {
            if (!closeCycles.contains(request.targetModule()))
                closeCycles.append(request.targetModule());
        } else if (seen != isGoneThrough) {
            placesSeen[request.target()] = isBeingGoneThrough;
            visits.append(Visit { request.targetModule(), 0 });
        }
    }
    return closeCycles;
}

Modules::LoadInFlight* Modules::earlierLoadToRunFirst(const LoadInFlight& load)
{
    goThroughLoadsOfEarlierJobs();
    if (m_loadsThatHoldModules.isEmpty() || status(load.root) >= ModuleStatus::Evaluating)
        return nullptr;
    auto earlierLoadThatHolds = [&](ModuleID module) -> LoadInFlight* {
        LoadInFlight* earlier = loadBeingLoadedThatHolds(module);
        return earlier && earlier->number < load.number && earlier->stepsOfLoaderAtCall != load.stepsOfLoaderAtCall ? earlier : nullptr;
    };
    if (LoadInFlight* earlier = earlierLoadThatHolds(load.root); earlier && !runsFirstBelow(earlier->root, load.root))
        return earlier;
    Vector<LoadInFlight*, 2> asked;
    for (ModuleID module : modulesThatCloseCyclesBelow(load.root)) {
        LoadInFlight* earlier = earlierLoadThatHolds(module);
        if (!earlier || asked.contains(earlier))
            continue;
        if (asksForLastTextBelow(*earlier, load.root))
            return earlier;
        asked.append(earlier);
    }
    return nullptr;
}

std::optional<ModuleID> Modules::rootOfRunningFileThatHoldsCycleBelow(ModuleID required)
{
    const FilesBeingRun& files = *m_innermostFilesBeingRun;
    if (files.fileOrFace == files.root || !data(files.root).isSynchronous())
        return std::nullopt;
    bool holdsCycle = modulesThatCloseCyclesBelow(required).containsIf([&](ModuleID module) {
        return leadsTo(files.root, module);
    });
    return holdsCycle ? std::optional(files.root) : std::nullopt;
}

bool ModuleRecord::holdGraphBehindEarlierCalls(JSCell* payload, bool isDeferred)
{
    return modules().holdGraphBehindEarlierCalls(*this, payload, isDeferred);
}

bool Modules::holdGraphBehindEarlierCalls(ModuleRecord& root, JSCell* payload, bool isDeferred)
{
    if (m_loadsInFlight.numberBeingLoaded() < 2) [[likely]]
        return false;
    LoadInFlight* load = nullptr;
    m_loadsInFlight.forEachOfRoot(root.module(), [&](LoadInFlight& ofRoot) {
        if (!load && m_loadsInFlight.isBeingLoaded(ofRoot))
            load = &ofRoot;
    });
    LoadInFlight* earlier = load ? earlierLoadToRunFirst(*load) : nullptr;
    if (!earlier)
        return false;
    VM& vm = *m_instance.vm;
    size_t place = m_graphsHeldBehindEarlierCalls.size();
    while (place && m_graphsHeldBehindEarlierCalls[place - 1].numberOfLoad > load->number)
        --place;
    m_graphsHeldBehindEarlierCalls.insert(place, GraphHeldBehindEarlierCall { Strong<JSCell>(vm, &root), Strong<JSCell>(vm, payload), isDeferred, load->number, earlier->number });
    countEvent(vm, RuntimeCounterID::GraphsHeldBehindEarlierCalls);
    return true;
}

void Modules::letGoGraphsHeldBehindEarlierCalls()
{
    if (m_graphsHeldBehindEarlierCalls.isEmpty()) [[likely]]
        return;
    Vector<GraphHeldBehindEarlierCall, 2> letGo;
    m_graphsHeldBehindEarlierCalls.removeAllMatching([&](GraphHeldBehindEarlierCall& held) {
        LoadInFlight* earlier = m_loadsInFlight.withNumber(held.numberOfEarlierLoad);
        if (earlier && m_loadsInFlight.isBeingLoaded(*earlier))
            return false;
        letGo.append(WTF::move(held));
        return true;
    });
    if (letGo.isEmpty())
        return;
    VM& vm = *m_instance.vm;
    MicrotaskQueue& queue = vm.defaultMicrotaskQueue();
    Ref<MicrotaskQueue> jobsThatWait = MicrotaskQueue::create(vm);
    queue.moveTasksBehind(0, jobsThatWait.get());
    for (GraphHeldBehindEarlierCall& held : letGo) {
        InternalMicrotask job = held.isDeferred ? InternalMicrotask::DynamicImportDeferLoadSettled : InternalMicrotask::DynamicImportLoadSettled;
        m_instance.globalObject->queueMicrotask(vm, job, static_cast<uint8_t>(JSPromise::Status::Fulfilled), held.root.get(), jsUndefined(), held.payload.get());
    }
    jobsThatWait->moveTasksBehind(0, queue);
    countEvent(vm, RuntimeCounterID::GraphsLetGoBehindEarlierCalls, letGo.size());
}

Modules::LoadInFlight* Modules::earlierLoadToSettleFirst(ModuleID module)
{
    if (m_loadsInFlight.isEmpty() || status(module) >= ModuleStatus::Evaluating)
        return nullptr;
    goThroughLoadsOfEarlierJobs();
    LoadInFlight* earlier = m_loadsThatHoldModules.isEmpty() ? nullptr : loadBeingLoadedThatHolds(module);
    return earlier && (earlier->root == module || (data(earlier->root).isSynchronous() && asksForLastTextBelow(*earlier, module))) ? earlier : nullptr;
}

void Modules::resumeDeferredImports(ModuleID file, uint8_t jobsUntilLoaded)
{
    m_deferredImports.removeAllMatching([&](DeferredImport& import) {
        if (import.file != file)
            return false;
        JSValue error;
        for (ModuleID evaluated : { file, m_table.faceOfFile(file).value_or(file) }) {
            if (status(evaluated) == ModuleStatus::Failed)
                error = recordIfExists(evaluated)->evaluationError();
        }
        uint8_t skippedJobs = error ? 2 : 0;
        queueStepOfImport(import.promise.get(), jsNumber(file.index()), jobsUntilLoaded + std::size(jobsOfImportOfEvaluatedModule) - 1 - skippedJobs, error);
        return true;
    });
}

JSPromise* Modules::askLoaderForImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, JSPromise* resultPromise)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (isLeftToHost(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, loader()->requestImportModuleForKey(m_instance.globalObject, keyWithoutForm(fileOf(module)), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), nullptr, resultPromise));
    RequestToHost requestUnderWay = std::exchange(m_isAskingHost, RequestToHost(true));
    ModuleRecord* record = nullptr;
    {
        SetForScope stackPointerOfImport(m_instance.vm->aotState().stackPointerOfImport, currentStackPointer());
        ModuleRecord* made = recordIfExists(module);
        bool isMadeInline = made && loader()->isMakingRecordInline(made->moduleKey());
        record = isCommonJSFileOrFace(module) ? recordOfCommonJSFile(module, isMadeInline ? LoadOfFile::IsAskedFor : LoadOfFile::IsRootOfImport) : exposeRecord(module, RecordHolder::Loader);
    }
    JSPromise* loaded = scope.exception() ? nullptr : loader()->requestImportModuleForKey(m_instance.globalObject, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), record, resultPromise);
    m_isAskingHost = requestUnderWay;
    RETURN_IF_EXCEPTION(scope, nullptr);
    didStartLoad(loaded, module);
    return loaded;
}

bool Modules::loaderOnlyHandsOverNamespace(ModuleID module) const
{
    if (status(module) != ModuleStatus::Evaluated || isCommonJSFileOrFace(module) || m_instance.vm->synchronousModuleQueue() || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead)
        return false;
    ModuleRecord* record = data(module).isSynchronous() ? nullptr : recordIfExists(module);
    if (!record)
        return true;
    CyclicModuleRecord* root = record->cycleRoot();
    return root && root->status() == CyclicModuleRecord::Status::Evaluated && !root->evaluationError();
}

void Modules::finishImportOfEvaluatedModule(JSPromise* importPromise, ModuleID module, JSValue error)
{
    VM& vm = *m_instance.vm;
    if (importPromise == m_promiseOfJobOfRound.get()) [[unlikely]]
        return runJobOfRound(module);
    if (importPromise == m_promiseOfJobThatMakesCallsAgain.get()) [[unlikely]]
        return runJobThatMakesCallsAgain(module);
    JSPromise* loaded = JSPromise::create(vm, m_instance.globalObject->promiseStructure());
    loaded->markAsHandled();
    if (error) [[unlikely]]
        loaded->reject(vm, error);
    else
        resolveWithNamespace(loaded, module);
    loaded->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, importPromise, jsUndefined());
}

std::unique_ptr<ModuleLinkWalk> Modules::takeLinkWalk(ModuleID root)
{
    std::unique_ptr<ModuleLinkWalk> walk = takeKeptLinkWalk(root);
    if (!walk) {
        countEvent(*m_instance.vm, RuntimeCounterID::LinksBelowRoots);
        walk = makeUnique<ModuleLinkWalk>(*this, root);
    }
    return walk;
}

std::unique_ptr<ModuleLinkWalk> Modules::takeKeptLinkWalk(ModuleID root)
{
    size_t index = m_linkWalksOfLoads.findIf([&](const std::unique_ptr<ModuleLinkWalk>& kept) { return kept->root() == root; });
    if (index == notFound)
        return nullptr;
    std::unique_ptr<ModuleLinkWalk> walk = WTF::move(m_linkWalksOfLoads[index]);
    m_linkWalksOfLoads.removeAt(index);
    if (!walk->bringUpToDate(*this))
        return nullptr;
    return walk;
}

void Modules::keepLinkWalk(std::unique_ptr<ModuleLinkWalk>&& walk)
{
    m_linkWalksOfLoads.append(WTF::move(walk));
}

void Modules::continueImport(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (isLeftToHost(module)) [[unlikely]] {
        JSPromise* loaded = askLoaderForImport(module, referrerAsyncOrder);
        if (scope.exception()) [[unlikely]] {
            importPromise->rejectWithCaughtException(vm, scope);
            return;
        }
        scope.release();
        importPromise->resolve(m_instance.globalObject, vm, loaded);
        return;
    }

    if (status(module) == ModuleStatus::Evaluating) {
        vm.whenIdle([realm = m_instance.globalObject, importPromise = Strong<JSPromise>(vm, importPromise), module, referrerAsyncOrder] {
            queueImportJob(realm, importPromise.get(), module, referrerAsyncOrder);
        });
        return;
    }

    m_modulesOfQueuedImports.clear(module.index());
    std::unique_ptr<ModuleLinkWalk> linkWalk = takeLinkWalk(module);
    bool didRunModule = loadRuntimeModules(*linkWalk);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    bool didRunFile = !didRunModule && evaluateImportedCommonJSFiles(module);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    if (didRunModule || didRunFile) {
        keepLinkWalk(WTF::move(linkWalk));
        queueImportJob(m_instance.globalObject, importPromise, module, referrerAsyncOrder);
        return;
    }

    JSPromise* evaluation = evaluate(module, referrerAsyncOrder, importPromise, linkWalk.get());
    RETURN_IF_EXCEPTION(scope, void());
    switch (evaluation->status()) {
    case JSPromise::Status::Pending:
        evaluation->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::CompiledModuleImportSettled, importPromise, moduleForJob(module));
        return;
    case JSPromise::Status::Fulfilled:
        RELEASE_AND_RETURN(scope, resolveWithNamespace(importPromise, module));
    case JSPromise::Status::Rejected:
        evaluation->markAsHandled();
        importPromise->reject(vm, evaluation->result());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Modules::resolveWithNamespace(JSPromise* importPromise, ModuleID module)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    importPromise->resolve(m_instance.globalObject, vm, namespaceObject);
}

JSPromise* Modules::importDynamic(JSValue specifier, JSValue options, ModuleID referrer)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);

    JSString* text = specifier.toString(realm);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    willLoadByName(realm, text, LoadByName::Import);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    Identifier key;
    {
        SetForScope isResolvingImportCall(m_instance.vm->aotState().isResolvingImportCall, true);
        key = loader()->resolve(realm, text, pathString(referrer), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    std::optional<ModuleID> found = *fetchParameters ? findWithAttributes(key, fetchParameters->get()) : findByName(key);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    if (found) {
        runDataFileAgainForImport(*found, ImportAnsweredBy::Loader);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        willAnswerWithModule(realm, *found, LoadByName::Import);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        RELEASE_AND_RETURN(scope, import(m_table.faceOfFile(*found).value_or(*found), referrer));
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
}

JSPromise* Modules::importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&& fetchParameters, JSValue stateOfWait)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    VM& vm = *m_instance.vm;
    auto askLoader = [&] {
        SetForScope frameOfImportThatAsksLoader(vm.aotState().frameOfImportThatAsksLoader, vm.topCallFrame);
        return loader()->requestImportModuleForKey(realm, key, moduleKey(referrer), WTF::move(fetchParameters), nullptr, false, asyncEvaluationOrder(referrer).value_or(-1));
    };
    std::optional<ModuleID> found = fetchParameters ? std::nullopt : m_table.findByPath(key.string());
    if (found && errorOfLoadEndedByMock(*found)) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(*found, ImportCaller::CompiledCode));
    if (found) {
        JSPromise* factories = factoryThatWaitsBelow(*found, FactoryAskedForBy::ImportCall, stateOfWait);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (factories) [[unlikely]] {
            JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importByKeyBehindFactories, ImplementationVisibility::Private);
            MarkedArgumentBuffer call;
            call.append(jsString(vm, key.string()));
            call.append(jsNumber(referrer.index()));
            call.append(stateOfWait);
            JSArray* heldCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, heldCall);
            return promise;
        }
    }
    SetForScope askerLoadsCompiledFilesAlone(vm.aotState().askerLoadsCompiledFilesAlone, loadsCompiledFilesAlone(referrer));
    JSPromise* loaded = askLoader();
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    tellHostOfImport(loaded, key.string());
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
}

bool Modules::loadsCompiledFilesAlone(ModuleID module) const
{
    return std::ranges::binary_search(m_instance.tables().modulesThatLoadCompiledFilesAlone(), module.index());
}

static bool isText(StringView path)
{
    return path.startsWith("data:"_s);
}

bool Modules::isTextNamedByProgram(StringView url) const
{
    return isText(url) && std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](const UncompiledModuleData& entry) {
        return url == m_table.pathText(ModuleID(entry.module));
    });
}

const Identifier& Modules::resolvedKey(ModuleID external)
{
    ASSERT(data(external).isUncompiledCode());
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_runsUncompiledCode = true;
    if (!m_admitsCodeOutsideImage && !isText(m_table.pathText(external)))
        m_admitsCodeOutsideImage = true;
    LoadedModule& loaded = ensureLoaded(external);
    if (!loaded.resolvedKey.isNull())
        return loaded.resolvedKey;
    Identifier key = loader()->resolve(m_instance.globalObject, pathString(external), pathString(*data(external).referrerModule()), nullptr, true);
    RETURN_IF_EXCEPTION(scope, loaded.resolvedKey);
    willAnswerNameLeftToHostWith(key);
    RETURN_IF_EXCEPTION(scope, loaded.resolvedKey);
    loaded.resolvedKey = WTF::move(key);
    loaded.keyOfLatestLoad = loaded.resolvedKey;
    return loaded.resolvedKey;
}

void Modules::willAnswerNameLeftToHostWith(const Identifier& key)
{
    std::optional<ModuleID> file = key.isSymbol() ? std::nullopt : m_table.findByPath(key.string());
    if (!file)
        return;
    RequestToHostScope requestForWhatLiesOutsideImage(*this);
    willAnswerWithModule(m_instance.globalObject, *file, LoadByName::Import);
}

void Modules::forgetWhatNamesResolvedTo()
{
    m_filesAboveNoEvictedFile.clearAll();
    for (const UncompiledModuleData& external : m_table.modulesOfUncompiledCode()) {
        if (LoadedModule* loaded = loadedIfExists(ModuleID(external.module)))
            loaded->resolvedKey = Identifier();
    }
}

void Modules::admitCodeNamedBy(JSValue specifier)
{
    if (m_admitsCodeOutsideImage || !specifier.isString())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto text = asString(specifier)->view(m_instance.globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    m_admitsCodeOutsideImage = std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](const UncompiledModuleData& entry) {
        String name = m_table.pathText(ModuleID(entry.module));
        return !isText(name) && (text == name || (text->startsWith(name) && text[name.length()] == '/'));
    });
}

ModuleRecord* Modules::exposeRecord(ModuleID module, RecordHolder holder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if ((status(module) == ModuleStatus::Unlinked || data(module).capturesExports()) && !m_isWalkingRequests && !m_levelsOfLoads.contains(module)) {
        std::unique_ptr<ModuleLinkWalk> linkWalk = takeLinkWalk(module);
        loadRuntimeModules(*linkWalk, NativeModuleLoading::Deferred);
        RETURN_IF_EXCEPTION(scope, nullptr);
        keepLinkWalk(WTF::move(linkWalk));
    }
    ModuleRecord* record = ensureRecord(module);
    if (holder == RecordHolder::Anyone)
        record->retain();
    else if (!record->isRetained()) {
        record->setIsHeldByLoader(true);
        noteThatGraphBegins(module);
        m_modulesWhoseRecordsLoaderHolds.append(module);
    }
    if (record->status() != CyclicModuleRecord::Status::New || !record->requestedModules().isEmpty())
        return record;

    if (isLoadedWithRequests(module)) {
        if (m_isWalkingRequests)
            record->setIsRequestedByWalkedModule();
        return record;
    }
    RELEASE_AND_RETURN(scope, appendRequestsOfUncompiledCode(module, record));
}

bool Modules::canTellHowGraphIsLoaded(ModuleID root)
{
    VM& vm = *m_instance.vm;
    if (data(root).isLoadedByLoader() || m_fileWhoseJobsWaitForLoader || m_rootWhoseFilesRanAhead == root || !m_textsOnTheirWay.isEmpty())
        return true;
    if (isEntryPointOfRealm(root))
        return false;
    if (vm.synchronousModuleQueue() || numberOfGraphsBeingLoaded() > 1)
        return true;
    for (const LoadInFlight& load : m_loadsInFlight) {
        if (load.isLoaded && status(load.root) != ModuleStatus::Evaluating && status(load.root) != ModuleStatus::EvaluatingAsync)
            return true;
    }
    return !vm.defaultMicrotaskQueue().allOf([&](const QueuedTask& task) {
        if (task.job() >= InternalMicrotask::ModuleRegistryFetchSettled && task.job() <= InternalMicrotask::ImportModuleNamespace)
            return true;
        if (m_isJobOfRoundQueued && task.arguments()[0] == JSValue(m_promiseOfJobOfRound.get()))
            return true;
        if (task.job() != InternalMicrotask::PromiseResolveThenableJobFast)
            return false;
        auto* promise = dynamicDowncast<JSPromise>(task.arguments()[0]);
        const LoadInFlight* load = promise ? m_loadsInFlight.find(promise) : nullptr;
        return load && !load->isLoaded;
    });
}

bool Modules::isBuiltinModuleOfLoader(ModuleID module) const
{
    const ModuleData& moduleData = data(module);
    return moduleData.exportRule == SyntheticExportRule::RuntimeModule && moduleData.registryID();
}

bool Modules::isBuiltinModuleNewToLoader(ModuleID module) const
{
    return !isSettledForLinks(module) && !m_filesLoadedWithoutLoader.get(module.index()) && isBuiltinModuleOfLoader(module);
}

RefPtr<ScriptFetchParameters> Modules::fetchParametersOf(ModuleID external) const
{
    String type = m_table.typeAttribute(external);
    std::optional<ScriptFetchParameters::Type> kind = ScriptFetchParameters::parseType(type);
    if (!kind)
        return nullptr;
    if (*kind == ScriptFetchParameters::Type::HostDefined)
        return ScriptFetchParameters::create(type);
    return ScriptFetchParameters::create(*kind);
}

static ModuleMapKey placeInLoader(const Identifier& key, const ScriptFetchParameters* fetchParameters)
{
    return { key.impl(), fetchParameters ? fetchParameters->type() : ScriptFetchParameters::Type::JavaScript };
}

ModuleRegistryEntry* Modules::registryEntry(const Identifier& key, const ScriptFetchParameters* fetchParameters)
{
    if (!fetchParameters)
        return loader()->registryEntry(key);
    return loader()->moduleMap().get(placeInLoader(key, fetchParameters)).get();
}

void Modules::appendRequestsForLoader(ModuleID module, ModuleRecord* record, ModuleGraphLoadingState& load)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    noteStepOfLoader();
    if (!isLoadedWithRequests(module) || !record->requestedModules().isEmpty())
        return;
    if (isMocked(module) || m_modulesWithMockToRun.get(module.index()))
        return;
    auto appendNextOnWay = [&] {
        std::unique_ptr<LevelOfLoad> level = m_levelsOfLoads.take(module);
        if (!level)
            return false;
        if (level->next && level->plan && (!m_levelsOfLoads.contains(*level->next) || !isStillToBeLoaded(*level->next)))
            layWayOfLoadAgain(*level);
        if (level->next && level->plan && !m_instance.vm->synchronousModuleQueue())
            countStepsOfLevel(*level->plan, level->depth + 1);
        if (m_showsWaysBetweenModuleObjects && level->next && level->plan && level->depth + 1 < level->plan->levels.size()) {
            for (const PlanOfLoad::Found& found : level->plan->level(level->depth + 1))
                noteRequestByLoader(found.module);
        }
        auto append = [&](ModuleID target) {
            Identifier key = keyOfRequest(target);
            record->appendRequestedModule(key, nullptr, ModulePhase::Evaluation);
            record->didRequestCompiledModule(key, target);
        };
        bool isNextLeft = level->next.has_value();
        auto appendNext = [&] {
            isNextLeft = false;
            if (auto asked = m_levelsOfLoads.find(*level->next); asked != m_levelsOfLoads.end())
                asked->value->isAskedFor = true;
            append(*level->next);
        };
        PlanOfLoad& plan = *level->plan;
        Vector<ModuleID, 8> listed;
        if (level->depth + 1 < plan.levels.size()) {
            std::span<PlanOfLoad::Found> below = plan.level(level->depth + 1);
            for (PlanOfLoad::Found& imported : below.subspan(std::ranges::lower_bound(below, level->place, { }, &PlanOfLoad::Found::importer) - below.begin())) {
                if (imported.importer != level->place)
                    break;
                if (level->next != imported.module && (!imported.leadsToFile || !isLeftToLoad(plan, imported)))
                    continue;
                listed.append(imported.module);
                if (imported.isFile || level->next == imported.module)
                    continue;
                ModuleRecord* known = recordIfExists(imported.module);
                if (known && !known->requestedModules().isEmpty())
                    continue;
                m_levelsOfLoads.ensure(imported.module, [&] {
                    auto branch = makeUnique<LevelOfLoad>();
                    branch->plan = &plan;
                    branch->depth = level->depth + 1;
                    branch->place = &imported - plan.found.span().data();
                    return branch;
                }).iterator->value->isAskedFor = true;
            }
        }
        for (ModuleID imported : listed) {
            if (level->next == imported) {
                appendNext();
                continue;
            }
            if (isCommonJSFileOrFace(imported)) {
                m_filesLoadedWithoutLoader.set(imported.index());
                if (status(imported) == ModuleStatus::Evaluated)
                    continue;
                noteLoadOfFile(imported);
                RETURN_IF_EXCEPTION(scope, true);
            }
            append(imported);
        }
        if (isNextLeft)
            appendNext();
        return true;
    };
    if (!m_levelsOfLoads.isEmpty() && appendNextOnWay())
        return;
    if (!record->isRequestedByWalkedModule()) {
        bool mustWaitForFile = importsFileBeingEvaluated(module);
        bool endsAtStatement = statementThatEndsLoad(module).has_value();
        bool isWalked = mustWaitForFile || endsAtStatement || canTellHowGraphIsLoaded(module);
        if (!isWalked) {
            evaluateImportedCommonJSFiles(module, JobsOfImportedFiles::WaitForLoader, ErrorOfImportedFile::IsLeftToLoader, &load);
            RETURN_IF_EXCEPTION(scope, void());
            if (!record->requestedModules().isEmpty())
                return;
            isWalked = m_rootWhoseFilesRanAhead == module;
        }
        if (!isWalked) {
            scope.release();
            appendRequestsOfUncompiledCode(module, record);
            return;
        }
        if (!m_instance.vm->synchronousModuleQueue() && m_rootWhoseFilesRanAhead != module && !m_loadsInFlight.hasRoot(module))
            didStartLoad(load.promise(), module, SettlingOfLoad::IsLookedFor);
        m_loadsInFlight.forEachOfRoot(module, [](LoadInFlight& load) {
            load.isWalked = true;
        });
        if (!data(module).isLoadedByLoader() && m_rootWhoseFilesRanAhead != module && !mustWaitForFile && !endsAtStatement && !runsFileOfLoadInHandsOfLoader() && planLevelsOfLoad(module)) {
            appendNextOnWay();
            return;
        }
    }

    countEvent(*m_instance.vm, RuntimeCounterID::CompiledModulesWalkedByLoader);
    UncheckedKeyHashSet<ModuleMapKey, ModuleMapHash> keys;
    WTF::Vector<Identifier, 8> keptAlive;
    for (const ModuleRequestData& request : m_table.imports(module)) {
        if (request.isHoisted)
            continue;
        ModuleID target = request.targetModule();
        if (wasLoadedWithEarlierGraph(target))
            continue;
        const ModuleData& targetData = data(target);
        if (targetData.isUncompiledCode()) {
            keptAlive.append(resolvedKey(target));
            if (Exception* exception = scope.exception()) [[unlikely]] {
                VM& vm = *m_instance.vm;
                TRY_CLEAR_EXCEPTION(scope, void());
                Identifier name = Identifier::fromString(vm, m_table.pathText(target));
                JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
                loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
                record->appendRequestedModule(name, nullptr, request.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation);
                record->setHasRequestThatFindsNothing();
                return;
            }
            if (std::optional<ModuleID> compiled = m_table.findByPath(keptAlive.last().string()); compiled && isCommonJSFileOrFace(*compiled)) {
                noteLoadOfFile(*compiled, loader()->registryEntry(keptAlive.last()) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
                RETURN_IF_EXCEPTION(scope, void());
                noteRequestByLoader(*compiled);
                if (!m_isMainCommonJSModuleDecided)
                    m_firstFilesOfLoads.append(fileOf(*compiled));
            }
        } else if (isCommonJSFileOrFace(target)) {
            keptAlive.append(keyOfRequest(target));
            bool isKnownToLoader = data(fileOf(target)).isEntryPoint() || loader()->registryEntry(keptAlive.last());
            noteLoadOfFile(target, isKnownToLoader ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
            RETURN_IF_EXCEPTION(scope, void());
            noteRequestByLoader(target);
        } else if (targetData.kind == ModuleKind::Data || isFileOfProgram(target) || isBuiltinModuleNewToLoader(target))
            keptAlive.append(keyOfRequest(target));
        else
            continue;
        RefPtr<ScriptFetchParameters> fetchParameters = targetData.isUncompiledCode() ? fetchParametersOf(target) : nullptr;
        if (!keys.add(placeInLoader(keptAlive.last(), fetchParameters.get())).isNewEntry)
            continue;
        record->appendRequestedModule(keptAlive.last(), WTF::move(fetchParameters), request.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation);
        if (!targetData.isUncompiledCode())
            record->didRequestCompiledModule(keptAlive.last(), target);
    }
    if (std::optional<ModuleFailureID> statement = statementThatEndsLoad(module); statement && m_table.failure(*statement).importerModule() == module) [[unlikely]]
        RELEASE_AND_RETURN(scope, appendRequestThatFindsNothing(*statement, record));
}

std::optional<ModuleFailureID> Modules::statementThatEndsLoad(ModuleID module) const
{
    std::optional<ModuleFailureID> endOfLoad = m_table.loadFailure(module);
    if (!endOfLoad) [[likely]]
        return std::nullopt;
    const ModuleFailureData& failure = m_table.failure(*endOfLoad);
    if (ProgramStrings::text(m_table.programTables(), failure.specifierString()) == m_table.pathText(failure.importerModule()))
        return std::nullopt;
    return endOfLoad;
}

void Modules::appendRequestThatFindsNothing(ModuleFailureID statement, ModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwFailure(statement, false);
    Exception* exception = scope.exception();
    RELEASE_ASSERT(exception);
    TRY_CLEAR_EXCEPTION(scope, void());
    Identifier name = Identifier::fromString(vm, ProgramStrings::text(m_table.programTables(), m_table.failure(statement).specifierString()));
    JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
    loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
    record->appendRequestedModule(name, nullptr, ModulePhase::Evaluation);
    record->setHasRequestThatFindsNothing();
}

void Modules::keepFailureOfRequest(ModuleID module, ModuleRecord* record, const Identifier& name)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (loader()->hasResolutionFailure({ record->moduleKey().impl(), name.impl() }))
        return;
    std::optional<ModuleFailureID> statement = statementThatEndsLoad(module);
    if (statement && m_table.failure(*statement).importerModule() == module && name.string() == ProgramStrings::text(m_table.programTables(), m_table.failure(*statement).specifierString()))
        throwFailure(*statement, false);
    else {
        for (const ModuleRequestData& request : m_table.imports(module)) {
            ModuleID target = request.targetModule();
            if (!data(target).isUncompiledCode() || name.string() != m_table.pathText(target))
                continue;
            resolvedKey(target);
            break;
        }
    }
    Exception* exception = scope.exception();
    if (!exception)
        return;
    TRY_CLEAR_EXCEPTION(scope, void());
    JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
    loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
}

struct Modules::RequestsOfUncompiledCode {
    struct Request {
        UniquedStringImpl* key;
        RefPtr<ScriptFetchParameters> fetchParameters;
    };

    UncheckedKeyHashSet<ModuleMapKey, ModuleMapHash> seen;
    WTF::Vector<Request, 8> list;
};

void Modules::collectRequestsOfUncompiledCode(ModuleID module, RequestsOfUncompiledCode& requests)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto isOrWillBeMocked = [&](ModuleID importer) { return isMocked(importer) || m_modulesWithMockToRun.get(importer.index()); };
    if (isOrWillBeMocked(module))
        return;
    BitVector visited(m_table.size());
    visited.quickSet(module.index());
    Vector<std::span<const ModuleRequestData>, 16> path { m_table.requests(module) };
    while (!path.isEmpty()) {
        std::span<const ModuleRequestData>& left = path.last();
        if (left.empty()) {
            path.removeLast();
            continue;
        }
        ModuleID target = left.front().targetModule();
        left = left.subspan(1);
        if (!data(target).reachesUncompiledCode())
            continue;
        if (data(target).isUncompiledCode()) {
            const Identifier& key = resolvedKey(target);
            RETURN_IF_EXCEPTION(scope, void());
            RefPtr<ScriptFetchParameters> fetchParameters = fetchParametersOf(target);
            if (requests.seen.add(placeInLoader(key, fetchParameters.get())).isNewEntry)
                requests.list.append({ key.impl(), WTF::move(fetchParameters) });
        } else if (status(target) == ModuleStatus::Unlinked && !isOrWillBeMocked(target) && !visited.quickSet(target.index()))
            path.append(m_table.requests(target));
    }
}

bool Modules::comesToCodeThatDoesNotLoad(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    RequestsOfUncompiledCode requests;
    collectRequestsOfUncompiledCode(root, requests);
    if (scope.exception()) [[unlikely]] {
        TRY_CLEAR_EXCEPTION(scope, true);
        return true;
    }
    for (const auto& request : requests.list) {
        if (ModuleRegistryEntry* entry = registryEntry(Identifier::fromUid(*m_instance.vm, request.key), request.fetchParameters.get()); entry && entry->fetchError())
            return true;
    }
    return false;
}

ModuleRecord* Modules::appendRequestsOfUncompiledCode(ModuleID module, ModuleRecord* record)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!data(module).reachesUncompiledCode())
        return record;
    RequestsOfUncompiledCode requests;
    collectRequestsOfUncompiledCode(module, requests);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& request : requests.list) {
        if (std::optional<ModuleID> compiled = m_table.findByPath(String(request.key)); compiled && isCommonJSFileOrFace(*compiled)) {
            noteLoadOfFile(*compiled, loader()->registryEntry(Identifier::fromUid(vm, request.key)) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
            RETURN_IF_EXCEPTION(scope, nullptr);
            noteRequestByLoader(*compiled);
            if (!m_isMainCommonJSModuleDecided)
                m_firstFilesOfLoads.append(fileOf(*compiled));
        }
        record->appendRequestedModule(Identifier::fromUid(vm, request.key), WTF::move(request.fetchParameters), ModulePhase::Evaluation);
    }
    return record;
}

ModuleRecord* Modules::registerWithLoader(ModuleID module, RecordHolder holder)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = exposeRecord(module, holder);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRegistryEntry* entry = loader()->ensureRegistered(m_instance.globalObject, record->moduleKey(), ScriptFetchParameters::Type::JavaScript);
    if (!entry->record() && entry->status() == ModuleRegistryEntry::Status::New)
        entry->provideModule(*m_instance.vm, record);
    return record;
}

AbstractModuleRecord* Modules::uncompiledRecord(ModuleID external)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    LoadedModule& loaded = ensureLoaded(external);
    if (loaded.uncompiledRecord)
        return loaded.uncompiledRecord;
    const Identifier& key = resolvedKey(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    ModuleRegistryEntry* entry = registryEntry(key, fetchParametersOf(external).get());
    AbstractModuleRecord* record = entry ? entry->record() : nullptr;
    if (!record) {
        if (std::optional<ModuleID> compiled = m_table.findByPath(key.string()); compiled && !isEvicted(*compiled))
            record = recordForResolution(moduleWithExportsOf(fileOf(*compiled)));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    if (!record) {
        throwException(m_instance.globalObject, scope, createExternalModuleNotLoadedError(m_instance.globalObject, m_table.pathText(external)));
        return nullptr;
    }
    loaded.uncompiledRecord.set(*this, record);
    return record;
}

AbstractModuleRecord* Modules::recordForResolution(ModuleID module)
{
    if (data(module).isUncompiledCode())
        return uncompiledRecord(module);
    ModuleRecord* record = ensureRecordWithoutStatus(module);
    record->retain();
    return record;
}

JSObject* Modules::uncompiledNamespace(ModuleID external, ModulePhase phase, NamespaceUse use, bool shouldPreventExtensions)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    AbstractModuleRecord* record = uncompiledRecord(external);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (dynamicDowncast<ModuleRecord>(record))
        RELEASE_AND_RETURN(scope, record->moduleNamespaceObject(m_instance.globalObject, phase));
    bool isShown = use == NamespaceUse::IsShownToProgram;
    JSModuleNamespaceObject* namespaceObject = record->getModuleNamespace(m_instance.globalObject, phase, isShown && shouldPreventExtensions);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (phase != ModulePhase::Evaluation)
        return namespaceObject;
    bool wasAskedFor = m_uncompiledNamespacesAskedFor.set(external.index());
    if (!isShown) {
        if (!wasAskedFor)
            m_uncompiledNamespacesNotShownYet.set(external.index());
        return namespaceObject;
    }
    if (m_uncompiledNamespacesNotShownYet.clear(external.index()) && shouldPreventExtensions) {
        JSObject::preventExtensions(namespaceObject, m_instance.globalObject);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return namespaceObject;
}

JSC_DEFINE_JIT_OPERATION(operationAOTModuleBuiltinCell, JSCell*, (Instance* instance, uint32_t builtin, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTModuleBuiltinCell);
    OPERATION_RETURN(scope, modulesOf(*instance).builtinCell(BuiltinID(builtin), ModuleID(module)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetModuleNamespace, JSCell*, (Instance* instance, uint32_t module, uint32_t phase))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetModuleNamespace);
    RELEASE_ASSERT(static_cast<ImportPhase>(phase) == ImportPhase::Evaluation || static_cast<ImportPhase>(phase) == ImportPhase::Defer);
    OPERATION_RETURN(scope, modulesOf(*instance).moduleNamespace(ModuleID(module), static_cast<ImportPhase>(phase) == ImportPhase::Defer ? Modules::ModulePhase::Defer : Modules::ModulePhase::Evaluation));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetNamespaceOfModuleItself, JSCell*, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTGetNamespaceOfModuleItself);
    OPERATION_RETURN(scope, modulesOf(*instance).namespaceOfModuleItself(ModuleID(module)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsNamespaceOfModule, size_t, (JSCell* cell, uint32_t module))
{
    auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
    return moduleNamespace && moduleNamespace->hasNamespaceLayoutOf(ModuleID(module));
}

JSC_DEFINE_JIT_OPERATION(operationAOTImportModule, EncodedJSValue, (Instance* instance, uint32_t module, EncodedJSValue options, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTImportModule);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).import(ModuleID(module), ModuleID(referrer), JSValue::decode(options))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireModule);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).require(ModuleID(module), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRecordRequire, void, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRecordRequire);
    modulesOf(*instance).recordRequire(ModuleID(module), ModuleID(referrer));
    OPERATION_RETURN(scope);
}

static bool isLoadedWithoutType(const Modules& modules, ModuleID form)
{
    String path = modules.table().pathText(form);
    size_t mark = path.reverseFind('?');
    if (modules.data(form).kind != ModuleKind::Data || mark == notFound)
        return false;
    std::optional<ModuleID> file = modules.table().findByPath(StringView(path).left(mark));
    return file && modules.data(*file).kind == ModuleKind::Data && modules.status(*file) == ModuleStatus::Evaluated;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireIfHeldForImport, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireIfHeldForImport);
    Modules& modules = modulesOf(*instance);
    if (!modules.isHeldForImport(ModuleID(module)) || isLoadedWithoutType(modules, ModuleID(module)))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    OPERATION_RETURN(scope, JSValue::encode(modules.require(ModuleID(module), ModuleID(referrer))));
}

static NEVER_INLINE Vector<uint64_t>* findCountedFile(Instance& instance, uint32_t module, StringID description)
{
    auto& files = instance.vm->aotState().countedFiles;
    while (files.byModule.size() <= module)
        files.byModule.append(nullptr);
    files.byModule[module] = files.byDescription.ensure(ProgramStrings::atomOf(*instance.program, description).string(), [] {
        return makeUnique<Vector<uint64_t>>();
    }).iterator->value.get();
    return files.byModule[module];
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAreLoaderFunctionsStored, size_t, (Instance* instance))
{
    return modulesOf(*instance).areLoaderFunctionsStored();
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSeesMockOf, size_t, (Instance* instance, uint32_t module, uint32_t referrer))
{
    return !!modulesOf(*instance).mockSeenBy(ModuleID(module), ModuleID(referrer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMockSeenBy, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    return JSValue::encode(modulesOf(*instance).mockSeenBy(ModuleID(module), ModuleID(referrer)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNamespaceSeenBy, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNamespaceSeenBy);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).namespaceSeenBy(ModuleID(module), ModuleID(referrer))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWasLoadedSynchronously, size_t, (Instance* instance, uint32_t module))
{
    return modulesOf(*instance).wasLoadedSynchronously(ModuleID(module));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCountExecution, void, (Instance* instance, uint32_t module, uint32_t description, uint32_t counter))
{
    const auto& byModule = instance->vm->aotState().countedFiles.byModule;
    Vector<uint64_t>* counts = module < byModule.size() ? byModule[module] : nullptr;
    if (!counts) [[unlikely]]
        counts = findCountedFile(*instance, module, StringID(description));
    while (counts->size() <= counter)
        counts->append(0);
    ++counts->at(counter);
}

void forEachCountedFile(VM& vm, const ScopedLambda<void(const String& description, std::span<const uint64_t> counts)>& each)
{
    for (auto& [description, counts] : vm.aotState().countedFiles.byDescription)
        each(description, counts->span());
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveModule, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t, uint32_t, uint32_t isURL))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveModule);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).resolve(ModuleID(module), isURL ? ResolvedSpecifierForm::URL : ResolvedSpecifierForm::Path)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrowModuleFailure, void, (Instance* instance, uint32_t failure, uint32_t isForRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTThrowModuleFailure);
    modulesOf(*instance).throwFailure(ModuleFailureID(failure), isForRequire);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTBeginModuleEvaluation, size_t, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTBeginModuleEvaluation);
    OPERATION_RETURN(scope, modulesOf(*instance).beginModuleInRecord(ModuleID(module)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTStopLoadingBehindFile, size_t, (Instance* instance, uint32_t module))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTStopLoadingBehindFile);
    OPERATION_RETURN(scope, modulesOf(*instance).stopLoadingBehindFile(ModuleID(module)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTAbandonModuleEvaluation, void, (Instance* instance, uint32_t module, JSCell* exception))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTAbandonModuleEvaluation);
    DeferTerminationForAWhile deferTermination(vm);
    modulesOf(*instance).abandonEvaluation(ModuleID(module), cellArgument<Exception>(exception, Entry::operationAOTAbandonModuleEvaluation)->value());
    scope.assertNoException();
}

JSC_DEFINE_JIT_OPERATION(operationAOTImportDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, EncodedJSValue options, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTImportDynamic);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).importDynamic(JSValue::decode(specifier), JSValue::decode(options), ModuleID(referrer))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireDynamic);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).requireSpecifier(JSValue::decode(specifier), ModuleID(referrer), Modules::NamedByBuild::No)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTResolveDynamic, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer, uint32_t isURL, uint32_t isForRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTResolveDynamic);
    ResolvedSpecifierForm form = isForRequire ? ResolvedSpecifierForm::PathForRequire : isURL ? ResolvedSpecifierForm::URL : ResolvedSpecifierForm::Path;
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).resolveDynamic(JSValue::decode(specifier), ModuleID(referrer), form)));
}

static bool s_logsEvaluations;

const ModuleHost& moduleHostOf(JSGlobalObject* realm)
{
    const ModuleHost* host = realm->globalObjectMethodTable()->moduleHost;
    RELEASE_ASSERT(host);
    return *host;
}

static void withModuleOfJob(JSGlobalObject* realm, JSValue moduleForJob, const ScopedLambda<void(Modules&, ModuleID)>& functor)
{
    functor(*modulesOf(realm), ModuleID(static_cast<uint32_t>(moduleForJob.asInt32())));
}

static std::optional<ModuleTable> currentModuleTable()
{
    const Image* image = Image::current();
    return image ? std::optional(ModuleTable(image->programTables())) : std::nullopt;
}

static Modules* modulesThatShowWaysBetweenModuleObjects(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && Image::current()->showsWaysBetweenModuleObjects() ? modules : nullptr;
}

static std::optional<ModuleID> findModuleInRunningImage(StringView publicPath, unsigned hashOfPath)
{
    const Image* image = Image::current();
    if (!image)
        return std::nullopt;
    ModuleTable table(image->programTables());
    if (publicPath.startsWith('#')) {
        std::optional<uint32_t> number = parseInteger<uint32_t>(publicPath.substring(1));
        if (number && *number < table.size() && table.pathText(ModuleID(*number)) == publicPath)
            return ModuleID(*number);
        return std::nullopt;
    }
    return table.findByPath(publicPath, hashOfPath);
}

std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath)
{
    return findModuleInRunningImage(publicPath, hashOfPath);
}

bool holdsFormThatAttributeAsksFor(StringView publicPath)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->holdsFormThatAttributeAsksFor(publicPath);
}

bool mayHoldPathStartingWith(StringView start)
{
    const Image* image = Image::current();
    return image && (start.isEmpty() || start.startsWith('#') || (image->mayHoldModulePathStartingWith(start) && ModuleTable(image->programTables()).holdsPathStartingWith(start)));
}

static StringView withoutSeparatorAtEnd(StringView directory)
{
    while (directory.length() > 1 && (directory[directory.length() - 1] == '/' || directory[directory.length() - 1] == '\\'))
        directory = directory.left(directory.length() - 1);
    return directory;
}

static StringView directoryOf(StringView path)
{
    size_t separator = path.reverseFind('/');
    if (size_t backslash = path.reverseFind('\\'); backslash != notFound && (separator == notFound || backslash > separator))
        separator = backslash;
    return separator == notFound ? StringView() : path.left(separator);
}

struct NamesLeftOutside {
    struct Key {
        StringView name;
        StringView directory;
    };
    static bool isLess(const Key& left, const Key& right)
    {
        std::strong_ordering order = codePointCompare(left.name, right.name);
        return order != 0 ? order < 0 : codePointCompareLessThan(left.directory, right.directory);
    }
    Key keyAt(uint32_t place) const { return { names[place], directories[place] }; }

    Vector<String> names;
    Vector<String> directories;
    Vector<uint32_t> placesInOrder;
};

static const NamesLeftOutside& namesLeftOutside(const Image& image, const ModuleTable& table)
{
    struct NamesOfImages {
        Lock lock;
        UncheckedKeyHashMap<unsigned, std::unique_ptr<NamesLeftOutside>> byImage WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<NamesOfImages> namesOfImages;
    NamesOfImages& ofImages = namesOfImages.get();
    Locker locker { ofImages.lock };
    return *ofImages.byImage.ensure(image.number() + 1, [&] {
        auto made = makeUniqueWithoutFastMallocCheck<NamesLeftOutside>();
        for (const UncompiledModuleData& entry : table.modulesOfUncompiledCode()) {
            ModuleID module(entry.module);
            String name = table.pathText(module);
            String directory;
            if (name.startsWith('.')) {
                std::optional<ModuleID> referrer = table.data(module).referrerModule();
                if (!referrer)
                    continue;
                directory = withoutSeparatorAtEnd(directoryOf(table.pathText(*referrer))).toString();
            }
            made->placesInOrder.append(made->names.size());
            made->names.append(WTF::move(name));
            made->directories.append(WTF::move(directory));
        }
        AOT::sort(made->placesInOrder, [&](uint32_t left, uint32_t right) {
            return NamesLeftOutside::isLess(made->keyAt(left), made->keyAt(right));
        });
        return made;
    }).iterator->value;
}

bool isLeftToCodeOutsideImage(StringView specifier, StringView directoryOfImporter)
{
    const Image* image = Image::current();
    if (!image)
        return false;
    ModuleTable table(image->programTables());
    if (table.modulesOfUncompiledCode().empty())
        return false;
    const NamesLeftOutside& left = namesLeftOutside(*image, table);
    auto holds = [&](StringView name, StringView directory) {
        return std::ranges::binary_search(left.placesInOrder, NamesLeftOutside::Key { name, directory }, NamesLeftOutside::isLess, [&](uint32_t place) {
            return left.keyAt(place);
        });
    };
    if (specifier.startsWith('.'))
        return holds(specifier, withoutSeparatorAtEnd(directoryOfImporter));
    if (holds(specifier, { }))
        return true;
    for (size_t slash = specifier.find('/'); slash != notFound; slash = specifier.find('/', slash + 1)) {
        StringView package = specifier.left(slash);
        if (package.startsWith('/'))
            return false;
        if (holds(package, { }))
            return true;
    }
    return false;
}

struct PathOfModule {
    StringImpl* path;
    StringImpl* pathOfFile;
    bool isEvaluatedText;
};

static String withoutQuery(const ModuleTable& table, ModuleID module, const String& path)
{
    size_t query = path.find('?');
    if (query == notFound)
        return path;
    String displayPath = table.displayPathText(module);
    if (displayPath.isEmpty() || displayPath.length() > query)
        return path;
    StringView end = StringView(path).substring(query - displayPath.length(), displayPath.length());
    for (unsigned index = 0; index < end.length(); ++index) {
        if (end[index] != displayPath[index] && (end[index] != '\\' || displayPath[index] != '/'))
            return path;
    }
    return path.left(query);
}

static PathOfModule pathOfModule(ModuleID module)
{
    struct PathsOfModules {
        Lock lock;
        UncheckedKeyHashMap<uint64_t, PathOfModule> byImageAndModule WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<PathsOfModules> pathsOfModules;
    PathsOfModules& paths = pathsOfModules.get();
    Locker locker { paths.lock };
    uint64_t key = static_cast<uint64_t>(Image::current()->number()) << 32 | (module.index() + 1);
    return paths.byImageAndModule.ensure(key, [&] {
        std::optional<ModuleTable> table = currentModuleTable();
        RELEASE_ASSERT(table);
        std::optional<ModuleID> evaluator = table->data(module).evaluatorModule();
        while (std::optional<ModuleID> outer = evaluator ? table->data(*evaluator).evaluatorModule() : std::nullopt)
            evaluator = outer;
        String ownURL = evaluator ? table->displayPathText(module) : String();
        bool hasOwnURL = !ownURL.isEmpty() && ownURL != table->pathText(module);
        String path = hasOwnURL ? ownURL : evaluator ? URL::fileURLWithFileSystemPath(table->pathText(*evaluator)).string() : table->data(module).registryID() ? table->displayPathText(module) : table->pathText(module);
        auto keep = [](const String& text) {
            return &(text.is8Bit() ? StringImpl::createStaticStringImpl(text.span8()) : StringImpl::createStaticStringImpl(text.span16())).leakRef();
        };
        StringImpl* kept = keep(path);
        String pathOfFile = evaluator || table->data(module).registryID() ? path : withoutQuery(*table, module, path);
        return PathOfModule { kept, pathOfFile.length() == path.length() ? kept : keep(pathOfFile), !!evaluator };
    }).iterator->value;
}

String modulePath(ModuleID module)
{
    return pathOfModule(module).path;
}

String pathOfFileOfModule(ModuleID module)
{
    return pathOfModule(module).pathOfFile;
}

String sourceProviderURL(ModuleID module)
{
    PathOfModule found = pathOfModule(module);
    return found.isEvaluatedText ? emptyString() : String(found.pathOfFile);
}

bool entryPointIsCommonJS(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->firstModuleRequestedByHost() && modules->data(*modules->firstModuleRequestedByHost()).kind == ModuleKind::CommonJs;
}

bool isESModule(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::SourceText;
}

bool isESModuleThatWaitsForNothing(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->data(module).kind == ModuleKind::SourceText && modules->data(module).isSynchronous() && !modules->importsModuleWhoseMockWaits(module);
}

std::optional<bool> findsBuiltinsReadOnly(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table || table->data(module).isOfLibrary())
        return std::nullopt;
    return std::ranges::binary_search(Image::current()->programTables().modulesThatFindBuiltinsReadOnly(), module.index());
}

bool isReachedUnder(ModuleID module, OtherNameOfModule name)
{
    if (!currentModuleTable())
        return false;
    return std::ranges::binary_search(Image::current()->programTables().modulesReachedUnderOtherNames(), moduleReachedUnder(module.index(), name));
}

bool isDataFile(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Data;
}

bool isHeldForImport(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isHeldForImport(module);
}

bool isNativeModule(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Runtime;
}

bool isOfLibrary(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

bool mayHoldEntryOfProgramInRequireCache(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

static void throwLoaderChangeRefused(JSGlobalObject* realm, LoaderChange change, StringView changed)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoaderChangeRefusedError(realm, change, changed));
}

enum class TakenOutBy : uint8_t { Deletion, Reset, FalsyStore, EntryOfProgram };

static bool isFoundBehindFalsyEntry(Modules& modules, ModuleID module)
{
    return modules.data(module).isOfLibrary() || modules.wasLoadedByImport(module);
}

static void evictModule(JSGlobalObject* realm, ModuleID module, TakenOutBy takenOutBy)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    if (takenOutBy == TakenOutBy::FalsyStore && isFoundBehindFalsyEntry(*modules, module))
        return;
    if (modules->data(module).isOfLibrary()) {
        String path = modules->table().pathText(module);
        if (takenOutBy == TakenOutBy::EntryOfProgram && modules->data(module).isHeldUnderNameWithoutPrefix() && !path.startsWith("node:"_s))
            return throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return modules->takeOutOfRequireCache(module);
    }
    modules->forgetObjectMadeByRequire(module);
    bool isDeleted = takenOutBy == TakenOutBy::Deletion || takenOutBy == TakenOutBy::Reset;
    if (isDeleted) {
        modules->forgetFirstLoad(module);
        modules->dropErrorOfLoadEndedByMock(module);
        modules->forgetRequestByLoader(module);
    }
    bool standsInPlaceOfFile = takenOutBy == TakenOutBy::EntryOfProgram;
    if (!modules->mayBeEvicted(module)) {
        if (standsInPlaceOfFile)
            throwLoaderChangeRefused(realm, LoaderChange::ReplaceInRequireCache, modules->table().pathText(module));
        else if (modules->wouldBeEvicted(module))
            throwLoaderChangeRefused(realm, LoaderChange::TakeOutOfRequireCache, modules->table().pathText(module));
        return;
    }
    modules->forgetFormAnsweredFirst(module);
    modules->evict(module, takenOutBy == TakenOutBy::Reset);
    if (isDeleted && modules->isEvicted(module))
        modules->takeOutOfLoader(module);
    if (standsInPlaceOfFile)
        modules->leaveToHostUnlessItHasRun(module);
}

void willRunEntryPointAgain(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->forgetEvaluation(module);
}

void didDeleteRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::Deletion);
}

void didResetModule(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::Reset);
}

void Modules::forEachFileThatFailed(const WTF::Function<void(ModuleID)>& functor) const
{
    for (auto& [module, record] : m_records) {
        const ModuleData& moduleData = data(module);
        if (statusOf(*record) != ModuleStatus::Failed || moduleData.isOfLibrary() || !m_table.hasFilePath(module))
            continue;
        ModuleID file = m_table.fileBehindFace(module).value_or(module);
        if (mayBeEvicted(file) && !isEvicted(file))
            functor(file);
    }
}

void takeOutFilesThatFailed(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    if (!modules || !modules->holdsCodeThatMayBeEvicted())
        return;
    Vector<ModuleID, 4> failed;
    modules->forEachFileThatFailed([&](ModuleID file) {
        if (!failed.contains(file))
            failed.append(file);
    });
    if (failed.isEmpty()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    for (ModuleID file : failed) {
        evictModule(realm, file, TakenOutBy::Reset);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool isEvictedFile(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isEvicted(module);
}

void willRunPreloads(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->setHostRunsPreloads(true);
}

void didRunPreloads(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->setHostRunsPreloads(false);
}

void willRunCodeThatKeepsFiles(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->willRunCodeThatKeepsFiles();
}

void keepFilesThatHaveRun(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->keepFilesThatHaveRun();
}

void bringBackKeptFiles(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    if (Modules* modules = modulesOf(realm))
        modules->bringBackKeptFiles(functor);
}

bool refusesToLoadAgain(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && !modules->data(module).isOfLibrary() && !modules->mayBeEvicted(module) && modules->wouldBeEvicted(module);
}

void willClearRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::FalsyStore);
}

void willReplaceRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::EntryOfProgram);
}

bool willLoadFileInAnotherWay(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules || modules->data(module).isOfLibrary())
        return true;
    if (!modules->mayBeEvicted(module))
        return false;
    modules->leaveToHostUnlessItHasRun(module);
    return true;
}

bool Modules::hasRun(ModuleID module)
{
    bool importsAreFollowed = false;
    return hasRun(module, importsAreFollowed);
}

bool Modules::hasRun(ModuleID module, bool& importsAreFollowed)
{
    if (status(module) == ModuleStatus::Evaluated && !mayWaitForItsCycle(module))
        return true;
    if (!std::exchange(importsAreFollowed, true))
        followImportsOfModulesThatHaveRun();
    return m_modulesSeenToHaveRun.get(module.index()) || m_modulesThatRanWithImporters.get(module.index());
}

void Modules::followImportsOfModulesThatHaveRun()
{
    size_t numberOfModules = m_table.size();
    if (m_statusesWhenImportsWereFollowed.isEmpty()) {
        m_statusesWhenImportsWereFollowed.fill(static_cast<uint8_t>(StoredModuleStatus::Unlinked), numberOfModules);
        m_modulesThatRanWithImporters.ensureSize(numberOfModules);
        m_modulesSeenToHaveRun.ensureSize(numberOfModules);
    }
    size_t numberLookedAt = 0;
    bool hasStatusTakenBack = false;
    Vector<ModuleID, 64> toFollow;
    auto lookAt = [&](ModuleID module) {
        bool hasRun = status(module) == ModuleStatus::Evaluated && !waitsForItsCycle(module, m_modulesThatWaitedWhenImportsWereFollowed);
        if (hasRun == m_modulesSeenToHaveRun.quickGet(module.index()))
            return;
        m_modulesSeenToHaveRun.quickSet(module.index(), hasRun);
        if (hasRun)
            toFollow.append(module);
        else
            hasStatusTakenBack = true;
    };

    std::span<const uint8_t> now = m_storedStatuses;
    std::span<uint8_t> before = m_statusesWhenImportsWereFollowed.mutableSpan();
    BitVector waited;
    bool hasFoundChange = false;
    constexpr size_t blockSize = 256;
    for (size_t start = 0; start < numberOfModules; start += blockSize) {
        size_t end = std::min(start + blockSize, numberOfModules);
        if (equalSpans(std::span<const uint8_t>(before.subspan(start, end - start)), now.subspan(start, end - start)))
            continue;
        if (!std::exchange(hasFoundChange, true))
            waited = std::exchange(m_modulesThatWaitedWhenImportsWereFollowed, { }).modules;
        numberLookedAt += end - start;
        for (size_t index = start; index < end; ++index) {
            if (std::exchange(before[index], now[index]) != now[index])
                lookAt(ModuleID(index));
        }
    }
    for (size_t index : waited)
        lookAt(ModuleID(index));
    constexpr auto inRecord = static_cast<uint8_t>(StoredModuleStatus::InRecord);
    Vector<size_t, 64> settled;
    for (size_t index : m_recordsNotSeenToHaveRun) {
        ++numberLookedAt;
        lookAt(ModuleID(index));
        if (now[index] != inRecord || m_modulesSeenToHaveRun.quickGet(index) || !m_records.contains(ModuleID(index)))
            settled.append(index);
    }
    for (size_t index : settled)
        m_recordsNotSeenToHaveRun.clear(index);
#if ASSERT_ENABLED
    for (ModuleID module : m_records.keys())
        ASSERT(now[module.index()] != inRecord || m_modulesSeenToHaveRun.quickGet(module.index()) == (statusInRecord(module) == ModuleStatus::Evaluated));
#endif
    if (!numberLookedAt)
        return;

    if (hasStatusTakenBack) {
        m_modulesThatRanWithImporters.clearAll();
        toFollow.shrink(0);
        for (size_t index : m_modulesSeenToHaveRun)
            toFollow.append(ModuleID(index));
    }
    while (!toFollow.isEmpty()) {
        ++numberLookedAt;
        ModuleID importer = toFollow.takeLast();
        if (standsForWholeModule(importer))
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (request.isDeferred || request.isRequire)
                continue;
            if (m_modulesOwedToImportersThatRan.get(request.target()) || m_modulesWithMockToLetGo.get(request.target()))
                continue;
            if (status(request.targetModule()) == ModuleStatus::Evaluating || waitsForItsCycle(request.targetModule(), m_modulesThatWaitedWhenImportsWereFollowed))
                continue;
            if (!m_modulesThatRanWithImporters.quickSet(request.target()))
                toFollow.append(request.targetModule());
        }
    }
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesForWhatHasRun, numberLookedAt);
}

void Modules::followImportsAnewAtNextQuestion()
{
    m_statusesWhenImportsWereFollowed.clear();
    m_modulesThatWaitedWhenImportsWereFollowed = { };
    m_modulesThatRanWithImporters.clearAll();
    m_modulesSeenToHaveRun.clearAll();
    for (ModuleID module : m_records.keys())
        didChangeStatusInRecord(module);
}

bool Modules::isListedBeforeItHasRun(ModuleID module)
{
    if (data(module).kind == ModuleKind::CommonJs)
        return status(module) == ModuleStatus::Evaluating || (status(module) <= ModuleStatus::Linked && wasRequestedByLoader(module));
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isKeptByRequire && (loaded->requiredValue || status(module) != ModuleStatus::Failed);
}

bool Modules::hasModuleObjectInRequireCache(ModuleID module) const
{
    if (data(module).kind == ModuleKind::CommonJs)
        return true;
    LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isKeptByRequire;
}

bool isEvaluatedFile(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return false;
    const ModuleData& data = modules->data(module);
    if (data.isOfLibrary())
        return modules->isLibraryModuleInRequireCache(module);
    if (data.isFaceOfFile() || !modules->table().hasFilePath(module) || modules->isEvicted(module))
        return false;
    if (!modules->hasRun(module) && !modules->isListedBeforeItHasRun(module))
        return false;
    return !isNamedByNumber(modules->table().pathText(module), module);
}

JSObject* requireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    ASSERT(isEvaluatedFile(realm, module));
    return modulesOf(realm)->requireCacheEntry(module);
}

void Modules::forEachFileAwaitingAtTopLevel(const WTF::Function<void(int64_t, const String&)>& functor)
{
    for (ModuleRecord* record : m_records.values()) {
        if (record->status() == CyclicModuleRecord::Status::EvaluatingAsync && record->hasTLA() && !record->pendingAsyncDependencies().value_or(0) && record->asyncEvaluationOrder().hasOrder())
            functor(record->asyncEvaluationOrder().order(), record->moduleKey().string());
    }
}

void forEachFileAwaitingAtTopLevel(JSGlobalObject* realm, const WTF::Function<void(int64_t, const String&)>& functor)
{
    if (Modules* modules = modulesOf(realm))
        modules->forEachFileAwaitingAtTopLevel(functor);
}

void forEachEvaluatedFile(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    BitVector isSeen(table.size());
    Vector<ModuleID, 64> inOrder;
    auto askFor = [&](ModuleID module) {
        if (isSeen.quickSet(module.index()))
            return false;
        if (!table.data(module).isOfLibrary() && isEvaluatedFile(realm, module))
            inOrder.append(module);
        return true;
    };
    BitVector isRequested(table.size());
    UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>> modulesFirstRequiredBy;
    for (auto [requirer, required] : modules->firstRequires()) {
        modulesFirstRequiredBy.add(requirer, Vector<ModuleID, 2>()).iterator->value.append(required);
        isRequested.quickSet(required.index());
    }
    auto firstRequiredBy = [&](ModuleID requirer) {
        auto found = modulesFirstRequiredBy.find(requirer);
        return found == modulesFirstRequiredBy.end() ? std::span<const ModuleID>() : found->value.span();
    };
    struct Step {
        Vector<ModuleID, 1> askers;
        bool areAskedFor;
    };
    Vector<Step, 64> steps;
    auto listFrom = [&](ModuleID root) {
        steps.append(Step { { root }, false });
        while (!steps.isEmpty()) {
            Step step = steps.takeLast();
            if (!step.areAskedFor && !askFor(step.askers[0]))
                continue;
            if (!modules->isLoadedWithRequests(step.askers[0]) || table.data(step.askers[0]).isFaceOfFile()) {
                auto requests = table.requests(step.askers[0]);
                for (size_t index = requests.size(); index--;)
                    steps.append(Step { { modules->moduleLoadedBy(requests[index]) }, false });
                for (ModuleID required : firstRequiredBy(step.askers[0]) | std::views::reverse)
                    steps.append(Step { { required }, false });
                continue;
            }
            Vector<ModuleID, 1> nextLevel;
            Vector<ModuleID, 8> filesOfLevel;
            Vector<ModuleID, 8> askedForLater;
            for (ModuleID importer : step.askers) {
                askedForLater.append(firstRequiredBy(importer));
                for (const ModuleRequestData& request : table.requests(importer)) {
                    ModuleID loaded = modules->moduleLoadedBy(request);
                    ModuleID target = table.fileBehindFace(loaded).value_or(loaded);
                    if (request.isHoisted || request.isRequire)
                        askedForLater.append(target);
                    else if (!askFor(target))
                        continue;
                    else if (modules->isLoadedWithRequests(target))
                        nextLevel.append(target);
                    else
                        filesOfLevel.append(target);
                }
            }
            for (ModuleID module : askedForLater | std::views::reverse)
                steps.append(Step { { module }, false });
            if (!nextLevel.isEmpty())
                steps.append(Step { WTF::move(nextLevel), true });
            for (ModuleID file : filesOfLevel | std::views::reverse)
                steps.append(Step { { file }, true });
        }
    };
    for (unsigned index = 0; index < table.size(); ++index) {
        for (const ModuleRequestData& request : table.requests(ModuleID(index)))
            isRequested.quickSet(modules->moduleLoadedBy(request).index());
    }
    for (bool startsAtEntryPoints : { true, false }) {
        for (unsigned index = 0; index < table.size(); ++index) {
            if (!isRequested.quickGet(index) && (!startsAtEntryPoints || table.data(ModuleID(index)).isEntryPoint()))
                listFrom(ModuleID(index));
        }
    }
    for (unsigned index = 0; index < table.size(); ++index)
        listFrom(ModuleID(index));
    for (bool hasModuleObject : { true, false }) {
        for (ModuleID module : inOrder) {
            if (modules->hasModuleObjectInRequireCache(module) == hasModuleObject)
                functor(table.pathText(module));
        }
    }
}

void forEachLibraryModuleInRequireCache(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    for (ModuleID module : modules->libraryModulesInRequireCache())
        functor(modules->table().pathText(module));
}

std::optional<ModuleID> findLibraryModuleInRequireCache(JSGlobalObject* realm, const String& name)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return std::nullopt;
    std::optional<ModuleID> module = findModuleInRunningImage(name, name.hash());
    if (module && modules->data(*module).isOfLibrary() && modules->isLibraryModuleInRequireCache(*module))
        return module;
    return std::nullopt;
}

bool hostAnswersInPlaceOf(JSGlobalObject* realm, ModuleID module, LoadByName way)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hostAnswersInPlaceOf(module, way);
}

void willLoadCopyFromDisk(JSGlobalObject* realm, ModuleID module, LoadByName way)
{
    if (Modules* modules = modulesOf(realm))
        modules->willLoadCopyFromDisk(module, way);
}

void forEachFirstLoad(VM& vm, const ScopedLambda<void(const String& path, bool wasSynchronous)>& functor)
{
    const VMState& state = vm.aotState();
    if (state.filesWithKnownFirstLoad.isEmpty())
        return;
    ModuleTable table(state.image->programTables());
    for (size_t file : state.filesWithKnownFirstLoad)
        functor(table.pathText(ModuleID(file)), state.filesFirstLoadedSynchronously.get(file));
}

void noteFirstLoadInEarlierProcess(VM& vm, const String& path, bool wasSynchronous)
{
    VMState& state = vm.aotState();
    if (!state.image)
        return;
    ModuleTable table(state.image->programTables());
    std::optional<ModuleID> found = table.findByPath(path);
    if (!found)
        return;
    ModuleID file = table.fileBehindFace(*found).value_or(*found);
    if (!state.filesWithKnownFirstLoad.set(file.index()))
        state.filesFirstLoadedSynchronously.set(file.index(), wasSynchronous);
}

bool leavesModuleToHost(JSGlobalObject* realm, ModuleID module, StartedBy startedBy)
{
    if (Modules* modules = modulesOf(realm))
        return startedBy == StartedBy::Host ? modules->isEvicted(module) : modules->isLeftToHost(module);
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).isOfLibrary();
}

void forEachModule(const ScopedLambda<IterationStatus(ModuleID)>& functor)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table)
        return;
    for (unsigned index = 0; index < table->size(); ++index) {
        if (table->hasFilePath(ModuleID(index)) && functor(ModuleID(index)) == IterationStatus::Done)
            return;
    }
}

void forEachFilePathStartingWith(StringView start, const ScopedLambda<IterationStatus(const String&)>& functor)
{
    Image::forEachPublishedImage([&](const Image& image) {
        if (!image.mayHoldModulePathStartingWith(start))
            return IterationStatus::Continue;
        return ModuleTable(image.programTables()).forEachFilePathStartingWith(start, functor);
    });
}

bool refusesCodeOutsideImage(JSGlobalObject* realm, StringView path)
{
    if (Modules* modules = modulesOf(realm))
        return modules->refusesCodeOutsideImage() && !modules->isTextNamedByProgram(path);
    const Image* image = Image::current();
    return image && !hasFlag(image->header().options.admitted(), AdmittedCode::Inspector) && !image->loadsCodeOutsideImage();
}

bool isChangedOutOfSightOfBuild(JSGlobalObject* realm, StringView path)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isChangedOutOfSightOfBuild(path);
}

bool willRunCopyFromDisk(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return true;
    if (modules->refusesOriginalsBesideMocks())
        return false;
    modules->willRunCopyFromDisk();
    return true;
}

bool refusesHostObjectsOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return refusesCodeOutsideImage(realm, { }) && !(modules && modules->isAskingHost());
}

JSObject* mainModuleObject(JSGlobalObject* realm, ModuleID module)
{
    if (!modulesThatShowWaysBetweenModuleObjects(realm))
        return nullptr;
    Modules* modules = modulesOf(realm);
    return modules ? modules->mainModuleObject(module) : nullptr;
}

void willStorePathOfMainModule(JSGlobalObject* realm)
{
    Instance* instance = realm->aotInstance();
    if (!instance || hasFlag(instance->options().admitted(), AdmittedCode::ReplacedBuiltins) || instance->program->storesPathOfMainModule())
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwTypeError(realm, scope, assignmentThatBuildDidNotSeeMessage("Bun.main"_s));
}

JSObject* parentModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->parentModuleObject(module) : nullptr;
}

bool mayHandOutModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->mayHandOutModuleObject(module);
}

bool forEachNewChild(JSGlobalObject* realm, ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    if (Modules* modules = modulesOf(realm); modules && modules->data(module).isOfLibrary())
        return true;
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->forEachNewChild(module, functor);
}

void dropNewChildren(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm))
        modules->dropNewChildren(module);
}

void Modules::stopWhereExportsLeaveCompiledCode(ModuleID module) const
{
    if (!data(module).exportsStayInCompiledCode()) [[likely]]
        return;
    dataLogLn("AOT: the exports of ", m_table.pathText(module), " are read or stored from outside compiled code, and the compiler has proven that compiled code alone reads and stores them. This is a bug in Bun.");
    CRASH();
}

JSValue moduleExports(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return jsUndefined();
    modules->stopWhereExportsLeaveCompiledCode(module);
    modules->noteLoadBehindMock(module, true);
    return modules->exportsGivenBackToCompiledCode(module);
}

void setModuleExports(JSGlobalObject* realm, ModuleID module, JSValue exports)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    ASSERT(modules->data(module).kind == ModuleKind::CommonJs);
    modules->setCommonJSExports(module, exports);
}

AskerOfModule::AskerOfModule(JSGlobalObject* realm, const String& path)
    : m_modules(modulesOf(realm))
    , m_outer(m_modules ? m_modules->exchangeAskerOfModule(&path) : nullptr)
{
}

AskerOfModule::~AskerOfModule()
{
    if (m_modules)
        m_modules->exchangeAskerOfModule(m_outer);
}

bool isAskedByCodeOutsideImage(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isAskedByCodeOutsideImage();
}

void throwFileOfImageRefused(JSGlobalObject* realm, ModuleID module, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    String path = modulePath(module);
    tellRefusedChangeListener(realm, makeString("Cannot reach '"_s, path, '\''));
    throwException(realm, scope, createFileOfImageRefusedError(realm, path, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    tellRefusedChangeListener(realm, makeString("Cannot reach '"_s, name, '\''));
    throwException(realm, scope, createLoadByNameRefusedError(realm, name, form == LoadByName::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

void Modules::refuseToShowExportsOf(ModuleID module)
{
    throwLoadByNameRefused(m_instance.globalObject, m_table.pathText(m_table.fileBehindFace(module).value_or(module)), LoadByName::Import);
}

bool isResolvingImportCall(VM& vm)
{
    return vm.aotState().isResolvingImportCall;
}

bool requireAsksFor(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && realm->vm().aotState().requireRunsMocks && modules->requireAsksFor(module);
}

bool importAsksFor(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->importAsksFor(module);
}

bool importThatWaitedAsksFor(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->importThatWaitedAsksFor(module);
}

bool requireRunsMocks(VM& vm)
{
    return vm.aotState().requireRunsMocks;
}

void willLoadByName(JSGlobalObject* realm, JSValue specifier, LoadByName form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesOf(realm);
    if (modules ? modules->refusesLoadsByName() && modules->refusesCodeOutsideImage() : !Image::current()->loadsModulesByName()) {
        String name = specifier.isString() ? asString(specifier)->value(realm) : String();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, throwLoadByNameRefused(realm, name, form));
    }
    if (modules)
        RELEASE_AND_RETURN(scope, modules->admitCodeNamedBy(specifier));
}

static Modules* modulesOrThrow(JSGlobalObject* realm, ModuleID module, LoadByName form = LoadByName::Import)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->refusesRequestFromHost(module)) [[likely]]
        return modules;
    if (modules) {
        if (modules->isAskedByCodeOutsideImage() && Image::current()->loadsCodeOutsideImage())
            throwFileOfImageRefused(realm, module, form);
        else
            throwLoadByNameRefused(realm, modulePath(module), form);
        return nullptr;
    }
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createModuleInOtherRealmError(realm, modulePath(module)));
    return nullptr;
}

void willAnswerWithModule(JSGlobalObject* realm, ModuleID module, LoadByName form)
{
    if (!modulesOf(realm) && isOfLibrary(module))
        return;
    modulesOrThrow(realm, module, form);
}

void setVerbose(bool isVerbose)
{
    s_logsEvaluations = isVerbose;
}

static Modules* modulesForEvaluation(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOrThrow(realm, module);
    if (!modules)
        return nullptr;
    modules->didRequestFromHost(module);
    if (s_logsEvaluations && modules->status(module) < ModuleStatus::Evaluating) [[unlikely]]
        writeToStandardError(makeString("[aot] module: "_s, modulePath(module), '\n'));
    return modules;
}

AbstractModuleRecord* moduleRecordOfNamespace(JSGlobalObject*, JSCell* cell)
{
    auto* moduleNamespace = dynamicDowncast<ModuleNamespace>(cell);
    if (!moduleNamespace)
        return nullptr;
    return moduleNamespace->recordForResolution();
}

AbstractModuleRecord* moduleRecord(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return nullptr;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    modules->letGoOfMockThatEnded(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    VMState& state = realm->vm().aotState();
    bool isFirstAskedForByImport = state.frameOfImportThatAsksLoader && (!state.stackPointerOfImport || state.stackPointerOfImport > static_cast<void*>(state.frameOfImportThatAsksLoader));
    SetForScope stackPointerOfImport(state.stackPointerOfImport, isFirstAskedForByImport ? currentStackPointer() : state.stackPointerOfImport);
    if (!modules->isAskingHost()) {
        if (std::optional<ModuleFailureID> failure = modules->loadFailure(module); failure && !modules->statementThatEndsLoad(module)) [[unlikely]] {
            scope.release();
            modules->throwFailure(*failure, false);
            return nullptr;
        }
    }
    if (modules->isCommonJSFileOrFace(module)) {
        if (!modules->isAskingHost())
            modules->didImportFileFromUncompiledCode(module);
        RELEASE_AND_RETURN(scope, modules->recordOfCommonJSFile(module, Modules::LoadOfFile::IsRequested));
    }
    modules->runDataFileAgainForImport(module, Modules::ImportAnsweredBy::Loader);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!modules->isAskingHost() || modules->isStatementAskingHost())
        modules->didImportFromUncompiledCode(module, isFirstAskedForByImport);
    RELEASE_AND_RETURN(scope, modules->exposeRecord(module, modules->isAskingHost() ? Modules::RecordHolder::Loader : Modules::RecordHolder::Anyone));
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

static void settleCallsInLine(VM& vm, JSArray* line)
{
    if (!line->getIndexQuickly(0).asBoolean())
        return;
    unsigned next = line->getIndexQuickly(1).asUInt32();
    for (; next < line->length(); ++next) {
        JSArray* call = asArray(line->getIndexQuickly(next));
        if (!call->getIndexQuickly(2).asBoolean())
            break;
        auto* promise = uncheckedDowncast<JSPromise>(call->getIndexQuickly(0));
        auto* loaded = uncheckedDowncast<JSPromise>(call->getIndexQuickly(1));
        if (loaded->status() == JSPromise::Status::Fulfilled)
            promise->fulfill(vm, loaded->result());
        else
            promise->reject(vm, loaded->result());
    }
    line->setIndexQuickly(vm, 1, jsNumber(next));
}

JSC_DEFINE_HOST_FUNCTION(settleCallsBehindEarlierCall, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* line = asArray(callFrame->argument(1));
    line->setIndexQuickly(realm->vm(), 0, jsBoolean(true));
    settleCallsInLine(realm->vm(), line);
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(settleCallBehindOwnLoad, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* call = asArray(callFrame->argument(1));
    call->setIndexQuickly(realm->vm(), 2, jsBoolean(true));
    settleCallsInLine(realm->vm(), asArray(call->getIndexQuickly(3)));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(importBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* heldCall = asArray(callFrame->argument(1));
    ModuleID module(heldCall->getIndexQuickly(0).asUInt32());
    JSValue order = heldCall->getIndexQuickly(1);
    std::optional<int64_t> referrerAsyncOrder = order.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(order.asNumber()));
    auto caller = heldCall->getIndexQuickly(2).asBoolean() ? Modules::ImportCaller::Host : Modules::ImportCaller::CompiledCode;
    return JSValue::encode(modulesOf(realm)->startImport(module, referrerAsyncOrder, caller, static_cast<Modules::TextOfImport>(heldCall->getIndexQuickly(3).asUInt32()), nullptr, heldCall->getIndexQuickly(4)));
}

JSC_DEFINE_HOST_FUNCTION(importByKeyBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* heldCall = asArray(callFrame->argument(1));
    ModuleID referrer(heldCall->getIndexQuickly(1).asUInt32());
    Identifier key = Identifier::fromString(realm->vm(), asString(heldCall->getIndexQuickly(0))->tryGetValue());
    return JSValue::encode(modulesOf(realm)->importUncompiled(key, referrer, nullptr, heldCall->getIndexQuickly(2)));
}

JSC_DEFINE_HOST_FUNCTION(continueImportWithModuleItself, (JSGlobalObject* realm, CallFrame* callFrame))
{
    ModuleID module(callFrame->argument(1).asUInt32());
    return JSValue::encode(modulesOf(realm)->importModuleItselfBesideMock(module, callFrame->argument(0)));
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (JSValue error = modules ? modules->errorOfLoadEndedByMock(module) : JSValue()) [[unlikely]]
        return JSPromise::rejectedPromise(realm, error);
    if (modules)
        modules->dropErrorOfMockKeptByLoader(module);
    return modules ? modules->startImport(module, referrerAsyncOrder, Modules::ImportCaller::Host) : nullptr;
}

void evaluateModuleSynchronously(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesForEvaluation(realm, module))
        modules->evaluateSynchronously(module, SynchronousEvaluationReason::Require);
}

JSObject* moduleNamespace(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOrThrow(realm, module);
    if (!modules)
        return nullptr;
    if (modules->isLeftAloneByMock(module)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        modules->evaluateModulesOwedBelowOriginal(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        RELEASE_AND_RETURN(scope, modules->moduleNamespace(module, Modules::ModulePhase::Evaluation));
    }
    return modules->moduleNamespace(module, Modules::ModulePhase::Evaluation);
}

bool mayExport(JSGlobalObject* realm, ModuleID module, UniquedStringImpl* name)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->refusesRequestFromHost(module) || modules->mayExport(module, name);
}

JSValue requireModule(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> referrer)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return { };
    modules->stopWhereExportsLeaveCompiledCode(module);
    modules->noteLoadBehindMock(module, true);
    return modules->require(module, referrer.value_or(ModuleID()), Modules::RequiredBy::Program);
}

bool refusesToRunFileForDefaultLoader(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->data(module).kind == ModuleKind::CommonJs && modules->status(module) > ModuleStatus::Linked;
}

JSValue runFileForDefaultLoader(JSGlobalObject* realm, ModuleID module)
{
    if (refusesToRunFileForDefaultLoader(realm, module)) {
        throwLoaderChangeRefused(realm, LoaderChange::RunFileAgain, modulesOf(realm)->table().pathText(module));
        return { };
    }
    return requireModule(realm, module);
}

void throwTextRefusedToCompileOfProgram(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        throwLoaderChangeRefused(realm, LoaderChange::HandTextToCompile, modules->table().pathText(module));
}

bool isInRequireCache(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isInRequireCache(module);
}

std::optional<OptionSet<ModuleObjectTrait>> traitsOfModuleObjectYetToBeMade(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->traitsOfModuleObjectYetToBeMade(module) : std::nullopt;
}

void takeModuleObject(JSGlobalObject* realm, ModuleID module, const ModuleObjects& objects)
{
    modulesOf(realm)->takeModuleObject(module, objects);
}

void giveBackModuleObject(JSGlobalObject* realm, ModuleID module)
{
    modulesOf(realm)->giveBackModuleObject(module);
}

JSObject* moduleObjectOverExportsVariableIfMade(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->moduleObjectOverExportsVariableIfMade(module) : nullptr;
}

void didLayExportsVariableOnModuleObject(JSGlobalObject* realm, ModuleID module)
{
    modulesOf(realm)->didLayExportsVariableOnModuleObject(module);
}

void didMakeModuleObjectForRequire(JSGlobalObject* realm, ModuleID module, JSObject* moduleObject)
{
    if (Modules* modules = modulesOf(realm))
        modules->keepObjectMadeByRequire(module, moduleObject);
}

void willRequireModuleFromOutsideImage(JSGlobalObject* realm, ModuleID module, JSObject* moduleObjectOfRequirer)
{
    if (Modules* modules = modulesOf(realm))
        modules->recordRequireFromOutsideImage(module, moduleObjectOfRequirer);
}

AbstractModuleRecord* Modules::recordOfRequestedModule(ModuleID requested)
{
    SetForScope isAskingHost(m_isAskingHost, true);
    return moduleRecord(m_instance.globalObject, requested);
}

void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID module, JSValue entry)
{
    Modules* modules = modulesOf(realm);
    if (!modules || !entry.toBoolean(realm))
        return;
    String path = modules->table().pathText(module);
    if (modules->data(module).isOfLibrary()) {
        if (!path.startsWith("node:"_s) && modules->data(module).isHeldUnderNameWithoutPrefix())
            throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return;
    }
    throwLoaderChangeRefused(realm, LoaderChange::ReplaceInRequireCache, path);
}

void willSetRequireCacheEntryUnderName(JSGlobalObject* realm, ModuleID module, const String& name)
{
    Modules* modules = modulesOf(realm);
    if (modules && modules->data(module).isHeldUnderNameWithoutPrefix())
        throwLoaderChangeRefused(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, name);
}

void willChangeRequireOverride(JSGlobalObject* realm, RequireOverride location, bool isOverridden)
{
    if (Modules* modules = modulesOf(realm))
        modules->noteLoaderFunction(location, isOverridden);
}

void willStoreLoaderOfExtension(JSGlobalObject* realm, StringView extension)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    for (unsigned index = 0; index < table.size(); ++index) {
        ModuleID module(index);
        if (!table.hasFilePath(module) || modules->data(module).isOfLibrary())
            continue;
        String path = table.pathText(module);
        if (!StringView(path).left(path.reverseFind('?')).endsWith(extension))
            continue;
        if (!modules->mayBeEvicted(module)) {
            throwLoaderChangeRefused(realm, LoaderChange::ReplaceLoaderFunction, "a loader in Module._extensions"_s);
            return;
        }
        modules->leaveToHostUnlessItHasRun(module);
    }
}

bool isModuleNamespace(JSCell* cell)
{
    return cell->inherits<ModuleNamespace>();
}

bool tryReplaceExport(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue value)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    return modulesOf(object->realm())->tryReplaceExport(object->module(), name, value);
}

bool mayBeMocked(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mayBeMocked(module);
}

bool setMock(JSGlobalObject* realm, ModuleID module, JSObject* mock, JSValue whatRequireAnswers, bool leavesEarlierMockToItsImporters)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, mock ? "setMock"_s : "setMock, none"_s);
    if (modules && mock && modules->isLeftAloneByMock(module))
        return modules->setMockOverModuleThatHasRun(module, mock, whatRequireAnswers);
    return modules && modules->setMockInPlace(module, mock, whatRequireAnswers, leavesEarlierMockToItsImporters);
}

bool willLeaveModuleAlone(JSGlobalObject* realm, ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->willLeaveModuleAlone(module, onlyIfLoadedInPreload, asking);
}

bool isLeftAloneByMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isLeftAloneByMock(module);
}

void didBeginTestFile(JSGlobalObject* realm)
{
    VMState& state = realm->vm().aotState();
    state.momentOfTestFile = ++state.momentOfLoads;
    if (Modules* modules = modulesOf(realm))
        modules->didBeginTestFile();
}

JSObject* mockInPlaceBehind(JSCell* moduleNamespace)
{
    auto* object = dynamicDowncast<ModuleNamespace>(moduleNamespace);
    return object ? object->mockInPlace() : nullptr;
}

bool hasMockInPlace(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mockInPlace(module) && !modules->hasMockThatEnded(module);
}

bool isStandingMockImportedByModuleYetToRun(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> asking)
{
    if (!hasMockInPlace(realm, module))
        return false;
    Modules* modules = modulesOf(realm);
    return modules->isGraphRunning() && modules->hasLinkedImporterYetToRun(module, asking);
}

bool willLeaveStandingMockAlone(JSGlobalObject* realm, ModuleID module, bool staysTheOriginal, bool keepsWhatThisTestFileLoaded)
{
    if (!hasMockInPlace(realm, module))
        return false;
    Modules* modules = modulesOf(realm);
    if (staysTheOriginal || (keepsWhatThisTestFileLoaded && modules->wasLoadedUnderMockInPlaceByThisTestFile(module))) {
        modules->noteCallOfMock(module, true);
        modules->letGoOfImportersOf(module, Modules::ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule);
        return true;
    }
    modules->forgetWhoLoadedItUnderMockInPlace(module);
    modules->letGoOfImportersOf(module, Modules::ImportersLetGo::All);
    return true;
}

bool wasLoadedUnderMockOfPreloadInThisTestFile(JSGlobalObject* realm, ModuleID module)
{
    if (!hasMockInPlace(realm, module))
        return false;
    Modules* modules = modulesOf(realm);
    return modules->mockInPlaceWasLaidInPreload(module) && modules->wasLoadedInThisTestFile(modules->moduleWithExportsOf(module));
}

bool wasRequiredUnderMockInPlace(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasRequiredUnderMockInPlace(module);
}

bool hasWordOfMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->table().wordOfMock(module);
}

void mockExport(JSGlobalObject* realm, ModuleID module, UniquedStringImpl* name, JSValue value)
{
    if (Modules* modules = modulesOf(realm))
        modules->tryReplaceExport(modules->moduleWithExportsOf(module), name, value);
}

bool hasBegunToLoad(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->hasBegunToLoad(module);
}

std::optional<ModuleID> formThatMockFinds(JSGlobalObject* realm, StringView path, std::optional<ModuleID> file)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return file;
    modules->goModuleByModuleFromNowOn();
    return modules->formThatMockFinds(path, file);
}

Vector<ModuleID, 2> formsThatAttributesAskFor(JSGlobalObject* realm, StringView path)
{
    Vector<ModuleID, 2> forms;
    if (Modules* modules = modulesOf(realm))
        forms = modules->table().formsThatAttributesAskFor(path);
    return forms;
}

bool isBeingEvaluated(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isBeingEvaluated(module);
}

void deferMock(JSGlobalObject* realm, ModuleID module, AfterFailure afterFailure)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, afterFailure == AfterFailure::AsksAgain ? "deferMock, asks again"_s : "deferMock"_s);
    if (modules)
        modules->deferMock(module, afterFailure);
}

JSValue exportsBeforeMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "exportsBeforeMock"_s);
    return modules ? modules->exportsBeforeMock(module) : JSValue();
}

bool runMocksBelow(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->runMocksBelowModuleThatLoaderFetches(module);
}

void didEndMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "didEndMock"_s);
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    bool isBack = modules->bringBackMockKeptByImportersOfThisTestFile(module);
    RETURN_IF_EXCEPTION(scope, void());
    if (isBack)
        return;
    modules->forgetCallOfMock(module);
    bool wasLoadedAsMock = modules->mockInPlace(module) && !modules->hasMockThatEnded(module);
    if (wasLoadedAsMock || modules->hasMockLaidOver(module))
        modules->noteEndOfLoadedMock(module);
    if (wasLoadedAsMock || modules->hasMockLaidOver(module) || modules->hasMockOfLibraryModule(module))
        modules->letGoOfImportersOf(module, Modules::ImportersLetGo::All);
    if (modules->isLeftAloneByMock(module)) {
        if (wasLoadedAsMock)
            modules->letGoOfModuleLoadedAsMock(module);
        modules->endMockOverModuleThatHasRun(module);
        RETURN_IF_EXCEPTION(scope, void());
        modules->dropErrorsOfLoadsEndedByMockOf(module);
        return;
    }
    bool isLibraryModule = modules->data(module).isOfLibrary() || !modules->table().hasFilePath(module);
    if (isLibraryModule && modules->letGoOfMockOfLibraryModuleAtNextLoad(module)) {
        modules->forgetMockThatWaits(module);
        modules->dropErrorsOfLoadsEndedByMockOf(module);
        return;
    }
    bool stoodInPlace = modules->mockInPlace(module);
    if (stoodInPlace) {
        modules->bringBackMockOfPreloadThatWasPatched(module);
        RETURN_IF_EXCEPTION(scope, void());
        modules->letGoOfMockAtNextLoad(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    modules->forgetMockThatWaits(module);
    modules->dropErrorsOfLoadsEndedByMockOf(module);
    if (isLibraryModule)
        RELEASE_AND_RETURN(scope, modules->endMockOfLibraryModule(module));
    if (!stoodInPlace)
        RELEASE_AND_RETURN(scope, modules->endMockLaidOverFile(module));
}

void didChangePlugins(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->forgetWhatNamesResolvedTo();
}

void willMockModule(JSGlobalObject* realm, ModuleID module, JSValue whatRequireAnswers, bool exportsAreOfCommonJSModule)
{
    Modules* found = modulesOf(realm);
    if (!found)
        return;
    MockDoor door(found, module, "willMockModule"_s);
    Modules& modules = *found;
    JSObject* exports = whatRequireAnswers.isObject() ? asObject(whatRequireAnswers) : constructEmptyObject(realm);
    modules.setExportsOfMockAreOfCommonJSModule(module, exportsAreOfCommonJSModule);
    if (modules.data(module).isOfLibrary() || !modules.table().hasFilePath(module)) {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        modules.letGoOfMockThatEnded(module);
        RETURN_IF_EXCEPTION(scope, void());
        ModuleStatus status = modules.status(module);
        bool comesAheadOfLoad = modules.data(module).kind == ModuleKind::SourceText && (status == ModuleStatus::Unlinked || status == ModuleStatus::Linked) && !modules.wasLoadedWhenMockWasMade(module);
        modules.willMockLibraryModule(module);
        bool standsInPlace = modules.letMockStandInPlaceOfLibraryModule(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (comesAheadOfLoad) {
            modules.fillExportsOfBodyThatNeverRuns(module);
            modules.leaveOutWhatMockLacks(module, exports);
            RETURN_IF_EXCEPTION(scope, void());
            modules.takeWhatMockAdds(module, true);
        } else if (!standsInPlace) {
            modules.evaluateSynchronously(module, SynchronousEvaluationReason::Require);
            RETURN_IF_EXCEPTION(scope, void());
        }
        modules.mockWhatRequireCacheHolds(module, exports);
        return;
    }
    bool isFirstMock = !modules.isMocked(module);
    modules.willMock(module);
    if (modules.data(module).kind != ModuleKind::CommonJs) {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        ModuleStatus status = modules.status(module);
        bool comesAheadOfLoad = (status == ModuleStatus::Unlinked || status == ModuleStatus::Linked) && !modules.wasLoadedWhenMockWasMade(module);
        if (!comesAheadOfLoad)
            modules.keepWhatMockIsLaidOver(module);
        if (!comesAheadOfLoad && isFirstMock)
            modules.letGoOfImportersOf(module, Modules::ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule);
        if (comesAheadOfLoad) {
            modules.fillExportsOfBodyThatNeverRuns(module);
            modules.leaveOutWhatMockLacks(module, exports);
            RETURN_IF_EXCEPTION(scope, void());
        }
        modules.takeWhatMockAdds(module, comesAheadOfLoad);
        modules.mockWhatRequireAnswered(module, exports);
        return;
    }
    ModuleStatus status = modules.status(module);
    bool hasRun = status != ModuleStatus::Unlinked && status != ModuleStatus::Linked;
    if (!hasRun)
        modules.markEvaluated(module);
    else
        modules.keepWhatMockIsLaidOver(module);
    if (hasRun && isFirstMock)
        modules.letGoOfImportersOf(module, Modules::ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule);
    modules.setCommonJSExports(module, whatRequireAnswers);
    modules.exportWhatMockNamesAlone(module, hasRun);
}

void didMockModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    MockDoor door(modules, module, "didMockModule"_s);
    modules->fillNamesThatMockLacks(module);
    RETURN_IF_EXCEPTION(scope, void());
    modules->answerFirstRequireBehindMock(module);
    RETURN_IF_EXCEPTION(scope, void());
    modules->takeWhatMockAdds(module, false);
    modules->didLayMock();
}

bool hasESModuleMarker(JSCell* moduleNamespace)
{
    return uncheckedDowncast<ModuleNamespace>(moduleNamespace)->hasESModuleMarker();
}

void setESModuleMarker(JSCell* moduleNamespace, bool hasMarker)
{
    uncheckedDowncast<ModuleNamespace>(moduleNamespace)->setESModuleMarker(hasMarker);
}

static constexpr unsigned invalidIndex = static_cast<unsigned>(-1);

const ClassInfo ModuleRecord::s_info = { "CompiledModuleRecord"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleRecord) };

Structure* ModuleRecord::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

ModuleRecord::ModuleRecord(Instance& instance, Structure* structure, JSModuleLoader* loader, const Identifier& moduleKey, ModuleID module)
    : Base(*instance.vm, structure, loader, moduleKey, SourceProviderSourceType::Module)
    , m_instance(instance)
    , m_module(module)
{
}

ModuleRecord* ModuleRecord::create(Instance& instance, Structure* structure, JSModuleLoader* loader, const Identifier& moduleKey, ModuleID module)
{
    auto* record = new (NotNull, allocateCell<ModuleRecord>(*instance.vm)) ModuleRecord(instance, structure, loader, moduleKey, module);
    record->finishCreation(instance.globalObject, *instance.vm);
    return record;
}

void ModuleRecord::destroy(JSCell* cell)
{
    static_cast<ModuleRecord*>(cell)->ModuleRecord::~ModuleRecord();
}

JSGlobalObject* ModuleRecord::instanceRealm() const
{
    return m_instance.globalObject;
}

Modules& ModuleRecord::modules() const
{
    return modulesOf(m_instance);
}

bool ModuleRecord::isMocked() const
{
    return modules().isMockedInItsImage(m_module);
}

unsigned ModuleRecord::innerModuleEvaluation(JSGlobalObject* globalObject, ModuleEvaluationStack& stack, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (hasLiveExports() && modules().recordIfExists(m_module) != this) [[unlikely]] {
        if (status() != Status::Evaluated) {
            setStatus(Status::Evaluated);
            setCycleRoot(globalObject->vm(), this);
        }
        return index;
    }
    if (takeRunsInItsTurn()) [[unlikely]] {
        modules().evaluateFilesOfLevel(m_module, true);
        RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    }
    EvaluationContext context { stack, index, referrerAsyncOrder < 0 ? std::nullopt : std::optional(referrerAsyncOrder), dynamicImportPromise };
    modules().innerModuleEvaluation(m_module, context);
    Exception* exception = scope.exception();
    if (!exception) [[likely]]
        return context.index;
    if (globalObject->vm().isTerminationException(exception) && status() == Status::Evaluated && !evaluationError()) {
        setEvaluationError(globalObject->vm(), exception->value());
        modules().didChangeStatusInRecord(m_module);
    }
    return invalidIndex;
}

void ModuleRecord::willLoadRequests(ModuleGraphLoadingState& load)
{
    countEvent(vm(), RuntimeCounterID::RecordsForStatesOfLoads);
    modules().appendRequestsForLoader(m_module, this, load);
}

void ModuleRecord::countLoaderStep()
{
    modules().noteStepOfLoader();
}

void ModuleRecord::didLoad()
{
    m_runsInItsTurn &= !m_runsWhenLoaded;
    modules().evaluateFilesOfLevel(m_module, m_runsWhenLoaded);
}

void ModuleRecord::didFailToLoad(JSGlobalObject* globalObject, const ModuleGraphLoadingState& state)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    DeferTerminationForAWhile deferTermination(vm);
    Exception* exception = scope.exception();
    if (exception)
        TRY_CLEAR_EXCEPTION(scope, void());
    state.iterateVisited([&](CyclicModuleRecord* visited) {
        auto* compiled = dynamicDowncast<ModuleRecord>(visited);
        if (!compiled || scope.exception())
            return;
        compiled->modules().didFailToLoadGraph(compiled->m_module);
        if (!scope.exception() && compiled->modules().hasDeferredLoads()) [[unlikely]]
            compiled->modules().didSettleLoad(state);
    });
    if (!scope.exception() && exception)
        throwException(globalObject, scope, exception);
}

std::optional<ModuleID> ModuleRecord::fileToWaitFor(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    return modules ? modules->fileToWaitFor(key) : std::nullopt;
}

JSPromise* ModuleRecord::deferRequest(JSGlobalObject* globalObject, ModuleID file, ModuleLoadingContext* context, bool useImportMap)
{
    return modulesOf(globalObject)->deferRequest(file, context, useImportMap);
}

void ModuleRecord::didRequestModule(JSGlobalObject* globalObject, const Identifier& key, JSCell* payload)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules)
        return;
    modules->noteStepOfLoader();
    if (modules->hasDeferredLoads()) [[unlikely]]
        modules->didRequestModule(key, payload);
}

void ModuleRecord::didSettleLoad(JSGlobalObject* globalObject, const ModuleGraphLoadingState& load)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->hasDeferredLoads()) [[unlikely]]
        modules->didSettleLoad(load);
}

void ModuleRecord::didFailToMakeModule(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->hasDeferredLoads()) [[unlikely]]
        modules->didFailToMakeModule(key);
}

void ModuleRecord::didLoseErrorOfModule(JSGlobalObject* globalObject, const Identifier& key)
{
    if (Modules* modules = modulesOf(globalObject))
        modules->didLoseErrorOfModule(key);
}

void ModuleRecord::didReplaceRecordInRegistry()
{
    modules().captureExportsAgain(m_module);
}

void ModuleRecord::willBeLoadedByImport()
{
    modules().willBeLoadedByImport(m_module);
}

bool ModuleRecord::takesEveryStepOfLoader()
{
    return modules().isLoadedByLoader(m_module);
}

bool ModuleRecord::takeStepOfMainModule()
{
    return !requestedModules().isEmpty() && modules().takeStepOfMainModule(m_module);
}

bool ModuleRecord::takeLoadPromiseBelowMainModule()
{
    return !requestedModules().isEmpty() && modules().takeLoadPromiseBelowMainModule(m_module);
}

JSPromise* ModuleRecord::evaluateMainModule(JSPromise* evaluation)
{
    return modules().evaluateMainModule(m_module, evaluation);
}

void ModuleRecord::didEvaluateModulesInJob(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->mainModuleWaitsForItsImport()) [[unlikely]]
        modules->runMainModuleThatWaited();
}

void ModuleRecord::didFailModulesInJob(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (modules && modules->mainModuleWaitsForItsImport()) [[unlikely]]
        modules->failMainModuleThatWaited();
}

void ModuleRecord::takeStatus()
{
    if (!m_holdsStatus)
        modules().ensureRecord(m_module);
}

bool ModuleRecord::isRunningCommonJSFile()
{
    return modules().isCommonJSFileOrFace(m_module) && modules().isImportedByLoader(m_module) && modules().status(m_module) == ModuleStatus::Evaluating;
}

BitVector ModuleRecord::letAllImagesAskHost(JSGlobalObject* globalObject)
{
    BitVector haveSetRequestAside;
    if (Modules* modules = modulesOf(globalObject)) {
        modules->setRequestToHostAside(true);
        haveSetRequestAside.set(0);
    }
    return haveSetRequestAside;
}

void ModuleRecord::restoreImagesThatAskHost(JSGlobalObject* globalObject, const BitVector& haveSetRequestAside)
{
    if (haveSetRequestAside.get(0))
        modulesOf(globalObject)->takeUpRequestToHost();
}

uint8_t ModuleRecord::willAskHostForRequest(const Identifier& key)
{
    bool wasWalkingRequests = modules().setIsWalkingRequests(!m_compiledRequests.isEmpty());
    modules().setRequestToHostAside(true);
    bool wasStatementAskingHost = modules().setIsStatementAskingHost(true);
    if (m_hasRequestThatFindsNothing) [[unlikely]]
        modules().keepFailureOfRequest(m_module, this, key);
    return static_cast<uint8_t>(wasWalkingRequests) << 1 | static_cast<uint8_t>(wasStatementAskingHost) << 2;
}

void ModuleRecord::didAskHostForRequest(uint8_t flagsBefore)
{
    modules().takeUpRequestToHost();
    modules().setIsWalkingRequests(flagsBefore & 2);
    modules().setIsStatementAskingHost(flagsBefore & 4);
}

uint8_t ModuleRecord::willAskHostForUncompiledModule(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules)
        return 0;
    bool wasWalkingRequests = modules->setIsWalkingRequests(false);
    modules->setRequestToHostAside(false);
    bool wasStatementAskingHost = modules->setIsStatementAskingHost(false);
    constexpr uint8_t hasSetRequestAside = 1;
    return hasSetRequestAside | static_cast<uint8_t>(wasWalkingRequests) << 1 | static_cast<uint8_t>(wasStatementAskingHost) << 2;
}

void ModuleRecord::didAskHostForUncompiledModule(JSGlobalObject* globalObject, uint8_t flagsBefore)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || !(flagsBefore & 1))
        return;
    modules->takeUpRequestToHost();
    modules->setIsWalkingRequests(flagsBefore & 2);
    modules->setIsStatementAskingHost(flagsBefore & 4);
}

void ModuleRecord::willAnswerRequestOfUncompiledModule(JSGlobalObject* globalObject, const Identifier& key, const Identifier& keyOfAsker)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || key.isSymbol())
        return;
    std::optional<ModuleID> found = modules->table().findByPath(key.string());
    if (!found)
        return;
    if (modules->table().someExportStaysIn(*found)) {
        modules->refuseToShowExportsOf(*found);
        return;
    }
    AskerOfModule asker(globalObject, keyOfAsker.isSymbol() ? emptyString() : keyOfAsker.string());
    willAnswerWithModule(globalObject, *found, LoadByName::Import);
}

void ModuleRecord::willAnswerFromRegistry(JSGlobalObject* globalObject, const Identifier& key, const String& pathOfAsker)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || key.isSymbol())
        return;
    std::optional<ModuleID> found = modules->table().findByPath(key.string());
    if (!found || modules->isRootThatCompiledCodeLoads(*found))
        return;
    AskerOfModule asker(globalObject, pathOfAsker);
    willAnswerWithModule(globalObject, *found, LoadByName::Import);
}

AbstractModuleRecord* ModuleRecord::recordOfRequest(JSGlobalObject*, const Identifier& key)
{
    for (unsigned index = m_nextCompiledRequest; index < m_compiledRequests.size(); ++index) {
        if (m_compiledRequests[index].first != key.impl())
            continue;
        m_nextCompiledRequest = index + 1;
        return modules().recordOfRequestedModule(m_compiledRequests[index].second);
    }
    return nullptr;
}

bool ModuleRecord::holdTextOfRequest(JSPromise* fetched, AbstractModuleRecord* requested)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(requested);
    return compiled && modules().holdTextOfRequest(fetched, compiled, m_module);
}

void ModuleRecord::generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords& visited)
{
    modules().captureSyntheticExports(m_module, visited);
}

unsigned ModuleRecord::innerModuleLinking(JSGlobalObject* globalObject, ModuleLinkingStack& stack, unsigned index, RefPtr<ScriptFetcher> scriptFetcher)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    LinkContext context { stack, index, WTF::move(scriptFetcher) };
    modules().queueJobsThatWaitForLoader();
    modules().didLoadGraph(m_module);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    modules().finishLink(m_module, context);
    if (!scope.exception() && status() == Status::Unlinked && !modules().data(m_module).needsLink())
        setStatus(Status::Linked);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::execute(JSGlobalObject*, JSPromise* capability)
{
    RELEASE_ASSERT(!capability);
    modules().callBodyOfModuleEnteredByLoader(m_module);
}

void ModuleRecord::executeAsync(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(status() == Status::Evaluating || status() == Status::EvaluatingAsync);
    ASSERT(hasTLA());

    JSValue result = modules().callBodyOfModuleEnteredByLoader(m_module);
    RETURN_IF_EXCEPTION(scope, void());
    auto* promise = dynamicDowncast<JSPromise>(result);
    if (!promise) {
        promise = JSPromise::resolvedPromise(globalObject, result);
        RETURN_IF_EXCEPTION(scope, void());
    }
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncModuleExecutionDone, nullptr, this);
}

bool ModuleRecord::standsForMock() const
{
    return (modules().isMocked(m_module) || modules().mockInPlace(m_module)) && !modules().hasMockLaidOver(m_module);
}

bool ModuleRecord::someMockStandsInPlace() const
{
    return modules().someMockStandsInPlace();
}

void ModuleRecord::appendDynamicStarTargets(StarExportTargets& targets)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    Modules& modules = this->modules();
    if (!modules.data(m_module).hasDynamicStarExport() || modules.standsForWholeModule(m_module))
        return;
    for (const ModuleRequestData& request : modules.table().requests(m_module)) {
        if (!request.isDynamicStarExport || modules.isMockedByFactoryThatTakesOriginal(request.targetModule()))
            continue;
        AbstractModuleRecord* target = modules.recordForResolution(request.targetModule());
        RETURN_IF_EXCEPTION(scope, void());
        targets.append(target);
    }
}

static bool takesNamesFromValue(const ModuleData& data)
{
    return data.capturesExports() || data.kind == ModuleKind::CommonJs;
}

auto ModuleRecord::resolveOwnExport(JSGlobalObject* globalObject, const Identifier& exportName, StarExportTargets& starTargets) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);
    bool isDefault = exportName == vm.propertyNames->defaultKeyword;
    Resolution absent = isDefault ? Resolution::error() : Resolution::notFound();

    retain();
    if (takesNamesFromValue(data)) {
        bool hasExport = modules.hasCapturedExport(modules.moduleWithNamesOf(m_module), exportName);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        return hasExport ? Resolution { Resolution::Type::Resolved, this, exportName } : absent;
    }

    if (m_resolveSet.contains(exportName.impl()))
        return Resolution::notFound();

    std::optional<unsigned> index = modules.table().findExport(m_module, exportName.impl());
    const ModuleExportData* entry = index ? &modules.table().exports(m_module)[*index] : nullptr;
    if (!entry && modules.isAddedByMock(m_module, exportName))
        return { Resolution::Type::Resolved, this, exportName };
    if (!entry && modules.table().isAmbiguousExport(m_module, exportName.impl()) && !modules.namesBehindStarsAreInTables(m_module))
        return Resolution::ambiguous();
    if (!isDefault && (!entry || entry->isFromStarExport || modules.leavesNameToStars(m_module, *index))) {
        appendDynamicStarTargets(starTargets);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
    }
    if (!entry && !isDefault && modules.data(m_module).hasDynamicStarExport() && !modules.standsForWholeModule(m_module)) {
        for (const ModuleRequestData& request : modules.table().requests(m_module)) {
            if (!request.isDynamicStarExport || !modules.isMockedByFactoryThatTakesOriginal(request.targetModule()))
                continue;
            bool isNameOfOriginal = modules.table().findExport(request.targetModule(), exportName.impl()) || modules.hasNameBehindItsStars(request.targetModule(), exportName.impl());
            RETURN_IF_EXCEPTION(scope, Resolution::error());
            if (!isNameOfOriginal)
                continue;
            AbstractModuleRecord* record = modules.recordForResolution(request.targetModule());
            RETURN_IF_EXCEPTION(scope, Resolution::error());
            return { Resolution::Type::Resolved, record, exportName };
        }
    }
    if (!entry || modules.isLeftOut(m_module, *index, Modules::TablesBehindStars::MayLackName))
        return absent;
    if (std::optional<Modules::AnswerOfMock> answer = modules.mockThatAnswersFor(m_module, *entry)) [[unlikely]] {
        AbstractModuleRecord* record = modules.recordForResolution(answer->mocked);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (record != this)
            RELEASE_AND_RETURN(scope, record->resolveExport(globalObject, answer->name));
    }
    if (entry->kind() == ModuleExportKind::Variable || (entry->kind() == ModuleExportKind::Namespace && entry->isDeferred))
        return { Resolution::Type::Resolved, this, exportName, *index };
    if (entry->kind() == ModuleExportKind::Namespace) {
        AbstractModuleRecord* record = modules.recordForResolution(entry->targetModule());
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        return { Resolution::Type::Resolved, record, vm.propertyNames->starNamespacePrivateName };
    }
    RELEASE_ASSERT(entry->kind() == ModuleExportKind::Indirect);
    const IndirectExportData& indirectExport = modules.table().indirectExport(*entry);
    AbstractModuleRecord* record = modules.recordForResolution(indirectExport.targetModule());
    RETURN_IF_EXCEPTION(scope, Resolution::error());
    if (!vm.isSafeToRecurse()) [[unlikely]] {
        throwException(globalObject, scope, createModuleGraphTooDeepError(globalObject));
        return Resolution::error();
    }
    m_resolveSet.append(exportName.impl());
    auto removeFromResolveSet = makeScopeExit([&] {
        m_resolveSet.removeLast();
    });
    RELEASE_AND_RETURN(scope, record->resolveExport(globalObject, modules.identifier(indirectExport.importName)));
}

static constinit MissingExportListener missingExportListener = nullptr;

void setMissingExportListener(MissingExportListener listener)
{
    missingExportListener = listener;
}

bool ModuleRecord::throwIfExportWasLeftOut(JSGlobalObject* globalObject, AbstractModuleRecord* importer, AbstractModuleRecord* exporter, const Identifier& name)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(exporter);
    if (!compiled || compiled->modules().data(compiled->m_module).isEntryPoint())
        return false;
    if (missingExportListener)
        missingExportListener(name.string(), exporter->moduleKey().string(), importer->moduleKey().string());
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    throwSyntaxError(globalObject, scope, makeString("Export named '"_s, name.string(), "' not found in module '"_s, exporter->moduleKey().string(), "'. The build keeps only the exports that something refers to: if the module exports it, this is a bug in Bun."_s));
    return true;
}

void ModuleRecord::collectExportedNames(JSGlobalObject* globalObject, WTF::Vector<Identifier>& names, StarExportTargets& starTargets)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();

    if (takesNamesFromValue(modules.data(m_module))) {
        RELEASE_AND_RETURN(scope, modules.forEachCapturedExport(modules.moduleWithNamesOf(m_module), [&](const Identifier& name, JSValue) {
            names.append(name);
        }));
    }
    std::span<const ModuleExportData> entries = modules.table().exports(m_module);
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (!hasLiveExports() || !modules.isLeftOut(m_module, index, Modules::TablesBehindStars::HaveName))
            names.append(modules.identifier(entries[index].name));
    }
    modules.appendNamesAddedByMock(m_module, names);
    RETURN_IF_EXCEPTION(scope, void());
    for (const AmbiguousExportData& entry : modules.table().ambiguousExports(m_module))
        names.append(Identifier::fromString(globalObject->vm(), ProgramStrings::text(modules.table().programTables(), entry.nameString())));
    RELEASE_AND_RETURN(scope, appendDynamicStarTargets(starTargets));
}

JSValue ModuleRecord::readExport(JSGlobalObject* globalObject, const Identifier& exportName, std::optional<unsigned> exportIndex)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (JSObject* mock = modules().mockInPlace(m_module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(globalObject, exportName));
    if (exportIndex && modules().table().exports(m_module)[*exportIndex].staysIn) {
        modules().refuseToShowExportsOf(m_module);
        return { };
    }
    if (exportIndex)
        RELEASE_AND_RETURN(scope, modules().readExport(m_module, *exportIndex));
    if (takesNamesFromValue(modules().data(m_module))) {
        JSValue value = modules().capturedValue(modules().moduleWithNamesOf(m_module), exportName);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    JSObject* namespaceObject = modules().moduleNamespace(m_module, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, namespaceObject->get(globalObject, exportName));
}

JSObject* ModuleRecord::moduleNamespaceObject(JSGlobalObject*, ModulePhase phase)
{
    return modules().moduleNamespace(m_module, phase);
}

static const void* identityOfModule(const Modules& modules, ModuleID module)
{
    JSValue value = modules.data(module).isHostValue() ? modules.value(module) : JSValue();
    return value && value.isObject() ? static_cast<const void*>(value.asCell()) : &modules.data(module);
}

using IdentityOfBinding = std::tuple<std::optional<SlotData>, const void*, UniquedStringImpl*>;

static IdentityOfBinding identityOfBinding(const Modules& modules, const AbstractModuleRecord::Resolution& resolution)
{
    ModuleID module = uncheckedDowncast<ModuleRecord>(resolution.moduleRecord)->module();
    UniquedStringImpl* name = resolution.localName.impl();
    if (resolution.compiledExportIndex) {
        const ModuleExportData& entry = modules.table().exports(module)[*resolution.compiledExportIndex];
        if (entry.kind() == ModuleExportKind::Variable)
            return { entry.variable, nullptr, nullptr };
        RELEASE_ASSERT(entry.kind() == ModuleExportKind::Namespace && entry.isDeferred);
        return { std::nullopt, identityOfModule(modules, entry.targetModule()), nullptr };
    }
    for (const SyntheticExportData& syntheticExport : modules.table().syntheticExports(module)) {
        if (syntheticExport.hasVariable && modules.identifier(syntheticExport.name).impl() == name)
            return { syntheticExport.variable, nullptr, nullptr };
    }
    return { std::nullopt, identityOfModule(modules, module), name };
}

bool ModuleRecord::isSameBinding(const Resolution& first, const Resolution& second)
{
    auto* firstRecord = uncheckedDowncast<ModuleRecord>(first.moduleRecord);
    if (&firstRecord->m_instance != &uncheckedDowncast<ModuleRecord>(second.moduleRecord)->m_instance)
        return false;
    Modules& modules = firstRecord->modules();
    return identityOfBinding(modules, first) == identityOfBinding(modules, second);
}

void ModuleRecord::gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen)
{
    modules().gatherAsynchronousTransitiveDependencies(m_module, result, seen);
}

bool ModuleRecord::readyForSyncExecution(VisitedModuleRecords& seen)
{
    return modules().readyForSyncExecution(m_module, seen);
}

const ClassInfo ModuleNamespace::s_info = { "Module"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleNamespace) };

Structure* ModuleNamespace::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool shouldPreventExtensions)
{
    Structure* structure = Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
    return shouldPreventExtensions ? Structure::preventExtensionsTransition(vm, structure) : structure;
}

ModuleNamespace::ModuleNamespace(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
    : Base(vm, structure)
    , m_dynamicExportNames(dynamicExports.names, WriteBarrierEarlyInit)
    , m_capturedValues(dynamicExports.capturedValues, WriteBarrierEarlyInit)
    , m_liveSources(dynamicExports.liveSources, WriteBarrierEarlyInit)
    , m_module(module)
    , m_isDeferred(phase == ModulePhase::Defer)
    , m_mayNeedEvaluation(m_isDeferred && modules().data(module).hasExportTable())
{
}

ModuleNamespace* ModuleNamespace::create(VM& vm, Structure* structure, ModuleID module, ModulePhase phase, const DynamicExports& dynamicExports)
{
    ModuleNamespace* object = new (NotNull, allocateCell<ModuleNamespace>(vm)) ModuleNamespace(vm, structure, module, phase, dynamicExports);
    object->finishCreation(vm);
    return object;
}

Modules& ModuleNamespace::modules() const
{
    return modulesOf(*realm()->aotInstance());
}

std::span<const ModuleExportData> ModuleNamespace::exports() const
{
    return modules().table().exports(m_module);
}

bool ModuleNamespace::isHiddenExport(unsigned index) const
{
    if (keepsToModule()) [[unlikely]]
        return modules().isHiddenInModuleItself(m_module, index);
    return (exports()[index].isFromStarExport && modules().isExportAmbiguous(m_module, index)) || modules().isLeftOut(m_module, index, Modules::TablesBehindStars::MayLackName);
}

void ModuleNamespace::ensureOwnStructure(VM& vm)
{
    if (hasOwnStructure() || isValidOffset(structure()->maxOffset()))
        return;
    setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype(), !structure()->isStructureExtensible()));
    m_hasOwnStructure = true;
}

template<typename Visitor>
void ModuleNamespace::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_dynamicExportNames);
    visitor.append(thisObject->m_capturedValues);
    visitor.append(thisObject->m_liveSources);
    visitor.append(thisObject->m_mockOfView);
}

DEFINE_VISIT_CHILDREN(ModuleNamespace);

std::optional<unsigned> ModuleNamespace::findWhetherShownOrNot(UniquedStringImpl* name) const
{
    if (isSymbolLikeKey(name))
        return std::nullopt;
    std::optional<unsigned> index = modules().table().findExport(m_module, name);
    return index && !isHiddenExport(*index) ? index : std::nullopt;
}

std::optional<unsigned> ModuleNamespace::find(UniquedStringImpl* name) const
{
    std::optional<unsigned> index = findWhetherShownOrNot(name);
    return index && !exports()[*index].staysIn ? index : std::nullopt;
}

bool ModuleNamespace::tryReadExports(JSGlobalObject* globalObject, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values)
{
    if (m_isDeferred || m_mayNeedEvaluation || m_dynamicExportNames || isValidOffset(structure()->maxOffset()))
        return false;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Modules& modules = this->modules();
    if (modules.mockInPlace(m_module) || m_mockOfView) [[unlikely]]
        return false;
    std::span<const ModuleExportData> entries = exports();
    if (modules.table().someExportStaysIn(m_module))
        return false;
    for (unsigned index = 0; index < entries.size(); ++index) {
        if (isHiddenExport(index))
            continue;
        JSValue value = modules.readExport(m_module, index);
        RETURN_IF_EXCEPTION(scope, true);
        names.append(modules.identifier(entries[index].name).impl());
        values.appendWithCrashOnOverflow(value);
    }
    return true;
}

AbstractModuleRecord* ModuleNamespace::recordForResolution()
{
    if (keepsToModule()) [[unlikely]]
        return nullptr;
    return modules().recordForResolution(m_module);
}

JSObject* ModuleNamespace::mockInPlace() const
{
    if (JSObject* ofView = m_mockOfView.get()) [[unlikely]]
        return ofView;
    return keepsToModule() ? nullptr : modules().mockInPlace(m_module);
}

bool ModuleNamespace::isAnsweredByMock() const
{
    return keepsToModule() || !!mockInPlace();
}

bool ModuleNamespace::isSymbolLikeKey(UniquedStringImpl* name) const
{
    return name->isSymbol() || (m_isDeferred && name == vm().propertyNames->then.impl());
}

bool ModuleNamespace::isOrdinaryKey(PropertyName name) const
{
    VM& vm = this->vm();
    if (name == vm.propertyNames->toStringTagSymbol)
        return false;
    return isSymbolLikeKey(name.uid()) || (name == vm.propertyNames->__esModule && !hasExport(name.uid()));
}

bool ModuleNamespace::evaluateIfNeeded(JSGlobalObject* globalObject)
{
    if (!m_mayNeedEvaluation)
        return true;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    modules().evaluateSynchronously(m_module, SynchronousEvaluationReason::DeferredNamespace);
    RETURN_IF_EXCEPTION(scope, false);
    m_mayNeedEvaluation = false;
    return true;
}

void ModuleNamespace::replaceCapturedValue(JSGlobalObject* globalObject, const Identifier& name, JSValue value)
{
    if (JSObject* capturedValues = m_capturedValues.get())
        capturedValues->putDirectMayBeIndex(globalObject, name, value);
    leaveStructureThatCachesKnow(globalObject->vm());
}

void ModuleNamespace::replaceDynamicExports(VM& vm, const DynamicExports& dynamicExports)
{
    m_dynamicExportNames.setMayBeNull(vm, this, dynamicExports.names);
    m_capturedValues.setMayBeNull(vm, this, dynamicExports.capturedValues);
    m_liveSources.setMayBeNull(vm, this, dynamicExports.liveSources);
    leaveStructureThatCachesKnow(vm);
}

void ModuleNamespace::leaveStructureThatCachesKnow(VM& vm)
{
    if (!m_hasOwnStructure)
        return;
    if (isValidOffset(structure()->maxOffset()))
        convertToDictionary(vm);
    else
        setStructure(vm, createStructure(vm, realm(), structure()->storedPrototype(), !structure()->isStructureExtensible()));
}

JSValue ModuleNamespace::capturedValueWithoutAllocating(UniquedStringImpl* name) const
{
    JSObject* table = m_capturedValues.get();
    if (!table || m_mayNeedEvaluation || parseIndex(*name) || !isDynamicExport(name) || modules().mockInPlace(m_module))
        return { };
    unsigned attributes;
    auto offsetIn = [&](JSObject* object) {
        return object->structure()->getWithoutMaterializing(name, attributes);
    };
    if (JSObject* liveSources = m_liveSources.get()) {
        if (std::optional<PropertyOffset> offset = offsetIn(liveSources); !offset || isValidOffset(*offset))
            return { };
    }
    std::optional<PropertyOffset> offset = offsetIn(table);
    return offset && isValidOffset(*offset) ? table->getDirect(*offset) : JSValue();
}

bool ModuleNamespace::isDynamicExport(UniquedStringImpl* name) const
{
    JSCellButterfly* names = m_dynamicExportNames.get();
    if (!names || isSymbolLikeKey(name))
        return false;
    unsigned from = 0;
    unsigned to = names->length();
    while (from < to) {
        unsigned middle = from + (to - from) / 2;
        const StringImpl* candidate = asString(names->get(middle))->tryGetValueImpl();
        if (candidate == name)
            return true;
        if (codePointCompare(StringView(*candidate), StringView(*name)) < 0)
            from = middle + 1;
        else
            to = middle;
    }
    return false;
}

JSValue ModuleNamespace::readDynamicExport(JSGlobalObject* globalObject, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Identifier identifier = Identifier::fromUid(globalObject->vm(), name.uid());
    if (JSObject* liveSources = m_liveSources.get()) {
        JSValue source = Modules::storedValue(globalObject, liveSources, identifier);
        RETURN_IF_EXCEPTION(scope, { });
        if (source)
            RELEASE_AND_RETURN(scope, asObject(source)->get(globalObject, name));
    }
    if (JSValue captured = Modules::storedValue(globalObject, m_capturedValues.get(), identifier))
        return captured;
    JSValue made = modules().capturedValue(m_module, identifier);
    RETURN_IF_EXCEPTION(scope, { });
    if (made) {
        m_capturedValues->putDirectMayBeIndex(globalObject, identifier, made);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return made;
}

static bool canAskMock(JSGlobalObject* globalObject, ThrowScope& scope)
{
    if (globalObject->vm().isSafeToRecurseSoft()) [[likely]]
        return true;
    throwStackOverflowError(globalObject, scope);
    return false;
}

bool ModuleNamespace::getOwnPropertySlotOfMock(JSGlobalObject* globalObject, JSObject* mock, PropertyName name, PropertySlot& slot)
{
    using InternalMethodType = PropertySlot::InternalMethodType;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (slot.internalMethodType() == InternalMethodType::VMInquiry) {
        slot.setValue(this, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
        return false;
    }
    if (!canAskMock(globalObject, scope)) [[unlikely]]
        return false;
    if (slot.internalMethodType() != InternalMethodType::Get || name == vm.propertyNames->__esModule) {
        bool hasExport = mock->hasProperty(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!hasExport) {
            if (name == vm.propertyNames->__esModule)
                RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(this, globalObject, name, slot));
            return false;
        }
        if (slot.internalMethodType() == InternalMethodType::HasProperty) {
            slot.setValue(this, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
            return true;
        }
    }
    JSValue value = mock->get(globalObject, name);
    RETURN_IF_EXCEPTION(scope, false);
    slot.setValue(this, static_cast<unsigned>(PropertyAttribute::None), value);
    return true;
}

static JSC_DECLARE_CUSTOM_GETTER(refuseToHandOutExport);

JSC_DEFINE_CUSTOM_GETTER(refuseToHandOutExport, (JSGlobalObject*, EncodedJSValue thisValue, PropertyName))
{
    uncheckedDowncast<ModuleNamespace>(JSValue::decode(thisValue))->refuseToShowExports();
    return { };
}

void ModuleNamespace::refuseToShowExports()
{
    modules().refuseToShowExportsOf(m_module);
}

bool ModuleNamespace::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    using InternalMethodType = PropertySlot::InternalMethodType;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    InternalMethodType method = slot.internalMethodType();
    slot.setIsTaintedByOpaqueObject();
    if (method != InternalMethodType::VMInquiry && !thisObject->isSymbolLikeKey(name.uid())) {
        thisObject->evaluateIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->mockInPlace()) [[unlikely]]
        RELEASE_AND_RETURN(scope, thisObject->getOwnPropertySlotOfMock(globalObject, mock, name, slot));
    std::optional<unsigned> index = thisObject->findWhetherShownOrNot(name.uid());
    if (!index && !thisObject->isDynamicExport(name.uid())) {
        if (thisObject->isOrdinaryKey(name))
            RELEASE_AND_RETURN(scope, Base::getOwnPropertySlot(thisObject, globalObject, name, slot));
        if (name != vm.propertyNames->toStringTagSymbol)
            return false;
        slot.setValue(thisObject, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete, jsNontrivialString(vm, thisObject->m_isDeferred ? "Deferred Module"_s : "Module"_s));
        return true;
    }
    if (method == InternalMethodType::VMInquiry) {
        slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::None), jsUndefined());
        return false;
    }
    JSValue value = jsUndefined();
    if (method != InternalMethodType::HasProperty) {
        if (index && thisObject->keepsToModule()) [[unlikely]]
            value = thisObject->modules().readExportOfModuleItself(thisObject->m_module, *index);
        else
            value = index ? thisObject->modules().readExport(thisObject->m_module, *index) : thisObject->readDynamicExport(globalObject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (index && thisObject->exports()[*index].staysIn) {
            slot.setCustom(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), refuseToHandOutExport);
            return true;
        }
    }
    slot.setValue(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), value);
    return true;
}

bool ModuleNamespace::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject* globalObject, unsigned index, PropertySlot& slot)
{
    return getOwnPropertySlot(object, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

void ModuleNamespace::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    thisObject->evaluateIfNeeded(globalObject);
    RETURN_IF_EXCEPTION(scope, void());
    Modules& modules = thisObject->modules();
    std::span<const ModuleExportData> entries = thisObject->exports();
    JSCellButterfly* dynamicNames = thisObject->m_dynamicExportNames.get();
    unsigned dynamicNameCount = dynamicNames ? dynamicNames->length() : 0;
    if (JSObject* mock = thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return;
        PropertyNameArrayBuilder namesOfMock(vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        mock->methodTable()->getOwnPropertyNames(mock, globalObject, namesOfMock, DontEnumPropertiesMode::Exclude);
        RETURN_IF_EXCEPTION(scope, void());
        for (const Identifier& name : namesOfMock)
            names.add(name);
        entries = { };
        dynamicNameCount = 0;
    }
    for (unsigned ownIndex = 0, dynamicIndex = 0; ownIndex < entries.size() || dynamicIndex < dynamicNameCount;) {
        if (ownIndex < entries.size() && thisObject->isHiddenExport(ownIndex)) {
            ++ownIndex;
            continue;
        }
        Identifier name = ownIndex < entries.size() ? modules.identifier(entries[ownIndex].name) : Identifier();
        bool isAnsweredByMock = false;
        bool isTakenFromValue = false;
        if (dynamicIndex == dynamicNameCount || (!name.isNull() && codePointCompare(StringView(name.string()), StringView(*asString(dynamicNames->get(dynamicIndex))->tryGetValueImpl())) < 0))
            isAnsweredByMock = !!modules.mockThatAnswersFor(thisObject->m_module, entries[ownIndex++]);
        else {
            name = asString(dynamicNames->get(dynamicIndex++))->toIdentifier(globalObject);
            isTakenFromValue = !thisObject->m_liveSources && modules.isNativeModule(thisObject->m_module);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (mode == DontEnumPropertiesMode::Exclude && !isAnsweredByMock) {
            PropertySlot slot(thisObject, PropertySlot::InternalMethodType::GetOwnProperty);
            bool hasProperty = isTakenFromValue ? modules.hasCapturedExport(thisObject->m_module, name) : getOwnPropertySlot(thisObject, globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, void());
            if (!hasProperty)
                continue;
        }
        names.add(name);
    }
    if (!names.includeSymbolProperties())
        return;
    unsigned index = 0;
    std::optional<PropertyNameArrayBuilder> structureNames;
    if (isValidOffset(thisObject->structure()->maxOffset())) [[unlikely]] {
        structureNames.emplace(vm, PropertyNameMode::StringsAndSymbols, names.privateSymbolMode());
        thisObject->structure()->getPropertyNamesFromStructure(vm, *structureNames, mode);
        for (; index < structureNames->size() && !(*structureNames)[index].isSymbol(); ++index)
            names.add((*structureNames)[index]);
    }
    if (mode == DontEnumPropertiesMode::Include)
        names.add(vm.propertyNames->toStringTagSymbol);
    for (; structureNames && index < structureNames->size(); ++index)
        names.add((*structureNames)[index]);
}

bool ModuleNamespace::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (name == vm.propertyNames->__esModule && !thisObject->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, Base::put(cell, globalObject, name, value, slot));
    if (JSObject* mock = thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        if (isThisValueAltered(slot, thisObject)) [[unlikely]]
            RELEASE_AND_RETURN(scope, ordinarySetSlow(globalObject, thisObject, name, value, slot.thisValue(), slot.isStrictMode()));
        PutPropertySlot slotOfMock(mock, slot.isStrictMode());
        RELEASE_AND_RETURN(scope, mock->methodTable()->put(mock, globalObject, name, value, slotOfMock));
    }
    return typeError(globalObject, scope, slot.isStrictMode(), ReadonlyPropertyWriteError);
}

bool ModuleNamespace::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (JSObject* mock = thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        RELEASE_AND_RETURN(scope, mock->methodTable()->putByIndex(mock, globalObject, index, value, shouldThrow));
    }
    return typeError(globalObject, scope, shouldThrow, ReadonlyPropertyWriteError);
}

bool ModuleNamespace::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(cell);
    if (thisObject->isSymbolLikeKey(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol)
        RELEASE_AND_RETURN(scope, Base::deleteProperty(thisObject, globalObject, name, slot));
    if (!thisObject->isSymbolLikeKey(name.uid())) {
        bool isEvaluated = thisObject->evaluateIfNeeded(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isEvaluated)
            return false;
    }
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        DeletePropertySlot slotOfMock;
        RELEASE_AND_RETURN(scope, mock->methodTable()->deleteProperty(mock, globalObject, name, slotOfMock));
    }
    RELEASE_AND_RETURN(scope, !thisObject->hasExport(name.uid()) && name != globalObject->vm().propertyNames->toStringTagSymbol);
}

bool ModuleNamespace::deletePropertyByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index)
{
    DeletePropertySlot slot;
    return deleteProperty(cell, globalObject, Identifier::from(globalObject->vm(), index), slot);
}

bool ModuleNamespace::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* thisObject = uncheckedDowncast<ModuleNamespace>(object);
    if (thisObject->isOrdinaryKey(name))
        RELEASE_AND_RETURN(scope, Base::defineOwnProperty(thisObject, globalObject, name, descriptor, shouldThrow));
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->mockInPlace()) [[unlikely]] {
        if (!canAskMock(globalObject, scope)) [[unlikely]]
            return false;
        RELEASE_AND_RETURN(scope, mock->methodTable()->defineOwnProperty(mock, globalObject, name, descriptor, shouldThrow));
    }
    PropertyDescriptor current;
    bool isCurrentDefined = object->getOwnPropertyDescriptor(globalObject, name, current);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isCurrentDefined || !thisObject->hasExport(name.uid()))
        RELEASE_AND_RETURN(scope, validateAndApplyPropertyDescriptor(globalObject, nullptr, name, false, descriptor, isCurrentDefined, current, shouldThrow));

    if (descriptor.isAccessorDescriptor())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToAccessorError);
    if (descriptor.writablePresent() && !descriptor.writable())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToNonWritableError);
    if (descriptor.enumerablePresent() && !descriptor.enumerable())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToNonEnumerableError);
    if (descriptor.configurablePresent() && descriptor.configurable())
        return typeError(globalObject, scope, shouldThrow, NamespaceBindingToConfigurableError);
    if (!descriptor.value())
        return true;
    bool isSame = sameValue(globalObject, descriptor.value(), current.value());
    RETURN_IF_EXCEPTION(scope, false);
    return isSame || typeError(globalObject, scope, shouldThrow, NamespaceBindingValueError);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
