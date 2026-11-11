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
#include "AOTRuntimeCheckFailure.h"
#include "AOTRuntimeReporter.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTSort.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "AsyncContextSwapScope.h"
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

class ResolveOfImportCall {
public:
    ResolveOfImportCall(VM& vm, const ScriptFetchParameters* attributes)
        : m_isResolvingImportCall(vm.aotState().isResolvingImportCall, true)
        , m_isResolvingImportCallWithoutAttributes(vm.aotState().isResolvingImportCallWithoutAttributes, !attributes)
    {
    }

private:
    SetForScope<bool> m_isResolvingImportCall;
    SetForScope<bool> m_isResolvingImportCallWithoutAttributes;
};

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
        visitor.appendUnbarriered(m_prototypeOfModuleObjects);
        visitor.appendUnbarriered(m_errorsOfExportsThatBuildFoundAbsent);
        visitor.appendUnbarriered(m_ownersOfVariables);
        visitor.appendUnbarriered(m_ownersHandedByMocks);
        visitor.appendUnbarriered(m_extensibleNamespaceStructure);
        visitor.appendUnbarriered(m_hostRequestRequirerObject);
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
    , m_setsMainModulePath(state.instance.vm->aotState().image->setsMainModulePath())
    , m_loadsModulesByName(state.instance.vm->aotState().image->loadsModulesByName())
    , m_refusesOriginalsBesideMocks(state.instance.vm->aotState().image->refusesOriginalsBesideMocks())
    , m_showsEntriesOfRequireCache(state.instance.vm->aotState().image->showsEntriesOfRequireCache())
    , m_placesSeenByPlans(state.instance.vm->aotState().placesSeenByPlans)
    , m_mockOperations(host().mockOperations ? host().mockOperations() : nullptr)
{
    m_instance.modules.status = state.tableArena.allocate<uint8_t>(m_table.size());
    m_storedStatuses = unsafeMakeSpan(m_instance.modules.status, m_table.size());
    m_instance.modules.evaluatingMark = static_cast<uint32_t>(StoredModuleStatus::Evaluating);
    m_instance.modules.runsImportedFilesAlone = 0;
    m_modulesSettledForLinks.fill(0, m_table.size() / 64 + 1);
    m_loadedGroups.fill(0, m_table.size() / 64 + 1);
    m_loadsModulesIndividually = isTestRun();
    if (m_loadsModulesIndividually) {
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
    m_transferredSynchronousLoads.clear();
    m_deferredLinkSteps.clear();
    m_evaluationOfImportOfMainModule.clear();
    m_capabilityOfMainModule.clear();
    m_loadsInFlight.clear();
    m_loadsThatHoldModules.clear();
    m_levelsOfLoads.clear();
    m_deferredRequests.clear();
    m_deferredImports.clear();
    m_importsAwaitingSource.clear();
    m_delayedImportTargets.clear();
    m_factoryBlockedImportRoots.clear();
    m_factoryBlockedImports.clear();
    m_failuresOfFactories.clear();
    m_missingMockFailures.clear();
    m_pendingActualKeyLoads.clear();
    m_factoryBlockedFetches.clear();
    m_modulesWithAbandonedFactory.clearAll();
    m_factoryBlockedRootsSnapshot.clearAll();
    m_mockImportTargetsSnapshot.clearAll();
    m_modulesWithPassthroughMock.clearAll();
    m_rootsOfImportsThatHaveGoneOn.clear();
    m_detachedLoads.clear();
    m_filesThatRanBesideMockThatOnlyWaited.clearAll();
    m_blockedRequiresWithError.clearAll();
    m_statementFetchesDelayedByTick.clearAll();
    m_importsCalledByBodiesSetAside.clear();
    m_graphsDeferredForImports.clear();
    m_queuedFetches.clear();
    clearBatchedFetches();
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
    ASSERT(!m_instance.vm->heap.isShuttingDown());
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
    return (status(file) == ModuleStatus::Evaluating && data(file).kind == ModuleKind::CommonJs) || status(file) == ModuleStatus::EvaluatingAsync || hasRun(file);
}

void Modules::retainModuleObjectFromRequire(ModuleID module, JSObject* moduleObject)
{
    LoadedModule& loaded = ensureLoaded(module);
    if (loaded.moduleObjectFromRequire || loaded.isBeingRequired)
        return;
    loaded.moduleObjectFromRequire.set(*this, moduleObject);
    JSValue exports = moduleObject->getDirect(*m_instance.vm, m_instance.vm->propertyNames->exports);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, exports);
    loaded.exportsWhileRequired.set(*this, exports && exports.isObject() ? asObject(exports) : nullptr);
}

void Modules::releaseModuleObjectFromRequire(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->isKeptByRequire)
        return;
    loaded->isKeptByRequire = false;
    loaded->moduleObjectFromRequire.set(*this, nullptr);
}

bool Modules::wasLoadedByImport(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (data(file).kind == ModuleKind::SourceText)
        return true;
    ModuleID imported = moduleWithExportsOf(file);
    if (imported != file && status(imported) >= ModuleStatus::Evaluating && !m_facadesCapturedForMock.get(imported.index()))
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
                if (linked && linked->uncompiledRecord && linked->keyOfLatestLoad.string() == m_table.fullPath(file))
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
            loader()->removeEntriesForRecord(record);
    }
}

bool Modules::hasNoEntryBehindMock(ModuleID fileOrModule)
{
    ModuleID file = fileOf(fileOrModule);
    if (!m_keysWithoutEntryBehindMock.get(file.index())) [[likely]]
        return false;
    if (activeMock(file))
        return hasRemovedMock(file);
    return status(file) <= ModuleStatus::Linked && status(moduleWithExportsOf(file)) <= ModuleStatus::Linked;
}

void Modules::noteEndOfMockOverFileThatNeverRan(ModuleID fileOrModule)
{
    ModuleID file = fileOf(fileOrModule);
    if (!isEvicted(file) && !wasOriginalEvaluated(file) && !wasOriginalEvaluated(moduleWithExportsOf(file)) && !m_keysWithFalsyEntryOfProgram.get(file.index()))
        m_keysWithoutEntryBehindMock.set(file.index());
}

void Modules::evict(ModuleID module, bool shouldRestoreIfRetained)
{
    ModuleID file = fileOf(module);
    bool hasThrown = status(file) == ModuleStatus::Failed || status(moduleWithExportsOf(file)) == ModuleStatus::Failed;
    if (!wouldBeEvicted(file) && !hasThrown)
        return;
    if (shouldRestoreIfRetained && !isEvicted(file) && m_retainedModules.get(file.index()))
        m_retainedModulesToRestore.set(file.index());
    delegateToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    if (status(file) == ModuleStatus::Evaluated && !m_retainedModulesToRestore.get(file.index()))
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

BitVector Modules::collectEvaluatedModules()
{
    followImportsOfModulesThatHaveRun();
    BitVector modules = m_modulesSeenToHaveRun;
    modules.merge(m_modulesThatRanWithImporters);
    return modules;
}

void Modules::snapshotEvaluatedModules()
{
    if (holdsCodeThatMayBeEvicted())
        m_evaluatedModulesSnapshot = collectEvaluatedModules();
}

void Modules::retainNewlyEvaluatedModules()
{
    if (!holdsCodeThatMayBeEvicted())
        return;
    BitVector modules = collectEvaluatedModules();
    modules.exclude(std::exchange(m_evaluatedModulesSnapshot, { }));
    for (size_t index : modules) {
        ModuleID module(index);
        if (!data(module).isOfLibrary() && fileOf(module) == module && mayBeEvicted(module) && !isEvicted(module))
            m_retainedModules.set(index);
    }
}

void Modules::takeBackFromHost(std::span<const ModuleID> files)
{
    for (ModuleID file : files)
        m_evictedFiles.clear(file.index());
    BitVector evictedWithImports = std::exchange(m_filesEvictedWithImports, { });
    m_evictedFiles.exclude(evictedWithImports);
    m_filesAboveEvictedFile.clearAll();
    m_filesAboveNoEvictedFile.clearAll();
    ++m_linkEpoch;
    for (size_t index : evictedWithImports)
        delegatesWithoutPlugins(ModuleID(index));
}

void Modules::restoreRetainedModules(const WTF::Function<void(const String&)>& functor)
{
    if (m_retainedModulesToRestore.isEmpty())
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    Vector<ModuleID, 16> back;
    for (size_t index : std::exchange(m_retainedModulesToRestore, { }))
        back.append(ModuleID(index));
    takeBackFromHost(back.span());
    for (ModuleID file : back) {
        functor(m_table.fullPath(file));
        if (data(file).kind != ModuleKind::CommonJs)
            continue;
        bool wasLoadedAgain = !m_filesToLoadAgain.get(file.index());
        m_filesToLoadAgain.clear(file.index());
        if (!recordIfExists(file))
            setStoredStatus(file, StoredModuleStatus::Evaluated);
        if (wasLoadedAgain) {
            recaptureExports(file);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

void Modules::resetEntryPointEvaluation(ModuleID module)
{
    const ModuleData& moduleData = data(module);
    if (!moduleData.isEntryPoint() || moduleData.isFacade() || isEvicted(module) || isMocked(module))
        return;
    ModuleStatus current = status(module);
    if (current != ModuleStatus::Evaluated && current != ModuleStatus::Failed)
        return;
    RELEASE_ASSERT(!m_innermostEvaluation);
    if (moduleData.runsAgainAfterThrow()) {
        for (ModuleID each : { moduleWithExportsOf(module), module }) {
            if (ModuleRecord* record = m_records.get(each)) {
                loader()->removeEntriesForRecord(record);
                takeOutRecord(each);
            }
            setStoredStatus(each, StoredModuleStatus::Linked);
        }
        discardFailedEvaluation(module);
        return;
    }
    if (!m_instance.program->runsEntryPointsAgain() || !moduleData.isESModuleOfProgram())
        return;
    makeESModuleReadyToRunAgain(module);
}

bool Modules::hasResettablePackageFailure(ModuleID module) const
{
    if (!data(module).bodyOfESModuleMayRunAgain() || !mayBeMocked(module) || isEvicted(module))
        return false;
    return (status(module) == ModuleStatus::Failed || errorOfModuleThatRanBesideMock(module)) && isOfPackage(module);
}

void Modules::resetPackageFailureIfNeeded(ModuleID module)
{
    if (!hasResettablePackageFailure(module))
        return;
    m_modulesWithResetFailure.set(module.index());
    makeESModuleReadyToRunAgain(module);
    LoadedModule& loaded = ensureLoaded(module);
    loaded.errorOfModuleThatRanBesideMock.set(*this, JSValue());
    loaded.capturedExports.set(*this, nullptr);
    loaded.capturedFields.set(*this, nullptr);
    loaded.capturedStars.set(*this, nullptr);
    loaded.sourceOfCapturedFields.set(*this, nullptr);
    m_originalsEvaluatedUnderMock.clear(module.index());
    m_modulesThatRanBesideMockAlone.clear(module.index());
    m_modulesLoadedBesideGroups.clear(module.index());
}

void Modules::resetFailedEvaluations()
{
    Vector<ModuleID, 4> failed;
    for (ModuleID module : m_records.keys()) {
        if (hasResettablePackageFailure(module) && !(isMocked(module) && hasFailedWithoutMock(module)))
            failed.append(module);
    }
    for (ModuleID module : failed)
        resetPackageFailureIfNeeded(module);
}

void Modules::makeESModuleReadyToRunAgain(ModuleID module)
{
    if (ModuleRecord* record = m_records.get(module)) {
        loader()->removeEntriesForRecord(record);
        takeOutRecord(module);
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
            return delegateToHost(module);
    }
}

void Modules::delegateToHostUnlessEvaluated(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (status(file) >= ModuleStatus::Evaluating || isEvicted(file) || hasRun(file))
        return;
    delegateToHost(file);
    m_admitsCodeOutsideImage = true;
    if (data(file).kind != ModuleKind::CommonJs)
        return;
    m_filesToLoadAgain.set(file.index());
    setStoredStatus(file, StoredModuleStatus::InRecord);
}

void Modules::delegateToHost(ModuleID file)
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
    if (!stampOfBuild || !host().computeFileStamp)
        return false;
    if (data(file).kind == ModuleKind::Data && host().printsDataFileAsJavaScript(m_instance.globalObject, m_table.fullPath(file)))
        m_dataFilesThatHostPrintsAsJavaScript.set(file.index());
    if (isOfKindWhoseTextHostKeeps(file) && host().hasSourceCache && host().hasSourceCache(m_instance.globalObject)) {
        VMState& state = m_instance.vm->aotState();
        bool isBelowVitest = host().isLoadingUnderVitest(m_instance.globalObject);
        if (state.filesWhoseKeptTextWasCompiled.get(file.index()) && state.filesWhoseKeptTextWasReadBelowVitest.get(file.index()) == isBelowVitest) {
            evictHostCachedSource(file);
            return false;
        }
        state.filesWhoseKeptTextWasCompiled.clear(file.index());
        state.filesWhoseKeptTextWasReadBelowVitest.set(file.index(), isBelowVitest);
    }
    countEvent(*m_instance.vm, RuntimeCounterID::FilesComparedWithDisk);
    ModuleTable::PathInUTF8 path = m_table.pathInUTF8(file);
    path.append('\0');
    if (host().computeFileStamp(path.span().first(path.size() - 1)) != *stampOfBuild)
        return true;
    evictHostCachedSource(file);
    return false;
}

void Modules::evictHostCachedSource(ModuleID file)
{
    if (m_instance.vm->aotState().modulesWithHostCachedSource.clear(file.index()))
        host().evictCachedSource(m_instance.globalObject, m_table.fullPath(file));
}

bool Modules::writesNameThatHostMayAnswerFor(ModuleID file, WalkForEvictedFiles& walk)
{
    if (m_table.namesOfStatements(file).empty() || !host().hasResolvePlugins || !mayBeEvicted(file))
        return false;
    if (!walk.pluginsAnswerForNames)
        walk.pluginsAnswerForNames = host().hasResolvePlugins(m_instance.globalObject);
    return *walk.pluginsAnswerForNames;
}

Modules::AnswersForNamesOfFile Modules::askHostAboutNamesWrittenBy(ModuleID file)
{
    AnswersForNamesOfFile answers;
    String importer = m_table.fullPath(file);
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
        String resolvedWithoutPlugins;
        PluginResolveResult answer = host().resolveWithPlugins(m_instance.globalObject, textOf(names[index]), importer, resolvedWithoutPlugins);
        if (answer == PluginResolveResult::Resolved) {
            answers.answer = answer;
            if (std::optional<uint32_t> linked = fileAt(resolvedWithoutPlugins))
                answers.filesThatAnswersLeadAwayFrom.append(*linked);
            continue;
        }
        if (answer == PluginResolveResult::Declined) {
            namesDeclined.append(index);
            continue;
        }
        answers.answer = answer;
        for (uint32_t inFront : namesDeclined) {
            if (std::optional<uint32_t> linked = fileAt(host().resolveWithoutPlugins(m_instance.globalObject, textOf(names[inFront]), importer)))
                answers.filesInFrontOfThrow.append(*linked);
        }
        break;
    }
    return answers;
}

bool Modules::wasModifiedSinceBuild(StringView path) const
{
    if (m_filesModifiedSinceBuild.isEmpty())
        return false;
    std::optional<ModuleID> found = m_table.findByPath(path);
    return found && m_filesModifiedSinceBuild.get(fileOf(*found).index());
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
    if (!m_table.listsNamesOfStatements() || !host().hasResolvePlugins)
        return false;
    if (!walk.pluginsAnswerForNames)
        walk.pluginsAnswerForNames = host().hasResolvePlugins(m_instance.globalObject);
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
        if (isMocked(file) || activeMock(file) || m_modulesWithPendingMock.get(file.index()))
            continue;
        AnswersForNamesOfFile answers;
        if (!filesAsked.get(file.index()) && mayBeAskedAbout(file) && writesNameThatHostMayAnswerFor(file, walk)) {
            if (filesAsked.isEmpty())
                host().beginOrEndPluginResolutions(m_instance.globalObject, nullptr);
            filesAsked.set(file.index());
            didAsk = true;
            uint64_t numberOfGraphsBegun = m_numberOfGraphsBegun;
            answers = askHostAboutNamesWrittenBy(file);
            if (m_numberOfGraphsBegun != numberOfGraphsBegun)
                walk.importsAreFollowed = false;
            if (answers.answer != PluginResolveResult::Declined)
                delegateToHostUnlessEvaluated(file);
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
            if (answers.answer != PluginResolveResult::Threw || answers.filesInFrontOfThrow.contains(target.index()))
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
        host().beginOrEndPluginResolutions(m_instance.globalObject, &leaves);
    });
    while (true) {
        WalkForEvictedFiles walk;
        isAboveEvictedFile = walkForEvictedFiles(module, walk);
        if (m_skipsResolvePlugins || !askHostLevelByLevel(module, walk, filesAsked))
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
    if (isMocked(file) || activeMock(file) || m_modulesWithPendingMock.get(file.index()) || hasFailedUnderMockThatStands(file))
        return false;
    if (m_filesAboveEvictedFile.get(file.index()))
        return true;
    if (isChangedOnDisk(file)) [[unlikely]] {
        if (!mayBeEvicted(file))
            m_filesModifiedSinceBuild.set(file.index());
        delegateToHostUnlessEvaluated(file);
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

bool Modules::delegatesWithoutPlugins(ModuleID module)
{
    SetForScope skipsResolvePlugins(m_skipsResolvePlugins, true);
    return delegatesToHost(module);
}

bool Modules::isRequiredModulePatchPending(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (data(file).kind != ModuleKind::CommonJs || !host().isPatchPending || !m_table.hasFilePath(file) || refusesOriginalsBesideMocks()) [[likely]]
        return false;
    return host().isPatchPending(m_instance.globalObject, m_table.fullPath(file)) && !wasLoadedByImport(file);
}

bool Modules::delegatesToHost(ModuleID module)
{
    resetModulesThatHaveRun();
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
        if (status == ModuleStatus::Unlinked)
            unsettleForLinks(module);
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
    if (JSObject* mock = activeMock(module)) [[unlikely]]
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
    if (record->ownsStatus())
        return record;

    ModuleStatus moduleStatus = status(module);
    if (moduleStatus == ModuleStatus::Evaluating && (data(module).evaluatesItself() || !data(module).bodyFunction())) {
        record->setStatus(CyclicModuleRecord::Status::Evaluating);
        return record;
    }
    record->setOwnsStatus();
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

void Modules::takeOutRecord(ModuleID module)
{
    dropAnswersKept();
    Locker locker { m_state.caches->lock() };
    ModuleRecord* record = m_records.take(module);
    if (record)
        forgetAnswersThatName(record);
}

void Modules::forgetAnswersThatName(ModuleRecord* record)
{
    record->forgetCachedResolutions();
    if (!record->isRetained() || std::exchange(m_someRetainedRecordHasLeft, true))
        return;
    for (ModuleRecord* other : m_records.values())
        other->forgetCachedResolutions();
    for (auto& loaded : m_loaded.values()) {
        if (JSObject* setAside = loaded->recordBeforeMockInPlace.get())
            uncheckedDowncast<ModuleRecord>(setAside)->forgetCachedResolutions();
    }
}

void Modules::retireRecord(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(record->status() == CyclicModuleRecord::Status::Evaluated);
    dropAnswersKept();
    ASSERT(!record->evaluationError());
    ASSERT(!record->asyncEvaluationOrder().hasOrder());
    setStoredStatus(record->module(), StoredModuleStatus::Evaluated);
    loader()->removeEntriesForRecord(record);
    Locker locker { m_state.caches->lock() };
    m_records.remove(record->module());
}

void Modules::retireRecordOfModuleThatHasNotBegun(ModuleRecord* record)
{
    ASSERT(!record->isRetained());
    ASSERT(status(record->module()) == ModuleStatus::Linked);
    dropAnswersKept();
    ASSERT(!record->evaluationError());
    setStoredStatus(record->module(), StoredModuleStatus::Linked);
    loader()->removeEntriesForRecord(record);
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
    if (m_instance.vm->synchronousModuleQueue() || m_innermostEvaluation || m_rootWhoseFilesRun || m_rootAwaitingLoader || m_moduleWithDeferredJobs || !m_levelsOfLoads.isEmpty() || !m_queuedFetches.isEmpty())
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
        record->adoptStatusIfNeeded();
        if (!record->ownsStatus())
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

void Modules::keepFailedImportError(JSPromise* settled)
{
    if (m_mocksWithIntactOriginal.isEmpty() || settled->status() != JSPromise::Status::Rejected) [[likely]]
        return;
    LoadInFlight* load = m_loadsInFlight.find(settled);
    ModuleRecord* original = load ? displacedRecord(load->root) : nullptr;
    if (!original || !original->evaluationError() || original->evaluationError() != settled->result() || wasDisplacedAfterFailure(load->root) || wasMockedAfterFailure(load->root))
        return;
    if (!activeMock(load->root) || m_modulesWithPendingMock.get(load->root.index()) || m_mockEntriesToEvict.get(load->root.index()))
        return;
    if (!m_modulesWithMockLoadError.set(load->root.index()))
        ensureLoaded(load->root).cachedLoadError.set(*this, settled->result());
}

void didSettleLoad(JSPromise* settled)
{
    JSGlobalObject* realm = settled->realm();
    if (Modules* modules = modulesOf(realm)) {
        modules->keepFailedImportError(settled);
        modules->didSettlePromiseOfImport(settled);
        modules->resumeGraphsDeferredForImports();
        modules->retireRecordsOfSettledLoads();
    }
}

void Modules::setFailed(ModuleID module, JSValue error)
{
    SuspendExceptionScope errorStillThrown(*m_instance.vm);
    DeferTerminationForAWhile noTerminationArrives(*m_instance.vm);
    ModuleRecord* record = ensureRecord(module);
    setStoredStatus(module, StoredModuleStatus::InRecord);
    record->setOwnsStatus();
    record->retain();
    record->setEvaluationError(*m_instance.vm, error);
    record->setStatus(CyclicModuleRecord::Status::Evaluated);
    if (m_mainCommonJSModule == fileOf(module))
        forgetFailedMainModule();
}

void Modules::throwEvaluationError(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ModuleRecord* record = recordIfExists(module);
    RELEASE_ASSERT(record && record->evaluationError());
    reportRetainedFailure(module);
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

Identifier Modules::moduleKey(ModuleID module) const
{
    if (ModuleRecord* record = m_records.get(module))
        return record->moduleKey();
    String path = m_table.fullPath(module);
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
    return jsString(*m_instance.vm, m_table.fullPath(module));
}

String Modules::pathOfExporterAsLoaderNamesIt(const RequiredNameData& requiredName)
{
    ModuleID exporter = m_table.moduleBehindFacade(requiredName.exporterModule()).value_or(requiredName.exporterModule());
    String path = data(exporter).isUncompiledCode() ? resolvedKey(exporter).string() : m_table.fullPath(exporter);
    return wasLoadedUnderActualKey(exporter) ? makeString(path, "?actual"_s) : path;
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

bool Modules::isRequestedByStarExport(const RequiredNameData& requiredName) const
{
    const ExportStatementData* statement = m_table.findExportStatement(requiredName);
    return statement && statement->isOfStar;
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
    if (someMockIsSeen() && (activeMock(module) || findMockForReexport(module, moduleExport))) [[unlikely]]
        return { };
    switch (moduleExport.kind()) {
    case ModuleExportKind::Variable:
        return ProgramStatics::readVariableWithoutAllocating(m_instance, moduleExport.variable);
    case ModuleExportKind::Namespace: {
        if (handedOnInPlaceOfNamespace(moduleExport)) [[unlikely]]
            return { };
        if (JSObject* linkedWith = namespaceThatCompiledCodeHolds(moduleExport.targetModule(), targetPhaseOf(moduleExport)))
            return linkedWith;
        if (hasIntactOriginal(moduleExport.targetModule()) || indexOfEndedMockSeenBy(fileOf(moduleExport.targetModule()), module)) [[unlikely]]
            return { };
        JSObject* namespaceObject = namespaceIfCreated(moduleExport.targetModule(), targetPhaseOf(moduleExport));
        return namespaceObject ? JSValue(namespaceObject) : JSValue();
    }
    case ModuleExportKind::Indirect:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSValue Modules::readExport(ModuleID module, unsigned index, MockMode mockOfModule)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    std::span<const ModuleExportData> exports = m_table.exports(module);
    RELEASE_ASSERT(index < exports.size());
    const ModuleExportData& moduleExport = exports[index];
    if (JSObject* mock = mockOfModule == MockMode::Bypass ? nullptr : activeMock(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(realm, identifier(moduleExport.name)));
    if (status(module) == ModuleStatus::Linked) [[unlikely]]
        recordEvaluationIfNothingIsLeftToRun(module);
    if (someMockIsSeen()) [[unlikely]] {
        if (std::optional<MockedReexport> answer = findMockForReexport(module, moduleExport, mockOfModule)) {
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
            RELEASE_AND_RETURN(scope, originalNamespace(moduleExport.targetModule()));
        if (targetPhaseOf(moduleExport) == ModulePhase::Evaluation)
            RELEASE_AND_RETURN(scope, namespaceSeenBy(moduleExport.targetModule(), module));
        RELEASE_AND_RETURN(scope, moduleNamespace(moduleExport.targetModule(), targetPhaseOf(moduleExport)));
    case ModuleExportKind::Indirect: {
        const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
        if (m_mocks || hasAnyActiveMock()) [[unlikely]] {
            bool asksTarget = mockOfModule == MockMode::Bypass || !hasAnyActiveMock();
            AbstractModuleRecord* record = recordForResolution(asksTarget ? indirectExport.targetModule() : module);
            RETURN_IF_EXCEPTION(scope, { });
            AbstractModuleRecord::Resolution resolution = record->resolveExport(realm, identifier(asksTarget ? indirectExport.importName : moduleExport.name));
            RETURN_IF_EXCEPTION(scope, { });
            if (resolution.isLiveExport(vm))
                RELEASE_AND_RETURN(scope, resolution.moduleRecord->getReexportedLiveExport(realm, resolution.localName));
            auto* provider = resolution.type == AbstractModuleRecord::Resolution::Type::Resolved && resolution.localName != vm.propertyNames->starNamespacePrivateName ? dynamicDowncast<ModuleRecord>(resolution.moduleRecord) : nullptr;
            if (provider && provider->module() != module && !asksTarget)
                RELEASE_AND_RETURN(scope, provider->readExport(realm, resolution.localName, resolution.compiledExportIndex));
        }
        JSObject* namespaceObject = moduleNamespace(indirectExport.targetModule(), ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, namespaceObject->get(realm, identifier(indirectExport.importName)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<std::pair<ModuleID, unsigned>> Modules::exportThatStaysInBehindIndirectEntries(ModuleID module, unsigned index)
{
    if (m_mocks || hasAnyActiveMock() || m_table.exports(module)[index].kind() != ModuleExportKind::Indirect)
        return std::nullopt;
    for (size_t steps = 0; steps <= m_table.size(); ++steps) {
        const ModuleExportData& entry = m_table.exports(module)[index];
        if (entry.kind() != ModuleExportKind::Indirect) {
            if (entry.kind() != ModuleExportKind::Variable || !entry.staysIn || (entry.isHiddenUntilInitialized && !readExportIfInitialized(module, index)))
                return std::nullopt;
            return std::pair { module, index };
        }
        const IndirectExportData& indirectExport = m_table.indirectExport(entry);
        std::optional<unsigned> found = m_table.findExport(indirectExport.targetModule(), identifier(indirectExport.importName).impl());
        if (!found)
            return std::nullopt;
        module = indirectExport.targetModule();
        index = *found;
    }
    return std::nullopt;
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
            if (m_moduleBeingPatched) {
                auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
                // variablesBeforeMock names the variables that the mock of this file has laid a value over, and is the owner of those values.
                // What the mock found in a variable is with the owners of the variable.
                LoadedModule& mocked = ensureLoaded(*m_moduleBeingPatched);
                if (!mocked.variablesBeforeMock)
                    mocked.variablesBeforeMock.set(*this, createTableOfExports());
                storeValue(m_instance.globalObject, mocked.variablesBeforeMock.get(), moduleExport.variable.encoded(), jsBoolean(true));
                RELEASE_AND_RETURN(scope, (layVariableOverride(moduleExport.variable, mocked.variablesBeforeMock.get(), value), true));
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

template<typename Functor>
static auto withOwnersOfVariable(Instance& instance, JSObject* table, SlotData variable, const Functor& functor)
{
    auto readVariable = [&] {
        return ProgramStatics::readVariable(instance, variable);
    };
    auto writeVariable = [&](JSValue value) {
        ProgramStatics::writeVariable(instance, variable, value);
    };
    ScopedLambda<JSValue()> read = readVariable;
    ScopedLambda<void(JSValue)> store = writeVariable;
    ExportOverrideLayers layers(instance.globalObject, table, variable.offset, read, store);
    return functor(layers);
}

void Modules::layVariableOverride(SlotData variable, JSCell* owner, JSValue value)
{
    if (!m_ownersOfVariables) {
        m_ownersOfVariables = createTableOfExports();
        m_storedReferences.recordAll();
    }
    withOwnersOfVariable(m_instance, m_ownersOfVariables, variable, [&](ExportOverrideLayers& layers) { layers.lay(owner, value); });
}

ExportOverrideEnd Modules::endVariableOverride(SlotData variable, JSCell* owner, JSCell** ownerAbove)
{
    if (!m_ownersOfVariables)
        return ExportOverrideEnd::NotOverridden;
    return withOwnersOfVariable(m_instance, m_ownersOfVariables, variable, [&](ExportOverrideLayers& layers) { return layers.end(owner, false, ownerAbove); });
}

bool Modules::foundByVariableOverride(SlotData variable, JSCell* owner, JSValue& found)
{
    return m_ownersOfVariables && withOwnersOfVariable(m_instance, m_ownersOfVariables, variable, [&](ExportOverrideLayers& layers) { return layers.found(owner, found); });
}

std::optional<SlotData> Modules::replaceableVariableOf(ModuleID module, UniquedStringImpl* name)
{
    for (unsigned steps = 0; steps <= m_table.size(); ++steps) {
        std::optional<unsigned> index = m_table.findExport(module, name);
        if (!index)
            return std::nullopt;
        const ModuleExportData& moduleExport = m_table.exports(module)[*index];
        switch (moduleExport.kind()) {
        case ModuleExportKind::Variable:
            if (!moduleExport.isReplaceable || syntheticExportKeptIn(moduleExport.variable))
                return std::nullopt;
            return moduleExport.variable;
        case ModuleExportKind::Namespace:
            return std::nullopt;
        case ModuleExportKind::Indirect: {
            const IndirectExportData& indirectExport = m_table.indirectExport(moduleExport);
            module = moduleLoadedUnder(indirectExport.targetModule());
            name = identifier(indirectExport.importName).impl();
            break;
        }
        }
    }
    return std::nullopt;
}

bool Modules::tryReplaceExportBehindStars(ModuleID module, UniquedStringImpl* name, JSValue value, BitVector& seenBehindStars)
{
    if (name == m_instance.vm->propertyNames->defaultKeyword.impl() || seenBehindStars.set(module.index()))
        return false;
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (ModuleRequestData request : m_table.dynamicStars(module)) {
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

bool Modules::hasStartedLoading(ModuleID module) const
{
    if (hasRemovedMock(module) || hasRunBesideMockAlone(module)) [[unlikely]]
        return false;
    ModuleStatus current = status(module);
    if (data(module).kind == ModuleKind::CommonJs)
        return !mayBeMocked(module) || (current != ModuleStatus::Unlinked && current != ModuleStatus::Linked);
    if ((current != ModuleStatus::Unlinked && current != ModuleStatus::Linked) || didCaptureExports(module))
        return true;
    if (m_modulesOwedToImportersThatRan.get(module.index()) || (wasReleasedByLoader(module) && data(module).needsEvaluation()))
        return false;
    return data(module).isESModuleOfProgram() && wasLoadedWithGraph(module);
}

bool Modules::wasMockedAfterFailure(ModuleID module) const
{
    auto laid = m_mocksWithIntactOriginal.find(module);
    return laid != m_mocksWithIntactOriginal.end() && laid->value.wasInstalledAfterFailure;
}

bool Modules::hasOnlyBeenLinked(ModuleID module)
{
    ModuleStatus current = status(module);
    return (current == ModuleStatus::Unlinked || current == ModuleStatus::Linked) && !didCaptureExports(module) && !hasRun(module);
}

std::optional<ModuleID> Modules::findVariantToMock(StringView path, std::optional<ModuleID> file) const
{
    Vector<ModuleID, 2> forms = m_table.importAttributeVariants(path);
    size_t numberLoaded = std::ranges::count_if(forms, [&](ModuleID form) { return hasStartedLoading(form); }) + (file && hasStartedLoading(*file));
    if (numberLoaded > 1)
        return std::nullopt;
    if (file && hasStartedLoading(*file))
        return file;
    std::optional<ModuleID> json;
    for (ModuleID form : forms) {
        if (!hasStartedLoading(form))
            continue;
        if (!m_table.fullPath(form).endsWith("?json"_s))
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
    m_libraryModulesThatMockReplaces.set(module.index(), !hasBeenImported(module));
}

bool Modules::letMockStandInPlaceOfLibraryModule(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (data(module).kind != ModuleKind::RuntimeSource || !m_libraryModulesThatMockReplaces.get(module.index()))
        return false;
    if (m_modulesWithPendingMock.get(module.index())) {
        runPendingMock(module);
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
    if (!isMocked(module) && !activeMock(module))
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
        dropAnswersKept(false);
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

static bool isReexportOf(const ModuleTable& table, const ModuleExportData& starExportEntry, ModuleID target, const ModuleExportData& targetEntry)
{
    if (exportSameBinding(table, starExportEntry, targetEntry))
        return true;
    if (starExportEntry.kind() != ModuleExportKind::Indirect)
        return false;
    const IndirectExportData& named = table.indirectExport(starExportEntry);
    return named.targetModule() == target && named.importName == targetEntry.name;
}

bool Modules::namesBehindStarsAreInTables(ModuleID module, SearchBehindStars& search) const
{
    StoredResolutions* answers = answersKept();
    BitVector& areInTables = answers ? answers->namesBehindStarsAreInTables : search.namesAreInTables;
    if (areInTables.get(module.index()))
        return true;
    BitVector isSeen(m_table.size());
    isSeen.merge(search.isOnPath);
    Vector<ModuleID, 8> toAsk { module };
    while (!toAsk.isEmpty()) {
        ModuleID asked = toAsk.takeLast();
        if (!data(asked).hasDynamicStarExport() || isSeen.quickSet(asked.index()) || areInTables.get(asked.index()) || standsForWholeModule(asked))
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
    areInTables.merge(isSeen);
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
    if (search.isAnswered.get(place)) {
        if (place < search.outermostRingOfAnswer.size())
            search.outermostRing = std::min(search.outermostRing, search.outermostRingOfAnswer[place]);
        return search.isLeftOut.get(place);
    }
    StoredResolutions* answers = answersKept();
    uint64_t placeOfExport = static_cast<uint64_t>(place) << 32 | index;
    if (answers) [[unlikely]] {
        if (auto found = answers->isLeftOut.find(placeOfExport); found != answers->isLeftOut.end())
            return found->value;
    }
    unsigned placeOnPath = search.path.size();
    unsigned outermostRingSoFar = std::exchange(search.outermostRing, SearchBehindStars::noRing);
    search.path.append(module);
    if (data(module).hasDynamicStarExport())
        tables = TablesBehindStars::HaveName;
    Identifier name = identifier(entry.name);
    bool isMissing = false;
    bool isAnyAsked = false;
    bool comesRoundInCircle = false;
    search.isOnPath.set(module.index());
    const Vector<PlaceOfExport, 1>* providers = tables == TablesBehindStars::HaveName ? &starExportProviders(module)[index] : nullptr;
    size_t next = 0;
    for (const ModuleRequestData& request : m_table.requests(module)) {
        ModuleID target = request.targetModule();
        if (request.isRequire || !data(target).hasExportTable())
            continue;
        std::optional<unsigned> targetIndex;
        if (!providers)
            targetIndex = m_table.findExport(target, name.impl());
        else if (next < providers->size() && providers->at(next).module == target)
            targetIndex = providers->at(next++).index;
        bool providesBinding = targetIndex && (providers || isReexportOf(m_table, entry, target, m_table.exports(target)[*targetIndex]));
        if (search.isOnPath.get(target.index())) {
            ++search.ringsMet;
            search.outermostRing = std::min(search.outermostRing, static_cast<unsigned>(search.path.find(target)));
            comesRoundInCircle |= providesBinding;
            continue;
        }
        if (providesBinding) {
            std::optional<bool> inViewOfModule = isLeftOutInViewOf(module, target, *targetIndex);
            isMissing = inViewOfModule ? *inViewOfModule : isLeftOut(target, *targetIndex, tables, search);
        }
        else if (!targetIndex && tables == TablesBehindStars::MayLackName)
            isMissing = false;
        else
            continue;
        isAnyAsked = true;
        if (!isMissing)
            break;
    }
    search.isOnPath.clear(module.index());
    search.path.removeLast();
    if (placeOnPath < search.answersThatHangOnPlaceOfPath.size()) {
        for (size_t kept : std::exchange(search.answersThatHangOnPlaceOfPath[placeOnPath], { })) {
            search.isAnswered.clear(kept);
            search.outermostRingOfAnswer[kept] = SearchBehindStars::noRing;
        }
    }
    bool answer = isAnyAsked ? isMissing : comesRoundInCircle;
    unsigned outermostRing = std::exchange(search.outermostRing, outermostRingSoFar);
    search.isAnswered.set(place);
    search.isLeftOut.set(place, answer);
    if (outermostRing < placeOnPath) {
        while (search.answersThatHangOnPlaceOfPath.size() <= outermostRing)
            search.answersThatHangOnPlaceOfPath.append({ });
        search.answersThatHangOnPlaceOfPath[outermostRing].append(place);
        while (search.outermostRingOfAnswer.size() <= place)
            search.outermostRingOfAnswer.append(SearchBehindStars::noRing);
        search.outermostRingOfAnswer[place] = outermostRing;
        search.outermostRing = std::min(outermostRingSoFar, outermostRing);
    } else if (answers) [[unlikely]]
        answers->isLeftOut.add(placeOfExport, answer);
    return answer;
}

void Modules::setMockHasCommonJSExports(ModuleID module, bool areOfCommonJSModule)
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
    if (JSObject* entry = loaded->moduleObjectFromRequire) {
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
    if (JSObject* entry = loaded.moduleObjectFromRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, exports);
}

void Modules::releaseMockInRequireCache(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (!loaded || !loaded->valueInRequireCache)
        return;
    JSValue before = loaded->valueInRequireCacheBeforeMock;
    loaded->valueInRequireCacheBeforeMock.set(*this, JSValue());
    loaded->valueInRequireCache.set(*this, before);
    setRequireCacheValue(module, before);
    if (!before) {
        loaded->moduleObjectFromRequire.set(*this, nullptr);
        forgetFirstRequirerOf(module);
        loaded->firstUncompiledRequirer.set(*this, nullptr);
        m_libraryModulesInRequireCache.removeFirst(module);
        return;
    }
    if (JSObject* entry = loaded->moduleObjectFromRequire)
        entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, before);
}

auto Modules::starExportProviders(ModuleID module) const -> const Vector<Vector<PlaceOfExport, 1>>&
{
    return m_starExportProviders.ensure(module, [&] {
        std::span<const ModuleExportData> entries = m_table.exports(module);
        Vector<Vector<PlaceOfExport, 1>> providers(entries.size());
        for (const ModuleRequestData& request : m_table.requests(module)) {
            ModuleID target = request.targetModule();
            if (request.isRequire || !data(target).hasExportTable())
                continue;
            std::span<const ModuleExportData> entriesOfTarget = m_table.exports(target);
            for (unsigned index = 0; index < entriesOfTarget.size(); ++index) {
                std::optional<unsigned> place = m_table.findExport(module, identifier(entriesOfTarget[index].name).impl());
                if (place && entries[*place].isFromStarExport && isReexportOf(m_table, entries[*place], target, entriesOfTarget[index]))
                    providers[*place].append(PlaceOfExport { target, index });
            }
        }
        return providers;
    }).iterator->value;
}

auto Modules::starTargetsByName(ModuleID module) const -> const StarTargetsByName&
{
    std::unique_ptr<StarTargetsByName>& kept = m_starTargetsByName.add(module, nullptr).iterator->value;
    if (kept)
        return *kept;
    auto stars = makeUnique<StarTargetsByName>();
    std::span<const StarAsWrittenData> asWritten = m_table.starsAsWritten(module);
    for (const StarAsWrittenData& star : asWritten)
        stars->targets.append(StarTarget { ModuleID(star.target), !!star.isFollowed });
    if (asWritten.empty()) {
        for (ModuleRequestData request : m_table.dynamicStars(module))
            stars->targets.append(StarTarget { request.targetModule() });
    }
    for (unsigned place = 0; place < stars->targets.size(); ++place) {
        StarTarget star = stars->targets[place];
        bool hasTable = data(star.module).hasExportTable();
        bool listsItsNames = hasTable && data(star.module).isESModuleOfProgram() && !data(star.module).capturesExports() && !data(star.module).hasDynamicStarExport() && !data(star.module).mayBeMocked() && m_table.ambiguousExports(star.module).empty()
            && std::ranges::none_of(m_table.requiredNames(star.module), [&](const RequiredNameData& requiredName) {
                   return !!m_table.findExportStatement(requiredName);
               });
        if (star.isFollowed && !listsItsNames)
            stars->targetsWithUnlistedNames.append(place);
        if (!hasTable)
            continue;
        for (const ModuleExportData& entry : m_table.exports(star.module)) {
            Vector<unsigned, 1>& places = stars->targetsListingName.add(identifier(entry.name).impl(), Vector<unsigned, 1>()).iterator->value;
            if (places.isEmpty() || places.last() != place)
                places.append(place);
        }
    }
    kept = WTF::move(stars);
    return *kept;
}

std::span<const uint32_t> Modules::requiredNamesOfStatementsThatExport(ModuleID module, const String& exportName) const
{
    auto& byExportName = m_requiredNamesOfStatementsByExportName.ensure(module, [&] {
        UncheckedKeyHashMap<String, Vector<uint32_t, 1>> listed;
        std::span<const RequiredNameData> requiredNames = m_table.requiredNames(module);
        for (uint32_t place = 0; place < requiredNames.size(); ++place) {
            if (const ExportStatementData* statement = m_table.findExportStatement(requiredNames[place]))
                listed.add(ProgramStrings::text(m_table.programTables(), statement->exportNameString()), Vector<uint32_t, 1>()).iterator->value.append(place);
        }
        return listed;
    }).iterator->value;
    auto found = byExportName.find(exportName);
    return found == byExportName.end() ? std::span<const uint32_t>() : found->value.span();
}

auto Modules::modulesOnReexportWay(ModuleID reexportingModule, unsigned indexOfReexport) const -> const Vector<ModuleID, 4>&
{
    return m_reexportWays.ensure(static_cast<uint64_t>(reexportingModule.index()) << 32 | indexOfReexport, [&] {
        const ModuleExportData& reexport = m_table.exports(reexportingModule)[indexOfReexport];
        Vector<ModuleID, 4> asked;
        auto indexOfExport = [&](ModuleID module, uint32_t name) -> std::optional<unsigned> {
            return data(module).hasExportTable() ? m_table.findExport(module, identifier(name).impl()) : std::nullopt;
        };
        if (reexport.kind() == ModuleExportKind::Indirect && !reexport.isFromStarExport) {
            for (uint32_t place : requiredNamesOfStatementsThatExport(reexportingModule, identifier(reexport.name).string()))
                asked.append(m_table.requiredNames(reexportingModule)[place].exporterModule());
            return asked;
        }
        if (reexport.kind() == ModuleExportKind::Indirect) {
            std::optional<unsigned> first = indexOfExport(reexportingModule, reexport.name);
            if (!first)
                return asked;
            BitVector isSeen(m_table.size());
            isSeen.quickSet(reexportingModule.index());
            Vector<PlaceOfExport, 4> toFollow;
            auto follow = [&](ModuleID holder, unsigned index) {
                const Vector<PlaceOfExport, 1>& providers = starExportProviders(holder)[index];
                for (size_t next = providers.size(); next--;)
                    toFollow.append(providers[next]);
            };
            follow(reexportingModule, *first);
            while (!toFollow.isEmpty()) {
                PlaceOfExport next = toFollow.takeLast();
                asked.append(next.module);
                if (isSeen.quickSet(next.module.index()))
                    continue;
                if (!m_table.exports(next.module)[next.index].isFromStarExport)
                    break;
                follow(next.module, next.index);
            }
            return asked;
        }
        UncheckedKeyHashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> seen;
        struct RequestedName {
            ModuleID module;
            uint32_t name;
        };
        Vector<RequestedName, 8> toAsk;
        toAsk.append(RequestedName { reexportingModule, reexport.name });
        while (!toAsk.isEmpty()) {
            RequestedName next = toAsk.takeLast();
            if (!seen.add(static_cast<uint64_t>(next.module.index()) << 32 | next.name).isNewEntry)
                continue;
            asked.append(next.module);
            std::span<const RequiredNameData> requiredNames = m_table.requiredNames(next.module);
            for (uint32_t place : requiredNamesOfStatementsThatExport(next.module, identifier(next.name).string()))
                toAsk.append(RequestedName { requiredNames[place].exporterModule(), requiredNames[place].name });
        }
        if (reexport.kind() != ModuleExportKind::Variable)
            return asked;
        if (!m_modulesWithSyntheticExport) {
            m_modulesWithSyntheticExport.emplace();
            for (uint32_t index = 0; index < m_table.size(); ++index) {
                for (const SyntheticExportData& entry : m_table.syntheticExports(ModuleID(index))) {
                    if (entry.hasVariable)
                        m_modulesWithSyntheticExport->add(std::bit_cast<uint32_t>(entry.variable), Vector<ModuleID, 1>()).iterator->value.append(ModuleID(index));
                }
            }
        }
        if (auto found = m_modulesWithSyntheticExport->find(std::bit_cast<uint32_t>(reexport.variable)); found != m_modulesWithSyntheticExport->end())
            asked.appendVector(found->value);
        return asked;
    }).iterator->value;
}

bool Modules::mayMockAnswerReexport(ModuleID reexportingModule, const ModuleExportData& reexportEntry) const
{
    if (!m_removedMocksInUse.isEmpty())
        return true;
    std::span<const ModuleExportData> entries = m_table.exports(reexportingModule);
    if (&reexportEntry < entries.data() || &reexportEntry >= entries.data() + entries.size())
        return true;
    return std::ranges::any_of(modulesOnReexportWay(reexportingModule, static_cast<unsigned>(&reexportEntry - entries.data())), [&](ModuleID module) {
        return m_modulesWithActiveMock.get(module.index()) || m_modulesWithActiveMock.get(fileOf(module).index());
    });
}

auto Modules::findMockForReexportSlow(ModuleID reexportingModule, const ModuleExportData& reexportEntry, MockMode reexportingModuleMockMode) const -> std::optional<MockedReexport>
{
    if (!someMockIsSeen() || !mayMockAnswerReexport(reexportingModule, reexportEntry))
        return std::nullopt;
    BitVector modulesWithMock = m_modulesWithActiveMock;
    for (ModuleID mocked : m_removedMocksInUse.keys())
        modulesWithMock.set(mocked.index());
    auto mockSeenOf = [&](ModuleID mocked, ModuleID holder) -> JSObject* {
        if (mocked != reexportingModule && moduleWithExportsOf(mocked) != reexportingModule)
            return mockSeenBy(mocked, holder);
        JSObject* standing = reexportingModuleMockMode == MockMode::Bypass ? nullptr : activeMock(mocked);
        return standing && seesMockOf(mocked, reexportingModule) ? standing : nullptr;
    };
    auto mockedModuleFor = [&](ModuleID module, ModuleID holder) -> std::optional<ModuleID> {
        for (size_t index : modulesWithMock) {
            if ((ModuleID(index) == module || moduleWithExportsOf(ModuleID(index)) == module) && mockSeenOf(ModuleID(index), holder))
                return ModuleID(index);
        }
        return std::nullopt;
    };
    if (reexportEntry.kind() == ModuleExportKind::Indirect && !reexportEntry.isFromStarExport) {
        std::span<const RequiredNameData> requiredNames = m_table.requiredNames(reexportingModule);
        for (uint32_t place : requiredNamesOfStatementsThatExport(reexportingModule, identifier(reexportEntry.name).string())) {
            ModuleID exporter = requiredNames[place].exporterModule();
            if (!std::ranges::contains(m_table.requests(reexportingModule), exporter, &ModuleRequestData::targetModule))
                continue;
            if (std::optional<ModuleID> mocked = mockedModuleFor(exporter, reexportingModule))
                return MockedReexport { mockSeenOf(*mocked, reexportingModule), identifier(requiredNames[place].name), *mocked };
        }
        return std::nullopt;
    }
    auto indexOfExport = [&](ModuleID module, uint32_t name) -> std::optional<unsigned> {
        return data(module).hasExportTable() ? m_table.findExport(module, identifier(name).impl()) : std::nullopt;
    };
    auto isOmittedByMock = [&](ModuleID module, uint32_t name, ModuleID holder) {
        std::optional<unsigned> index = indexOfExport(module, name);
        if (!index)
            return false;
        if (std::optional<bool> inView = isLeftOutInViewOf(holder, module, *index))
            return *inView;
        auto leftOut = m_exportsLeftOut.find(module);
        return leftOut != m_exportsLeftOut.end() && leftOut->value.get(*index);
    };
    if (reexportEntry.kind() == ModuleExportKind::Indirect) {
        struct ProviderToVisit {
            ModuleID holder;
            PlaceOfExport place;
        };
        Vector<ProviderToVisit, 4> toFollow;
        auto follow = [&](ModuleID holder, unsigned index) {
            const Vector<PlaceOfExport, 1>& providers = starExportProviders(holder)[index];
            for (size_t next = providers.size(); next--;)
                toFollow.append(ProviderToVisit { holder, providers[next] });
        };
        std::optional<unsigned> laid = indexOfExport(reexportingModule, reexportEntry.name);
        if (!laid)
            return std::nullopt;
        BitVector isSeen(m_table.size());
        isSeen.quickSet(reexportingModule.index());
        follow(reexportingModule, *laid);
        std::optional<MockedReexport> mockLackingName;
        while (!toFollow.isEmpty()) {
            ProviderToVisit next = toFollow.takeLast();
            ModuleID module = next.place.module;
            const ModuleExportData& entry = m_table.exports(module)[next.place.index];
            if (std::optional<ModuleID> mocked = mockedModuleFor(module, next.holder)) {
                MockedReexport answer { mockSeenOf(*mocked, next.holder), identifier(entry.name), *mocked };
                if (!isOmittedByMock(module, entry.name, next.holder))
                    return answer;
                if (!mockLackingName)
                    mockLackingName = answer;
                mockLackingName->lacksName = true;
                continue;
            }
            if (isSeen.quickSet(module.index()))
                continue;
            if (!entry.isFromStarExport)
                return std::nullopt;
            follow(module, next.place.index);
        }
        return mockLackingName;
    }
    struct Asked {
        ModuleID module;
        uint32_t name;
        bool isAskedByStar;
        ModuleID holder;
    };
    UncheckedKeyHashSet<std::pair<uint64_t, uint32_t>, DefaultHash<std::pair<uint64_t, uint32_t>>, PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>> seen;
    Vector<Asked, 8> toAsk { Asked { reexportingModule, reexportEntry.name, false, reexportingModule } };
    std::optional<MockedReexport> mockLackingName;
    while (!toAsk.isEmpty()) {
        Asked asked = toAsk.takeLast();
        if (!seen.add({ static_cast<uint64_t>(asked.module.index()) << 32 | asked.name, asked.holder.index() }).isNewEntry)
            continue;
        if (std::optional<ModuleID> mocked = mockedModuleFor(asked.module, asked.holder)) {
            MockedReexport answer { mockSeenOf(*mocked, asked.holder), identifier(asked.name), *mocked };
            if (!asked.isAskedByStar || !isOmittedByMock(asked.module, asked.name, asked.holder))
                return answer;
            if (!mockLackingName)
                mockLackingName = answer;
            mockLackingName->lacksName = true;
            continue;
        }
        std::optional<unsigned> index = indexOfExport(asked.module, asked.name);
        bool hasNameByStar = index && m_table.exports(asked.module)[*index].isFromStarExport;
        size_t countBefore = toAsk.size();
        std::span<const RequiredNameData> requiredNames = m_table.requiredNames(asked.module);
        for (uint32_t place : requiredNamesOfStatementsThatExport(asked.module, identifier(asked.name).string()))
            toAsk.append(Asked { requiredNames[place].exporterModule(), requiredNames[place].name, hasNameByStar, asked.module });
        if (toAsk.size() == countBefore) {
            mockLackingName = std::nullopt;
            break;
        }
        std::ranges::reverse(toAsk.mutableSpan().subspan(countBefore));
    }
    if (mockLackingName)
        return mockLackingName;
    if (reexportEntry.kind() != ModuleExportKind::Variable)
        return std::nullopt;
    std::optional<unsigned> place = indexOfExport(reexportingModule, reexportEntry.name);
    ModuleID writer = place ? m_table.writerOfExport(reexportingModule, *place).value_or(reexportingModule) : reexportingModule;
    for (size_t index : modulesWithMock) {
        JSObject* mock = mockSeenOf(ModuleID(index), writer);
        if (!mock)
            continue;
        for (const SyntheticExportData& entry : m_table.syntheticExports(moduleWithExportsOf(ModuleID(index)))) {
            if (entry.hasVariable && entry.variable == reexportEntry.variable)
                return MockedReexport { mock, identifier(entry.name), ModuleID(index) };
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
        bool declaresNothing = isMockedByFactoryThatTakesOriginal(record->module());
        auto declare = [&](const Identifier& name) {
            if (declaresNothing)
                return;
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

void Modules::releaseMockRecord(ModuleID module)
{
    ModuleRecord* record = m_records.get(module);
    if (!record || !record->hasLiveExports())
        return;
    if (storedStatus(module) == StoredModuleStatus::InRecord) {
        setStoredStatus(module, StoredModuleStatus::Unlinked);
        forgetCapturedExports(module);
    }
    loader()->removeEntriesForRecord(record);
    takeOutRecord(module);
}

void Modules::takeNamesOfMockInPlace(ModuleID module, JSObject* mock)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    LoadedModule& loaded = ensureLoaded(module);
    bool hasTable = data(module).hasExportTable() && !data(module).capturesExports();
    loaded.wasImportedUnderActiveMock = false;
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

bool Modules::setMockInPlaceOfLibraryModule(ModuleID module, JSObject* mock, JSValue requireReturnValue)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    bool stoodInPlace = !!activeMock(module);
    JSObject* endedMock = mock ? nullptr : activeMock(module);
    if (mock) {
        willMockLibraryModule(module);
        if (!m_libraryModulesThatMockReplaces.get(module.index())) {
            if (ensureLoaded(module).preloadMock.get() == mock)
                releaseRemovedPreloadMocks(module);
            return false;
        }
        auto standing = m_activeMockCalls.find(module);
        std::optional<uint32_t> momentStandingMockWasLaid = stoodInPlace && standing != m_activeMockCalls.end() ? std::optional(standing->value.moment) : std::nullopt;
        noteMomentMockComesInPlace(module, mock, stoodInPlace, false);
        if (momentStandingMockWasLaid)
            m_activeMockCalls.find(module)->value.moment = *momentStandingMockWasLaid;
        if (LoadedModule& held = ensureLoaded(module); held.preloadMock.get() == mock)
            held.preloadMockRequireValue.set(*this, requireReturnValue);
        takeNamesOfMockInPlace(module, mock);
        RETURN_IF_EXCEPTION(scope, false);
        if (requireReturnValue && requireReturnValue.isObject())
            mockWhatRequireCacheHolds(module, asObject(requireReturnValue));
        m_modulesWithPendingMock.clear(module.index());
    } else if (!stoodInPlace)
        return false;
    m_activeMockCount += !!mock - stoodInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithActiveMock.set(module.index(), !!mock);
    ensureLoaded(module).activeMock.set(*this, mock);
    if (!mock)
        releaseMockRecord(module);
    else if (ModuleRecord* record = m_records.get(module))
        letRecordAnswerByMock(record, mock);
    if (endedMock) {
        keepMockThatStoodInPlace(module, module, endedMock, BitVector());
        discardNamespace(module);
    }
    if (std::optional<SlotData> word = m_table.mockSlot(module))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : emptyMockSlotValue(module));
    return true;
}

bool Modules::setActiveMock(ModuleID fileOrModule, JSObject* mock, JSValue requireReturnValue, bool keepsEarlierMockForImporters)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    leaveImportersAbove(fileOrModule);
    if (data(fileOrModule).isOfLibrary() || !m_table.hasFilePath(fileOrModule))
        RELEASE_AND_RETURN(scope, setMockInPlaceOfLibraryModule(fileOrModule, mock, requireReturnValue));
    if (mock && hasRemovedMock(fileOrModule)) [[unlikely]] {
        setActiveMock(fileOrModule, nullptr, { });
        RETURN_IF_EXCEPTION(scope, false);
    }
    ModuleID module = moduleWithExportsOf(fileOrModule);
    bool isFile = data(fileOrModule).kind == ModuleKind::CommonJs;
    bool stoodInPlace = !!activeMock(fileOrModule);
    m_modulesWithRemovedMock.clear(fileOrModule.index());
    if (mock)
        m_originalsRetainedAfterMock.set(module.index(), keepsEarlierMockForImporters && (isOfPackage(module) || isImportedByFailedRoot(module) || !data(module).bodyFunction()));
    m_modulesThatOwnFactoryMayLoad.clear(fileOrModule.index());
    m_importsMadeByOwnFactory.removeAll(fileOrModule);
    m_originalsDueUnderMock.clear(fileOrModule.index());
    if (!mock && !stoodInPlace)
        return false;
    if (!m_mockedImports.isEmpty()) [[unlikely]]
        updateRetainedImportNamespaces(module);
    JSObject* endedMock = mock ? nullptr : activeMock(fileOrModule);
    BitVector leftOutByEndedMock = endedMock ? exportsLeftOutOf(module) : BitVector();
    if (mock)
        noteMomentMockComesInPlace(fileOrModule, mock, stoodInPlace, keepsEarlierMockForImporters);
    if (!mock || keepsEarlierMockForImporters || !ensureLoaded(module).wasImportedUnderActiveMock)
        takeNamesOfMockInPlace(module, mock);
    RETURN_IF_EXCEPTION(scope, false);
    m_activeMockCount += !!mock - stoodInPlace;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithActiveMock.set(fileOrModule.index(), !!mock);
    if (mock)
        willMock(fileOrModule);
    if (mock && m_originalsEvaluatedUnderMock.get(module.index())) [[unlikely]]
        setAsideErrorOfModuleThatRanBesideMock(module);
    ensureLoaded(fileOrModule).activeMock.set(*this, mock);
    ensureLoaded(module).activeMock.set(*this, mock);
    for (ModuleID each : { fileOrModule, module }) {
        if (!mock)
            releaseMockRecord(each);
        else if (ModuleRecord* record = m_records.get(each))
            letRecordAnswerByMock(record, mock);
    }
    if (endedMock)
        keepMockThatStoodInPlace(fileOrModule, module, endedMock, WTF::move(leftOutByEndedMock));
    didReplaceNamespaceHandedOn(fileOrModule);
    if (module != fileOrModule)
        didReplaceNamespaceHandedOn(module);
    if (std::optional<SlotData> word = m_table.mockSlot(fileOrModule))
        ProgramStatics::writeVariable(m_instance, *word, mock ? JSValue(mock) : emptyMockSlotValue(fileOrModule));

    LoadedModule& loaded = ensureLoaded(fileOrModule);
    bool originalRuns = isFile && m_originalsBeingEvaluated.get(fileOrModule.index());
    if (mock && loaded.preloadMock.get() == mock) {
        loaded.preloadMockRequireValue.set(*this, requireReturnValue);
        ensureLoaded(module).wasImportedUnderActiveMock |= m_loadedInPreload.get(module.index());
        if (loaded.returnsPreloadMockNamespace) {
            requireReturnValue = namespaceForRequire(module);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    if (mock) {
        if (originalRuns) [[unlikely]]
            loaded.answerOfMockWhileOriginalRuns.set(*this, requireReturnValue);
        else if (isFile) {
            if (m_modulesEvaluatedBeforeMock.get(fileOrModule.index()) && status(fileOrModule) == ModuleStatus::Evaluated && !loaded.exportsOfOriginalBesideMock)
                loaded.exportsOfOriginalBesideMock.set(*this, commonJSExports(fileOrModule));
            markEvaluated(fileOrModule);
            setCommonJSExports(fileOrModule, requireReturnValue);
        } else
            loaded.requiredValue.set(*this, requireReturnValue);
        if (!isFile && loaded.preloadMock.get() == mock && m_loadedInPreload.get(fileOrModule.index())) {
            for (ModuleID each : { fileOrModule, module }) {
                if (status(each) == ModuleStatus::Unlinked || status(each) == ModuleStatus::Linked)
                    setStatus(each, ModuleStatus::Evaluated);
            }
        }
        if (JSObject* entry = loaded.moduleObjectFromRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, requireReturnValue);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, requireReturnValue);
        }
        return true;
    }

    bool wasEvaluatedUnderMock = m_originalsEvaluatedUnderMock.get(module.index());
    bool staysLoaded = wasEvaluatedUnderMock && m_originalsRetainedAfterMock.get(module.index());
    noteWhereOriginalLiesBehindEndOfMock(fileOrModule);
    JSValue releasedBodyError = wasEvaluatedUnderMock && !staysLoaded ? ensureLoaded(module).errorOfModuleThatRanBesideMock.get() : JSValue();
    if (releasedBodyError) [[unlikely]]
        ensureLoaded(module).errorOfModuleThatRanBesideMock.set(*this, JSValue());
    didRemoveMock(fileOrModule);
    forgetWhoLoadedItUnderMockInPlace(fileOrModule);
    bringBackErrorOfModuleThatRanBesideMock(module);
    if (releasedBodyError) [[unlikely]]
        ensureLoaded(module).errorOfModuleThatRanBesideMock.set(*this, releasedBodyError);
    loaded.wasRequiredUnderActiveMock = false;
    JSValue original = isFile ? loaded.exportsOfOriginalBesideMock.get() : JSValue();
    loaded.exportsOfOriginalBesideMock.set(*this, JSValue());
    m_originalsEvaluatedUnderMock.clear(module.index());
    m_modulesRetainedByInFlightLoads.clear(module.index());
    m_originalsDueUnderMock.clear(module.index());
    startLoadingModulesIndividually();
    for (ModuleID each : { fileOrModule, module }) {
        m_modulesThatRanBesideMockAlone.set(each.index(), wasEvaluatedUnderMock && !staysLoaded);
        if (staysLoaded) [[unlikely]] {
            if (status(each) < ModuleStatus::Evaluating)
                setStatus(each, ModuleStatus::Evaluated);
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
            discardNamespace(each);
    }
    restoreNamespaceOfOriginal(module);
    if (originalRuns) [[unlikely]]
        loaded.answerOfMockWhileOriginalRuns.set(*this, JSValue());
    else if (isFile)
        setCommonJSExports(fileOrModule, original ? original : jsUndefined());
    else
        loaded.requiredValue.set(*this, JSValue());
    loaded.moduleObjectFromRequire.set(*this, nullptr);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    return true;
}

void Modules::restoreNamespaceOfOriginal(ModuleID module)
{
    LoadedModule& loaded = ensureLoaded(module);
    JSObject* own = loaded.namespaceBeforeMockInPlace.get();
    if (!own) [[likely]]
        return;
    uncheckedDowncast<ModuleNamespace>(own)->setKeepsToModule(false);
    loaded.namespaceBeforeMockInPlace.set(*this, nullptr);
    if (!isCommonJSModuleOrFacade(module) || status(module) >= ModuleStatus::Evaluating)
        setNamespace(module, ModulePhase::Evaluation, own);
}

void Modules::stashActualKeyExports(ModuleID file)
{
    LoadedModule& loaded = ensureLoaded(file);
    loaded.stashedActualKeyExports.set(*this, commonJSExports(file));
    startLoadingModulesIndividually();
    for (ModuleID each : { file, moduleWithExportsOf(file) }) {
        if (status(each) == ModuleStatus::Evaluated)
            setStatus(each, ModuleStatus::Unlinked);
        m_modulesLoadedBesideGroups.clear(each.index());
        m_modulesOwedToImportersThatRan.set(each.index());
        unsettleForLinks(each);
        discardNamespace(each);
    }
    m_modulesWithNothingOwedBelow.clearAll();
    m_mockedModulesPassedOverByWalks.clear();
    followImportsAnewAtNextQuestion();
    setCommonJSExports(file, jsUndefined());
    loaded.moduleObjectFromRequire.set(*this, nullptr);
    loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
}

void Modules::restoreActualKeyExports(ModuleID file)
{
    LoadedModule* loaded = loadedIfExists(file);
    JSValue parked = loaded ? loaded->stashedActualKeyExports.get() : JSValue();
    if (!parked)
        return;
    loaded->stashedActualKeyExports.set(*this, JSValue());
    if (status(file) > ModuleStatus::Linked) {
        setIsUnderActualKey(file, false);
        return;
    }
    setCommonJSExports(file, parked);
    markEvaluated(file);
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

void Modules::noteEndOfMockThatOnlyWaited(ModuleID module)
{
    if (data(module).kind == ModuleKind::CommonJs && hasRun(module))
        m_filesThatRanBesideMockThatOnlyWaited.set(module.index());
}

void Modules::noteEndOfMockWhoseFactoryWasNeverCalled(ModuleID module)
{
    m_firstMocksInTestFile.remove(module);
}

void Modules::countMocksAnewBehindLoad(ModuleID module)
{
    auto first = m_firstMocksInTestFile.find(module);
    if (first == m_firstMocksInTestFile.end() || !std::exchange(first->value.hasEnded, false))
        return;
    if (!hasRemovedMock(module) && !wasReleasedByLoader(module) && !m_modulesOwedToImportersThatRan.get(module.index()))
        m_firstMocksInTestFile.remove(first);
}

void Modules::didRemoveActiveMockEntry(ModuleID fileOrModule)
{
    LoadedModule& loaded = ensureLoaded(moduleWithExportsOf(fileOrModule));
    loaded.nextRequestReloadsActiveMock = true;
    loaded.activeMockReloadTestFile = 0;
    LoadedModule& held = ensureLoaded(fileOf(fileOrModule));
    held.wasRequiredUnderPreloadMock &= isPreloadMockDisplaced(fileOf(fileOrModule));
    held.wasImportedUnderPreloadMock = false;
    if (std::exchange(held.returnsPreloadMockNamespace, false))
        setCommonJSExports(fileOf(fileOrModule), held.preloadMockRequireValue.get());
    for (auto* moments : { &m_activeMockCalls, &m_preloadMockCalls }) {
        if (auto laid = moments->find(fileOf(fileOrModule)); laid != moments->end())
            laid->value.importsWrapWhatRequireMapHolds = false;
    }
}

void Modules::noteRequireOfMockLaidInPreload(ModuleID moduleOrFacade)
{
    LoadedModule* held = loadedIfExists(fileOf(moduleOrFacade));
    if (!held || !held->activeMock || held->activeMock.get() != held->preloadMock.get())
        return;
    held->wasRequiredUnderPreloadMock = true;
}

bool Modules::wasRequiredUnderPreloadMock(ModuleID file)
{
    LoadedModule* held = loadedIfExists(file);
    return held && held->wasRequiredUnderPreloadMock;
}

void Modules::noteAskUnderMockInPlace(ModuleID fileOrModule)
{
    LoadedModule& loaded = ensureLoaded(moduleWithExportsOf(fileOrModule));
    if (std::exchange(loaded.nextRequestReloadsActiveMock, false))
        loaded.activeMockReloadTestFile = m_instance.vm->aotState().momentOfTestFile;
}

LoadedBy Modules::activeMockReloadedBy(ModuleID fileOrModule)
{
    uint32_t moment = ensureLoaded(moduleWithExportsOf(fileOrModule)).activeMockReloadTestFile;
    if (!moment)
        return LoadedBy::Nobody;
    return moment == m_instance.vm->aotState().momentOfTestFile ? LoadedBy::ThisTestFile : LoadedBy::EarlierTestFile;
}

LoadedBy Modules::evaluatedBy(ModuleID module)
{
    if (!m_hasBegunTestFile || isEvicted(module))
        return LoadedBy::Nobody;
    if (data(module).kind == ModuleKind::CommonJs) {
        if (status(module) != ModuleStatus::Evaluated)
            return LoadedBy::Nobody;
        if (m_filesThatRanInPreload.get(module.index()))
            return LoadedBy::Preload;
        return m_filesThatRanBeforeTestFile.get(module.index()) ? LoadedBy::EarlierTestFile : LoadedBy::ThisTestFile;
    }
    if (m_momentsOfLoads.isEmpty() || !hasRun(module))
        return LoadedBy::Nobody;
    if (m_loadedInPreload.get(module.index()))
        return LoadedBy::Preload;
    if (data(module).kind == ModuleKind::Data)
        return m_dataModulesLoadedBeforeTestFile.get(module.index()) ? LoadedBy::EarlierTestFile : LoadedBy::ThisTestFile;
    return wasLoadedByTestFile(module) ? LoadedBy::ThisTestFile : LoadedBy::EarlierTestFile;
}

LoadedBy Modules::fetchedBy(ModuleID module)
{
    if (m_refetchedFailures.get(module.index()) && isFailedESModule(module))
        return LoadedBy::ThisTestFile;
    LoadedBy loadedBy = evaluatedBy(module);
    if (loadedBy != LoadedBy::Nobody || !m_hasBegunTestFile || isEvicted(module) || m_momentsOfLoads.isEmpty() || !m_momentsOfLoads[module.index()])
        return loadedBy;
    if (m_loadedInPreload.get(module.index()))
        return LoadedBy::Preload;
    return wasLoadedByTestFile(module) ? LoadedBy::ThisTestFile : LoadedBy::EarlierTestFile;
}

JSValue Modules::exportsBeforePatch(ModuleID file) const
{
    const LoadedModule* loaded = loadedIfExists(file);
    return loaded && loaded->isPatchedByMock && loaded->wasEvaluatedWhenPatched ? loaded->valueBeforeMock.get() : JSValue();
}

void Modules::retainOriginalExports(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (isEvicted(file) || status(file) != ModuleStatus::Evaluated)
        return;
    LoadedModule& loaded = ensureLoaded(file);
    JSValue original;
    if (data(file).kind != ModuleKind::CommonJs) {
        if (!data(file).isESModuleOfProgram() || !(hasIntactOriginal(file) || activeMock(file) || loaded.isPatchedByMock))
            return;
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
        original = exportsBeforeMock(file, false, OwedBelowOriginal::StaysOwed);
        if (scope.exception()) [[unlikely]] {
            if (!scope.tryClearException())
                return;
            original = JSValue();
        }
    } else if (hasIntactOriginal(file) || activeMock(file)) {
        original = loaded.exportsOfOriginalBesideMock.get();
        if (!original && !activeMock(file))
            original = commonJSExports(file);
    } else if (loaded.isPatchedByMock)
        original = exportsBeforePatch(file);
    else if (m_modulesEvaluatedBeforeMock.get(file.index()))
        original = commonJSExports(file);
    loaded.retainedOriginalExports.set(*this, original);
    loaded.retainedOriginalModuleObject.set(*this, original ? loaded.requirer.get() : nullptr);
}

void Modules::clearRetainedOriginalExports(ModuleID module)
{
    if (LoadedModule* loaded = loadedIfExists(fileOf(module))) {
        loaded->retainedOriginalExports.set(*this, JSValue());
        loaded->retainedOriginalModuleObject.set(*this, nullptr);
    }
}

JSObject* Modules::retainedOriginalModuleObject(ModuleID module) const
{
    const LoadedModule* loaded = loadedIfExists(fileOf(module));
    return loaded ? loaded->retainedOriginalModuleObject.get() : nullptr;
}

JSValue Modules::revalidateRetainedOriginalExports(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSValue kept = retainedOriginalExports(file);
    if (!kept || wasLoadedByTestFile(file))
        return kept;
    if (data(file).kind == ModuleKind::CommonJs) {
        for (const ModuleRequestData& request : m_table.requests(file)) {
            if (m_modulesOwedToImportersThatRan.get(request.target()) || m_modulesWithRemovedMock.get(request.target()))
                return { };
        }
        return kept;
    }
    evaluateDueDependencies(file);
    RETURN_IF_EXCEPTION(scope, { });
    return kept;
}

JSValue Modules::retainedOriginalExports(ModuleID module) const
{
    const LoadedModule* loaded = loadedIfExists(fileOf(module));
    return loaded ? loaded->retainedOriginalExports.get() : JSValue();
}

void Modules::willRunBodyOfRecord(ModuleID module, ModuleRecord* record)
{
    if (!isMocked(module)) [[likely]]
        return;
    const LoadedModule* loaded = loadedIfExists(module);
    if (loaded && loaded->recordBeforeMockInPlace.get() == record)
        m_originalsEvaluatedUnderMock.set(module.index());
}

bool Modules::isPatchedByMock(ModuleID module) const
{
    const LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isPatchedByMock;
}

bool Modules::wasPatchedAfterEvaluation(ModuleID module) const
{
    const LoadedModule* loaded = loadedIfExists(module);
    return loaded && loaded->isPatchedByMock && loaded->wasEvaluatedWhenPatched;
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
        bool hasFailed = status(importer) == ModuleStatus::Failed || wasMockedAfterFailure(importer);
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID imported = moduleLoadedBy(request);
            if (isEntered && data(imported).needsEvaluation() && status(imported) == ModuleStatus::Evaluating)
                break;
            if (hasFailed && status(imported) == ModuleStatus::Failed && !data(imported).hasTopLevelAwait())
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

bool Modules::hasUnevaluatedLinkedImporter(ModuleID module, std::optional<ModuleID> asking, bool onlyOfProject) const
{
    std::optional<BitVector> passed;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (!data(importer).isESModuleOfProgram() || data(importer).isFacade() || !wasLoadedWithGraph(importer))
            continue;
        bool keepsStatus = keepsStatusOfRunBeforeLoadThatRuns(importer);
        if (status(importer) > ModuleStatus::Evaluating && !keepsStatus)
            continue;
        if (asking && importer == *asking)
            continue;
        if (onlyOfProject && isOfPackage(importer))
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

BitVector Modules::filesThatHaveRun()
{
    BitVector files;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        const ModuleData& moduleData = data(ModuleID(index));
        if (moduleData.kind == ModuleKind::CommonJs && !moduleData.isOfLibrary() && status(ModuleID(index)) == ModuleStatus::Evaluated && !activeMock(ModuleID(index)))
            files.set(index);
    }
    return files;
}

BitVector Modules::dataFilesThatHaveRun()
{
    BitVector files;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        if (data(ModuleID(index)).kind == ModuleKind::Data && status(ModuleID(index)) == ModuleStatus::Evaluated && !activeMock(ModuleID(index)))
            files.set(index);
    }
    return files;
}

bool Modules::wasLoadedBeforeTestFile(ModuleID module)
{
    if (data(module).kind == ModuleKind::Data)
        return m_hasBegunTestFile && !isEvicted(module) && m_dataModulesLoadedBeforeTestFile.get(module.index());
    LoadedBy loadedBy = evaluatedBy(module);
    return loadedBy == LoadedBy::EarlierTestFile || loadedBy == LoadedBy::Preload;
}

bool Modules::tryKeepOriginalIntact(ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking, bool evenIfEvaluationFailed)
{
    bool wasRunByThisMockCall = std::exchange(m_moduleWhoseBodyMockCallRan, std::nullopt) == module;
    if (auto waiting = m_mocksWithIntactOriginal.find(module); waiting != m_mocksWithIntactOriginal.end() && !waiting->value.isInPlace && wasReleasedByLoader(module))
        return true;
    const ModuleData& moduleData = data(module);
    bool isFile = moduleData.kind == ModuleKind::CommonJs && !moduleData.isOfLibrary();
    bool isData = moduleData.kind == ModuleKind::Data && !moduleData.isOfLibrary();
    if ((!isFile && !isData && (!moduleData.isESModuleOfProgram() || moduleData.isFacade())) || !m_table.hasFilePath(module) || !m_table.mockSlot(module))
        return false;
    bool hasFailed = (evenIfEvaluationFailed || wasReleasedByLoader(module)) && moduleData.isESModuleOfProgram() && !moduleData.isFacade() && status(module) == ModuleStatus::Failed;
    bool isBehindAwait = moduleData.isESModuleOfProgram() && !moduleData.isFacade() && status(module) == ModuleStatus::EvaluatingAsync;
    if ((!hasFailed && !isBehindAwait && !hasRun(module)) || isMocked(module) || activeMock(module) || isEvicted(module) || m_momentsOfLoads.isEmpty())
        return false;
    ModuleID withExports = moduleWithExportsOf(module);
    bool ranInPreload = isFile && m_filesThatRanInPreload.get(module.index());
    bool wasEvaluatedUnderMock = isFile && m_filesThatRanBesideMockThatOnlyWaited.get(module.index());
    if (ModuleStatus facadeStatus = status(withExports); isFile && (!onlyIfLoadedInPreload || ranInPreload || wasEvaluatedUnderMock) && withExports != module && (facadeStatus == ModuleStatus::Unlinked || facadeStatus == ModuleStatus::Linked)) {
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        evaluateSynchronously(withExports, SynchronousEvaluationReason::Require);
        RETURN_IF_EXCEPTION(scope, false);
        m_facadesCapturedForMock.set(withExports.index());
    }
    if (isFile && (withExports == module || status(withExports) != ModuleStatus::Evaluated || !didCaptureExports(withExports)))
        return false;
    if (onlyIfLoadedInPreload && !ranInPreload && !wasEvaluatedUnderMock && !m_loadedInPreload.get(withExports.index()))
        return false;
    if ((isGraphRunning() || hasFailed) && hasUnevaluatedLinkedImporter(module, asking))
        return false;
    if (wasRunByThisMockCall && hasUnevaluatedLinkedImporter(module, asking, true))
        return false;
    bool isOfThisTestFile = wasLoadedByTestFile(withExports);
    if (!isOfThisTestFile)
        releaseImportersOf(module, ReleasedImporters::All);
    VMState& state = m_instance.vm->aotState();
    auto laid = m_mocksWithIntactOriginal.set(module, MockWithIntactOriginal { momentOfFirstMockInTestFile(module), state.momentOfTestFile });
    laid.iterator->value.momentThisWasLaid = ++state.momentOfLoads;
    laid.iterator->value.wasLoadedByTestFile = isOfThisTestFile;
    laid.iterator->value.wasInstalledAfterFailure = hasFailed;
    if (isOfThisTestFile && !wasLoadedBeforeTestFile(module))
        laid.iterator->value.filesThatHadRun = filesThatHaveRun();
    auto setAsideRecord = [&](ModuleID each) {
        ModuleRecord* record = m_records.get(each);
        if (!record)
            return;
        loader()->removeEntriesForRecord(record);
        if (JSValue error = record->evaluationError())
            ensureLoaded(each).errorOfModuleThatRanBesideMock.set(*this, error);
        if (storedStatus(each) == StoredModuleStatus::InRecord)
            setStoredStatus(each, StoredModuleStatus::Evaluated);
        JSObject* madeForModuleItself = ensureLoaded(each).recordBeforeMockInPlace.get();
        ensureLoaded(each).recordBeforeMockInPlace.set(*this, record);
        if (!isFile && record->status() != CyclicModuleRecord::Status::Evaluating && record->status() != CyclicModuleRecord::Status::EvaluatingAsync)
            record->setKeepsToModule();
        dropAnswersKept();
        Locker locker { m_state.caches->lock() };
        if (madeForModuleItself && madeForModuleItself != record)
            forgetAnswersThatName(uncheckedDowncast<ModuleRecord>(madeForModuleItself));
        forgetAnswersThatName(record);
        m_records.remove(each);
    };
    setAsideRecord(module);
    if (isFile)
        setAsideRecord(withExports);
    return true;
}

bool Modules::setMockWithIntactOriginal(ModuleID module, JSObject* mock, JSValue requireReturnValue, bool keepsEarlierMockForImporters)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    leaveImportersAbove(module);
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
    auto found = m_mocksWithIntactOriginal.find(module);
    if (found == m_mocksWithIntactOriginal.end())
        return false;
    MockWithIntactOriginal& kept = found->value;
    LoadedModule& loaded = ensureLoaded(module);
    loaded.activeMock.set(*this, mock);
    ensureLoaded(withExports).activeMock.set(*this, mock);
    ProgramStatics::writeVariable(m_instance, *m_table.mockSlot(module), mock);
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
    auto* madeUnderEarlierMock = kept.isInPlace && !keepsEarlierMockForImporters ? dynamicDowncast<ModuleNamespace>(namespaceIfCreated(withExports, ModulePhase::Evaluation)) : nullptr;
    if (!madeUnderEarlierMock)
        setNamespace(withExports, ModulePhase::Evaluation, nullptr);
    BitVector reaching = transitiveImportersOf(module);
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (each == withExports)
            continue;
        JSObject* made = namespaceIfCreated(each, ModulePhase::Evaluation);
        LoadedModule* other = loadedIfExists(each);
        JSObject* setAside = other ? other->namespaceBeforeMockInPlace.get() : nullptr;
        for (JSObject* object : { made, setAside }) {
            if (auto* filed = dynamicDowncast<ModuleNamespace>(object))
                filed->leaveStructureThatCachesKnow(*m_instance.vm);
        }
        if (!data(each).hasDynamicStarExport() || !reaching.quickGet(index))
            continue;
        if (JSObject* unfilled = other ? other->namespaceToRestore.get() : nullptr) {
            other->namespaceToRestore.set(*this, nullptr);
            if (!made && !setAside && !activeMock(each))
                other->namespaceBeforeMockInPlace.set(*this, unfilled);
        }
        if (!made)
            continue;
        LoadedModule& behindStars = ensureLoaded(each);
        if (!behindStars.namespaceBeforeMockInPlace && !activeMock(each))
            behindStars.namespaceBeforeMockInPlace.set(*this, made);
        discardNamespace(each);
    }
    if (ModuleRecord* record = m_records.get(module))
        letRecordAnswerByMock(record, mock);
    if (ModuleRecord* record = isFile ? m_records.get(withExports) : nullptr)
        letRecordAnswerByMock(record, mock);
    auto answerRequire = [&] {
        loaded.requiredValue.set(*this, requireReturnValue);
        if (isFile)
            setCommonJSExports(module, requireReturnValue);
        if (JSObject* entry = loaded.moduleObjectFromRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, requireReturnValue);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, requireReturnValue);
        }
    };
    if (kept.isInPlace) {
        answerRequire();
        if (madeUnderEarlierMock) {
            madeUnderEarlierMock->setMockOfView(*m_instance.vm, mock);
            ModuleNamespace::DynamicExports exportsOfModule;
            createDynamicExports(withExports, exportsOfModule.names, exportsOfModule.capturedValues, exportsOfModule.liveSources);
            RETURN_IF_EXCEPTION(scope, false);
            madeUnderEarlierMock->replaceDynamicExports(*m_instance.vm, exportsOfModule);
        }
        return true;
    }
    kept.isInPlace = true;
    kept.exportsLeftOutBefore = WTF::move(exportsLeftOutBefore);
    kept.exportsHiddenBefore = WTF::move(exportsHiddenBefore);
    ++m_activeMockCount;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithActiveMock.set(module.index());
    loaded.requiredValueBeforeMockInPlace.set(*this, loaded.requiredValue.get());
    if (isFile)
        loaded.exportsOfOriginalBesideMock.set(*this, commonJSExports(module));
    else {
        loaded.displacedModuleObject.set(*this, loaded.moduleObjectFromRequire.get());
        loaded.moduleObjectFromRequire.set(*this, nullptr);
        loaded.exportsGivenToObjectMadeByRequire.set(*this, JSValue());
    }
    answerRequire();
    return true;
}

bool Modules::reachesStarCycle(ModuleID root) const
{
    BitVector isFinished(m_table.size());
    BitVector isOnPath(m_table.size());
    Vector<std::pair<ModuleID, unsigned>, 8> path { { root, 0 } };
    isOnPath.quickSet(root.index());
    while (!path.isEmpty()) {
        auto [module, place] = path.last();
        DynamicStars stars = m_table.dynamicStars(module);
        if (place == stars.numberOfPlaces()) {
            isOnPath.quickClear(module.index());
            isFinished.quickSet(module.index());
            path.removeLast();
            continue;
        }
        ++path.last().second;
        std::optional<ModuleRequestData> request = stars.at(place);
        if (!request || isFinished.quickGet(request->target()))
            continue;
        if (isOnPath.quickSet(request->target()))
            return true;
        path.append({ request->targetModule(), 0 });
    }
    return false;
}

void Modules::removeMockWithIntactOriginal(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockWithIntactOriginal kept = m_mocksWithIntactOriginal.take(module);
    releaseSettledImports();
    m_modulesWithPendingMock.clear(module.index());
    m_modulesEvaluatedBeforeMock.clear(module.index());
    m_modulesWithDeferredMock.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didRemoveMock(module);
    ModuleID withExports = moduleWithExportsOf(module);
    bool isFile = withExports != module;
    LoadedModule& loaded = ensureLoaded(module);
    auto bringBackRecord = [&](ModuleID each) {
        if (ModuleRecord* recordOfMock = m_records.get(each)) {
            loader()->removeEntriesForRecord(recordOfMock);
            if (storedStatus(each) == StoredModuleStatus::InRecord)
                setStoredStatus(each, StoredModuleStatus::Evaluated);
            takeOutRecord(each);
        }
        LoadedModule& held = ensureLoaded(each);
        JSObject* before = held.recordBeforeMockInPlace.get();
        if (!before)
            return;
        auto* record = uncheckedDowncast<ModuleRecord>(before);
        held.recordBeforeMockInPlace.set(*this, nullptr);
        record->setKeepsToModule(false);
        {
            Locker locker { m_state.caches->lock() };
            m_records.add(each, record);
            m_storedReferences.recordAll();
        }
        didChangeStatusInRecord(each);
        if (record->ownsStatus())
            setStoredStatus(each, StoredModuleStatus::InRecord);
    };
    bringBackRecord(module);
    if (isFile)
        bringBackRecord(withExports);
    bool takesCopiesAnew = isFile && m_facadesCapturedForMock.get(withExports.index());
    if (!kept.isInPlace) {
        if (takesCopiesAnew)
            RELEASE_AND_RETURN(scope, recaptureExports(withExports));
        return;
    }
    LoadedModule& exporting = ensureLoaded(withExports);
    BitVector leftOutByEndedMock = exportsLeftOutOf(withExports);
    takeNamesOfMockInPlace(withExports, nullptr);
    scope.assertNoExceptionExceptTermination();
    if (kept.exportsLeftOutBefore)
        m_exportsLeftOut.set(withExports, WTF::move(*kept.exportsLeftOutBefore));
    if (JSObject* own = exporting.capturedExportsBeforeMockInPlace.get()) {
        exporting.capturedExports.set(*this, own);
        exporting.capturedExportsBeforeMockInPlace.set(*this, nullptr);
    }
    m_pendingMockCalls.remove(module);
    keepNamespacesMadeUnder(module, withExports, loaded.activeMock.get(), keepEndedMock(module, loaded.activeMock.get(), { kept.momentThisWasLaid, kept.momentOfTestFile }, 0, WTF::move(leftOutByEndedMock)));
    loaded.activeMock.set(*this, nullptr);
    exporting.activeMock.set(*this, nullptr);
    --m_activeMockCount;
    m_importersAskedAboutOriginal.clear();
    m_modulesWithActiveMock.clear(module.index());
    ProgramStatics::writeVariable(m_instance, *m_table.mockSlot(module), emptyMockSlotValue(module));
    loaded.requiredValue.set(*this, loaded.requiredValueBeforeMockInPlace.get());
    loaded.requiredValueBeforeMockInPlace.set(*this, JSValue());
    if (isFile) {
        JSValue original = loaded.exportsOfOriginalBesideMock.get();
        loaded.exportsOfOriginalBesideMock.set(*this, JSValue());
        setCommonJSExports(module, original);
        if (JSObject* entry = loaded.moduleObjectFromRequire) {
            entry->putDirect(*m_instance.vm, m_instance.vm->propertyNames->exports, original);
            loaded.exportsGivenToObjectMadeByRequire.set(*this, original);
        }
    } else {
        JSObject* entry = loaded.displacedModuleObject.get();
        loaded.displacedModuleObject.set(*this, nullptr);
        loaded.moduleObjectFromRequire.set(*this, entry);
        loaded.exportsGivenToObjectMadeByRequire.set(*this, entry ? loaded.requiredValue.get() : JSValue());
    }
    Vector<ModuleID, 4> othersThatStand;
    for (auto& other : m_mocksWithIntactOriginal) {
        if (other.value.isInPlace)
            othersThatStand.append(other.key);
    }
    bool anotherMockStands = !othersThatStand.isEmpty();
    BitVector reaching = transitiveImportersOf(module);
    BitVector reachingAnotherThatStands = transitiveImportersOf(othersThatStand.span());
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID each(index);
        if (!reaching.quickGet(index))
            continue;
        if (each == withExports || data(each).hasDynamicStarExport())
            discardNamespace(each);
        if (each == withExports || !data(each).hasDynamicStarExport() || reachingAnotherThatStands.quickGet(index))
            continue;
        LoadedModule* behindStars = loadedIfExists(each);
        if (JSObject* before = behindStars ? behindStars->namespaceBeforeMockInPlace.get() : nullptr) {
            behindStars->namespaceBeforeMockInPlace.set(*this, nullptr);
            if (!reachesStarCycle(each))
                behindStars->namespaceToRestore.set(*this, before);
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
        RELEASE_AND_RETURN(scope, recaptureExports(withExports));
}

JSObject* Modules::originalNamespace(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto mock = m_mocksWithIntactOriginal.find(fileOf(module));
    if (mock == m_mocksWithIntactOriginal.end() && (isMocked(module) || activeMock(module))) [[unlikely]]
        RELEASE_AND_RETURN(scope, namespaceOfModuleThatRanBesideMock(module));
    if (mock == m_mocksWithIntactOriginal.end())
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
    auto mock = m_mocksWithIntactOriginal.find(fileOf(module));
    if (mock != m_mocksWithIntactOriginal.end() && mock->value.isInPlace)
        return mock->value.exportsHiddenBefore.get(index);
    if (activeMock(module)) [[unlikely]]
        return m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index);
    return (m_table.exports(module)[index].isFromStarExport && isExportAmbiguous(module, index)) || isLeftOut(module, index, TablesBehindStars::MayLackName);
}

JSValue Modules::readExportOfModuleItself(ModuleID module, unsigned index)
{
    return readExport(module, index, MockMode::Bypass);
}






const MockOperations& Modules::mockOperations()
{
    static constexpr MockOperations operations {
        .runMocksInGraph = [](Modules& modules, ModuleID root, LoadRootKind rootOfLoad) { return modules.runMocksInGraphSlow(root, rootOfLoad); },
        .runMocksInFetchedGraph = [](Modules& modules, ModuleID module) { return modules.runMocksInFetchedGraphSlow(module); },
        .releaseRemovedMock = [](Modules& modules, ModuleID module) { modules.releaseRemovedMockSlow(module); },
        .runPendingMock = [](Modules& modules, ModuleID module) { return modules.runPendingMockSlow(module); },
        .removeUnusedLingeringMocks = [](Modules& modules) { modules.removeUnusedLingeringMocksSlow(); },
        .findMockForReexport = [](const Modules& modules, ModuleID reexportingModule, const ModuleExportData& reexportEntry, MockMode reexportingModuleMockMode) { return modules.findMockForReexportSlow(reexportingModule, reexportEntry, reexportingModuleMockMode); },
        .isMockedThroughStarExports = [](Modules& modules, ModuleID module, MockFilter which) { return modules.isMockedThroughStarExportsSlow(module, which); },
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
    std::optional<ModuleID> facade = data(module).kind == ModuleKind::CommonJs ? m_table.facadeOf(module) : std::nullopt;
    return facade.value_or(module);
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
    if (isListed ? !found->isReplaceable : !data(m_table.moduleBehindFacade(module).value_or(module)).mayBeMocked())
        return false;
    if (!isListed && data(module).hasExportTable() && !m_table.moduleBehindFacade(module))
        RELEASE_AND_RETURN(scope, tryReplaceExportAddedByMock(module, name, value));

    Identifier key = Identifier::fromUid(vm, name);
    JSObject* captured = capturedExports(module);
    RETURN_IF_EXCEPTION(scope, false);
    LoadedModule& loaded = ensureLoaded(module);
    bool hasItsNames = loaded.exportsWhatMockNamesAlone ? loaded.wasLoadedBehindMock : !m_libraryModulesThatMockReplaces.get(module.index());
    if (hasItsNames && !storedValue(realm, captured, key))
        return false;
    if ((m_libraryModulesWithMock.get(module.index()) || m_moduleBeingPatched) && !loaded.exportsWhatMockNamesAlone) {
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
    SetForScope isBypassingMocks(m_isBypassingMocks, false);
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
        if (std::optional<ModuleID> facade = m_table.facadeOf(module); facade && data(*facade).capturesExports())
            module = *facade;
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

void Modules::discardNamespace(ModuleID module)
{
    LoadedModule* loaded = loadedIfExists(module);
    if (loaded && loaded->namespaceToRestore)
        loaded->namespaceToRestore.set(*this, nullptr);
    JSObject* made = namespaceIfCreated(module, ModulePhase::Evaluation);
    if (!made)
        return;
    setNamespace(module, ModulePhase::Evaluation, nullptr);
    loaded = loadedIfExists(module);
    if (!loaded || loaded->requiredValue.get() != JSValue(made))
        return;
    loaded->requiredValue.set(*this, JSValue());
    loaded->moduleObjectFromRequire.set(*this, nullptr);
    loaded->exportsGivenToObjectMadeByRequire.set(*this, JSValue());
}

void Modules::setNamespace(ModuleID module, ModulePhase phase, JSObject* namespaceObject)
{
    if (phase == ModulePhase::Evaluation && data(module).holdsNamespaceInValue() && !m_modulesWithEarlierNamespaceInValue.get(module.index()))
        ProgramStatics::writeNamespace(m_instance, data(module).value, namespaceObject);
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
        if (std::optional<ModuleID> facade = m_table.facadeOf(module); facade && data(*facade).capturesExports())
            RELEASE_AND_RETURN(scope, moduleNamespace(*facade, phase, shouldPreventExtensions));
    }
    if (std::optional<ModuleID> file = m_table.moduleBehindFacade(module); file && !moduleData.capturesExports() && m_table.exports(module).empty()) {
        JSObject* namespaceOfFile = moduleNamespace(*file, phase, shouldPreventExtensions);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (auto* compiled = dynamicDowncast<ModuleNamespace>(namespaceOfFile))
            compiled->setFacade(module);
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

    bool isRequired = phase == ModulePhase::Evaluation && m_modulesMarkedAsRequired.get(module.index());
    if (phase == ModulePhase::Evaluation && m_modulesWithExtensibleNamespace.get(module.index()))
        shouldPreventExtensions = false;
    LoadedModule* loadedBeforeMock = phase == ModulePhase::Evaluation ? loadedIfExists(module) : nullptr;
    auto* restored = loadedBeforeMock ? dynamicDowncast<ModuleNamespace>(loadedBeforeMock->namespaceToRestore.get()) : nullptr;
    ModuleNamespace::DynamicExports exportsBefore;
    if (restored) [[unlikely]] {
        loadedBeforeMock->namespaceToRestore.set(*this, nullptr);
        exportsBefore = restored->dynamicExports();
        restored->replaceDynamicExports(vm, { });
    }
    ModuleNamespace* namespaceObject = restored ? restored : ModuleNamespace::create(vm, namespaceStructure(shouldPreventExtensions), module, phase, { });
    size_t place = m_namespacesBeingMade.size();
    size_t outermostReadBefore = std::exchange(m_outermostNamespaceReadWhileBeingMade, notFound);
    auto forgetsNamespacesThatLackNames = makeScopeExit([&] {
        m_namespacesThatLackNames.removeAllMatching([&](const NamespaceThatLacksNames& lacking) {
            if (lacking.outermostRead < place)
                return false;
            if (namespaceIfCreated(lacking.module, ModulePhase::Evaluation) == lacking.object)
                setNamespace(lacking.module, ModulePhase::Evaluation, nullptr);
            if (LoadedModule* loaded = loadedIfExists(lacking.module); loaded && loaded->namespaceBeforeMockInPlace.get() == lacking.object)
                loaded->namespaceBeforeMockInPlace.set(*this, nullptr);
            return true;
        });
    });
    if (phase == ModulePhase::Evaluation)
        m_namespacesBeingMade.append({ module, namespaceObject });
    ModuleNamespace::DynamicExports exportObjects;
    createDynamicExports(module, exportObjects.names, exportObjects.capturedValues, exportObjects.liveSources);
    if (phase == ModulePhase::Evaluation)
        m_namespacesBeingMade.removeLast();
    size_t outermostRead = m_outermostNamespaceReadWhileBeingMade;
    bool lacksNames = phase == ModulePhase::Evaluation && outermostRead < place;
    m_outermostNamespaceReadWhileBeingMade = std::min(outermostReadBefore, outermostRead);
    if (scope.exception()) [[unlikely]] {
        if (restored) {
            restored->replaceDynamicExports(vm, exportsBefore);
            ensureLoaded(module).namespaceToRestore.set(*this, restored);
        }
        return nullptr;
    }
    namespaceObject->replaceDynamicExports(vm, exportObjects);
    if (isRequired && !namespaceObject->hasExport(vm.propertyNames->__esModule.impl())) {
        if (m_modulesWithDeferredMark.get(module.index()))
            namespaceObject->setMarkerDeferred(true);
        else
            namespaceObject->setESModuleMarker(true);
    }
    if (JSObject* mock = phase == ModulePhase::Evaluation && hasIntactOriginal(module) ? activeMock(module) : nullptr) [[unlikely]]
        namespaceObject->setMockOfView(vm, mock);
    setNamespace(module, phase, namespaceObject);
    if (lacksNames)
        m_namespacesThatLackNames.append({ module, namespaceObject, outermostRead });
    return namespaceObject;
}

void Modules::createDynamicExports(ModuleID module, JSCellButterfly*& names, JSObject*& capturedValues, JSObject*& liveSources)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    KeepsAnswers keepsAnswers(*this);
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
    if (m_activeMockCount && activeMock(module)) [[unlikely]]
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
        if (builtin != Builtins::ModuleRequireResolve || !host().ensureResolveFunction)
            return member;
        RELEASE_AND_RETURN(scope, host().ensureResolveFunction(m_instance.globalObject, ensureLoaded(module).requirer, member));
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

bool Modules::standsForFileOutsideImage(ModuleID module) const
{
    return data(module).isHostValue() && isAbsoluteFileSystemPath(m_table.fullPath(module));
}

JSString* Modules::resolve(ModuleID module, ResolvedSpecifierForm form)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isURL = form == ResolvedSpecifierForm::URL;
    if (m_table.hasFilePath(module) && isURL)
        return jsString(vm, URL::fileURLWithFileSystemPath(m_table.fullPath(module)).string());
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
        String path = m_table.fullPath(module);
        loaded.sourceOrigin = makeUniqueWithoutFastMallocCheck<SourceOrigin>(m_table.hasFilePath(module) ? URL::fileURLWithFileSystemPath(path) : URL(path));
    }
    return *loaded.sourceOrigin;
}

bool Modules::hasMadeOrigin(const SourceOrigin& origin) const
{
    for (const auto& loaded : m_loaded.values()) {
        if (loaded->sourceOrigin && loaded->sourceOrigin->url() == origin.url())
            return true;
    }
    return false;
}

bool isOriginOfCompiledFile(const Instance& instance, const SourceOrigin& origin)
{
    return modulesOf(const_cast<Instance&>(instance)).hasMadeOrigin(origin);
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
    if (JSValue error = ensureLoaded(importer).cachedLoadError) {
        throwException(m_instance.globalObject, scope, error);
        return;
    }
    throwNewFailure(id, isForRequire);
    if (Exception* exception = scope.exception(); exception && !vm.isTerminationException(exception))
        ensureLoaded(importer).cachedLoadError.set(*this, exception->value());
}

void Modules::throwNewFailure(ModuleFailureID id, bool isForRequire)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    const ModuleFailureData& failure = m_table.failure(id);
    String specifier = ProgramStrings::text(m_table.programTables(), failure.specifierString());
    if (failure.isAbsentExport()) {
        JSObject* error = nullptr;
        if (failure.isFromExportStatement())
            error = createExportStatementFindsNothingError(realm, specifier, ProgramStrings::text(m_table.programTables(), failure.exportSpecifierString()));
        else {
            String exporterPath = m_table.fullPath(failure.importerModule());
            if (m_originalsEvaluatedUnderMock.get(fileOf(failure.importerModule()).index()) && wasLoadedUnderActualKey(failure.importerModule())) [[unlikely]]
                exporterPath = makeString(exporterPath, "?actual"_s);
            error = createExportNotFoundError(realm, specifier, exporterPath);
        }
        uncheckedDowncast<ErrorInstance>(error)->setStackFrames(vm, { });
        if (isTestRun()) {
            if (!m_errorsOfExportsThatBuildFoundAbsent) {
                m_errorsOfExportsThatBuildFoundAbsent = createTableOfExports();
                m_storedReferences.recordAll();
            }
            storeValue(realm, m_errorsOfExportsThatBuildFoundAbsent, m_failuresByExportsThatBuildFoundAbsent.size(), error);
            m_failuresByExportsThatBuildFoundAbsent.append(id);
        }
        throwException(realm, scope, error);
        return;
    }
    String importer = m_table.fullPath(failure.importerModule());
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
    if (stepsLeft == std::size(jobsOfImportOfEvaluatedModule)) {
        if (Modules* modules = modulesOf(promise->realm()))
            modules->didRunFirstImportStep(promise);
    }
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
        if (JSValue error = modules.mockLoadError(imported)) [[unlikely]]
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

static ASCIILiteral typeThatReadsTextAsData(const ScriptFetchParameters& attributes)
{
    if (attributes.type() == ScriptFetchParameters::Type::JSON)
        return "json"_s;
    if (attributes.type() != ScriptFetchParameters::Type::HostDefined)
        return { };
    for (ASCIILiteral type : { "jsonc"_s, "json5"_s, "toml"_s, "yaml"_s, "xml"_s, "text"_s }) {
        if (attributes.hostDefinedImportType() == type)
            return type;
    }
    return { };
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
    throwException(realm, scope, mockLoadError(module));
    return caller == ImportCaller::CompiledCode ? importPromiseRejectedByLoader(realm, scope) : rejectedPromiseWithPendingException(realm, scope);
}

static JSC_DECLARE_HOST_FUNCTION(importBehindFactories);
static JSC_DECLARE_HOST_FUNCTION(rejectImportAfterBody);
static JSC_DECLARE_HOST_FUNCTION(didSettleImportBehindFactories);
static JSC_DECLARE_HOST_FUNCTION(didFulfillImportThatLinkedAnew);
static JSC_DECLARE_HOST_FUNCTION(didRejectImportThatLinkedAnew);
static JSC_DECLARE_HOST_FUNCTION(didFulfillImportUnderMock);
static JSC_DECLARE_HOST_FUNCTION(didRejectImportUnderMock);
static JSC_DECLARE_HOST_FUNCTION(settleCallsBehindEarlierCall);
static JSC_DECLARE_HOST_FUNCTION(settleCallBehindOwnLoad);
static constexpr unsigned firstCallInLine = 2;
static JSC_DECLARE_HOST_FUNCTION(importByKeyBehindFactories);

static JSC_DECLARE_HOST_FUNCTION(continueImportWithModuleItself);
static JSC_DECLARE_HOST_FUNCTION(didFetchFactoryChainImport);

static JSC_DECLARE_HOST_FUNCTION(resumeDelayedImport);

JSC_DEFINE_HOST_FUNCTION(resumeDelayedImport, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->resumeDelayedImport(asArray(callFrame->argument(1)));
    return JSValue::encode(jsUndefined());
}

void Modules::resumeDelayedImport(JSArray* deferredCall)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint32_t jobsLeft = deferredCall->getIndexQuickly(0).asUInt32();
    if (jobsLeft) {
        deferredCall->setIndexQuickly(vm, 0, jsNumber(jobsLeft - 1));
        JSPromise* fulfilled = JSPromise::create(vm, realm->promiseStructure());
        fulfilled->fulfill(vm, jsUndefined());
        JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), JSC::AOT::resumeDelayedImport, ImplementationVisibility::Private);
        fulfilled->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), jsUndefined(), deferredCall);
        return;
    }
    m_isResumingDelayedImport = true;
    m_delayedImportSequenceNumber = deferredCall->getIndexQuickly(4).asUInt32();
    m_isDelayedImportFromDependency = deferredCall->getIndexQuickly(5).asBoolean();
    m_delayedImportTargets.removeFirst(ModuleID(deferredCall->getIndexQuickly(1).asUInt32()));
    JSValue referrer = deferredCall->getIndexQuickly(2);
    JSPromise* loaded = import(ModuleID(deferredCall->getIndexQuickly(1).asUInt32()), referrer.isUndefined() ? ModuleID() : ModuleID(referrer.asUInt32()));
    RETURN_IF_EXCEPTION(scope, void());
    uncheckedDowncast<JSPromise>(deferredCall->getIndexQuickly(3))->pipeFrom(vm, loaded);
}

JSPromise* Modules::import(ModuleID module, ModuleID referrer, JSValue options)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope currentImportCallerModule(m_currentImportCallerModule, referrer);
    SetForScope currentImportRequestedModule(m_currentImportRequestedModule, module);
    uint32_t callSequenceNumber = m_delayedImportSequenceNumber;
    if (auto* willLoad = m_mocks ? host().willLoad : nullptr; willLoad && referrer && !m_isResumingDelayedImport) [[unlikely]]
        callSequenceNumber = willLoad(realm, fileOf(referrer), fileOf(module), LoadKind::Import);
    SetForScope currentImportSequenceNumber(m_currentImportSequenceNumber, callSequenceNumber);

    bool wasDelayed = std::exchange(m_isResumingDelayedImport, false);
    bool isCallFromDependency = wasDelayed ? m_isDelayedImportFromDependency : referrer && isMocked(module) && status(fileOf(referrer)) == ModuleStatus::Evaluating && importsStatically(module, fileOf(referrer));
    SetForScope isCurrentImportFromDependency(m_isCurrentImportFromDependency, isCallFromDependency);
    removeUnusedLingeringMocks();
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (isMockLingering(module) && m_modulesWithPendingMock.get(module.index()) && !isEvaluatedByLoader(module) && isResolvedForInFlightLoads(module)) [[unlikely]] {
        removeLingeringMock(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    bool hasMockToRun = (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(fileOf(module).index())) && !m_mocksRetriedAfterFailure.get(module.index()) && !m_mocksRetriedAfterFailure.get(fileOf(module).index()) && !m_instance.vm->synchronousModuleQueue();
    if (!wasDelayed && (hasMockToRun || m_originalBeingEvaluated == module || (isCallFromDependency && m_modulesWithRunningFactory.get(module.index()))) && options.isUndefined()) [[unlikely]] {
        JSPromise* promise = JSPromise::create(*m_instance.vm, realm->promiseStructure());
        MarkedArgumentBuffer call;
        call.append(jsNumber(3));
        call.append(jsNumber(module.index()));
        call.append(referrer ? jsNumber(referrer.index()) : jsUndefined());
        call.append(promise);
        call.append(jsNumber(callSequenceNumber));
        call.append(jsBoolean(isCallFromDependency));
        JSArray* deferredCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (!m_delayedImportTargets.contains(module))
            m_mockEntriesToEvict.clear(module.index());
        m_delayedImportTargets.append(module);
        scope.release();
        resumeDelayedImport(deferredCall);
        return promise;
    }

    bool isFromOwnFactory = isRequestedByOwnFactory(module);
    SetForScope importThatBodySetAsideCallsNow(m_importThatBodySetAsideCallsNow, referrer && !m_mocksWithIntactOriginal.isEmpty() ? std::optional(std::pair(referrer, module)) : std::nullopt);
    if (m_mocks) [[unlikely]]
        noteLoadBegunByFactories(module);
    if (!m_facadesCapturedForMock.isEmpty()) [[unlikely]] {
        takeCopiesAnewWhereMockCallAloneTookThem(moduleWithExportsOf(module));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    ModuleID walkedBelow = module;
    bool walkStopped = false;
    auto start = [&](ModuleID started) -> JSPromise* {
        size_t numberOfRoots = m_factoryBlockedImportRoots.size();
        JSPromise* promise = startImport(started, asyncEvaluationOrder(referrer), ImportCaller::CompiledCode, ImportSourceState::MayArriveLater, nullptr, JSValue(), started == walkedBelow ? std::optional(walkStopped) : std::nullopt);
        RETURN_IF_EXCEPTION(scope, promise);
        if (!m_modulesWithLingeringMock.isEmpty() && m_factoryBlockedImportRoots.size() == numberOfRoots) [[unlikely]]
            holdMockThatStandsUntilImportSettles(started, promise);
        if (holdsLinksOfLoadThatMayFail(started)) [[unlikely]]
            hearWhetherImportThatLinkedAnewFails(started, promise);
        if (m_mocks) [[unlikely]]
            hearWhetherImportUnderMockFails(started, promise);
        ModuleID file = fileOf(started);
        if (isFromOwnFactory || importsOriginalDespiteMock(started, referrer, OriginalRequester::ImportCall)) [[unlikely]] {
            VM& vm = *m_instance.vm;
            JSPromise* ofModuleItself = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), continueImportWithModuleItself, ImplementationVisibility::Private);
            promise->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), ofModuleItself, jsNumber(started.index()));
            promise = ofModuleItself;
        }
        if (referrer && !m_mocksWithIntactOriginal.isEmpty()) [[unlikely]]
            recordImportForOriginalBodies(started, asyncEvaluationOrder(referrer), promise);
        scope.release();
        tellHostOfImport(promise, data(file).mayBeMocked() ? keyWithoutForm(file).string() : String(), file);
        return promise;
    };

    if (JSPromise* failed = m_didFinishTests ? nullptr : findMissingMockFailure(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, failed));
    if (m_modulesWithMockLoadError.get(module.index())) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(module, ImportCaller::CompiledCode));
    if (m_modulesWithRunningFactory.get(module.index()) || isFromOwnFactory) [[unlikely]]
        m_importsMadeByOwnFactory.append(module);
    else if (!m_importsMadeByOwnFactory.contains(module)) [[likely]] {
        clearCachedMockError(module);
        m_originalsDueUnderMock.clear(module.index());
    }
    bool isNestedInBlockedImport = wasDelayed && referrer && lastFactoryBlockedImport(module) && importsStatically(module, fileOf(referrer));
    SetForScope importDeferringMocks(m_importDeferringMocks, !isNestedInBlockedImport && shouldRunMocksAfterSource(module) ? std::optional(module) : std::nullopt);
    if (!m_importDeferringMocks && !shouldLeaveImportPending(module) && !shouldRunMocksAtFetch(module) && !hasPermanentFailureInPackage(module) && !hasPermanentFailureInProject(module)) {
        SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, m_modulesWithRunningFactory.get(module.index()) ? std::nullopt : std::optional(module));
        SetForScope currentImportTarget(m_currentImportTarget, module);
        SetForScope currentImportWaitState(m_currentImportWaitState, JSValue());
        walkStopped = runMocksInGraph(module);
    }
    if (Exception* exception = scope.exception(); exception && !m_instance.vm->isTerminationException(exception)) [[unlikely]]
        noteImportFailedUnderMock(module, exception->value());
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    bool delegatesToHost = delegatesWithoutPlugins(module);
    bool importsBlockingMock = !wasDelayed && referrer && m_rejectedRequires.containsIf([&](const RejectedRequire& pair) {
        return pair.mocked == module && pair.call == pendingFactoryCallID(module);
    }) && host().isInPendingFactoryChain && host().isInPendingFactoryChain(realm, fileOf(referrer), module);
    if (!delegatesToHost && ((isFromOwnFactory && hasFactoryBlockedFetch(module)) || importsBlockingMock)) [[unlikely]]
        RELEASE_AND_RETURN(scope, importModuleItselfWithoutLoader(module));
    if (!delegatesToHost && !wasDelayed && referrer && !m_hasFactoryChainImportSource && !m_rejectedRequires.isEmpty() && isFactoryChainImport(module, referrer)) [[unlikely]]
        RELEASE_AND_RETURN(scope, deferFactoryChainImport(module, referrer));
    if (delegatesToHost || isReplacedByHost(module, LoadKind::Import)) [[unlikely]] {
        Identifier key;
        {
            ResolveOfImportCall resolve(*m_instance.vm, fetchParameters->get());
            key = loader()->resolve(realm, delegatesToHost ? jsString(*m_instance.vm, keyWithoutForm(fileOf(module)).string()) : pathString(module), pathString(referrer), nullptr, true);
        }
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        JSPromise* promise = importUncompiled(key, referrer, WTF::move(*fetchParameters));
        RETURN_IF_EXCEPTION(scope, promise);
        if (holdsLinksOfLoadThatMayFail(module)) [[unlikely]] {
            if (promise->status() == JSPromise::Status::Rejected)
                bringBackLinksOfLoadThatFailed(module);
            else
                hearWhetherImportThatLinkedAnewFails(module, promise);
        }
        return promise;
    }
    if (isRequiredModulePatchPending(module)) [[unlikely]] {
        Identifier ownKey = keyWithoutForm(fileOf(module));
        Identifier key;
        {
            ResolveOfImportCall resolve(*m_instance.vm, fetchParameters->get());
            key = loader()->resolve(realm, jsString(*m_instance.vm, ownKey.string()), pathString(referrer), nullptr, true);
        }
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        if (key != ownKey)
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
                module = m_table.facadeOf(*form).value_or(*form);
            else if (ASCIILiteral type = typeThatReadsTextAsData(*attributes); !type.isNull() && (data(file).kind == ModuleKind::SourceText || data(file).kind == ModuleKind::CommonJs) && !data(file).bodyThrowsAtLoad && !m_table.fullPath(file).contains('?')) [[unlikely]] {
                throwException(realm, scope, createFormNotHeldError(realm, m_table.fullPath(file), type));
                return importPromiseRejectedByLoader(realm, scope);
            } else
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

JSPromise* Modules::importModuleItselfWithoutLoader(ModuleID module)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSPromise* sourceArrival = JSPromise::create(vm, realm->promiseStructure());
    JSPromise* ofModuleItself = JSPromise::create(vm, realm->promiseStructure());
    JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), continueImportWithModuleItself, ImplementationVisibility::Private);
    sourceArrival->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), ofModuleItself, jsNumber(module.index()));
    enqueueFetch(sourceArrival, module, ensureRecord(module), std::nullopt, m_sourceArrivalCount);
    return ofModuleItself;
}

JSPromise* Modules::deferFactoryChainImport(ModuleID module, ModuleID referrer)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer call;
    call.append(jsNumber(module.index()));
    call.append(jsNumber(referrer.index()));
    JSArray* deferredCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    JSPromise* sourceArrival = JSPromise::create(vm, realm->promiseStructure());
    JSPromise* answer = JSPromise::create(vm, realm->promiseStructure());
    JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), didFetchFactoryChainImport, ImplementationVisibility::Private);
    sourceArrival->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), answer, deferredCall);
    enqueueFetch(sourceArrival, module, ensureRecord(module), std::nullopt, m_sourceArrivalCount);
    return answer;
}

bool Modules::hasFactoryBlockedFetch(ModuleID module)
{
    if (!m_mocksWhoseFactoriesRanInsideTheirFetch.get(module.index()))
        return false;
    ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(module));
    return entry && !entry->record() && (entry->status() == ModuleRegistryEntry::Status::New || entry->status() == ModuleRegistryEntry::Status::Fetching);
}

bool Modules::shouldRunMocksAtFetch(ModuleID module)
{
    if (m_modulesWithPendingMock.isEmpty() || !m_modulesWithRunningFactory.isEmpty() || m_innermostEagerLoad || m_instance.vm->synchronousModuleQueue()) [[likely]]
        return false;
    if (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(fileOf(module).index()) || isMocked(module) || isMocked(fileOf(module)) || data(module).isUncompiledCode() || !delegatesWithoutPlugins(module))
        return false;
    if (m_mocks) {
        for (size_t mocked : m_mocks->modules) {
            if (mockState(ModuleID(mocked)) == MockState::Runs)
                return false;
        }
    }
    BitVector reached = staticImportClosure(module, MockTraversal::Stop);
    for (size_t mocked : m_modulesWithPendingMock) {
        if (reached.get(mocked) && !isFileOfProgram(fileOf(ModuleID(mocked))))
            return false;
    }
    return true;
}

bool Modules::shouldRunMocksAfterSource(ModuleID module)
{
    if ((m_modulesWithPendingMock.isEmpty() && m_modulesWithDeferredMock.isEmpty()) || !m_modulesWithRunningFactory.isEmpty() || m_innermostEagerLoad || m_instance.vm->synchronousModuleQueue() || !hasUnreadSource(module)) [[likely]]
        return false;
    if ((needsOriginalBeforeFactory(module) || needsOriginalBeforeFactory(fileOf(module))) && !m_rootsOfImportsThatFailedUnderMocks.get(module.index()))
        return true;
    if (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(fileOf(module).index()) || isMocked(module) || isMocked(fileOf(module)) || data(module).isUncompiledCode() || delegatesWithoutPlugins(module))
        return false;
    BitVector reached = staticImportClosure(module, MockTraversal::Stop);
    bool comesToMock = false;
    for (size_t mocked : m_modulesWithDeferredMock)
        comesToMock |= reached.get(mocked) && mockState(ModuleID(mocked)) == MockState::Runs;
    for (size_t mocked : m_modulesWithPendingMock) {
        if (!reached.get(mocked))
            continue;
        if (!isFileOfProgram(fileOf(ModuleID(mocked))))
            return false;
        comesToMock = true;
    }
    return comesToMock;
}

bool Modules::isReplacedByHost(ModuleID module, LoadKind way)
{
    const ModuleData& replaced = data(module);
    bool isNative = replaced.kind == ModuleKind::Runtime;
    bool isCodeOfLibrary = replaced.kind == ModuleKind::RuntimeSource || (replaced.kind == ModuleKind::SourceText && replaced.isOfLibrary());
    if (!isNative && (!isCodeOfLibrary || !replaced.mayBeMocked()))
        return false;
    String name = m_table.fullPath(module);
    if (!host().replacesNativeModule || !host().replacesNativeModule(m_instance.globalObject, name))
        return false;
    if (way == LoadKind::Require && (name == "bun"_s || isInRequireCache(module)))
        return false;
    return m_libraryModulesThatMockReplaces.get(module.index());
}

bool Modules::isKeptInRequireCache(ModuleID module) const
{
    if (!data(module).isOfLibrary())
        return false;
    if (!m_libraryModulesWithCheckedName.get(module.index())) {
        String name = m_table.fullPath(module);
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
        loaded.moduleObjectFromRequire.set(*this, nullptr);
        forgetFirstRequirerOf(module);
        loaded.firstUncompiledRequirer.set(*this, nullptr);
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
        if (hasStartedLoading(module) || isRunningFilesOf(module)) {
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
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesScannedForRequireCache, m_table.size() + toFollow.size());
    return listed;
}

bool Modules::isListedOnceStatementReaches(ModuleID module) const
{
    return isKeptInRequireCache(module) && !isInRequireCache(module) && !m_libraryModulesTakenFromLoader.get(module.index()) && !m_table.fullPath(module).startsWith("bun:"_s);
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
        throwException(m_instance.globalObject, scope, createEntryOfRequireCacheRefusedError(m_instance.globalObject, m_table.fullPath(module)));
        return nullptr;
    }
    if (!data(module).isOfLibrary())
        RELEASE_AND_RETURN(scope, moduleObjectMadeByRequire(module));
    if (valueOfLibraryFileStaysWithCompiledCode(module)) [[unlikely]] {
        RuntimeCheckFailure failure("escape check failed"_s);
        failure.atTopCall(vm);
        failure.detail("require.cache exposes the exports of the built-in module ", m_table.fullPath(module), ", but the compiler assumed that only compiled code uses them");
        failure.report();
        CRASH();
    }
    if (JSObject* entry = ensureLoaded(module).moduleObjectFromRequire)
        return entry;
    if (!isInRequireCache(module)) {
        JSObject* namespaceObject = moduleNamespace(module, ModulePhase::Evaluation);
        RETURN_IF_EXCEPTION(scope, nullptr);
        addToRequireCache(module, namespaceObject);
    }
    OptionSet<ModuleObjectTrait> traits { ModuleObjectTrait::IsNotCommonJSModule };
    if (data(module).kind == ModuleKind::SourceText)
        traits.add(ModuleObjectTrait::IsESModule);
    ModuleObjects objects;
    host().createModuleObject(m_instance.globalObject, pathString(module), module, traits, objects);
    RETURN_IF_EXCEPTION(scope, nullptr);
    notePrototypeOfModuleObjects(objects.module);
    LoadedModule& loaded = ensureLoaded(module);
    objects.module->putDirect(vm, vm.propertyNames->exports, loaded.valueInRequireCache.get());
    loaded.moduleObjectFromRequire.set(*this, objects.module);
    setRequireCacheValue(module, JSValue());
    return objects.module;
}

std::optional<ModuleID> Modules::findByName(const Identifier& key)
{
    std::optional<ModuleID> found = m_table.findByPath(key.string());
    if (found && isReplacedByHost(*found, LoadKind::Import))
        return std::nullopt;
    if (found && !data(*found).isOfLibrary() && host().hasMatchingLoadPlugin && host().hasMatchingLoadPlugin(m_instance.globalObject, key.string()) && !hasRun(*found) && !namespaceIfCreated(*found, ModulePhase::Evaluation))
        return std::nullopt;
    return found;
}

Identifier Modules::keyWithoutForm(ModuleID file)
{
    if (data(file).kind != ModuleKind::Data)
        return moduleKey(file);
    String path = m_table.fullPath(file);
    size_t mark = path.reverseFind('?');
    return mark == notFound ? moduleKey(file) : Identifier::fromString(*m_instance.vm, path.left(mark));
}

std::optional<ModuleID> Modules::findFormThatStatementLoaded(const Identifier& key, const ScriptFetchParameters& fetchParameters, std::optional<ModuleID> asked) const
{
    if (fetchParameters.type() != ScriptFetchParameters::Type::HostDefined)
        return std::nullopt;
    Vector<ModuleID, 2> forms = m_table.importAttributeVariants(key.string());
    forms.removeAllMatching([&](ModuleID form) {
        return data(form).kind != ModuleKind::Data || m_table.fullPath(form).endsWith("?json"_s);
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
            else if (data(target).kind == ModuleKind::Data && !m_table.fullPath(target).endsWith("?json"_s))
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
    String path = m_table.fullPath(form);
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
        return entry.value.name == key || entry.value.module == file;
    });
}

std::optional<ModuleID> Modules::findWithAttributes(const Identifier& key, const ScriptFetchParameters* fetchParameters, FormWithoutAttributes formWithoutAttributes, std::optional<ModuleID> foundUnderWrittenName)
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
    if (auto first = m_formsAnsweredFirst.find(keyOfLoader); first != m_formsAnsweredFirst.end()) {
        m_mockEvictedDataModules.clear(first->value.module.index());
        return first->value.module;
    }
    std::optional<ModuleID> module = foundUnderWrittenName ? foundUnderWrittenName : host().findModuleWithAttributes(m_instance.globalObject, key.string(), *fetchParameters);
    if (module && formWithoutAttributes == FormWithoutAttributes::IsAnotherModule && m_table.fullPath(*module) == key.string())
        module = std::nullopt;
    if (std::optional<ModuleID> loaded = findFormThatStatementLoaded(key, *fetchParameters, module))
        module = loaded;
    if (module && !data(*module).bodyThrowsAtLoad) {
        m_formsAnsweredFirst.add(keyOfLoader, FormAnsweredFirst { key, *module });
        m_mockEvictedDataModules.clear(module->index());
    }
    return module;
}

String pathBesideFile(StringView file, StringView written)
{
    bool hasTwoSeparators = !file.startsWith('/') || file.startsWith("//"_s);
    auto isSeparator = [&](char16_t unit) {
        return unit == '/' || (unit == '\\' && hasTwoSeparators);
    };
    auto endOfSegment = [&](StringView text, unsigned place) {
        while (place < text.length() && !isSeparator(text[place]))
            ++place;
        return place;
    };
    unsigned lengthOfRoot = 0;
    if (file.length() > 1 && isASCIIAlpha(file[0]) && file[1] == ':')
        lengthOfRoot = 2;
    else if (file.length() > 1 && isSeparator(file[0]) && isSeparator(file[1]))
        lengthOfRoot = std::min(file.length(), endOfSegment(file, endOfSegment(file, 2) + 1));
    unsigned lengthOfDirectory = file.length();
    while (lengthOfDirectory > lengthOfRoot && !isSeparator(file[lengthOfDirectory - 1]))
        --lengthOfDirectory;
    char16_t separator = lengthOfDirectory > lengthOfRoot ? file[--lengthOfDirectory] : '/';
    StringBuilder path;
    path.append(file.left(lengthOfDirectory));
    for (unsigned place = 0; place < written.length(); ++place) {
        unsigned end = endOfSegment(written, place);
        StringView segment = written.substring(place, end - place);
        place = end;
        if (segment == ".."_s) {
            unsigned length = path.length();
            while (length > lengthOfRoot && !isSeparator(path[length - 1]))
                --length;
            path.shrink(length > lengthOfRoot ? length - 1 : lengthOfRoot);
        } else if (!segment.isEmpty() && segment != "."_s)
            path.append(separator, segment);
    }
    return path.toString();
}

std::optional<ModuleID> Modules::findFormUnderWrittenName(JSString* name, ModuleID referrer, const ScriptFetchParameters& fetchParameters)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!host().findModuleWithAttributes)
        return std::nullopt;
    String written = name->value(m_instance.globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!written.startsWith("./"_s) && !written.startsWith("../"_s))
        return std::nullopt;
    std::optional<ModuleID> found = host().findModuleWithAttributes(m_instance.globalObject, written, fetchParameters);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!found)
        return std::nullopt;
    Identifier keyBesideAsker = Identifier::fromString(*m_instance.vm, pathBesideFile(m_table.fullPath(referrer), written));
    std::optional<ModuleID> first = findWithAttributes(keyBesideAsker, &fetchParameters, FormWithoutAttributes::MayAnswer, found);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    if (!first || first == found)
        return first;
    std::optional<ModuleID> ofSameTypeBesideAsker = host().findModuleWithAttributes(m_instance.globalObject, keyBesideAsker.string(), fetchParameters);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return first == ofSameTypeBesideAsker ? found : first;
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
    return m_innermostEagerLoad || m_rootAwaitingLoader || m_loadsInFlight.hasModuleEvaluatedDuringLoad();
}

bool Modules::hasUnreadSource(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (!isFileOfProgram(file) || m_instance.vm->synchronousModuleQueue() || isFetchedInsideCall(file) || activeMock(module) || activeMock(file))
        return false;
    if (loader()->registryEntry(moduleKey(module)) || loader()->registryEntry(moduleKey(file)))
        return false;
    if (!isCommonJSModuleOrFacade(module))
        return isNotLoaded(module);
    if (hasNotBegunAsFile(module) && hasNotBegunAsFile(file))
        return true;
    return !isTestRun() && !isEvaluatedByLoader(file) && !wasLoadedByImport(file);
}

void Modules::noteThatTextHasCome(ModuleID module)
{
    m_turnsInWhichTextsCame.set(fileOf(module), ++m_sourceArrivalCount);
}

void Modules::enqueueFetch(JSPromise* promise, ModuleID module, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, uint32_t turnOfRequest)
{
    VM& vm = *m_instance.vm;
    putCallsThatWaitApartInLine();
    uint64_t place = static_cast<uint64_t>(turnOfRequest) << 32 | m_numberOfTextsAskedFor++;
    bool isScheduledByHost = host().scheduleFetchFulfillment && isTestRun();
    uint64_t precedingDiskReads = isScheduledByHost ? host().scheduleFetchFulfillment(m_instance.globalObject) : 0;
    m_queuedFetches.appendAndBubble(QueuedFetch { place, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder, m_loaderStepCount, vm.aotState().momentOfLoads, m_importDeferringMocks == module ? Strong<Unknown>(vm, AsyncContextSwapScope::current(vm, m_instance.globalObject)) : Strong<Unknown>(), m_currentImportCallerModule, m_currentImportSequenceNumber, precedingDiskReads }, [&](const QueuedFetch& inLine) {
        return inLine.place > place;
    });
    if (isScheduledByHost)
        return;
    auto ticket = vm.deferredWorkTimer->addPendingWork(DeferredWorkTimer::WorkType::ImminentlyScheduled, vm, promise, { });
    vm.deferredWorkTimer->scheduleWorkSoonIfActive(ticket, [](DeferredWorkTimer::Ticket& ticket) {
        if (Modules* modules = modulesOf(ticket.target()->realm()))
            modules->fulfillFirstQueuedFetch();
    });
}

void Modules::fulfillFirstQueuedFetch()
{
    if (m_queuedFetches.isEmpty())
        return;
    QueuedFetch text = m_queuedFetches.takeFirst();
    noteThatTextHasCome(text.module);
    if (text.requested) {
        text.promise->fulfill(*m_instance.vm, text.requested.get());
        return;
    }
    Vector<QueuedFetch, 2> callsMadeWhileLoaderTookNoStep;
    while (!m_queuedFetches.isEmpty() && !m_queuedFetches.first().requested && m_queuedFetches.first().loaderStepsAtCall == text.loaderStepsAtCall)
        callsMadeWhileLoaderTookNoStep.append(m_queuedFetches.takeFirst());
    SetForScope loaderStepsAtCall(m_resumingCallLoaderSteps, text.loaderStepsAtCall);
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope callSequenceNumber(m_resumingCallSequenceNumber, text.callSequenceNumber);
    auto start = [&](QueuedFetch& call) {
        std::optional<AsyncContextSwapScope> asyncContext;
        if (JSValue ofCall = call.asyncContextOfCallWhoseMocksRunNow.get())
            asyncContext.emplace(*m_instance.vm, m_instance.globalObject, ofCall);
        SetForScope currentImportCallerModule(m_currentImportCallerModule, call.importCallerModule);
        SetForScope currentImportRequestedModule(m_currentImportRequestedModule, call.module);
        SetForScope currentImportSequenceNumber(m_currentImportSequenceNumber, call.importSequenceNumber);
        startImportWhoseTextHasCome(call.promise.get(), call.module, call.referrerAsyncOrder);
    };
    start(text);
    RETURN_IF_EXCEPTION(scope, void());
    for (QueuedFetch& call : callsMadeWhileLoaderTookNoStep) {
        noteThatTextHasCome(call.module);
        m_resumingCallSequenceNumber = call.callSequenceNumber;
        start(call);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool Modules::tryDeferFetchFulfillment(JSPromise* fetched, ModuleRecord* requested, ModuleID requester)
{
    ModuleID file = fileOf(requested->module());
    bool canProgramTellWhenTextsCome = !m_queuedFetches.isEmpty() || m_canJobsOfFileTellWhenTextsCome || data(requester).isLoadedByLoader();
    if (!canProgramTellWhenTextsCome || !isFileOfProgram(file) || m_instance.vm->synchronousModuleQueue() || isFetchedInsideCall(file)) {
        noteThatTextHasCome(file);
        return false;
    }
    enqueueFetch(fetched, requested->module(), requested, std::nullopt, turnInWhichTextCame(requester));
    return true;
}

JSPromise* Modules::startImportInLaterTurn(ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    JSPromise* importPromise = JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure());
    m_modulesOfQueuedImports.set(module.index());
    std::optional<ModuleID> evaluatingCaller = m_innermostEagerLoad ? evaluatingModuleOrFacade(*m_innermostEagerLoad) : std::nullopt;
    enqueueFetch(importPromise, module, nullptr, referrerAsyncOrder, evaluatingCaller ? turnInWhichTextCame(*evaluatingCaller) : m_sourceArrivalCount);
    return importPromise;
}

void Modules::startImportWhoseTextHasCome(JSPromise* importPromise, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    m_modulesOfQueuedImports.clear(module.index());
    bool wasHeldBeforeWalk = holdsLinksOfLoadThatMayFail(module);
    JSPromise* loaded = startImport(module, referrerAsyncOrder, ImportCaller::Host, ImportSourceState::Arrived);
    bool walkFiledLinks = !wasHeldBeforeWalk && holdsLinksOfLoadThatMayFail(module);
    if (scope.exception()) [[unlikely]] {
        importPromise->rejectWithCaughtException(vm, scope);
        if (walkFiledLinks)
            bringBackLinksOfLoadThatFailed(module);
        return;
    }
    if (walkFiledLinks) [[unlikely]]
        hearWhetherImportThatLinkedAnewFails(module, loaded);
    scope.release();
    settleAsLoaderDoes(m_instance.globalObject, importPromise, loaded);
}

void Modules::hearWhetherImportThatLinkedAnewFails(ModuleID root, JSPromise* promise)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSFunction* didFulfill = JSFunction::create(vm, realm, 2, String(), JSC::AOT::didFulfillImportThatLinkedAnew, ImplementationVisibility::Private);
    JSFunction* didReject = JSFunction::create(vm, realm, 2, String(), JSC::AOT::didRejectImportThatLinkedAnew, ImplementationVisibility::Private);
    bool wasHandled = promise->isHandled();
    promise->performPromiseThenWithContext(vm, realm, didFulfill, didReject, jsUndefined(), jsNumber(root.index()));
    if (!wasHandled)
        promise->setFlags(promise->flags() & ~JSPromise::isHandledFlag);
}

void Modules::hearWhetherImportUnderMockFails(ModuleID root, JSPromise* promise)
{
    if (promise->status() == JSPromise::Status::Fulfilled)
        return forgetImportFailedUnderMock(root);
    if (promise->status() == JSPromise::Status::Rejected)
        return noteImportFailedUnderMock(root, promise->result());
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSFunction* didFulfill = JSFunction::create(vm, realm, 2, String(), JSC::AOT::didFulfillImportUnderMock, ImplementationVisibility::Private);
    JSFunction* didReject = JSFunction::create(vm, realm, 2, String(), JSC::AOT::didRejectImportUnderMock, ImplementationVisibility::Private);
    bool wasHandled = promise->isHandled();
    promise->performPromiseThenWithContext(vm, realm, didFulfill, didReject, jsUndefined(), jsNumber(root.index()));
    if (!wasHandled)
        promise->setFlags(promise->flags() & ~JSPromise::isHandledFlag);
}

void Modules::holdMockThatStandsUntilImportSettles(ModuleID root, JSPromise* promise)
{
    bool asksForModuleItself = false;
    for (size_t module : m_modulesWithLingeringMock)
        asksForModuleItself |= fileOf(ModuleID(module)) == fileOf(root);
    if (!asksForModuleItself)
        return;
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    m_factoryBlockedImportRoots.append(root);
    m_rootsOfImportsThatHaveGoneOn.append(root);
    JSFunction* didSettle = JSFunction::create(vm, realm, 2, String(), JSC::AOT::didSettleImportBehindFactories, ImplementationVisibility::Private);
    bool wasHandled = promise->isHandled();
    promise->performPromiseThenWithContext(vm, realm, didSettle, didSettle, jsUndefined(), jsNumber(root.index()));
    if (!wasHandled)
        promise->setFlags(promise->flags() & ~JSPromise::isHandledFlag);
}

JSPromise* Modules::ensureActualKeyLoad(ModuleID file)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (const auto& copy : m_pendingActualKeyLoads) {
        if (copy.first == file && copy.second->status() == JSPromise::Status::Pending)
            return copy.second.get();
    }
    JSPromise* actualKeyLoad = loader()->requestImportModule(realm, actualKey(file), moduleKey(file), nullptr, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    m_pendingActualKeyLoads.append({ file, Strong<JSPromise>(vm, actualKeyLoad) });
    return actualKeyLoad;
}

JSPromise* Modules::startImport(ModuleID module, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller, ImportSourceState text, JSPromise* promiseOfCaller, JSValue importWaitState, std::optional<bool> priorRunMocksResult)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto promiseForCaller = [&] {
        return promiseOfCaller ? promiseOfCaller : JSPromise::create(*m_instance.vm, realm->promiseStructure());
    };
    bool hadWaited = !!importWaitState;
    bool wasHeldForBody = std::exchange(m_wasHeldForBody, false);
    bool isNewCallAfterTests = isImportCallAfterTests() && text == ImportSourceState::MayArriveLater && !hadWaited;
    if (isNewCallAfterTests && shouldLeaveImportPending(module)) [[unlikely]]
        return promiseForCaller();
    if (m_loadsModulesIndividually) [[unlikely]]
        m_loadRoots.set(module.index());
    if (m_loadsModulesIndividually) [[unlikely]]
        recordVariantRequest(module);
    if (m_mocks) [[unlikely]]
        recordEntryRequest(module, m_currentImportCallerModule, LoadKind::Import);
    if (text == ImportSourceState::MayArriveLater && (isDeferredByCurrentSweep(module) || m_originalBeingEvaluated == module)) [[unlikely]]
        return startImportInLaterTurn(module, referrerAsyncOrder);
    runDataFileAgainForImport(module, caller == ImportCaller::Host ? ImportResolver::Loader : ImportResolver::Build);
    if (scope.exception()) [[unlikely]] {
        if (text == ImportSourceState::Arrived)
            m_importsAwaitingSource.removeFirstMatching([&](const ImportAwaitingSource& each) { return each.module == module; });
        return rejectedPromiseWithPendingException(realm, scope);
    }
    if (isCommonJSModuleOrFacade(module)) {
        if (ModuleID file = fileOf(module); data(file).runsAgainAfterThrow() && !isAwaitedByImport(file)) [[unlikely]]
            ensureLoaded(file).isAwaitedByCallThatComesInLaterJob = status(file) == ModuleStatus::Evaluating || !m_instance.vm->synchronousModuleQueue();
    }

    std::optional<SetForScope<ModuleID>> currentImportTarget;
    std::optional<SetForScope<JSValue>> currentImportWaitState;
    currentImportTarget.emplace(m_currentImportTarget, module);
    currentImportWaitState.emplace(m_currentImportWaitState, importWaitState);
    EnsureStillAliveScope keepsStateOfWait(importWaitState);
    if (text == ImportSourceState::Arrived && !m_failuresOfFactories.isEmpty()) [[unlikely]] {
        if (JSValue error = takeErrorOfFactoryThatFailedWhileCallWasHeldForItsText(module)) {
            throwException(realm, scope, error);
            return rejectedPromiseWithPendingException(realm, scope);
        }
    }
    bool needsOriginal = false;
    if (text == ImportSourceState::Arrived) {
        if (JSPromise* failed = takeMissingMockFailure(module)) [[unlikely]]
            return failed;
        if (takeImportWithRemovedMock(module) && !isInAnotherLoadGraph(fileOf(module))) [[unlikely]] {
            JSPromise* actualKeyLoad = ensureActualKeyLoad(fileOf(module));
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            return actualKeyLoad;
        }
        needsOriginal = needsOriginalDespiteMock(module);
    }
    if (text != ImportSourceState::MayArriveLater && !m_modulesWithPendingMock.isEmpty()) [[unlikely]]
        forgetMockLaidBehindCallWhoseTextHasCome(fileOf(module));
    bool runsDeferredMocks = text == ImportSourceState::ArrivedWithDeferredMocks || (text == ImportSourceState::Arrived && m_importsAwaitingSource.removeFirstMatching([&](const ImportAwaitingSource& each) { return each.module == module && !each.missingMockFailure; }));
    if (runsDeferredMocks) [[unlikely]] {
        removeUnusedLingeringMocks();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    bool walkStopped = false;
    {
        SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, runsDeferredMocks ? std::optional(module) : m_moduleThatProgramImportsNow);
        walkStopped = priorRunMocksResult ? *priorRunMocksResult : runMocksInGraph(module);
    }
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (m_innermostEagerLoad) [[unlikely]] {
        transferGraphsToLoader();
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (m_innermostEagerLoad->runsBeforeLoaderSteps) {
            transferSynchronousLoad(*m_innermostEagerLoad);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        }
    }
    PendingFactoryRequester askedBy = text == ImportSourceState::MayArriveLater || runsDeferredMocks || importWaitState ? PendingFactoryRequester::ImportCall : PendingFactoryRequester::ImportCallAfterSource;
    JSPromise* factories = text == ImportSourceState::MayArriveLater && m_importDeferringMocks == module ? nullptr : pendingFactoryInGraph(module, askedBy, importWaitState);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    while (!factories && walkStopped) [[unlikely]] {
        walkStopped = runMocksInGraph(module);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        factories = pendingFactoryInGraph(module, askedBy, importWaitState);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    }
    if (!factories && needsOriginal) [[unlikely]] {
        factories = ensureActualKeyLoad(fileOf(module));
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        m_missingMockFailures.append({ module, Strong<JSPromise>(*m_instance.vm, factories) });
    }
    const FactoryBlockedImport* earlier = text == ImportSourceState::Arrived && !hadWaited ? lastFactoryBlockedImport(module) : nullptr;
    bool followsEarlierCall = earlier && earlier->followingCallCount;
    size_t followingCallCount = followsEarlierCall ? earlier->followingCallCount - 1 : std::ranges::count_if(m_importsAwaitingSource, [&](const ImportAwaitingSource& each) { return each.module == module; });
    if (followsEarlierCall)
        factories = earlier->promise.get();
    if (factories) [[unlikely]] {
        VM& vm = *m_instance.vm;
        JSPromise* promise = promiseForCaller();
        JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importBehindFactories, ImplementationVisibility::Private);
        MarkedArgumentBuffer call;
        call.append(jsNumber(module.index()));
        call.append(referrerAsyncOrder ? jsNumber(static_cast<double>(*referrerAsyncOrder)) : jsUndefined());
        call.append(jsBoolean(caller == ImportCaller::Host));
        call.append(jsNumber(static_cast<uint32_t>(runsDeferredMocks && !importWaitState ? ImportSourceState::ArrivedWithDeferredMocks : text)));
        call.append(importWaitState);
        call.append(jsNumber(vm.aotState().momentOfLoads));
        auto* isAwaitingOriginalBody = host().isAwaitingOriginalBody;
        bool isHeldForBody = hadWaited ? wasHeldForBody : isAwaitingOriginalBody && isAwaitingOriginalBody(realm, module);
        call.append(jsBoolean(isHeldForBody));
        JSArray* deferredCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        JSValue reject = isHeldForBody ? JSValue(JSFunction::create(vm, realm, 2, String(), rejectImportAfterBody, ImplementationVisibility::Private)) : jsUndefined();
        factories->performPromiseThenWithContext(vm, realm, goOn, reject, promise, deferredCall);
        if (!hadWaited) {
            m_factoryBlockedImportRoots.append(module);
            m_factoryBlockedImports.append({ module, Strong<JSPromise>(vm, promise), followingCallCount });
            JSFunction* didSettle = JSFunction::create(vm, realm, 2, String(), JSC::AOT::didSettleImportBehindFactories, ImplementationVisibility::Private);
            bool wasHandled = promise->isHandled();
            promise->performPromiseThenWithContext(vm, realm, didSettle, didSettle, jsUndefined(), jsNumber(module.index()));
            if (!wasHandled)
                promise->setFlags(promise->flags() & ~JSPromise::isHandledFlag);
        }
        return promise;
    }
    currentImportWaitState.reset();
    currentImportTarget.reset();
    if (text == ImportSourceState::MayArriveLater && (!m_queuedFetches.isEmpty() || hasFileOfGraphBeingLoadedBegun() || data(module).isLoadedByLoader() || m_importDeferringMocks == module) && hasUnreadSource(module)) [[unlikely]] {
        if (isNewCallAfterTests)
            return promiseForCaller();
        if (m_importDeferringMocks == module)
            addImportAwaitingSource(module);
        return startImportInLaterTurn(module, referrerAsyncOrder);
    }
    if (text == ImportSourceState::MayArriveLater && !hasTextAtHand() && textOfImportComesInLaterTurn(module, AskedBy::Import)) [[unlikely]] {
        if (isNewCallAfterTests)
            return promiseForCaller();
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
        bool isNoPartOfLoad = isCalledByProgram && !m_loadsModulesIndividually && !(isCommonJSModuleOrFacade(module) && (hasBegun || data(fileOf(module)).isImportableDuringEvaluation()));
        SetForScope noSynchronousLoad(vm.m_suspendedSynchronousModuleQueue, isNoPartOfLoad ? vm.m_synchronousModuleQueue : vm.m_suspendedSynchronousModuleQueue);
        if (isCalledByProgram && (!isBuiltinModule || isBuiltinModuleThatLoaderHas) && loaderOnlyHandsOverNamespace(module)) {
            JSPromise* promise = promiseForCaller();
            if (promise->isThenFastAndNonObservable()) [[likely]] {
                if (activeMock(module)) [[unlikely]]
                    recordMockedImport(promise, module);
                if (wasHeldForBody) [[unlikely]] {
                    didRunFirstImportStep(promise);
                    finishImportOfEvaluatedModule(promise, module, { });
                    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
                    return promise;
                }
                queueStepOfImport(promise, moduleForJob(module), std::size(jobsOfImportOfEvaluatedModule));
                return promise;
            }
        }
        bool isCalledUnderRequire = vm.synchronousModuleQueue() && isCommonJSModuleOrFacade(module) && isEvaluatedByLoader(fileOf(module));
        if (isCalledUnderRequire || (shouldDeferAsyncLoad(module) && !loader()->registryEntry(moduleKey(module)))) [[unlikely]] {
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
        LoadInFlight* earlierLoad = isCalledByProgram && isFile && !m_innermostEagerLoad && m_queuedFetches.isEmpty() ? loadToSettleFirst(module) : nullptr;
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
        SetForScope rootLoadedByCompiledCode(m_rootLoadedByCompiledCode, std::optional(module));
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
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
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
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
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
        if (load->loaderStepsAtCall == m_loaderStepCount)
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
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
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
    PlacesSeenByPlans& placesSeen = m_placesSeenByPlans;
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

Modules::LoadInFlight* Modules::loadToEvaluateFirst(const LoadInFlight& load)
{
    goThroughLoadsOfEarlierJobs();
    if (m_loadsThatHoldModules.isEmpty() || status(load.root) >= ModuleStatus::Evaluating)
        return nullptr;
    auto earlierLoadThatHolds = [&](ModuleID module) -> LoadInFlight* {
        LoadInFlight* earlier = loadBeingLoadedThatHolds(module);
        return earlier && earlier->number < load.number && earlier->loaderStepsAtCall != load.loaderStepsAtCall ? earlier : nullptr;
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
    const EagerLoad& files = *m_innermostEagerLoad;
    if (files.moduleOrFacade == files.root || !data(files.root).isSynchronous())
        return std::nullopt;
    bool holdsCycle = modulesThatCloseCyclesBelow(required).containsIf([&](ModuleID module) {
        return leadsTo(files.root, module);
    });
    return holdsCycle ? std::optional(files.root) : std::nullopt;
}

bool ModuleRecord::tryDeferForEarlierImports(JSCell* payload, bool isDeferred)
{
    return modules().tryDeferForEarlierImports(*this, payload, isDeferred);
}

bool Modules::tryDeferForEarlierImports(ModuleRecord& root, JSCell* payload, bool isDeferred)
{
    if (m_loadsInFlight.numberBeingLoaded() < 2) [[likely]]
        return false;
    LoadInFlight* load = nullptr;
    m_loadsInFlight.forEachOfRoot(root.module(), [&](LoadInFlight& ofRoot) {
        if (!load && m_loadsInFlight.isBeingLoaded(ofRoot))
            load = &ofRoot;
    });
    LoadInFlight* earlier = load ? loadToEvaluateFirst(*load) : nullptr;
    if (!earlier)
        return false;
    VM& vm = *m_instance.vm;
    size_t place = m_graphsDeferredForImports.size();
    while (place && m_graphsDeferredForImports[place - 1].numberOfLoad > load->number)
        --place;
    m_graphsDeferredForImports.insert(place, GraphDeferredForImports { Strong<JSCell>(vm, &root), Strong<JSCell>(vm, payload), isDeferred, load->number, earlier->number });
    countEvent(vm, RuntimeCounterID::GraphsDeferredForImports);
    return true;
}

void Modules::resumeGraphsDeferredForImports()
{
    if (m_graphsDeferredForImports.isEmpty()) [[likely]]
        return;
    Vector<GraphDeferredForImports, 2> letGo;
    m_graphsDeferredForImports.removeAllMatching([&](GraphDeferredForImports& held) {
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
    Ref<MicrotaskQueue> deferredJobs = MicrotaskQueue::create(vm);
    queue.moveTasksAfter(0, deferredJobs.get());
    for (GraphDeferredForImports& held : letGo) {
        InternalMicrotask job = held.isDeferred ? InternalMicrotask::DynamicImportDeferLoadSettled : InternalMicrotask::DynamicImportLoadSettled;
        m_instance.globalObject->queueMicrotask(vm, job, static_cast<uint8_t>(JSPromise::Status::Fulfilled), held.root.get(), jsUndefined(), held.payload.get());
    }
    deferredJobs->moveTasksAfter(0, queue);
    countEvent(vm, RuntimeCounterID::DeferredGraphsResumed, letGo.size());
}

Modules::LoadInFlight* Modules::loadToSettleFirst(ModuleID module)
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
        for (ModuleID evaluated : { file, m_table.facadeOf(file).value_or(file) }) {
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
    if (delegatesToHost(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, loader()->requestImportModuleForKey(m_instance.globalObject, keyWithoutForm(fileOf(module)), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), nullptr, resultPromise));
    if (status(module) == ModuleStatus::Failed) [[unlikely]]
        reportRetainedFailure(module);
    HostRequest requestUnderWay = std::exchange(m_currentHostRequest, HostRequest(true));
    ModuleRecord* record = nullptr;
    {
        SetForScope stackPointerOfImport(m_instance.vm->aotState().stackPointerOfImport, currentStackPointer());
        ModuleRecord* made = recordIfExists(module);
        bool isBeingMadeInline = made && loader()->isMakingModuleInline(made->moduleKey());
        record = isCommonJSModuleOrFacade(module) ? recordOfCommonJSFile(module, isBeingMadeInline ? LoadOfFile::IsAskedFor : LoadOfFile::IsRootOfImport) : exposeRecord(module, RecordHolder::Loader);
    }
    JSPromise* loaded = scope.exception() ? nullptr : loader()->requestImportModuleForKey(m_instance.globalObject, record->moduleKey(), Identifier(), nullptr, nullptr, false, referrerAsyncOrder.value_or(-1), record, resultPromise);
    m_currentHostRequest = requestUnderWay;
    RETURN_IF_EXCEPTION(scope, nullptr);
    didStartLoad(loaded, module);
    return loaded;
}

bool Modules::loaderOnlyHandsOverNamespace(ModuleID module) const
{
    if (status(module) != ModuleStatus::Evaluated || isCommonJSModuleOrFacade(module) || m_instance.vm->synchronousModuleQueue() || m_moduleWithDeferredJobs || m_rootAwaitingLoader)
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
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (importPromise == m_promiseOfJobOfRound.get()) [[unlikely]]
        RELEASE_AND_RETURN(scope, runJobOfRound(module));
    if (importPromise == m_promiseOfJobInWhichLoadAsksForOriginal.get()) [[unlikely]]
        RELEASE_AND_RETURN(scope, letLoadInFlightAskForOriginal(module));
    if (importPromise == m_promiseOfJobThatMakesCallsAgain.get()) [[unlikely]]
        RELEASE_AND_RETURN(scope, runJobThatMakesCallsAgain(module));
    JSObject* namespaceKept = m_mockedImports.isEmpty() ? nullptr : takeRetainedImportNamespace(importPromise);
    EnsureStillAliveScope keepsNamespaceKept(namespaceKept);
    if (!error && !namespaceKept && (status(module) == ModuleStatus::EvaluatingAsync || status(module) == ModuleStatus::Failed)) [[unlikely]]
        RELEASE_AND_RETURN(scope, startImportWhoseTextHasCome(importPromise, module, std::nullopt));
    if (!error && !m_importersEnteredAboveMocks.isEmpty()) [[unlikely]] {
        BitVector seen;
        evaluateImportersBehindModuleThatHasRun(module, seen);
        if (scope.exception()) [[unlikely]] {
            importPromise->rejectWithCaughtException(vm, scope);
            return;
        }
    }
    JSPromise* loaded = JSPromise::create(vm, m_instance.globalObject->promiseStructure());
    loaded->markAsHandled();
    scope.release();
    if (error) [[unlikely]]
        loaded->reject(vm, error);
    else if (namespaceKept) [[unlikely]]
        loaded->resolve(m_instance.globalObject, vm, namespaceKept);
    else
        resolveWithNamespace(loaded, module);
    loaded->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::PromiseResolveWithoutHandlerJob, importPromise, jsUndefined());
}

std::unique_ptr<ModuleLinkWalk> Modules::takeLinkWalk(ModuleID root)
{
    std::unique_ptr<ModuleLinkWalk> walk = takeKeptLinkWalk(root);
    if (!walk) {
        countEvent(*m_instance.vm, RuntimeCounterID::ModuleLinkWalksCreated);
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

    if (delegatesToHost(module)) [[unlikely]] {
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
    bool didRunFile = !didRunModule && evaluateImportedCommonJSModules(module);
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

JSPromise* Modules::behindStepsOfImportThatWaitsFor(JSPromise* evaluation, ModuleID module)
{
    if (!isImportedByLoader(module))
        return evaluation;
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSPromise* step = evaluation;
    for (size_t stepsLeft = numberOfJobsBehindEvaluation - 1; stepsLeft; --stepsLeft) {
        JSPromise* next = JSPromise::create(vm, realm->promiseStructure());
        step->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), next);
        step = next;
    }
    return step;
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
    willLoadByName(realm, text, LoadKind::Import);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    auto fetchParameters = JSModuleLoader::fetchParametersFromImportOptions(realm, options);
    RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
    if (!fetchParameters)
        return fetchParameters.error();
    std::optional<ModuleID> found = *fetchParameters ? findFormUnderWrittenName(text, referrer, **fetchParameters) : std::nullopt;
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    Identifier key;
    {
        ResolveOfImportCall resolve(*m_instance.vm, fetchParameters->get());
        if (!found)
            key = loader()->resolve(realm, text, pathString(referrer), nullptr, true);
    }
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    if (!found)
        found = *fetchParameters ? findWithAttributes(key, fetchParameters->get()) : findByName(key);
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    if (!found && !*fetchParameters)
        found = originalThatOwnFactoryAsksFor(key);
    if (found) {
        runDataFileAgainForImport(*found, ImportResolver::Loader);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        checkModuleAccess(realm, *found, LoadKind::Import);
        RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
        RELEASE_AND_RETURN(scope, import(m_table.facadeOf(*found).value_or(*found), referrer));
    }
    RELEASE_AND_RETURN(scope, importUncompiled(key, referrer, WTF::move(*fetchParameters)));
}

std::optional<ModuleID> Modules::originalNamedBy(const Identifier& key) const
{
    if (key.isSymbol())
        return std::nullopt;
    StringView path = key.string();
    if (path.endsWith("&spy"_s))
        path = path.left(path.length() - "&spy"_s.length());
    size_t lengthOfKey = path.length();
    while (path.endsWith("?actual"_s) || path.endsWith("&actual"_s))
        path = path.left(path.length() - "?actual"_s.length());
    return path.length() == lengthOfKey ? std::nullopt : m_table.findByPath(path);
}

std::optional<ModuleID> Modules::originalThatOwnFactoryAsksFor(const Identifier& key)
{
    if (m_modulesWithRunningFactory.isEmpty() && m_modulesWithPendingMock.isEmpty() && !m_mocks) [[likely]]
        return std::nullopt;
    if (key.isSymbol() || !key.string().endsWith("?actual"_s))
        return std::nullopt;
    std::optional<ModuleID> original = originalNamedBy(key);
    return original && isRequestedByOwnFactory(*original) ? original : std::nullopt;
}

JSPromise* Modules::importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&& fetchParameters, JSValue importWaitState)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    VM& vm = *m_instance.vm;
    auto askLoader = [&] {
        SetForScope importFrameInLoader(vm.aotState().importFrameInLoader, vm.topCallFrame);
        return loader()->requestImportModuleForKey(realm, key, moduleKey(referrer), WTF::move(fetchParameters), nullptr, false, asyncEvaluationOrder(referrer).value_or(-1));
    };
    std::optional<ModuleID> found = fetchParameters ? std::nullopt : m_table.findByPath(key.string());
    if (found && mockLoadError(*found)) [[unlikely]]
        RELEASE_AND_RETURN(scope, rejectImportWithErrorOfLoad(*found, ImportCaller::CompiledCode));
    if (found) {
        JSPromise* factories = pendingFactoryInGraph(*found, PendingFactoryRequester::ImportCall, importWaitState);
        RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
        if (factories) [[unlikely]] {
            JSPromise* promise = JSPromise::create(vm, realm->promiseStructure());
            JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), importByKeyBehindFactories, ImplementationVisibility::Private);
            MarkedArgumentBuffer call;
            call.append(jsString(vm, key.string()));
            call.append(jsNumber(referrer.index()));
            call.append(importWaitState);
            JSArray* deferredCall = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), call);
            RETURN_IF_EXCEPTION(scope, rejectedPromiseWithPendingException(realm, scope));
            factories->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), promise, deferredCall);
            return promise;
        }
    }
    SetForScope requesterRejectsUncompiledCode(vm.aotState().requesterRejectsUncompiledCode, moduleRejectsUncompiledCode(referrer));
    JSPromise* loaded = askLoader();
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    tellHostOfImport(loaded, key.string());
    RETURN_IF_EXCEPTION(scope, importPromiseRejectedByLoader(realm, scope));
    RELEASE_AND_RETURN(scope, createPromiseOfProgram(realm, loaded));
}

bool Modules::moduleRejectsUncompiledCode(ModuleID module) const
{
    return std::ranges::binary_search(m_instance.tables().modulesRejectingUncompiledCode(), module.index());
}

static bool isDataURL(StringView path)
{
    return path.startsWith("data:"_s);
}

bool Modules::isImportedDataURL(StringView url) const
{
    return isDataURL(url) && std::ranges::any_of(m_table.modulesOfUncompiledCode(), [&](const UncompiledModuleData& entry) {
        return url == m_table.fullPath(ModuleID(entry.module));
    });
}

const Identifier& Modules::resolvedKey(ModuleID external)
{
    ASSERT(data(external).isUncompiledCode());
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_runsUncompiledCode = true;
    if (!m_admitsCodeOutsideImage && !isDataURL(m_table.fullPath(external)))
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
    HostRequestScope uncompiledModuleRequest(*this);
    checkModuleAccess(m_instance.globalObject, *file, LoadKind::Import);
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
        String name = m_table.fullPath(ModuleID(entry.module));
        return !isDataURL(name) && (text == name || (text->startsWith(name) && text[name.length()] == '/'));
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
    if (data(root).isLoadedByLoader() || m_moduleWithDeferredJobs || m_rootAwaitingLoader == root || !m_queuedFetches.isEmpty())
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
    countLoaderStep();
    if (!isLoadedWithRequests(module) || !record->requestedModules().isEmpty())
        return;
    if (isMocked(module) || m_modulesWithPendingMock.get(module.index()))
        return;
    auto appendNextOnWay = [&] {
        std::unique_ptr<LevelOfLoad> level = m_levelsOfLoads.take(module);
        if (!level)
            return false;
        if (level->plan)
            level->plan->levelsRequested = std::max(level->plan->levelsRequested, level->depth + 1);
        if (level->next && level->plan && (!m_levelsOfLoads.contains(*level->next) || !isStillToBeLoaded(*level->next)))
            layWayOfLoadAgain(*level);
        if (level->next && level->plan && !m_instance.vm->synchronousModuleQueue())
            countStepsOfLevel(*level->plan, level->depth + 1);
        if (m_showsWaysBetweenModuleObjects && level->next && level->plan && level->depth + 1 < level->plan->levels.size()) {
            for (const PlanOfLoad::Found& found : level->plan->level(level->depth + 1))
                noteRequestByLoader(found.module);
        }
        PlanOfLoad& plan = *level->plan;
        auto append = [&](ModuleID target, bool isFetchedByHost) {
            Identifier key = keyOfRequest(target);
            record->appendRequestedModule(key, nullptr, ModulePhase::Evaluation);
            if (!isFetchedByHost)
                record->didRequestCompiledModule(key, target);
        };
        bool isNextLeft = level->next.has_value();
        auto appendNext = [&] {
            isNextLeft = false;
            bool isFetchedByHost = false;
            if (auto asked = m_levelsOfLoads.find(*level->next); asked != m_levelsOfLoads.end()) {
                asked->value->isAskedFor = true;
                isFetchedByHost = asked->value->plan == &plan && plan.found[asked->value->place].isFetchedByHost;
            }
            append(*level->next, isFetchedByHost);
        };
        struct Listed {
            ModuleID module;
            bool isFetchedByHost;
        };
        Vector<Listed, 8> listed;
        if (level->depth + 1 < plan.levels.size()) {
            std::span<PlanOfLoad::Found> below = plan.level(level->depth + 1);
            for (PlanOfLoad::Found& imported : below.subspan(std::ranges::lower_bound(below, level->place, { }, &PlanOfLoad::Found::importer) - below.begin())) {
                if (imported.importer != level->place)
                    break;
                if (level->next != imported.module && (!imported.leadsToFile || !isLeftToLoad(plan, imported)))
                    continue;
                listed.append({ imported.module, static_cast<bool>(imported.isFetchedByHost) });
                if (imported.isBehindName && isCommonJSModuleOrFacade(imported.module)) {
                    noteRequestByLoader(imported.module);
                    if (!m_isMainCommonJSModuleDecided)
                        m_firstFilesOfLoads.append(fileOf(imported.module));
                }
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
        for (auto [imported, isFetchedByHost] : listed) {
            if (isFetchedByHost && isCommonJSModuleOrFacade(imported)) {
                noteLoadOfFile(imported, loader()->registryEntry(keyOfRequest(imported)) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
                RETURN_IF_EXCEPTION(scope, true);
            }
            if (level->next == imported) {
                appendNext();
                continue;
            }
            if (isCommonJSModuleOrFacade(imported) && !isFetchedByHost) {
                m_filesLoadedWithoutLoader.set(imported.index());
                if (status(imported) == ModuleStatus::Evaluated)
                    continue;
                noteLoadOfFile(imported);
                RETURN_IF_EXCEPTION(scope, true);
            }
            append(imported, isFetchedByHost);
        }
        if (isNextLeft)
            appendNext();
        return true;
    };
    if (!m_levelsOfLoads.isEmpty() && appendNextOnWay())
        return;
    if (!record->isRequestedByWalkedModule()) {
        if (m_table.hasLoadFailures()) [[unlikely]] {
            if (std::optional<ModuleFailureID> failure = findUnmockedFileFailure(module))
                RELEASE_AND_RETURN(scope, appendUnresolvableRequest(*failure, record));
        }
        bool mustWaitForFile = importsFileBeingEvaluated(module);
        bool endsAtStatement = statementThatEndsLoad(module).has_value();
        bool isWalked = mustWaitForFile || endsAtStatement || canTellHowGraphIsLoaded(module);
        bool isPlanned = !isWalked && data(module).reachesUncompiledCode() && !isInTransferredSynchronousLoad() && planLevelsOfLoad(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (isPlanned) {
            if (!m_instance.vm->synchronousModuleQueue() && !m_loadsInFlight.hasRoot(module))
                didStartLoad(load.promise(), module, SettlingOfLoad::IsLookedFor);
            m_loadsInFlight.forEachOfRoot(module, [](LoadInFlight& load) {
                load.isWalked = true;
                load.hasMarksBelowRoot = true;
            });
            appendNextOnWay();
            return;
        }
        if (!isWalked) {
            evaluateImportedCommonJSModules(module, EagerLoadJobPolicy::DeferJobs, ErrorOfImportedFile::IsLeftToLoader, &load);
            RETURN_IF_EXCEPTION(scope, void());
            if (!record->requestedModules().isEmpty())
                return;
            isWalked = m_rootAwaitingLoader == module;
        }
        if (!isWalked) {
            scope.release();
            appendRequestsOfUncompiledCode(module, record);
            return;
        }
        if (!m_instance.vm->synchronousModuleQueue() && m_rootAwaitingLoader != module && !m_loadsInFlight.hasRoot(module))
            didStartLoad(load.promise(), module, SettlingOfLoad::IsLookedFor);
        m_loadsInFlight.forEachOfRoot(module, [](LoadInFlight& load) {
            load.isWalked = true;
        });
        isPlanned = !data(module).isLoadedByLoader() && m_rootAwaitingLoader != module && !mustWaitForFile && !endsAtStatement && !isInTransferredSynchronousLoad() && planLevelsOfLoad(module);
        RETURN_IF_EXCEPTION(scope, void());
        if (isPlanned) {
            appendNextOnWay();
            return;
        }
    }

    countEvent(*m_instance.vm, RuntimeCounterID::CompiledModulesWalkedByLoader);
    UncheckedKeyHashSet<ModuleMapKey, ModuleMapHash> keys;
    WTF::Vector<Identifier, 8> keptAlive;
    bool comesInOrderOfRequests = false;
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
                Identifier name = Identifier::fromString(vm, m_table.fullPath(target));
                JSModuleLoader::attachErrorInfo(m_instance.globalObject, exception, nullptr, name, ScriptFetchParameters::Type::JavaScript, JSModuleLoader::ModuleFailure::Kind::Instantiation);
                loader()->addResolutionFailure(vm, { record->moduleKey().impl(), name.impl() }, exception->value());
                record->appendRequestedModule(name, nullptr, request.isDeferred ? ModulePhase::Defer : ModulePhase::Evaluation);
                record->setHasUnresolvableRequest();
                return;
            }
            if (std::optional<ModuleID> compiled = m_table.findByPath(keptAlive.last().string()); compiled && isCommonJSModuleOrFacade(*compiled)) {
                noteLoadOfFile(*compiled, loader()->registryEntry(keptAlive.last()) ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
                RETURN_IF_EXCEPTION(scope, void());
                noteRequestByLoader(*compiled);
                if (!m_isMainCommonJSModuleDecided)
                    m_firstFilesOfLoads.append(fileOf(*compiled));
            }
        } else if (isCommonJSModuleOrFacade(target)) {
            keptAlive.append(keyOfRequest(target));
            bool isKnownToLoader = data(fileOf(target)).isEntryPoint() || loader()->registryEntry(keptAlive.last());
            LoadedModule* loadedFile = loadedIfExists(fileOf(target));
            comesInOrderOfRequests |= loadedFile && loadedFile->beganInsideSynchronousLoad;
            noteLoadOfFile(target, isKnownToLoader && !comesInOrderOfRequests ? LoadOfFile::HasComeToFile : LoadOfFile::IsRequested);
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
        RELEASE_AND_RETURN(scope, appendUnresolvableRequest(*statement, record));
}

std::optional<ModuleFailureID> Modules::findUnmockedFileFailure(ModuleID root) const
{
    std::optional<ModuleFailureID> failureOfRoot = loadFailure(root);
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (seen.quickSet(module.index()) || !data(module).isESModuleOfProgram())
            continue;
        if (module != root) {
            bool hasBegun = status(module) >= ModuleStatus::Evaluating && status(module) <= ModuleStatus::Evaluated;
            if (hasBegun || isMocked(module) || activeMock(module))
                continue;
            std::optional<ModuleFailureID> failure = loadFailure(module);
            if (failure && failure != failureOfRoot && data(module).mayBeMocked() && m_table.failure(*failure).importerModule() == module && !m_table.failure(*failure).isAbsentExport() && !statementThatEndsLoad(module))
                return failure;
        }
        size_t firstBelow = work.size();
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !request.isDeferred)
                work.append(request.targetModule());
        }
        std::ranges::reverse(work.mutableSpan().subspan(firstBelow));
    }
    return std::nullopt;
}

std::optional<ModuleFailureID> Modules::statementThatEndsLoad(ModuleID module) const
{
    std::optional<ModuleFailureID> endOfLoad = m_table.loadFailure(module);
    if (!endOfLoad) [[likely]]
        return std::nullopt;
    const ModuleFailureData& failure = m_table.failure(*endOfLoad);
    if (ProgramStrings::text(m_table.programTables(), failure.specifierString()) == m_table.fullPath(failure.importerModule()))
        return std::nullopt;
    return endOfLoad;
}

void Modules::appendUnresolvableRequest(ModuleFailureID statement, ModuleRecord* record)
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
    record->setHasUnresolvableRequest();
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
            if (!data(target).isUncompiledCode() || name.string() != m_table.fullPath(target))
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
    auto isOrWillBeMocked = [&](ModuleID importer) { return isMocked(importer) || m_modulesWithPendingMock.get(importer.index()); };
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
        if (std::optional<ModuleID> compiled = m_table.findByPath(String(request.key)); compiled && isCommonJSModuleOrFacade(*compiled)) {
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
        throwException(m_instance.globalObject, scope, createExternalModuleNotLoadedError(m_instance.globalObject, m_table.fullPath(external)));
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
    bool wasRequested = m_uncompiledNamespacesAskedFor.set(external.index());
    if (!isShown) {
        if (!wasRequested)
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
    Modules& modules = modulesOf(*instance);
    if (BuiltinID(builtin) == Builtins::ModuleObject || BuiltinID(builtin) == Builtins::ModuleObjectExports)
        modules.didMaterializeModuleObject(ModuleID(module));
    OPERATION_RETURN(scope, modules.builtinCell(BuiltinID(builtin), ModuleID(module)));
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
    OPERATION_RETURN(scope, modulesOf(*instance).originalNamespace(ModuleID(module)));
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

JSC_DEFINE_JIT_OPERATION(operationAOTRequireModuleWithRequirer, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer, EncodedJSValue requirerObject))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireModuleWithRequirer);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.require(ModuleID(module), ModuleID(referrer), Modules::RequiredBy::Referrer, modules.findStaleRequirerObject(ModuleID(referrer), JSValue::decode(requirerObject)))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTRecordRequire, void, (Instance* instance, uint32_t module, uint32_t referrer, uint32_t fromCreatedRequire))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRecordRequire);
    modulesOf(*instance).recordRequire<Modules::RequireResolver::CompiledCode>(ModuleID(module), ModuleID(referrer), fromCreatedRequire);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTRecordRequireWithRequirer, void, (Instance* instance, uint32_t module, uint32_t referrer, uint32_t fromCreatedRequire, uint32_t followsLoad, EncodedJSValue requirerObject))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRecordRequireWithRequirer);
    Modules& modules = modulesOf(*instance);
    modules.recordRequire<Modules::RequireResolver::CompiledCode>(ModuleID(module), ModuleID(referrer), fromCreatedRequire && !Modules::isCreatedRequireFunction(JSValue::decode(requirerObject)), modules.findStaleRequirerObject(ModuleID(referrer), JSValue::decode(requirerObject)), followsLoad);
    OPERATION_RETURN(scope);
}

static bool isLoadedWithoutType(const Modules& modules, ModuleID form)
{
    String path = modules.table().fullPath(form);
    size_t mark = path.reverseFind('?');
    if (modules.data(form).kind != ModuleKind::Data || mark == notFound)
        return false;
    std::optional<ModuleID> file = modules.table().findByPath(StringView(path).left(mark));
    return file && modules.data(*file).kind == ModuleKind::Data && modules.status(*file) == ModuleStatus::Evaluated;
}

JSC_DEFINE_JIT_OPERATION(operationAOTRequireIfImported, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireIfImported);
    Modules& modules = modulesOf(*instance);
    if (!modules.hasBeenImported(ModuleID(module)) || isLoadedWithoutType(modules, ModuleID(module)))
        OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
    if (modules.isUnevaluatedTextForm(ModuleID(module))) [[unlikely]] {
        String path = modules.table().fullPath(ModuleID(module));
        if (std::optional<ModuleID> file = modules.table().findByPath(StringView(path).left(path.reverseFind('?'))); file && modules.data(*file).kind == ModuleKind::Data && modules.data(*file).requireResult == RequireResult::NamespaceIfImported) {
            modules.moduleNamespace(*file, Modules::ModulePhase::Evaluation);
            OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    }
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

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSeesMockOf, size_t, (Instance* instance, uint32_t module, uint32_t referrer, uint32_t reader, uint32_t thatKeepsNamesOfOriginal))
{
    if (thatKeepsNamesOfOriginal)
        return modulesOf(*instance).seesMockThatKeepsNamesOfOriginal(ModuleID(module), ModuleID(referrer), ModuleID(reader));
    return !!modulesOf(*instance).mockSeenBy(ModuleID(module), ModuleID(referrer), ModuleID(reader));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTIsLinkedBelow, size_t, (Instance* instance, uint32_t module, uint32_t referrer))
{
    return modulesOf(*instance).isLinkedBelow(ModuleID(module), ModuleID(referrer));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMockSeenBy, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer, uint32_t reader))
{
    return JSValue::encode(modulesOf(*instance).mockSeenBy(ModuleID(module), ModuleID(referrer), ModuleID(reader)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTNamespaceSeenBy, EncodedJSValue, (Instance* instance, uint32_t module, uint32_t referrer, uint32_t reader))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTNamespaceSeenBy);
    OPERATION_RETURN(scope, JSValue::encode(modulesOf(*instance).namespaceSeenBy(ModuleID(module), ModuleID(referrer), ModuleID(reader))));
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

JSC_DEFINE_JIT_OPERATION(operationAOTRequireDynamicWithRequirer, EncodedJSValue, (Instance* instance, EncodedJSValue specifier, uint32_t referrer, EncodedJSValue requirerObject))
{
    AOT_OPERATION_BEGIN(instance, Entry::operationAOTRequireDynamicWithRequirer);
    Modules& modules = modulesOf(*instance);
    OPERATION_RETURN(scope, JSValue::encode(modules.requireSpecifier(JSValue::decode(specifier), ModuleID(referrer), Modules::NamedByBuild::No, modules.findStaleRequirerObject(ModuleID(referrer), JSValue::decode(requirerObject)))));
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
        if (number && *number < table.size() && table.fullPath(ModuleID(*number)) == publicPath)
            return ModuleID(*number);
        return std::nullopt;
    }
    return table.findByPath(publicPath, hashOfPath);
}

std::optional<ModuleID> findModule(StringView publicPath, unsigned hashOfPath)
{
    return findModuleInRunningImage(publicPath, hashOfPath);
}

bool hasAttributeVariant(StringView publicPath)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->hasAttributeVariant(publicPath);
}

bool mayContainPathWithPrefix(StringView start)
{
    const Image* image = Image::current();
    return image && (start.isEmpty() || start.startsWith('#') || (image->mayContainModulePathWithPrefix(start) && ModuleTable(image->programTables()).containsPathWithPrefix(start)));
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

struct ExternalSpecifiers {
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

static const ExternalSpecifiers& externalSpecifiers(const Image& image, const ModuleTable& table)
{
    struct NamesOfImages {
        Lock lock;
        UncheckedKeyHashMap<unsigned, std::unique_ptr<ExternalSpecifiers>> byImage WTF_GUARDED_BY_LOCK(lock);
    };
    static NeverDestroyed<NamesOfImages> namesOfImages;
    NamesOfImages& ofImages = namesOfImages.get();
    Locker locker { ofImages.lock };
    return *ofImages.byImage.ensure(image.number() + 1, [&] {
        auto made = makeUniqueWithoutFastMallocCheck<ExternalSpecifiers>();
        for (const UncompiledModuleData& entry : table.modulesOfUncompiledCode()) {
            ModuleID module(entry.module);
            String name = table.fullPath(module);
            String directory;
            if (name.startsWith('.')) {
                std::optional<ModuleID> referrer = table.data(module).referrerModule();
                if (!referrer)
                    continue;
                directory = withoutSeparatorAtEnd(directoryOf(table.fullPath(*referrer))).toString();
            }
            made->placesInOrder.append(made->names.size());
            made->names.append(WTF::move(name));
            made->directories.append(WTF::move(directory));
        }
        AOT::sort(made->placesInOrder, [&](uint32_t left, uint32_t right) {
            return ExternalSpecifiers::isLess(made->keyAt(left), made->keyAt(right));
        });
        return made;
    }).iterator->value;
}

bool isExternalSpecifier(StringView specifier, StringView importerDirectory)
{
    const Image* image = Image::current();
    if (!image)
        return false;
    ModuleTable table(image->programTables());
    if (table.modulesOfUncompiledCode().empty())
        return false;
    const ExternalSpecifiers& left = externalSpecifiers(*image, table);
    auto holds = [&](StringView name, StringView directory) {
        return std::ranges::binary_search(left.placesInOrder, ExternalSpecifiers::Key { name, directory }, ExternalSpecifiers::isLess, [&](uint32_t place) {
            return left.keyAt(place);
        });
    };
    if (specifier.startsWith('.'))
        return holds(specifier, withoutSeparatorAtEnd(importerDirectory));
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
    StringImpl* displayPath;
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
        bool hasOwnURL = !ownURL.isEmpty() && ownURL != table->fullPath(module);
        String path = hasOwnURL ? ownURL : evaluator ? URL::fileURLWithFileSystemPath(table->fullPath(*evaluator)).string() : table->data(module).registryID() ? table->displayPathText(module) : table->fullPath(module);
        auto keep = [](const String& text) {
            return &(text.is8Bit() ? StringImpl::createStaticStringImpl(text.span8()) : StringImpl::createStaticStringImpl(text.span16())).leakRef();
        };
        StringImpl* kept = keep(path);
        String pathOfFile = evaluator || table->data(module).registryID() ? path : withoutQuery(*table, module, path);
        StringImpl* keptPathOfFile = pathOfFile.length() == path.length() ? kept : keep(pathOfFile);
        String displayPath = evaluator ? String() : table->displayPathText(module);
        bool isRelativeToFolderOfBuild = !displayPath.isEmpty() && displayPath != pathOfFile;
        if (isRelativeToFolderOfBuild && !displayPath.startsWith("../"_s))
            displayPath = makeString(AOT_BUILD_FOLDER_PREFIX ""_s, displayPath);
        return PathOfModule { kept, keptPathOfFile, isRelativeToFolderOfBuild ? keep(displayPath) : keptPathOfFile, !!evaluator };
    }).iterator->value;
}

String modulePath(ModuleID module)
{
    return pathOfModule(module).path;
}

String moduleFilePath(ModuleID module)
{
    return pathOfModule(module).pathOfFile;
}

String moduleDisplayPath(ModuleID module)
{
    return pathOfModule(module).displayPath;
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

bool isSynchronousESModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->data(module).kind == ModuleKind::SourceText && modules->data(module).isSynchronous() && !modules->importsModuleWithPendingMock(module);
}

std::optional<bool> hasReadOnlyBuiltins(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table || table->data(module).isOfLibrary())
        return std::nullopt;
    return std::ranges::binary_search(Image::current()->programTables().modulesWithReadOnlyBuiltins(), module.index());
}

bool isImportedByAlias(ModuleID module, ModuleAliasKind name)
{
    if (!currentModuleTable())
        return false;
    return std::ranges::binary_search(Image::current()->programTables().importedModuleAliases(), encodeModuleAlias(module.index(), name));
}

bool isDataFile(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return table && table->data(module).kind == ModuleKind::Data;
}

bool hasModuleBody(ModuleID module)
{
    std::optional<ModuleTable> table = currentModuleTable();
    return !table || table->data(module).bodyFunction();
}

bool hasBeenImported(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasBeenImported(module);
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

static void throwLoaderChangeError(JSGlobalObject* realm, LoaderChange change, StringView changed)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createLoaderChangeError(realm, change, changed));
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
    modules->noteWhetherProgramStoredFalsyEntry(module, takenOutBy == TakenOutBy::FalsyStore);
    if (takenOutBy == TakenOutBy::FalsyStore && isFoundBehindFalsyEntry(*modules, module))
        return modules->answerRequireWithNamespaceOfFile(module);
    if (modules->data(module).isOfLibrary()) {
        String path = modules->table().fullPath(module);
        if (takenOutBy == TakenOutBy::EntryOfProgram && modules->data(module).rejectsRequireCacheOverride() && !path.startsWith("node:"_s))
            return throwLoaderChangeError(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return modules->takeOutOfRequireCache(module);
    }
    modules->releaseModuleObjectFromRequire(module);
    if (takenOutBy == TakenOutBy::EntryOfProgram)
        modules->retainOriginalExports(module);
    else if (takenOutBy == TakenOutBy::Reset) {
        modules->letRootsOfFailedImportsGoWithWhatIsReset();
        modules->clearRetainedOriginalExports(module);
        modules->setIsUnderActualKey(module, false);
    }
    bool isDeleted = takenOutBy == TakenOutBy::Deletion || takenOutBy == TakenOutBy::Reset;
    if (isDeleted) {
        modules->answerRequireWithExportsOfFile(module);
        modules->forgetFirstLoad(module);
        modules->clearMockLoadError(module);
        modules->forgetImportFailedUnderMock(module);
        modules->forgetRequestByLoader(module);
    }
    bool standsInPlaceOfFile = takenOutBy == TakenOutBy::EntryOfProgram;
    if (!modules->mayBeEvicted(module)) {
        if (standsInPlaceOfFile)
            throwLoaderChangeError(realm, LoaderChange::ReplaceInRequireCache, modules->table().fullPath(module));
        else if (modules->wouldBeEvicted(module))
            throwLoaderChangeError(realm, LoaderChange::TakeOutOfRequireCache, modules->table().fullPath(module));
        return;
    }
    modules->forgetFormAnsweredFirst(module);
    if (!standsInPlaceOfFile && modules->hasNoEntryBehindMock(module))
        return;
    if (takenOutBy == TakenOutBy::Deletion)
        modules->retainMockForLinkedImporters(module);
    modules->evict(module, takenOutBy == TakenOutBy::Reset);
    if (isDeleted && modules->isEvicted(module))
        modules->takeOutOfLoader(module);
    if (standsInPlaceOfFile)
        modules->delegateToHostUnlessEvaluated(module);
}

void willReevaluateEntryPoint(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->resetEntryPointEvaluation(module);
}

void didDeleteDisplacedModuleEntry(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->retainMockForLinkedImporters(module);
}

void didDeleteRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->activeMock(module) && (modules->isMocked(module) || modules->hasPendingMock(module)))
        didDeletePendingMockEntry(realm, module);
    else if (modules && modules->activeMock(module) && !modules->hasRemovedMock(module))
        modules->didRemoveActiveMockEntry(module);
    evictModule(realm, module, TakenOutBy::Deletion);
}

void didDeletePendingMockEntry(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->settleFailedImportsUnder(module, Modules::FailedImports::MakeUnevictable);
}

void didFailMockImportRoot(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didFailMockImportRoot(module);
}

bool isLoadInFlight(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isLoadInFlight(module);
}

bool isResolvedForInFlightLoads(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->isResolvedForInFlightLoads(module);
}

void didResetModule(JSGlobalObject* realm, ModuleID module)
{
    evictModule(realm, module, TakenOutBy::Reset);
}

void Modules::forEachFailedModule(const WTF::Function<void(ModuleID)>& functor) const
{
    for (auto& [module, record] : m_records) {
        const ModuleData& moduleData = data(module);
        if (statusOf(*record) != ModuleStatus::Failed || moduleData.isOfLibrary() || !m_table.hasFilePath(module))
            continue;
        ModuleID file = m_table.moduleBehindFacade(module).value_or(module);
        if (mayBeEvicted(file) && !isEvicted(file))
            functor(file);
    }
    for (ModuleID module : m_mocksWithIntactOriginal.keys()) {
        if (errorOfModuleThatRanBesideMock(module) && mayBeEvicted(module) && !isEvicted(module))
            functor(module);
    }
}

void evictFailedModules(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    modules->resetFailedEvaluations();
    modules->releaseFailedESModules();
    modules->forgetFailuresOfExportsThatBuildFoundAbsent();
    if (!modules->holdsCodeThatMayBeEvicted())
        return;
    Vector<ModuleID, 4> failed;
    modules->forEachFailedModule([&](ModuleID file) {
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

void didCacheSourceInHost(VM& vm, ModuleID module, bool isPluginSource)
{
    std::optional<ModuleTable> table = currentModuleTable();
    if (!table || !isPluginSource)
        return;
    ModuleID file = table->moduleBehindFacade(module).value_or(module);
    vm.aotState().modulesWithHostCachedSource.set(file.index());
    vm.aotState().filesWhoseKeptTextWasCompiled.clear(file.index());
}

bool isEvicted(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isEvicted(module);
}

void willRunPreloads(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->setIsRunningPreloads(true);
}

void didRunPreloads(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->setIsRunningPreloads(false);
}

void snapshotEvaluatedModules(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->snapshotEvaluatedModules();
}

void retainNewlyEvaluatedModules(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->retainNewlyEvaluatedModules();
}

void restoreRetainedModules(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    if (Modules* modules = modulesOf(realm))
        modules->restoreRetainedModules(functor);
}

bool rejectsReload(JSGlobalObject* realm, ModuleID module)
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

bool tryDelegateModuleToHost(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules || modules->data(module).isOfLibrary())
        return true;
    if (!modules->mayBeEvicted(module))
        return false;
    modules->delegateToHostUnlessEvaluated(module);
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
        ModuleRecord* record = recordIfExists(ModuleID(index));
        if (now[index] != inRecord || m_modulesSeenToHaveRun.quickGet(index) || !record || record->status() < CyclicModuleRecord::Status::Evaluating)
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
            if (m_modulesOwedToImportersThatRan.get(request.target()) || m_modulesWithRemovedMock.get(request.target()))
                continue;
            if (status(request.targetModule()) == ModuleStatus::Evaluating || waitsForItsCycle(request.targetModule(), m_modulesThatWaitedWhenImportsWereFollowed))
                continue;
            if (!m_modulesThatRanWithImporters.quickSet(request.target()))
                toFollow.append(request.targetModule());
        }
    }
    countEvent(*m_instance.vm, RuntimeCounterID::ModulesScannedForEvaluationStatus, numberLookedAt);
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

bool hasPendingMock(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasPendingMock(file);
}

static bool hasRequireCacheEntry(Modules* modules, ModuleID module, bool& importsAreFollowed)
{
    const ModuleData& data = modules->data(module);
    if (data.isOfLibrary())
        return modules->isLibraryModuleInRequireCache(module);
    if (data.isFacade() || !modules->table().hasFilePath(module) || modules->isEvicted(module) || modules->hasNoEntryBehindMock(module))
        return false;
    if (!modules->hasRun(module, importsAreFollowed) && !modules->isListedBeforeItHasRun(module))
        return false;
    return !isNamedByNumber(modules->table().fullPath(module), module);
}

bool hasRequireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    bool importsAreFollowed = false;
    return modules && hasRequireCacheEntry(modules, module, importsAreFollowed);
}

JSObject* requireCacheEntry(JSGlobalObject* realm, ModuleID module)
{
    ASSERT(hasRequireCacheEntry(realm, module));
    return modulesOf(realm)->requireCacheEntry(module);
}

void Modules::forEachTLASuspendedModule(const WTF::Function<void(int64_t, const String&)>& functor)
{
    for (ModuleRecord* record : m_records.values()) {
        if (record->status() == CyclicModuleRecord::Status::EvaluatingAsync && record->hasTLA() && !record->pendingAsyncDependencies().value_or(0) && record->asyncEvaluationOrder().hasOrder())
            functor(record->asyncEvaluationOrder().order(), record->moduleKey().string());
    }
}

void forEachEvaluatingAsyncModule(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    for (uint32_t index = 0; index < table.size(); ++index) {
        ModuleID module(index);
        if (!table.data(module).isOfLibrary() && !table.data(module).isFacade() && table.hasFilePath(module) && !modules->isEvicted(module) && modules->status(module) == ModuleStatus::EvaluatingAsync)
            functor(table.fullPath(module));
    }
}

BitVector Modules::modulesBelowEvaluatingOnes(Vector<ModuleID>& topmost) const
{
    BitVector below(m_table.size());
    BitVector imported(m_table.size());
    Vector<ModuleID, 32> evaluating;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        if (status(ModuleID(index)) == ModuleStatus::Evaluating)
            evaluating.append(ModuleID(index));
    }
    Vector<ModuleID, 32> work = evaluating;
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (below.quickSet(module.index()) || data(module).isOfLibrary())
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (request.isDeferred)
                continue;
            ModuleID target = moduleLoadedUnder(request.targetModule());
            imported.quickSet(target.index());
            work.append(target);
        }
    }
    topmost.shrink(0);
    for (ModuleID module : evaluating) {
        if (!imported.quickGet(module.index()))
            topmost.append(module);
    }
    if (topmost.isEmpty())
        topmost.append(evaluating.span());
    return below;
}

void Modules::resetModulesAfterEvaluation(const WTF::Function<bool(const String&)>& isKept)
{
    BitVector listed(m_table.size());
    for (ModuleID file : m_modulesToResetOnceRun)
        listed.quickSet(file.index());
    for (size_t index : modulesBelowEvaluatingOnes(m_modulesThatResetWaitsFor)) {
        ModuleID module(index);
        ModuleStatus current = status(module);
        if (current != ModuleStatus::Evaluating && current != ModuleStatus::Linked)
            continue;
        ModuleID file = fileOf(module);
        if (data(file).isOfLibrary() || !m_table.hasFilePath(file) || isEvicted(file) || !mayBeEvicted(file) || listed.quickSet(file.index()))
            continue;
        if (!isKept(m_table.fullPath(file)))
            m_modulesToResetOnceRun.append(file);
    }
}

void Modules::resetModulesOnceRun()
{
    bool hasOneEnded = m_modulesThatResetWaitsFor.isEmpty() || std::ranges::any_of(m_modulesThatResetWaitsFor, [&](ModuleID module) {
        return status(module) != ModuleStatus::Evaluating;
    });
    if (!hasOneEnded)
        return;
    BitVector below = modulesBelowEvaluatingOnes(m_modulesThatResetWaitsFor);
    Vector<ModuleID> haveRun;
    m_modulesToResetOnceRun.removeAllMatching([&](ModuleID file) {
        if (below.get(file.index()) || below.get(moduleWithExportsOf(file).index()))
            return false;
        haveRun.append(file);
        return true;
    });
    for (ModuleID file : haveRun)
        didResetModule(m_instance.globalObject, file);
}

void resetModulesAfterEvaluation(JSGlobalObject* realm, const WTF::Function<bool(const String&)>& isKept)
{
    if (Modules* modules = modulesOf(realm))
        modules->resetModulesAfterEvaluation(isKept);
}

void forEachModuleWithProgramEntry(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    for (uint32_t index = 0; index < table.size(); ++index) {
        if (!table.data(ModuleID(index)).isFacade() && modules->retainedOriginalExports(ModuleID(index)))
            functor(table.fullPath(ModuleID(index)));
    }
}

void forEachTLASuspendedModule(JSGlobalObject* realm, const WTF::Function<void(int64_t, const String&)>& functor)
{
    if (Modules* modules = modulesOf(realm))
        modules->forEachTLASuspendedModule(functor);
}

void forEachRequireCacheKey(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    BitVector isSeen(table.size());
    Vector<ModuleID, 64> inOrder;
    bool importsAreFollowed = false;
    auto askFor = [&](ModuleID module) {
        if (isSeen.quickSet(module.index()))
            return false;
        if (!table.data(module).isOfLibrary() && hasRequireCacheEntry(modules, module, importsAreFollowed))
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
            if (!modules->isLoadedWithRequests(step.askers[0]) || table.data(step.askers[0]).isFacade()) {
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
                    ModuleID target = table.moduleBehindFacade(loaded).value_or(loaded);
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
                functor(table.fullPath(module));
        }
    }
}

void forEachLibraryModuleInRequireCache(JSGlobalObject* realm, const WTF::Function<void(const String&)>& functor)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    for (ModuleID module : modules->libraryModulesInRequireCache())
        functor(modules->table().fullPath(module));
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

bool isReplacedByHost(JSGlobalObject* realm, ModuleID module, LoadKind way)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isReplacedByHost(module, way);
}

void willLoadCopyFromDisk(JSGlobalObject* realm, ModuleID module, LoadKind way)
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
        functor(table.fullPath(ModuleID(file)), state.filesFirstLoadedSynchronously.get(file));
}

void recordFirstLoadFromEarlierProcess(VM& vm, const String& path, bool wasSynchronous)
{
    VMState& state = vm.aotState();
    if (!state.image)
        return;
    ModuleTable table(state.image->programTables());
    std::optional<ModuleID> found = table.findByPath(path);
    if (!found)
        return;
    ModuleID file = table.moduleBehindFacade(*found).value_or(*found);
    if (!state.filesWithKnownFirstLoad.set(file.index()))
        state.filesFirstLoadedSynchronously.set(file.index(), wasSynchronous);
}

bool delegatesModuleToHost(JSGlobalObject* realm, ModuleID module, StartedBy startedBy)
{
    if (Modules* modules = modulesOf(realm))
        return startedBy == StartedBy::Host ? modules->delegatesToStartingHost(module) : modules->delegatesToHost(module);
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

void forEachFilePathWithPrefix(StringView start, const ScopedLambda<IterationStatus(const String&)>& functor)
{
    Image::forEachPublishedImage([&](const Image& image) {
        if (!image.mayContainModulePathWithPrefix(start))
            return IterationStatus::Continue;
        return ModuleTable(image.programTables()).forEachFilePathWithPrefix(start, functor);
    });
}

bool rejectsUncompiledCode(JSGlobalObject* realm, StringView path)
{
    if (Modules* modules = modulesOf(realm))
        return modules->rejectsUncompiledCode() && !modules->isImportedDataURL(path);
    const Image* image = Image::current();
    return image && !hasFlag(image->header().options.admitted(), AdmittedCode::Inspector) && !image->loadsCodeOutsideImage();
}

bool wasModifiedSinceBuild(JSGlobalObject* realm, StringView path)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasModifiedSinceBuild(path);
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

bool rejectsUncompiledHostObjects(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return rejectsUncompiledCode(realm, { }) && !(modules && modules->isInHostRequest());
}

JSObject* mainModuleObject(JSGlobalObject* realm, ModuleID module)
{
    if (!modulesThatShowWaysBetweenModuleObjects(realm))
        return nullptr;
    Modules* modules = modulesOf(realm);
    return modules ? modules->mainModuleObject(module) : nullptr;
}

void willSetMainModulePath(JSGlobalObject* realm)
{
    Instance* instance = realm->aotInstance();
    if (!instance || hasFlag(instance->options().admitted(), AdmittedCode::ReplacedBuiltins) || instance->program->setsMainModulePath())
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwTypeError(realm, scope, assignmentThatBuildDidNotSeeMessage("Bun.main"_s));
}

JSObject* parentModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->parentModuleObject(module) : nullptr;
}

bool canExposeModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->canExposeModuleObject(module);
}

bool forEachNewChild(JSGlobalObject* realm, ModuleID module, const ScopedLambda<void(JSObject*)>& functor)
{
    if (Modules* modules = modulesOf(realm); modules && modules->data(module).isOfLibrary())
        return true;
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules && modules->forEachNewChild(module, functor);
}

void clearNewChildren(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm))
        modules->clearNewChildren(module);
}

void Modules::stopWhereExportsLeaveCompiledCode(ModuleID module) const
{
    if (!data(module).hasUnescapedExports()) [[likely]]
        return;
    RuntimeCheckFailure failure("escape check failed"_s);
    failure.detail("the exports of ", m_table.fullPath(module), " were accessed from outside compiled code, but the compiler proved that only compiled code accesses them");
    failure.report();
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
    modules->noteExportsStoredThroughModuleObject(module, exports);
}

const String& compiledCodeRequester()
{
    static NeverDestroyed<const String> word(MAKE_STATIC_STRING_IMPL("the program"));
    return word;
}

ModuleRequesterScope::ModuleRequesterScope(JSGlobalObject* realm, const String& path)
    : m_modules(modulesOf(realm))
    , m_previousRequester(m_modules ? m_modules->exchangeModuleRequester(&path) : nullptr)
{
}

ModuleRequesterScope::~ModuleRequesterScope()
{
    if (m_modules)
        m_modules->exchangeModuleRequester(m_previousRequester);
}

bool hasUncompiledRequester(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasUncompiledRequester();
}

void throwCompiledModuleAccessError(JSGlobalObject* realm, ModuleID module, LoadKind form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    String path = modulePath(module);
    notifyRejectedChangeListener(realm, makeString("Cannot reach '"_s, path, '\''));
    throwException(realm, scope, createCompiledModuleAccessError(realm, path, form == LoadKind::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

static void throwLoadByNameRefused(JSGlobalObject* realm, StringView name, LoadKind form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    notifyRejectedChangeListener(realm, makeString("Cannot reach '"_s, name, '\''));
    throwException(realm, scope, createLoadByNameRefusedError(realm, name, form == LoadKind::Import ? "ERR_MODULE_NOT_FOUND"_s : "MODULE_NOT_FOUND"_s));
}

void Modules::refuseToShowExportsOf(ModuleID module)
{
    throwLoadByNameRefused(m_instance.globalObject, m_table.fullPath(m_table.moduleBehindFacade(module).value_or(module)), LoadKind::Import);
}

void Modules::refuseToHandOutExportOf(ModuleID module, StringView name)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    String path = m_table.fullPath(m_table.moduleBehindFacade(module).value_or(module));
    notifyRejectedChangeListener(realm, makeString("Cannot reach '"_s, path, '\''));
    throwException(realm, scope, createExportNotHandedOutError(realm, path, name));
}

bool isResolvingImportCall(VM& vm)
{
    return vm.aotState().isResolvingImportCall;
}

bool isResolvingImportCallWithoutAttributes(VM& vm)
{
    return vm.aotState().isResolvingImportCallWithoutAttributes;
}

bool isCurrentRequireTarget(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && realm->vm().aotState().requireRunsMocks && modules->isCurrentRequireTarget(module);
}

bool isRequiringAnotherModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && realm->vm().aotState().requireRunsMocks && modules->isRequiringAnotherModule(module);
}

bool isCurrentImportTarget(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isCurrentImportTarget(module);
}

std::optional<ModuleID> currentLoadTarget(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->currentLoadTarget() : std::nullopt;
}

bool isMockedForImportCall(JSGlobalObject* realm, ModuleID mocked, ModuleID importer)
{
    Modules* modules = modulesOf(realm);
    return !modules || !modules->importsOriginalDespiteMock(mocked, importer, Modules::OriginalRequester::ImportCall);
}

std::optional<CurrentImportCall> currentImportCall(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->currentImportCall() : std::nullopt;
}

BitVector modulesInLoadGraph(JSGlobalObject* realm, ModuleID root, LoadGraphMode graphMode)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->modulesInLoadGraph(root, graphMode) : BitVector();
}

BitVector unfinishedModulesInLoadGraph(JSGlobalObject* realm, ModuleID root, LoadGraphMode graphMode, const BitVector& unsettledMocks)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return { };
    BitVector result = modules->modulesInLoadGraph(root, graphMode);
    result.set(root.index());
    bool importsAreFollowed = false;
    Vector<size_t, 64> finished;
    for (size_t index : result) {
        ModuleID module(index);
        ModuleID file = modules->table().moduleBehindFacade(module).value_or(module);
        if (!unsettledMocks.get(file.index()) && !modules->isBeingEvaluated(module) && hasRequireCacheEntry(modules, module, importsAreFollowed))
            finished.append(index);
    }
    for (size_t index : finished)
        result.quickClear(index);
    return result;
}

bool isRetainedByInFlightLoads(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isRetainedByInFlightLoads(module);
}

void retainRecordForInFlightLoads(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->retainRecordForInFlightLoads(module);
}

bool isInLoadGraph(JSGlobalObject* realm, ModuleID root, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->modulesInLoadGraph(root).quickGet(module.index());
}

JSValue currentImportWaitState(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->currentImportWaitState() : JSValue();
}

bool isCurrentImportTargetAfterWait(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isCurrentImportTargetAfterWait(module);
}

bool requireRunsMocks(VM& vm)
{
    return vm.aotState().requireRunsMocks;
}

String requirerPath(JSGlobalObject* realm, ModuleID required)
{
    Modules* modules = modulesOf(realm);
    return modules && realm->vm().aotState().requireRunsMocks ? modules->requirerPath(required) : String();
}

void willLoadByName(JSGlobalObject* realm, JSValue specifier, LoadKind form)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesOf(realm);
    if (modules ? modules->rejectsLoadsByName() && modules->rejectsUncompiledCode() : !Image::current()->loadsModulesByName()) {
        String name = specifier.isString() ? asString(specifier)->value(realm) : String();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, throwLoadByNameRefused(realm, name, form));
    }
    if (modules)
        RELEASE_AND_RETURN(scope, modules->admitCodeNamedBy(specifier));
}

static Modules* modulesOrThrow(JSGlobalObject* realm, ModuleID module, LoadKind form = LoadKind::Import)
{
    Modules* modules = modulesOf(realm);
    if (modules && !modules->refusesRequestFromHost(module)) [[likely]]
        return modules;
    if (modules) {
        if (modules->hasUncompiledRequester() && Image::current()->loadsCodeOutsideImage())
            throwCompiledModuleAccessError(realm, module, form);
        else
            throwLoadByNameRefused(realm, modulePath(module), form);
        return nullptr;
    }
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    throwException(realm, scope, createModuleInOtherRealmError(realm, modulePath(module)));
    return nullptr;
}

void checkModuleAccess(JSGlobalObject* realm, ModuleID module, LoadKind form)
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

AbstractModuleRecord* moduleRecordForNamespace(JSGlobalObject*, JSCell* cell)
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
    modules->releaseRemovedMock(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    VMState& state = realm->vm().aotState();
    bool isFirstAskedForByImport = state.importFrameInLoader && (!state.stackPointerOfImport || state.stackPointerOfImport > static_cast<void*>(state.importFrameInLoader));
    SetForScope stackPointerOfImport(state.stackPointerOfImport, isFirstAskedForByImport ? currentStackPointer() : state.stackPointerOfImport);
    if (!modules->isInHostRequest()) {
        if (std::optional<ModuleFailureID> failure = modules->loadFailure(module); failure && !modules->statementThatEndsLoad(module)) [[unlikely]] {
            scope.release();
            modules->throwFailure(*failure, false);
            return nullptr;
        }
    }
    if (modules->isCommonJSModuleOrFacade(module)) {
        if (!modules->isInHostRequest())
            modules->didImportFileFromUncompiledCode(module);
        RELEASE_AND_RETURN(scope, modules->recordOfCommonJSFile(module, Modules::LoadOfFile::IsRequested));
    }
    modules->runDataFileAgainForImport(module, Modules::ImportResolver::Loader);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!modules->isInHostRequest() || modules->isStatementHostRequest())
        modules->didImportFromUncompiledCode(module, isFirstAskedForByImport);
    RELEASE_AND_RETURN(scope, modules->exposeRecord(module, modules->isInHostRequest() ? Modules::RecordHolder::Loader : Modules::RecordHolder::Anyone));
}

bool isCompiledModuleRecord(AbstractModuleRecord* record)
{
    return record->inherits<ModuleRecord>();
}

std::optional<ModuleID> compiledModuleForRecord(JSGlobalObject* realm, AbstractModuleRecord* record)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(record);
    Modules* modules = modulesOf(realm);
    if (!compiled || !modules)
        return std::nullopt;
    return modules->table().moduleBehindFacade(compiled->module()).value_or(compiled->module());
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

JSC_DEFINE_HOST_FUNCTION(didFulfillImportThatLinkedAnew, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->letLinksOfLoadStand(ModuleID(callFrame->argument(1).asUInt32()));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(didRejectImportThatLinkedAnew, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->bringBackLinksOfLoadThatFailed(ModuleID(callFrame->argument(1).asUInt32()));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(didFulfillImportUnderMock, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->forgetImportFailedUnderMock(ModuleID(callFrame->argument(1).asUInt32()));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(didRejectImportUnderMock, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->noteImportFailedUnderMock(ModuleID(callFrame->argument(1).asUInt32()), callFrame->argument(0));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(didSettleImportBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didSettleImportBehindFactories(ModuleID(callFrame->argument(1).asUInt32()));
    return JSValue::encode(jsUndefined());
}

JSC_DEFINE_HOST_FUNCTION(importBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* deferredCall = asArray(callFrame->argument(1));
    ModuleID module(deferredCall->getIndexQuickly(0).asUInt32());
    modulesOf(realm)->didResumeAfterFactories(module);
    if (JSValue error = modulesOf(realm)->errorOfFactoryThatFailedWhileCallWaited(module, deferredCall->getIndexQuickly(5).asUInt32())) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        throwException(realm, scope, error);
        return { };
    }
    JSValue order = deferredCall->getIndexQuickly(1);
    std::optional<int64_t> referrerAsyncOrder = order.isUndefined() ? std::nullopt : std::optional(static_cast<int64_t>(order.asNumber()));
    auto caller = deferredCall->getIndexQuickly(2).asBoolean() ? Modules::ImportCaller::Host : Modules::ImportCaller::CompiledCode;
    SetForScope wasHeldForBody(modulesOf(realm)->m_wasHeldForBody, deferredCall->getIndexQuickly(6).asBoolean());
    return JSValue::encode(modulesOf(realm)->startImport(module, referrerAsyncOrder, caller, static_cast<Modules::ImportSourceState>(deferredCall->getIndexQuickly(3).asUInt32()), nullptr, deferredCall->getIndexQuickly(4)));
}

JSC_DEFINE_HOST_FUNCTION(rejectImportAfterBody, (JSGlobalObject* realm, CallFrame* callFrame))
{
    VM& vm = realm->vm();
    JSPromise* rejected = JSPromise::create(vm, realm->promiseStructure());
    rejected->markAsHandled();
    rejected->reject(vm, callFrame->argument(0));
    return JSValue::encode(rejected);
}

JSC_DEFINE_HOST_FUNCTION(importByKeyBehindFactories, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->didResumeAfterFactories();
    JSArray* deferredCall = asArray(callFrame->argument(1));
    ModuleID referrer(deferredCall->getIndexQuickly(1).asUInt32());
    Identifier key = Identifier::fromString(realm->vm(), asString(deferredCall->getIndexQuickly(0))->tryGetValue());
    return JSValue::encode(modulesOf(realm)->importUncompiled(key, referrer, nullptr, deferredCall->getIndexQuickly(2)));
}

JSC_DEFINE_HOST_FUNCTION(didFetchFactoryChainImport, (JSGlobalObject* realm, CallFrame* callFrame))
{
    JSArray* deferredCall = asArray(callFrame->argument(1));
    return JSValue::encode(modulesOf(realm)->resumeFactoryChainImport(ModuleID(deferredCall->getIndexQuickly(0).asUInt32()), ModuleID(deferredCall->getIndexQuickly(1).asUInt32())));
}

JSC_DEFINE_HOST_FUNCTION(continueImportWithModuleItself, (JSGlobalObject* realm, CallFrame* callFrame))
{
    ModuleID module(callFrame->argument(1).asUInt32());
    return JSValue::encode(modulesOf(realm)->importModuleItselfBesideMock(module, callFrame->argument(0)));
}

JSPromise* importModule(JSGlobalObject* realm, ModuleID module, std::optional<int64_t> referrerAsyncOrder)
{
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    Modules* modules = modulesForEvaluation(realm, module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (!modules)
        return nullptr;
    modules->removeUnusedLingeringMocks();
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (JSValue error = modules->mockLoadError(module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, JSPromise::rejectedPromise(realm, error));
    modules->clearCachedMockError(module);
    RELEASE_AND_RETURN(scope, modules->startImport(module, referrerAsyncOrder, Modules::ImportCaller::Host));
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
    if (modules->hasIntactOriginal(module)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        modules->evaluateDueDependencies(module);
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

JSValue requireModule(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> referrer, ReadsExports readsExports)
{
    Modules* modules = modulesForEvaluation(realm, module);
    if (!modules)
        return { };
    modules->stopWhereExportsLeaveCompiledCode(module);
    modules->noteLoadBehindMock(module, true);
    return modules->require(module, referrer.value_or(ModuleID()), Modules::RequiredBy::Program, modules->takeHostRequestRequirerObject(), readsExports);
}

bool hasCommonJSEvaluationStarted(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->data(module).kind == ModuleKind::CommonJs && modules->status(module) > ModuleStatus::Linked;
}

JSValue evaluateForDefaultLoader(JSGlobalObject* realm, ModuleID module)
{
    if (hasCommonJSEvaluationStarted(realm, module)) {
        throwLoaderChangeError(realm, LoaderChange::EvaluateModuleAgain, modulesOf(realm)->table().fullPath(module));
        return { };
    }
    return requireModule(realm, module);
}

void throwCompileOverrideError(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        throwLoaderChangeError(realm, LoaderChange::PassSourceToCompile, modules->table().fullPath(module));
}

bool isInRequireCache(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isInRequireCache(module);
}

std::optional<OptionSet<ModuleObjectTrait>> newModuleObjectTraits(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->newModuleObjectTraits(module) : std::nullopt;
}

void adoptModuleObject(JSGlobalObject* realm, ModuleID module, const ModuleObjects& objects)
{
    modulesOf(realm)->adoptModuleObject(module, objects);
}

void releaseModuleObject(JSGlobalObject* realm, ModuleID module)
{
    modulesOf(realm)->releaseModuleObject(module);
}

JSObject* moduleObjectWithExportsVariable(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->moduleObjectWithExportsVariable(module) : nullptr;
}

void didBindExportsVariable(JSGlobalObject* realm, ModuleID module)
{
    modulesOf(realm)->didBindExportsVariable(module);
}

void didCreateModuleObject(JSGlobalObject* realm, ModuleID module, JSObject* moduleObject)
{
    if (Modules* modules = modulesOf(realm))
        modules->retainModuleObjectFromRequire(module, moduleObject);
}

JSObject* Modules::childModuleObject(ModuleID module)
{
    bool isShown = (isFileOfProgram(module) || data(module).kind == ModuleKind::Data) && !hidesModuleObject(module);
    return isShown ? moduleObjectMadeByRequire(module) : nullptr;
}

JSObject* childModuleObject(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesThatShowWaysBetweenModuleObjects(realm);
    return modules ? modules->childModuleObject(module) : nullptr;
}

void willRequireFromUncompiledCode(JSGlobalObject* realm, ModuleID module, JSObject* requirerModuleObject)
{
    if (Modules* modules = modulesOf(realm))
        modules->recordRequireFromUncompiledCode(module, requirerModuleObject);
}

AbstractModuleRecord* Modules::recordForRequestedModule(ModuleID requested)
{
    SetForScope isInHostRequest(m_currentHostRequest, true);
    return moduleRecord(m_instance.globalObject, requested);
}

void willSetRequireCacheEntry(JSGlobalObject* realm, ModuleID module, JSValue entry)
{
    Modules* modules = modulesOf(realm);
    if (!modules || !entry.toBoolean(realm))
        return;
    String path = modules->table().fullPath(module);
    if (modules->data(module).isOfLibrary()) {
        if (!path.startsWith("node:"_s) && modules->data(module).rejectsRequireCacheOverride())
            throwLoaderChangeError(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, path);
        return;
    }
    throwLoaderChangeError(realm, LoaderChange::ReplaceInRequireCache, path);
}

void willSetBuiltinCacheEntry(JSGlobalObject* realm, ModuleID module, const String& name)
{
    Modules* modules = modulesOf(realm);
    if (modules && modules->data(module).rejectsRequireCacheOverride())
        throwLoaderChangeError(realm, LoaderChange::StoreUnderNameOfModuleInRequireCache, name);
}

void willChangeRequireOverride(JSGlobalObject* realm, RequireOverride location, bool isOverridden)
{
    if (Modules* modules = modulesOf(realm))
        modules->recordRequireOverride(location, isOverridden);
}

void willSetExtensionLoader(JSGlobalObject* realm, StringView extension)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    const ModuleTable& table = modules->table();
    bool hasQueryInDirectory = table.hasQueryInDirectory();
    for (unsigned index = 0; index < table.size(); ++index) {
        ModuleID module(index);
        if (!table.hasFilePath(module) || modules->data(module).isOfLibrary() || !table.pathWithoutQueryEndsWith(module, extension, hasQueryInDirectory))
            continue;
        if (!modules->mayBeEvicted(module)) {
            throwLoaderChangeError(realm, LoaderChange::ReplaceLoaderFunction, "a loader in Module._extensions"_s);
            return;
        }
        modules->delegateToHostUnlessEvaluated(module);
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

bool layExportOverride(JSCell* moduleNamespace, UniquedStringImpl* name, JSValue value, JSCell* owner)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    Modules* modules = modulesOf(object->realm());
    auto scope = DECLARE_THROW_SCOPE(object->realm()->vm());
    std::optional<SlotData> variable = modules->replaceableVariableOf(object->module(), name);
    if (!variable)
        return false;
    RELEASE_AND_RETURN(scope, (modules->layVariableOverride(*variable, owner, value), true));
}

ExportOverrideEnd removeExportOverride(JSCell* moduleNamespace, UniquedStringImpl* name, JSCell* owner, JSCell** ownerAbove)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    Modules* modules = modulesOf(object->realm());
    auto scope = DECLARE_THROW_SCOPE(object->realm()->vm());
    std::optional<SlotData> variable = modules->replaceableVariableOf(object->module(), name);
    if (!variable)
        return ExportOverrideEnd::NotOverridden;
    RELEASE_AND_RETURN(scope, modules->endVariableOverride(*variable, owner, ownerAbove));
}

bool exportFoundByOverride(JSCell* moduleNamespace, UniquedStringImpl* name, JSCell* owner, JSValue& found)
{
    auto* object = uncheckedDowncast<ModuleNamespace>(moduleNamespace);
    Modules* modules = modulesOf(object->realm());
    std::optional<SlotData> variable = modules->replaceableVariableOf(object->module(), name);
    return variable && modules->foundByVariableOverride(*variable, owner, found);
}

void takeOwnersHandedByMocks(JSGlobalObject* realm, const ScopedLambda<void(JSCell* owner, JSValue found)>& functor)
{
    Modules* modules = modulesOf(realm);
    JSArray* handed = modules ? modules->takeOwnersHandedByMocks() : nullptr;
    if (!handed)
        return;
    for (unsigned index = 0; index + 1 < handed->length(); index += 2)
        functor(handed->getDirectIndex(realm, index).asCell(), handed->getDirectIndex(realm, index + 1));
}

bool mayBeMocked(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mayBeMocked(module);
}

bool setMock(JSGlobalObject* realm, ModuleID module, JSObject* mock, JSValue requireReturnValue, bool keepsEarlierMockForImporters)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, mock ? "setMock"_s : "setMock, none"_s);
    if (modules && mock) {
        modules->evictRequiredImporters(module);
        modules->retainFactoryLoadedImporters(module);
        modules->endStaleDetachedLoads(module);
    }
    if (modules && mock && modules->hasIntactOriginal(module))
        return modules->setMockWithIntactOriginal(module, mock, requireReturnValue, keepsEarlierMockForImporters);
    return modules && modules->setActiveMock(module, mock, requireReturnValue, keepsEarlierMockForImporters);
}

bool tryKeepOriginalIntact(JSGlobalObject* realm, ModuleID module, bool onlyIfLoadedInPreload, std::optional<ModuleID> asking, bool evenIfEvaluationFailed)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->tryKeepOriginalIntact(module, onlyIfLoadedInPreload, asking, evenIfEvaluationFailed);
}

bool hasIntactOriginal(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasIntactOriginal(module);
}

void didDisplaceIntactOriginal(JSGlobalObject* realm, ModuleID module, bool evenIfLoadedEarlier)
{
    modulesOf(realm)->didDisplaceIntactOriginal(module, evenIfLoadedEarlier);
}

bool isDisplacedByMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isDisplacedByMock(module);
}

void didBeginTestFile(JSGlobalObject* realm)
{
    VMState& state = realm->vm().aotState();
    state.momentOfTestFile = ++state.momentOfLoads;
    if (Modules* modules = modulesOf(realm))
        modules->didBeginTestFile();
}

JSObject* activeMockForNamespace(JSCell* moduleNamespace)
{
    auto* object = dynamicDowncast<ModuleNamespace>(moduleNamespace);
    return object ? object->activeMock() : nullptr;
}

bool hasActiveMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->activeMock(module) && !modules->hasRemovedMock(module);
}

bool hasUnevaluatedActiveMockImporter(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> asking)
{
    if (!hasActiveMock(realm, module))
        return false;
    Modules* modules = modulesOf(realm);
    return modules->isGraphRunning() && modules->hasUnevaluatedLinkedImporter(module, asking);
}

bool tryKeepActiveMock(JSGlobalObject* realm, ModuleID module, bool remainsOriginal, bool keepsCurrentTestFileLoads)
{
    if (!hasActiveMock(realm, module))
        return false;
    Modules* modules = modulesOf(realm);
    if (remainsOriginal || (keepsCurrentTestFileLoads && modules->wasLoadedUnderMockInPlaceByThisTestFile(module))) {
        modules->noteCallOfMock(module, true);
        modules->releaseImportersOf(module, Modules::ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule);
        return true;
    }
    modules->forgetWhoLoadedItUnderMockInPlace(module);
    modules->releaseImportersOf(module, Modules::ReleasedImporters::All);
    return true;
}

ActiveMockExports retainedActiveMockExports(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->retainedActiveMockExports(module) : ActiveMockExports { };
}

bool wasRequiredUnderActiveMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasRequiredUnderActiveMock(module);
}

bool wasRequiredUnderPreloadMock(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasRequiredUnderPreloadMock(file);
}

bool hasMockSlot(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->table().mockSlot(module);
}

void mockExport(JSGlobalObject* realm, ModuleID module, UniquedStringImpl* name, JSValue value)
{
    if (Modules* modules = modulesOf(realm))
        modules->tryReplaceExport(modules->moduleWithExportsOf(module), name, value);
}

bool hasImportStartedLoading(JSGlobalObject* realm, ModuleID moduleOrFacade)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return true;
    ModuleID file = modules->table().moduleBehindFacade(moduleOrFacade).value_or(moduleOrFacade);
    return modules->hasStartedLoading(modules->table().facadeOf(file).value_or(file));
}

bool wasLoadedByImport(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->wasLoadedByImport(module);
}

bool hasStartedLoading(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return !modules || modules->hasStartedLoading(module);
}

bool hasOnlyBeenLinked(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasOnlyBeenLinked(module);
}

std::optional<ModuleID> findVariantToMock(JSGlobalObject* realm, StringView path, std::optional<ModuleID> file)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return file;
    modules->startLoadingModulesIndividually();
    return modules->findVariantToMock(path, file);
}

Vector<ModuleID, 2> importAttributeVariants(JSGlobalObject* realm, StringView path)
{
    Vector<ModuleID, 2> forms;
    if (Modules* modules = modulesOf(realm))
        forms = modules->table().importAttributeVariants(path);
    return forms;
}

bool wasDisplacedDuringEvaluation(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    ModuleRecord* record = modules ? modules->displacedRecord(module) : nullptr;
    return record && !record->isSCCEvaluated();
}

bool isBeingEvaluated(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isBeingEvaluated(module);
}

bool wasRegisteredByFailedLoad(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasRegisteredByFailedLoad(module);
}

bool wasLinkedByFailedGraph(JSGlobalObject* realm, ModuleID module, bool byEarlierTestFile)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLinkedByFailedGraph(module, byEarlierTestFile);
}

bool isLinkedForPendingImporter(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> asking)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isLinkedForPendingImporter(module, asking);
}

bool wasLinkedBeforeLinkFailure(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLinkedBeforeLinkFailure(module);
}

bool wasDisplacedAfterFailure(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasDisplacedAfterFailure(module);
}

bool isFailedLoadRoot(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isFailedLoadRoot(module);
}

void clearLoadedBeforeTestFile(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->clearLoadedBeforeTestFile(module);
}

void markEntryOwnedByTestFile(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->markEntryOwnedByTestFile(module);
}

bool hasFailedESModule(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasFailedESModule(module);
}

void releaseFailedESModule(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->releaseFailedESModule(module);
}

void markVariantEvictedByMock(JSGlobalObject* realm, ModuleID form)
{
    if (Modules* modules = modulesOf(realm))
        modules->markVariantEvictedByMock(form);
}

bool wasVariantEvictedByMock(JSGlobalObject* realm, ModuleID form)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasVariantEvictedByMock(form);
}

bool wasVariantRegisteredByPreload(JSGlobalObject* realm, ModuleID form)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasVariantRegisteredByPreload(form);
}

void scheduleVariantRestore(JSGlobalObject* realm, ModuleID restoredVariant, const Vector<ModuleID, 2>& forms)
{
    if (Modules* modules = modulesOf(realm))
        modules->scheduleVariantRestore(restoredVariant, forms);
}

bool wasVariantFetchedByTestFile(JSGlobalObject* realm, ModuleID form)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasVariantFetchedByTestFile(form);
}

void Modules::markImportersEvictedWithVariant(ModuleID form)
{
    listImportersAndRequirers();
    Vector<uint32_t, 8> work { form.index() };
    if (std::optional<ModuleID> facade = m_table.facadeOf(form))
        work.append(facade->index());
    while (!work.isEmpty()) {
        for (uint32_t importer : m_importersByStatement[work.takeLast()]) {
            if (status(ModuleID(importer)) <= ModuleStatus::Linked || m_loadedInPreload.get(importer) || m_importersEvictedWithVariants.set(importer))
                continue;
            work.append(importer);
        }
    }
}

void Modules::recordStaticImportRequests(ModuleID importer)
{
    for (const ModuleRequestData& request : m_table.imports(importer)) {
        if (!request.isHoisted && !request.isDeferred)
            recordVariantRequest(moduleLoadedBy(request));
    }
}

void Modules::recordVariantsRegisteredByPreload()
{
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID form(index);
        if (hasStartedLoading(form) && (activeMock(form) || !wasVariantEvictedByMock(form)))
            m_variantsRegisteredByPreload.set(index);
    }
}

void Modules::runScheduledVariantRestores()
{
    for (auto& [restoredVariant, forms] : std::exchange(m_scheduledVariantRestores, { })) {
        for (ModuleID form : forms)
            m_variantsEvictedByMock.set(form.index(), form != restoredVariant);
    }
}

void didRemoveEntryAfterTestFile(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRemoveEntryAfterTestFile(module);
}

void didRestoreEntryForPreload(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRestoreEntryForPreload(module);
}

void didRemoveEntryForMock(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRemoveEntryForMock(module);
}

void markFetchOrphaned(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->markFetchOrphaned(module);
}

bool hasOrphanedFetch(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasOrphanedFetch(module);
}

bool isEntryOwnedByTestFile(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isEntryOwnedByTestFile(module);
}

bool wasLoadedBeforeTestFile(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLoadedBeforeTestFile(module);
}

void didFailImportOriginalFactory(JSGlobalObject* realm, ModuleID module, JSValue error)
{
    if (Modules* modules = modulesOf(realm))
        modules->recordFactoryFailure(module, error);
}

bool hasInFlightOriginalRequest(JSGlobalObject* realm, ModuleID asked)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasInFlightOriginalRequest(asked);
}

void markMockPassthrough(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->markMockPassthrough(module);
}

void markFactoryAbandoned(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->markFactoryAbandoned(module);
}

void didFinishTests(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->didFinishTests();
}

bool hasDeferredFetchToEvict(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasDeferredFetchToEvict();
}

bool hasMockImportsAwaitingSource(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasMockImportsAwaitingSource();
}

void deferMock(JSGlobalObject* realm, ModuleID module, FactoryFailurePolicy failurePolicy)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, failurePolicy == FactoryFailurePolicy::RetryFactory ? "deferMock, asks again"_s : "deferMock"_s);
    if (!modules)
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    if (modules->isMockLingering(module))
        modules->removeLingeringMocks();
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, modules->deferMock(module, failurePolicy));
}

JSValue exportsBeforeMock(JSGlobalObject* realm, ModuleID module, bool evenIfEvaluationFailed, bool* didExportReadThrow)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "exportsBeforeMock"_s);
    return modules ? modules->exportsBeforeMock(module, evenIfEvaluationFailed, Modules::OwedBelowOriginal::IsPaid, didExportReadThrow) : JSValue();
}

bool isPatchedByMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isPatchedByMock(module);
}

bool wasPatchedAfterEvaluation(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasPatchedAfterEvaluation(module);
}

void didRemoveActiveMockEntry(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRemoveActiveMockEntry(module);
}

bool wasActiveMockEntryRemoved(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasActiveMockEntryRemoved(module);
}

LoadedBy activeMockReloadedBy(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->activeMockReloadedBy(module) : LoadedBy::Nobody;
}

LoadedBy fetchedBy(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->fetchedBy(module) : LoadedBy::Nobody;
}

LoadedBy evaluatedBy(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->evaluatedBy(module) : LoadedBy::Nobody;
}

JSValue exportsBeforePatch(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->exportsBeforePatch(file) : JSValue();
}

void retainOriginalExports(JSGlobalObject* realm, ModuleID file)
{
    if (Modules* modules = modulesOf(realm))
        modules->retainOriginalExports(file);
}

void stashActualKeyExports(JSGlobalObject* realm, ModuleID file)
{
    if (Modules* modules = modulesOf(realm))
        modules->stashActualKeyExports(file);
}

void restoreActualKeyExports(JSGlobalObject* realm, ModuleID file)
{
    if (Modules* modules = modulesOf(realm))
        modules->restoreActualKeyExports(file);
}

JSObject* retainedOriginalModuleObject(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->retainedOriginalModuleObject(file) : nullptr;
}

void clearRetainedOriginalExports(JSGlobalObject* realm, ModuleID file)
{
    if (Modules* modules = modulesOf(realm))
        modules->clearRetainedOriginalExports(file);
}

bool isUnderActualKey(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isUnderActualKey(file);
}

void setIsUnderActualKey(JSGlobalObject* realm, ModuleID file, bool ran)
{
    if (Modules* modules = modulesOf(realm))
        modules->setIsUnderActualKey(file, ran);
}

bool wasOriginalRestoredToOwnKey(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasOriginalRestoredToOwnKey(file);
}

void setWasOriginalRestoredToOwnKey(JSGlobalObject* realm, ModuleID file, bool cameBack)
{
    if (Modules* modules = modulesOf(realm))
        modules->setWasOriginalRestoredToOwnKey(file, cameBack);
}

JSValue revalidateRetainedOriginalExports(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->revalidateRetainedOriginalExports(file) : JSValue();
}

JSValue retainedOriginalExports(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->retainedOriginalExports(file) : JSValue();
}

JSObject* namespaceBeforeMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "namespaceBeforeMock"_s);
    return modules ? modules->namespaceBeforeMock(module) : nullptr;
}

void didCallHostImport(JSGlobalObject* realm, ModuleID imported, JSPromise* promise)
{
    if (Modules* modules = modulesOf(realm))
        modules->recordImportForOriginalBodies(imported, std::nullopt, promise);
}

JSPromise* pendingOriginalEvaluation(JSGlobalObject* realm, ModuleID module, std::optional<ModuleID> factoryCallerLoad)
{
    Modules* modules = modulesOf(realm);
    if (modules && modules->bodySetAsideCallsImportNow(module))
        return modules->evaluationOnceBodySetAsideHasGoneOn(module, factoryCallerLoad);
    return modules && !modules->bodySetAsideWaitsForItsMock(module, factoryCallerLoad) ? modules->displacedRecordEvaluation(module) : nullptr;
}

JSValue mockedModuleEvaluationError(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return { };
    if (ModuleRecord* record = modules->displacedRecord(module); record && record->evaluationError())
        return record->evaluationError();
    return modules->errorOfModuleThatRanBesideMock(module);
}

bool runMocksInGraph(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->runMocksInFetchedGraph(module);
}

void didRemoveMock(JSGlobalObject* realm, ModuleID module, RetainedOriginalPolicy original, bool wasInstalledOnPreloadMock, bool restoresDisplacedModule)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "didRemoveMock"_s);
    modules->didRestoreEntryAfterMock(module);
    modules->discardDetachedLoadErrorsReaching(module);
    modules->keepMockForPendingImports(module, Modules::MockChange::Removed);
    modules->noteDeedOnMockUnderCallsOfImport(module, Modules::DeedOnMock::Ends);
    modules->forgetLinksInRefusedLoadsThatCameTo(module);
    if (original == RetainedOriginalPolicy::DiscardWithMock && modules->isPatchedByMock(module))
        modules->clearRetainedOriginalExports(module);
    modules->didMockEvictDataModule(module);
    if (modules->tryMarkMockLingering(module, wasInstalledOnPreloadMock))
        return;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    bool isBack = wasInstalledOnPreloadMock && modules->tryRestorePreloadMock(module);
    RETURN_IF_EXCEPTION(scope, void());
    if (isBack)
        return;
    modules->forgetCallOfMock(module);
    bool factoryWasCalled = modules->takeWhetherFactoryWasCalled(module);
    bool wasLoadedAsMock = modules->activeMock(module) && !modules->hasRemovedMock(module);
    if (wasLoadedAsMock || modules->isPatchedByMock(module))
        modules->noteEndOfLoadedMock(module);
    else if (!factoryWasCalled)
        modules->noteEndOfMockWhoseFactoryWasNeverCalled(module);
    if (!wasLoadedAsMock && !modules->isPatchedByMock(module) && !modules->hasIntactOriginal(module))
        modules->noteEndOfMockThatOnlyWaited(module);
    if (wasLoadedAsMock || modules->isPatchedByMock(module) || modules->hasMockOfLibraryModule(module))
        modules->releaseImportersOf(module, Modules::ReleasedImporters::All);
    if (modules->hasIntactOriginal(module)) {
        if (wasLoadedAsMock)
            modules->letGoOfModuleLoadedAsMock(module, restoresDisplacedModule);
        modules->removeMockWithIntactOriginal(module);
        RETURN_IF_EXCEPTION(scope, void());
        modules->clearLoadErrorsForMock(module);
        return;
    }
    bool isLibraryModule = modules->data(module).isOfLibrary() || !modules->table().hasFilePath(module);
    if (isLibraryModule && modules->releaseLibraryMockOnLoad(module)) {
        modules->forgetMockThatWaits(module);
        modules->clearLoadErrorsForMock(module);
        return;
    }
    bool stoodInPlace = modules->activeMock(module);
    if (stoodInPlace) {
        modules->noteEndOfMockOverFileThatNeverRan(module);
        modules->bringBackMockOfPreloadThatWasPatched(module);
        RETURN_IF_EXCEPTION(scope, void());
        modules->noteWhereOriginalLiesBehindEndOfMock(module);
        modules->releaseMockAtNextLoad(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    modules->forgetMockThatWaits(module);
    modules->clearLoadErrorsForMock(module);
    modules->settleFailedImportsUnder(module, Modules::FailedImports::Evict);
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

void willMockModule(JSGlobalObject* realm, ModuleID module, JSValue requireReturnValue, bool hasCommonJSExports, bool hasFactory)
{
    Modules* found = modulesOf(realm);
    if (!found)
        return;
    MockDoor door(found, module, "willMockModule"_s);
    Modules& modules = *found;
    modules.leaveImportersAbove(module);
    JSObject* exports = requireReturnValue.isObject() ? asObject(requireReturnValue) : constructEmptyObject(realm);
    modules.setMockHasCommonJSExports(module, hasCommonJSExports);
    modules.setMockHasFactory(module, hasFactory);
    if (modules.data(module).isOfLibrary() || !modules.table().hasFilePath(module)) {
        auto scope = DECLARE_THROW_SCOPE(realm->vm());
        modules.releaseRemovedMock(module);
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
            modules.saveExportsBeforePatch(module);
        if (!comesAheadOfLoad && isFirstMock)
            modules.releaseImportersOf(module, Modules::ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule);
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
        modules.saveExportsBeforePatch(module);
    if (hasRun && isFirstMock)
        modules.releaseImportersOf(module, Modules::ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule);
    modules.setCommonJSExports(module, requireReturnValue);
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
    RELEASE_AND_RETURN(scope, modules->answerFirstRequireBehindMock(module));
}

void didStopInstallingMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    modules->takeWhatMockAdds(module, false);
    modules->didInstallMock();
}

bool hasESModuleMarker(JSCell* moduleNamespace)
{
    return uncheckedDowncast<ModuleNamespace>(moduleNamespace)->showsESModuleMarker();
}

void setESModuleMarker(JSCell* moduleNamespace, bool hasMarker)
{
    uncheckedDowncast<ModuleNamespace>(moduleNamespace)->storeESModuleMarker(hasMarker);
}

bool ModuleNamespace::isDeferredMarkerReady() const
{
    ModuleStatus status = modules().status(m_module);
    return status != ModuleStatus::Evaluating && status != ModuleStatus::EvaluatingAsync && status != ModuleStatus::Failed;
}

void ModuleNamespace::storeESModuleMarker(bool hasMarker)
{
    if (m_isMarkerDeferred && isDeferredMarkerReady())
        m_isMarkerDeferred = false;
    m_hasESModuleMarker = hasMarker;
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
    return modules().isMockedThroughStarExports(m_module);
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
    if (keepsToModule()) [[unlikely]] {
        VM& vm = globalObject->vm();
        if (status() == Status::Evaluated) {
            if (JSValue error = evaluationError()) {
                throwException(globalObject, scope, error);
                return invalidIndex;
            }
            return index;
        }
        setStatus(Status::Evaluated);
        setCycleRoot(vm, this);
        modules().evaluateModuleItself(m_module);
        Exception* thrown = scope.exception();
        if (!thrown) [[likely]]
            return index;
        if (!vm.isTerminationException(thrown))
            setEvaluationError(vm, thrown->value());
        return invalidIndex;
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
    recordTerminationAfterEvaluation(globalObject->vm(), exception);
    return invalidIndex;
}

void ModuleRecord::recordTerminationAfterEvaluation(VM& vm, Exception* exception)
{
    if (!vm.isTerminationException(exception) || status() != Status::Evaluated || evaluationError())
        return;
    setEvaluationError(vm, exception->value());
    modules().didChangeStatusInRecord(m_module);
}

void ModuleRecord::willLoadRequests(ModuleGraphLoadingState& load)
{
    countEvent(vm(), RuntimeCounterID::RecordsForStatesOfLoads);
    modules().appendRequestsForLoader(m_module, this, load);
}

void ModuleRecord::countLoaderStep()
{
    modules().countLoaderStep();
}

void ModuleRecord::didLoad()
{
    if (keepsToModule()) [[unlikely]]
        return;
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

std::optional<ModuleID> ModuleRecord::findEvaluatingCommonJSModule(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    return modules ? modules->findEvaluatingCommonJSModule(key) : std::nullopt;
}

bool ModuleRecord::wasRequestedByEnclosingLoad(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    return modules && modules->wasRequestedByEnclosingLoad(key);
}

bool ModuleRecord::tryDeferLinkStep(JSGlobalObject* globalObject, const VM::SynchronousModuleTask& step)
{
    if (step.task != InternalMicrotask::ModuleLoadLinkEvaluateSettled && step.task != InternalMicrotask::DynamicImportLoadSettled)
        return false;
    Modules* modules = modulesOf(globalObject);
    return modules && modules->tryDeferLinkStep(step);
}

bool ModuleRecord::isModuleBeingTransferred(JSGlobalObject* globalObject, const Identifier& key)
{
    Modules* modules = modulesOf(globalObject);
    return modules && modules->isModuleBeingTransferred(key);
}

bool ModuleRecord::isBlockedOnModuleTransfer(JSGlobalObject* globalObject, const Identifier& key, bool& shouldRequeueStep)
{
    Modules* modules = modulesOf(globalObject);
    return modules && modules->isBlockedOnModuleTransfer(key, shouldRequeueStep);
}

bool Modules::isBlockedOnModuleTransfer(const Identifier& key, bool& shouldRequeueStep)
{
    std::optional<ModuleID> asked = findByKey(key);
    if (!asked)
        return false;
    if (!isCommonJSModuleOrFacade(*asked)) {
        ModuleRecord* record = m_moduleBeingTransferred && hasNotBegun(*asked) ? m_records.get(*asked) : nullptr;
        if (!record)
            return false;
        shouldRequeueStep = record->markStepQueued(m_activeSynchronousLoad);
        return true;
    }
    ModuleStatus statusOfFile = status(fileOf(*asked));
    if (statusOfFile == ModuleStatus::Evaluated || (statusOfFile != ModuleStatus::Failed && !m_moduleBeingTransferred))
        return false;
    ModuleRecord* record = m_records.get(*asked);
    if (!record)
        return false;
    shouldRequeueStep = record->markStepQueued(m_activeSynchronousLoad);
    return true;
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
    modules->countLoaderStep();
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

void ModuleRecord::didDiscardModuleError(JSGlobalObject* globalObject, const Identifier& key)
{
    if (Modules* modules = modulesOf(globalObject))
        modules->didDiscardModuleError(key);
}

void ModuleRecord::didReplaceRecordInRegistry()
{
    modules().recaptureExports(m_module);
}

void ModuleRecord::willBeLoadedByImport()
{
    modules().willBeLoadedByImport(m_module);
}

bool ModuleRecord::isLoadedByLoader()
{
    return modules().isLoadedByLoader(m_module);
}

bool ModuleRecord::importsEvaluatingCommonJSModule()
{
    return modules().importsEvaluatingCommonJSModule(m_module);
}

bool ModuleRecord::takeMainModuleLoadStep()
{
    return !requestedModules().isEmpty() && modules().takeMainModuleLoadStep(m_module);
}

bool ModuleRecord::takeMainPromiseSplit()
{
    return !requestedModules().isEmpty() && modules().takeMainPromiseSplit(m_module);
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

void ModuleRecord::adoptStatusIfNeeded()
{
    if (!m_ownsStatus)
        modules().ensureRecord(m_module);
}

bool ModuleRecord::isEvaluatingCommonJSModule()
{
    return modules().isCommonJSModuleOrFacade(m_module) && modules().isImportedByLoader(m_module) && modules().status(m_module) == ModuleStatus::Evaluating;
}

BitVector ModuleRecord::willRefetchFromHost(JSGlobalObject* globalObject)
{
    BitVector savedHostRequests;
    if (Modules* modules = modulesOf(globalObject)) {
        modules->pushHostRequest(true);
        savedHostRequests.set(0);
    }
    return savedHostRequests;
}

void ModuleRecord::didRefetchFromHost(JSGlobalObject* globalObject, const BitVector& savedHostRequests)
{
    if (savedHostRequests.get(0))
        modulesOf(globalObject)->popHostRequest();
}

uint8_t ModuleRecord::willLoadImportedModule(const Identifier& key)
{
    bool wasWalkingRequests = modules().setIsWalkingRequests(!m_compiledRequests.isEmpty());
    modules().pushHostRequest(true);
    bool wasStatementHostRequest = modules().setIsStatementHostRequest(true);
    if (m_hasUnresolvableRequest) [[unlikely]]
        modules().keepFailureOfRequest(m_module, this, key);
    return static_cast<uint8_t>(wasWalkingRequests) << 1 | static_cast<uint8_t>(wasStatementHostRequest) << 2;
}

void ModuleRecord::didLoadImportedModule(uint8_t flagsBefore)
{
    modules().popHostRequest();
    modules().setIsWalkingRequests(flagsBefore & 2);
    modules().setIsStatementHostRequest(flagsBefore & 4);
}

uint8_t ModuleRecord::willLoadForUncompiledReferrer(JSGlobalObject* globalObject)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules)
        return 0;
    bool wasWalkingRequests = modules->setIsWalkingRequests(false);
    modules->pushHostRequest(false);
    bool wasStatementHostRequest = modules->setIsStatementHostRequest(false);
    constexpr uint8_t didPushHostRequest = 1;
    return didPushHostRequest | static_cast<uint8_t>(wasWalkingRequests) << 1 | static_cast<uint8_t>(wasStatementHostRequest) << 2;
}

void ModuleRecord::didLoadForUncompiledReferrer(JSGlobalObject* globalObject, uint8_t flagsBefore)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || !(flagsBefore & 1))
        return;
    modules->popHostRequest();
    modules->setIsWalkingRequests(flagsBefore & 2);
    modules->setIsStatementHostRequest(flagsBefore & 4);
}

void ModuleRecord::checkAccessFromUncompiledReferrer(JSGlobalObject* globalObject, const Identifier& key, const Identifier& requesterKey)
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
    ModuleRequesterScope requesterScope(globalObject, modules->requesterForRegistryKey(requesterKey.isSymbol() ? emptyString() : requesterKey.string()));
    checkModuleAccess(globalObject, *found, LoadKind::Import);
}

void ModuleRecord::checkAccessOnRegistryHit(JSGlobalObject* globalObject, const Identifier& key, const String& requesterPath)
{
    Modules* modules = modulesOf(globalObject);
    if (!modules || key.isSymbol())
        return;
    std::optional<ModuleID> found = modules->table().findByPath(key.string());
    if (!found || modules->isRootLoadedByCompiledCode(*found))
        return;
    ModuleRequesterScope requesterScope(globalObject, modules->requesterForRegistryKey(requesterPath));
    checkModuleAccess(globalObject, *found, LoadKind::Import);
}

AbstractModuleRecord* ModuleRecord::nextRequestedRecord(JSGlobalObject*, const Identifier& key)
{
    for (unsigned index = m_nextCompiledRequest; index < m_compiledRequests.size(); ++index) {
        if (m_compiledRequests[index].first != key.impl())
            continue;
        m_nextCompiledRequest = index + 1;
        return modules().recordForRequestedModule(m_compiledRequests[index].second);
    }
    return nullptr;
}

AbstractModuleRecord* ModuleRecord::findRequestedRecord(const Identifier& key)
{
    for (unsigned index = m_nextCompiledRequest; index--;) {
        if (m_compiledRequests[index].first == key.impl())
            return modules().recordForRequestedModule(m_compiledRequests[index].second);
    }
    for (unsigned index = m_nextCompiledRequest; index < m_compiledRequests.size(); ++index) {
        if (m_compiledRequests[index].first == key.impl())
            return modules().recordForRequestedModule(m_compiledRequests[index].second);
    }
    return nullptr;
}

bool ModuleRecord::tryDeferFetchFulfillment(JSPromise* fetched, AbstractModuleRecord* requested)
{
    auto* compiled = dynamicDowncast<ModuleRecord>(requested);
    return compiled && modules().tryDeferFetchFulfillment(fetched, compiled, m_module);
}

void ModuleRecord::generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords& visited)
{
    modules().captureSyntheticExports(m_module, visited);
}

unsigned ModuleRecord::innerModuleLinking(JSGlobalObject* globalObject, ModuleLinkingStack& stack, unsigned index, RefPtr<ScriptFetcher> scriptFetcher)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (keepsToModule()) [[unlikely]] {
        if (status() == Status::New || status() == Status::Unlinked)
            setStatus(Status::Linked);
        return index;
    }
    LinkContext context { stack, index, WTF::move(scriptFetcher) };
    modules().resumeDeferredJobs();
    modules().didLoadGraph(m_module);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    if (!stack.isEmpty() && modules().someMockThatEndedWaits()) [[unlikely]] {
        modules().releaseRemovedMocksFrom(m_module);
        RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    }
    {
        auto catching = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        modules().finishLink(m_module, context);
        Exception* thrown = catching.exception();
        if (thrown && status() == Status::Evaluated && evaluationError() == thrown->value() && catching.tryClearException()) [[unlikely]]
            return index;
    }
    if (!scope.exception() && status() == Status::Unlinked && (!modules().data(m_module).needsLink() || modules().status(m_module) != ModuleStatus::Unlinked))
        setStatus(Status::Linked);
    RETURN_IF_EXCEPTION_WITH_TRAPS_DEFERRED(scope, invalidIndex);
    return context.index;
}

void ModuleRecord::execute(JSGlobalObject*, JSPromise* capability)
{
    RELEASE_ASSERT(!capability);
    modules().willRunBodyOfRecord(m_module, this);
    modules().callBodyOfModuleEnteredByLoader(m_module);
}

void ModuleRecord::executeAsync(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(status() == Status::Evaluating || status() == Status::EvaluatingAsync);
    ASSERT(hasTLA());

    modules().willRunBodyOfRecord(m_module, this);
    JSValue result = modules().callBodyOfModuleEnteredByLoader(m_module);
    RETURN_IF_EXCEPTION(scope, void());
    auto* promise = dynamicDowncast<JSPromise>(result);
    if (!promise) {
        promise = JSPromise::resolvedPromise(globalObject, result);
        RETURN_IF_EXCEPTION(scope, void());
    }
    promise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::AsyncModuleExecutionDone, nullptr, this);
}

bool ModuleRecord::isReplacedByMock() const
{
    return !keepsToModule() && modules().displacedRecord(m_module) != this && (modules().isMocked(m_module) || modules().activeMock(m_module)) && !modules().isPatchedByMock(m_module);
}

bool ModuleRecord::hasAnyActiveMock() const
{
    return modules().hasAnyActiveMock();
}

bool ModuleRecord::mayOutlive(AbstractModuleRecord* other) const
{
    return other != this && modules().someRetainedRecordHasLeft();
}

auto ModuleRecord::storedResolution(UniquedStringImpl* exportName) -> const Resolution*
{
    Modules::StoredResolutions* answers = modules().answersKept();
    if (!answers || answers->resolutions.isEmpty()) [[likely]]
        return nullptr;
    auto found = answers->resolutions.find({ this, exportName });
    return found == answers->resolutions.end() ? nullptr : &found->value;
}

void ModuleRecord::storeResolution(UniquedStringImpl* exportName, const Resolution& resolution)
{
    if (Modules::StoredResolutions* answers = modules().answersKept())
        answers->resolutions.add({ this, exportName }, resolution);
}

void ModuleRecord::appendDynamicStarTargets(StarExportTargets& targets)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    Modules& modules = this->modules();
    if (!modules.data(m_module).hasDynamicStarExport() || (modules.standsForWholeModule(m_module) && !keepsToModule()))
        return;
    for (ModuleRequestData request : modules.table().dynamicStars(m_module)) {
        AbstractModuleRecord* target = recordBehindStar(request.targetModule());
        RETURN_IF_EXCEPTION(scope, void());
        targets.append(target);
    }
}

void ModuleRecord::appendDynamicStarTargets(const Identifier& exportName, StarExportTargets& targets)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    Modules& modules = this->modules();
    if (!modules.data(m_module).hasDynamicStarExport() || (modules.standsForWholeModule(m_module) && !keepsToModule()))
        return;
    const Modules::StarTargetsByName& stars = modules.starTargetsByName(m_module);
    stars.visitCandidates(exportName.impl(), [&](unsigned place) {
        if (!stars.targets[place].isFollowed)
            return true;
        AbstractModuleRecord* target = recordBehindStar(stars.targets[place].module);
        if (scope.exception()) [[unlikely]]
            return false;
        targets.append(target);
        return true;
    });
}

AbstractModuleRecord* ModuleRecord::recordBehindStar(ModuleID asked)
{
    Modules& modules = this->modules();
    for (auto& [module, original] : modules.originalsBeingAsked()) {
        if (module == asked)
            return original;
    }
    bool isMocked = modules.activeMock(asked) || modules.isMocked(asked);
    bool seesModuleItself = isMocked && !modules.seesMockOf(asked, m_module);
    return seesModuleItself ? modules.ensureDisplacedRecord(asked) : modules.recordForResolution(asked);
}

bool ModuleRecord::appendStarsThatMayProvide(const Identifier& exportName, unsigned index, StarExportTargets& targets)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    Modules& modules = this->modules();
    if (!modules.data(m_module).hasDynamicStarExport() || (modules.standsForWholeModule(m_module) && !keepsToModule()))
        return false;
    const ModuleExportData& entry = modules.table().exports(m_module)[index];
    const Vector<Modules::PlaceOfExport, 1>& providers = modules.starExportProviders(m_module)[index];
    std::optional<Modules::PlaceOfExport> firstProvider = providers.isEmpty() ? std::nullopt : std::optional(providers.first());
    if (!firstProvider)
        return false;
    auto isFirstProvider = [&](ModuleID asked) {
        return asked == firstProvider->module;
    };
    auto providesBinding = [&](ModuleID asked) {
        if (isFirstProvider(asked))
            return true;
        std::optional<unsigned> targetIndex = modules.data(asked).hasExportTable() ? modules.table().findExport(asked, exportName.impl()) : std::nullopt;
        return targetIndex && exportSameBinding(modules.table(), entry, modules.table().exports(asked)[*targetIndex]);
    };
    const Modules::StarTargetsByName& stars = modules.starTargetsByName(m_module);
    bool someStarProvidesBinding = false;
    stars.visitCandidates(exportName.impl(), [&](unsigned place) {
        someStarProvidesBinding = providesBinding(stars.targets[place].module);
        return !someStarProvidesBinding;
    });
    if (!someStarProvidesBinding)
        return false;
    stars.visitCandidates(exportName.impl(), [&](unsigned place) {
        Modules::StarTarget star = stars.targets[place];
        if (!star.isFollowed && !providesBinding(star.module))
            return true;
        AbstractModuleRecord* target = recordBehindStar(star.module);
        if (scope.exception()) [[unlikely]]
            return false;
        targets.append(target);
        return true;
    });
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

static bool takesNamesFromValue(const ModuleData& data)
{
    return data.capturesExports() || data.kind == ModuleKind::CommonJs;
}

bool ModuleRecord::takesNamesFromModuleItself()
{
    return takesNamesFromValue(modules().data(m_module)) && modules().displacedRecord(m_module) == this;
}

bool ModuleRecord::resolvesPerReexporter() const
{
    return modules().hasRemovedMockDuringStarCollection(m_module) && takesNamesFromValue(modules().data(m_module));
}

bool ModuleRecord::hasRemovedMockInUse() const
{
    return modules().hasRemovedMockInUseFor(m_module);
}

bool ModuleRecord::isNamedByEndedMockInFrontOfItsStar(JSGlobalObject* globalObject, const Identifier& exportName, unsigned index)
{
    Modules& modules = this->modules();
    if (!modules.keepsSomeEndedMock()) [[likely]]
        return false;
    const ModuleExportData& entry = modules.table().exports(m_module)[index];
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    m_resolveSet.append(exportName.impl());
    auto removeFromResolveSet = makeScopeExit([&] {
        m_resolveSet.removeLast();
    });
    auto providesSameBinding = [&](ModuleRequestData request) {
        ModuleID target = request.targetModule();
        std::optional<unsigned> targetIndex = request.isRequire || !modules.data(target).hasExportTable() ? std::nullopt : modules.table().findExport(target, exportName.impl());
        return targetIndex && exportSameBinding(modules.table(), entry, modules.table().exports(target)[*targetIndex]);
    };
    auto isNamedInFront = [&](auto&& requests) {
        for (ModuleRequestData request : requests) {
            if (providesSameBinding(request))
                return false;
            if (!request.isDynamicStarExport)
                continue;
            AbstractModuleRecord* record = modules.recordForResolution(request.targetModule());
            RETURN_IF_EXCEPTION(scope, false);
            Resolution inFront = record->resolveExportViaReexporter(globalObject, exportName, this);
            RETURN_IF_EXCEPTION(scope, false);
            if (inFront.type == Resolution::Type::Resolved && inFront.isFromRemovedMock)
                return true;
        }
        return false;
    };
    DynamicStars stars = modules.table().dynamicStars(m_module);
    bool writerIsAmongStarsAsWritten = false;
    if (stars.placesAreWritten()) [[unlikely]] {
        for (ModuleRequestData request : stars)
            writerIsAmongStarsAsWritten |= providesSameBinding(request);
    }
    RELEASE_AND_RETURN(scope, writerIsAmongStarsAsWritten ? isNamedInFront(stars) : isNamedInFront(modules.table().requests(m_module)));
}

static AbstractModuleRecord* originalFromTextInModules(JSGlobalObject* globalObject, Modules& modules, ModuleID module)
{
    String path = modulePath(modules.table().moduleBehindFacade(module).value_or(module));
    if (path.isEmpty())
        return nullptr;
    ModuleRegistryEntry* entry = globalObject->moduleLoader()->registryEntry(Identifier::fromString(globalObject->vm(), makeString(path, path.contains('?') ? '&' : '?', "actual"_s)));
    auto* record = entry ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
    return record && !record->inherits<ModuleRecord>() && record->status() == CyclicModuleRecord::Status::Evaluated && !record->evaluationError() ? record : nullptr;
}

static JSModuleNamespaceObject* namespaceOfOriginalFromTextInModules(JSGlobalObject* globalObject, Modules& modules, ModuleID module)
{
    for (auto& [asked, original] : modules.originalsBeingAsked()) {
        if (asked == module)
            return nullptr;
    }
    AbstractModuleRecord* record = originalFromTextInModules(globalObject, modules, module);
    if (!record)
        return nullptr;
    modules.originalsBeingAsked().append({ module, modules.ensureDisplacedRecord(module) });
    JSModuleNamespaceObject* shown = record->getModuleNamespace(globalObject);
    modules.originalsBeingAsked().removeLast();
    modules.originalsAnswered().removeAllMatching([&](const Modules::OriginalAnswered& each) {
        return each.depth > modules.originalsBeingAsked().size();
    });
    return shown;
}

bool ModuleRecord::originalShows(JSGlobalObject* globalObject, const Identifier& exportName, std::span<const Asker> askers)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    bool mayComeOfEndedMock = modules().isNamedByEndedMockBelow(m_module, exportName);
    RETURN_IF_EXCEPTION(scope, false);
    JSModuleNamespaceObject* loaded = mayComeOfEndedMock ? nullptr : namespaceOfOriginalFromTextInModules(globalObject, modules(), m_module);
    RETURN_IF_EXCEPTION(scope, false);
    bool isLackedByLoadedOriginal = loaded && !loaded->hasProperty(globalObject, exportName);
    RETURN_IF_EXCEPTION(scope, false);
    if (isLackedByLoadedOriginal)
        return false;
    auto& answered = modules().originalsAnswered();
    unsigned depth = modules().originalsBeingAsked().size();
    for (const Modules::OriginalAnswered& each : answered) {
        if (each.depth == depth && each.module == m_module && each.name == exportName.impl())
            return each.isShown;
    }
    Modules::AsksAnew asksAnew(modules());
    m_resolveSet.append(exportName.impl());
    for (const Asker& asker : askers)
        asker.record->m_resolveSet.append(asker.name);
    ModuleRecord* original = modules().ensureDisplacedRecord(m_module);
    modules().originalsBeingAsked().append({ m_module, original });
    Resolution shown = original->resolveExport(globalObject, exportName);
    bool isShown = !scope.exception() && shown.type == Resolution::Type::Resolved;
    if (auto* provider = isShown ? dynamicDowncast<ModuleRecord>(shown.moduleRecord) : nullptr; provider && provider->hasLiveExports()) {
        StarExportTargets starsOfProvider;
        Statement laid;
        Resolution declared = provider->resolveOwnExport(globalObject, shown.localName, starsOfProvider, nullptr, laid);
        isShown = !scope.exception() && declared.type == Resolution::Type::Resolved;
        if (!scope.exception() && laid.claimsWhatStarsFind && provider != this)
            isShown = provider->originalShows(globalObject, shown.localName, askers);
    }
    modules().originalsBeingAsked().removeLast();
    for (const Asker& asker : askers)
        asker.record->m_resolveSet.removeLast();
    m_resolveSet.removeLast();
    answered.removeAllMatching([&](const Modules::OriginalAnswered& each) {
        return each.depth > depth;
    });
    if (depth && !scope.exception())
        answered.append({ depth, m_module, exportName.impl(), isShown });
    return isShown;
}

bool ModuleRecord::originalShows(JSGlobalObject* globalObject, const Identifier& exportName, std::span<ModuleRecord* const> askersOfSameName)
{
    Vector<Asker, 8> askers;
    for (ModuleRecord* asker : askersOfSameName)
        askers.append({ asker, exportName.impl() });
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    RELEASE_AND_RETURN(scope, originalShows(globalObject, exportName, askers.span()));
}

auto ModuleRecord::claimAsOwn(const Identifier& exportName) -> Resolution
{
    std::optional<unsigned> index = modules().table().findExport(m_module, exportName.impl());
    bool isVariable = index && modules().table().exports(m_module)[*index].kind() == ModuleExportKind::Variable;
    return { Resolution::Type::Resolved, this, exportName, isVariable ? Markable<unsigned>(*index) : Markable<unsigned>() };
}

auto ModuleRecord::resolveOwnExport(JSGlobalObject* globalObject, const Identifier& exportName, StarExportTargets& starTargets, ModuleRecord* reexporter, Statement& statement) -> Resolution
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Modules& modules = this->modules();
    const ModuleData& data = modules.data(m_module);
    bool isDefault = exportName == vm.propertyNames->defaultKeyword;
    Resolution absent = isDefault ? Resolution::error() : Resolution::notFound();

    retain();
    if (takesNamesFromValue(data)) {
        ModuleID named = modules.moduleWithNamesOf(m_module);
        std::optional<bool> isNamedByEndedMock = reexporter ? modules.endedMockHasNameAsSeenBy(named, reexporter->m_module, exportName) : std::nullopt;
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (reexporter && modules.importsOriginalDespiteMock(named, reexporter->m_module)) [[unlikely]] {
            modules.evaluatePastPendingMock(named);
            RETURN_IF_EXCEPTION(scope, Resolution::error());
        }
        JSObject* moduleItself = takesNamesFromModuleItself() ? modules.originalNamespace(m_module) : nullptr;
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        bool hasExport = moduleItself ? moduleItself->hasOwnProperty(globalObject, exportName) : isNamedByEndedMock ? *isNamedByEndedMock : modules.hasCapturedExport(named, exportName);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (!hasExport)
            return absent;
        Resolution found { Resolution::Type::Resolved, this, exportName };
        found.isFromRemovedMock = isNamedByEndedMock.has_value();
        return found;
    }

    if (m_resolveSet.contains(exportName.impl()))
        return Resolution::notFound();

    std::optional<unsigned> index = modules.table().findExport(m_module, exportName.impl());
    const ModuleExportData* entry = index ? &modules.table().exports(m_module)[*index] : nullptr;
    if (!isDefault && !keepsToModule() && modules.isMockedByFactoryThatTakesOriginal(m_module)) [[unlikely]] {
        bool mayCollideInOriginal = entry ? entry->isFromStarExport : modules.table().isAmbiguousExport(m_module, exportName.impl());
        if (AbstractModuleRecord* loaded = mayCollideInOriginal ? originalFromTextInModules(globalObject, modules, m_module) : nullptr) {
            m_resolveSet.append(exportName.impl());
            Resolution inOriginal = loaded->resolveExport(globalObject, exportName);
            m_resolveSet.removeLast();
            RETURN_IF_EXCEPTION(scope, Resolution::error());
            if (inOriginal.type == Resolution::Type::Ambiguous)
                return Resolution::ambiguous();
        }
        if (entry && entry->kind() != ModuleExportKind::Indirect)
            return claimAsOwn(exportName);
        AbstractModuleRecord* original = modules.ensureDisplacedRecord(m_module);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        starTargets.append(original);
        statement.claimsWhatStarsFind = true;
        return absent;
    }
    if (!entry && !keepsToModule() && modules.isAddedByMock(m_module, exportName))
        return { Resolution::Type::Resolved, this, exportName };
    if (!entry && reexporter) {
        std::optional<bool> isNamedByEndedMock = modules.endedMockHasNameAsSeenBy(m_module, reexporter->m_module, exportName);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (isNamedByEndedMock.value_or(false)) [[unlikely]] {
            Resolution added { Resolution::Type::Resolved, this, exportName };
            added.isFromRemovedMock = true;
            return added;
        }
    }
    if (!entry && modules.table().isAmbiguousExport(m_module, exportName.impl()) && !modules.namesBehindStarsAreInTables(m_module) && !modules.isMockedThroughStarExports(m_module))
        return Resolution::ambiguous();
    if (entry && entry->isFromStarExport && !isDefault && (modules.hasAnyActiveMock() || modules.keepsSomeEndedMock())) [[unlikely]] {
        bool starsAnswer = appendStarsThatMayProvide(exportName, *index, starTargets);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (starsAnswer)
            return absent;
    }
    if (!isDefault && (!entry || entry->isFromStarExport || modules.leavesNameToStars(m_module, *index))) {
        appendDynamicStarTargets(exportName, starTargets);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
    }
    bool isSeenAsEndedMock = entry && reexporter && modules.endedMockSeenBy(m_module, reexporter->m_module);
    if (isSeenAsEndedMock && modules.isLeftOutInViewOf(reexporter->m_module, m_module, *index).value_or(false)) [[unlikely]] {
        absent.isFromRemovedMock = true;
        return absent;
    }
    if (!entry || (keepsToModule() ? modules.isHiddenInModuleItself(m_module, *index) : modules.isLeftOut(m_module, *index, Modules::TablesBehindStars::MayLackName)))
        return absent;
    auto follow = [&](AbstractModuleRecord* record, const Identifier& name) {
        auto* compiled = dynamicDowncast<ModuleRecord>(record);
        statement = { record, name, compiled && compiled->resolvesPerReexporter() ? this : nullptr, entry->isFromStarExport };
        return Resolution::notFound();
    };
    auto mockOfModule = keepsToModule() ? Modules::MockMode::Bypass : Modules::MockMode::Use;
    if (std::optional<Modules::MockedReexport> answer = modules.findMockForReexport(m_module, *entry, mockOfModule)) [[unlikely]] {
        if (reexporter && answer->lacksName)
            return absent;
        AbstractModuleRecord* record = modules.recordForResolution(answer->mocked);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        if (record != this)
            return follow(record, answer->name);
    }
    if (entry->kind() == ModuleExportKind::Variable || (entry->kind() == ModuleExportKind::Namespace && entry->isDeferred)) {
        Resolution own { Resolution::Type::Resolved, this, exportName, *index };
        own.isFromRemovedMock = isSeenAsEndedMock;
        own.cameFromStarExportEntry = entry->isFromStarExport && isNamedByEndedMockInFrontOfItsStar(globalObject, exportName, *index);
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        return own;
    }
    if (entry->kind() == ModuleExportKind::Namespace) {
        if (!keepsToModule() && modules.activeMock(m_module)) [[unlikely]]
            return { Resolution::Type::Resolved, this, exportName };
        AbstractModuleRecord* record = modules.recordForResolution(entry->targetModule());
        RETURN_IF_EXCEPTION(scope, Resolution::error());
        Resolution whole { Resolution::Type::Resolved, record, vm.propertyNames->starNamespacePrivateName };
        whole.isFromRemovedMock = isSeenAsEndedMock;
        return whole;
    }
    RELEASE_ASSERT(entry->kind() == ModuleExportKind::Indirect);
    const IndirectExportData& indirectExport = modules.table().indirectExport(*entry);
    AbstractModuleRecord* record = modules.recordForResolution(indirectExport.targetModule());
    RETURN_IF_EXCEPTION(scope, Resolution::error());
    return follow(record, modules.identifier(indirectExport.importName));
}

static constinit MissingExportListener missingExportListener = nullptr;

void setMissingExportListener(MissingExportListener listener)
{
    missingExportListener = listener;
}

bool ModuleRecord::throwIfExportWasOmitted(JSGlobalObject* globalObject, AbstractModuleRecord* importer, AbstractModuleRecord* exporter, const Identifier& name)
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

    if (takesNamesFromModuleItself()) [[unlikely]] {
        JSObject* moduleItself = modules.originalNamespace(m_module);
        RETURN_IF_EXCEPTION(scope, void());
        PropertyNameArrayBuilder listed(globalObject->vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        moduleItself->methodTable()->getOwnPropertyNames(moduleItself, globalObject, listed, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, void());
        for (const Identifier& name : listed)
            names.append(name);
        return;
    }
    if (takesNamesFromValue(modules.data(m_module))) {
        RELEASE_AND_RETURN(scope, modules.forEachCapturedExport(modules.moduleWithNamesOf(m_module), [&](const Identifier& name, JSValue) {
            names.append(name);
        }));
    }
    if (!keepsToModule() && modules.isMockedByFactoryThatTakesOriginal(m_module)) [[unlikely]] {
        JSModuleNamespaceObject* original = namespaceOfOriginalFromTextInModules(globalObject, modules, m_module);
        RETURN_IF_EXCEPTION(scope, void());
        if (original) {
            PropertyNameArrayBuilder shown(globalObject->vm(), PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
            original->methodTable()->getOwnPropertyNames(original, globalObject, shown, DontEnumPropertiesMode::Include);
            RETURN_IF_EXCEPTION(scope, void());
            for (const Identifier& name : shown)
                names.append(name);
            return;
        }
    }
    std::span<const ModuleExportData> entries = modules.table().exports(m_module);
    bool standsOverMocks = !hasLiveExports() && !keepsToModule() && modules.someMockIsSeen();
    for (unsigned index = 0; index < entries.size(); ++index) {
        bool isAsked = hasLiveExports() || (standsOverMocks && entries[index].isFromStarExport);
        if (!isAsked || !modules.isLeftOut(m_module, index, Modules::TablesBehindStars::HaveName))
            names.append(modules.identifier(entries[index].name));
    }
    if (!keepsToModule()) [[likely]]
        modules.appendNamesAddedByMock(m_module, names);
    RETURN_IF_EXCEPTION(scope, void());
    for (const AmbiguousExportData& entry : modules.table().ambiguousExports(m_module))
        names.append(Identifier::fromString(globalObject->vm(), ProgramStrings::text(modules.table().programTables(), entry.nameString())));
    RELEASE_AND_RETURN(scope, appendDynamicStarTargets(starTargets));
}

JSValue ModuleRecord::readExport(JSGlobalObject* globalObject, const Identifier& exportName, std::optional<unsigned> exportIndex)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (takesNamesFromModuleItself()) [[unlikely]] {
        JSObject* moduleItself = modules().originalNamespace(m_module);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, moduleItself->get(globalObject, exportName));
    }
    if (JSObject* mock = keepsToModule() ? nullptr : modules().activeMock(m_module)) [[unlikely]]
        RELEASE_AND_RETURN(scope, mock->get(globalObject, exportName));
    if (exportIndex && modules().table().exports(m_module)[*exportIndex].staysIn) {
        modules().refuseToHandOutExportOf(m_module, exportName.string());
        return { };
    }
    if (exportIndex)
        RELEASE_AND_RETURN(scope, keepsToModule() ? modules().readExportOfModuleItself(m_module, *exportIndex) : modules().readExport(m_module, *exportIndex));
    if (takesNamesFromValue(modules().data(m_module))) {
        JSValue value = modules().capturedValue(modules().moduleWithNamesOf(m_module), exportName);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
    }
    JSObject* namespaceObject = keepsToModule() ? modules().originalNamespace(m_module) : modules().moduleNamespace(m_module, ModulePhase::Evaluation);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, namespaceObject->get(globalObject, exportName));
}

JSObject* ModuleRecord::moduleNamespaceObject(JSGlobalObject*, ModulePhase phase)
{
    if (JSObject* own = phase == ModulePhase::Evaluation ? modules().namespaceOfRecordSetAside(this) : nullptr) [[unlikely]]
        return own;
    if (phase == ModulePhase::Evaluation && keepsToModule()) [[unlikely]]
        return modules().originalNamespace(m_module);
    return modules().moduleNamespace(m_module, phase);
}

ModuleRecord* Modules::displacedRecord(ModuleID module)
{
    LoadedModule* loaded = m_mocksWithIntactOriginal.isEmpty() ? nullptr : loadedIfExists(module);
    JSObject* record = loaded ? loaded->recordBeforeMockInPlace.get() : nullptr;
    return record ? uncheckedDowncast<ModuleRecord>(record) : nullptr;
}

ModuleRecord* Modules::ensureDisplacedRecord(ModuleID module)
{
    if (JSObject* record = ensureLoaded(module).recordBeforeMockInPlace.get())
        return uncheckedDowncast<ModuleRecord>(record);
    ModuleRecord* record = ModuleRecord::create(m_instance, m_instance.vm->aotState().moduleRecordStructure.get(), loader(), moduleKey(module), module);
    record->setHasTLA(data(module).hasTopLevelAwait());
    if (!takesNamesFromValue(data(module)))
        record->setKeepsToModule();
    ensureLoaded(module).recordBeforeMockInPlace.set(*this, record);
    return record;
}

JSPromise* Modules::displacedRecordEvaluation(ModuleID module)
{
    ModuleRecord* record = displacedRecord(module);
    if (!record || record->status() != CyclicModuleRecord::Status::EvaluatingAsync)
        return nullptr;
    return record->evaluate(m_instance.globalObject);
}

void Modules::recordImportForOriginalBodies(ModuleID imported, std::optional<int64_t> orderOfCaller, JSPromise* promise)
{
    if (!promise || m_mocksWithIntactOriginal.isEmpty()) [[likely]]
        return;
    if (m_importsCalledByBodiesSetAside.size() >= m_settledImportsReleaseThreshold) {
        releaseSettledImports();
        m_settledImportsReleaseThreshold = 2 * m_importsCalledByBodiesSetAside.size() + 1;
    }
    m_importsCalledByBodiesSetAside.append({ imported, orderOfCaller, Strong<JSPromise>(*m_instance.vm, promise) });
}

void Modules::releaseSettledImports()
{
    m_importsCalledByBodiesSetAside.removeAllMatching([](const ImportCalledByBodySetAside& call) {
        return call.promise->status() != JSPromise::Status::Pending;
    });
}

bool Modules::bodySetAsideCallsImportNow(ModuleID module)
{
    return m_importThatBodySetAsideCallsNow && displacedRecord(module) && staticImportClosure(module, MockTraversal::Continue).quickGet(m_importThatBodySetAsideCallsNow->first.index()) && staticImportClosure(m_importThatBodySetAsideCallsNow->second, MockTraversal::Stop).get(module.index());
}

static JSC_DECLARE_HOST_FUNCTION(decideWhetherBodySetAsideWaits);

JSC_DEFINE_HOST_FUNCTION(decideWhetherBodySetAsideWaits, (JSGlobalObject* realm, CallFrame* callFrame))
{
    modulesOf(realm)->decideWhetherBodySetAsideWaits(asArray(callFrame->argument(1)));
    return JSValue::encode(jsUndefined());
}

JSPromise* Modules::evaluationOnceBodySetAsideHasGoneOn(ModuleID module, std::optional<ModuleID> factoryCallerLoad)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* decided = JSPromise::create(vm, realm->promiseStructure());
    MarkedArgumentBuffer question;
    question.append(jsNumber(module.index()));
    question.append(factoryCallerLoad ? jsNumber(factoryCallerLoad->index()) : jsUndefined());
    question.append(decided);
    JSArray* asked = constructArray(realm, static_cast<JSC::ArrayAllocationProfile*>(nullptr), question);
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSPromise* fulfilled = JSPromise::create(vm, realm->promiseStructure());
    fulfilled->fulfill(vm, jsUndefined());
    JSFunction* goOn = JSFunction::create(vm, realm, 2, String(), JSC::AOT::decideWhetherBodySetAsideWaits, ImplementationVisibility::Private);
    fulfilled->performPromiseThenWithContext(vm, realm, goOn, jsUndefined(), jsUndefined(), asked);
    return decided;
}

void Modules::decideWhetherBodySetAsideWaits(JSArray* asked)
{
    VM& vm = *m_instance.vm;
    ModuleID module(asked->getIndexQuickly(0).asUInt32());
    JSValue load = asked->getIndexQuickly(1);
    JSPromise* decided = uncheckedDowncast<JSPromise>(asked->getIndexQuickly(2));
    JSPromise* evaluation = bodySetAsideWaitsForItsMock(module, load.isUndefined() ? std::nullopt : std::optional(ModuleID(load.asUInt32()))) ? nullptr : displacedRecordEvaluation(module);
    if (evaluation)
        decided->pipeFrom(vm, evaluation);
    else
        decided->fulfill(vm, jsUndefined());
}

bool Modules::bodySetAsideWaitsForItsMock(ModuleID module, std::optional<ModuleID> factoryCallerLoad)
{
    releaseSettledImports();
    ModuleRecord* record = displacedRecord(module);
    if (!record)
        return false;
    CyclicModuleRecord* root = record->cycleRoot() ? record->cycleRoot() : record;
    AbstractModuleRecord::AsyncEvaluationOrder order = root->asyncEvaluationOrder();
    auto waitsFor = [&](const ImportCalledByBodySetAside& call) {
        if (!staticImportClosure(call.imported, MockTraversal::Stop).get(module.index()))
            return false;
        return (order.hasOrder() && call.orderOfCaller == order.order()) || AbstractModuleRecord::importPromiseGatesAsyncDependency(call.promise.get(), root);
    };
    if (!factoryCallerLoad)
        return std::ranges::any_of(m_importsCalledByBodiesSetAside, waitsFor);
    bool beganLoad = true;
    return std::ranges::any_of(m_importsCalledByBodiesSetAside, [&](const ImportCalledByBodySetAside& each) {
        if (fileOf(each.imported) != fileOf(*factoryCallerLoad))
            return false;
        if (std::exchange(beganLoad, false))
            return waitsFor(each);
        return staticImportClosure(each.imported, MockTraversal::Stop).get(module.index()) && AbstractModuleRecord::importPromiseGatesAsyncDependency(each.promise.get(), root);
    });
}

JSObject* Modules::namespaceOfRecordSetAside(ModuleRecord* record)
{
    LoadedModule* loaded = m_mocksWithIntactOriginal.isEmpty() ? nullptr : loadedIfExists(record->module());
    if (!loaded || loaded->recordBeforeMockInPlace.get() != record || !loaded->namespaceBeforeMockInPlace)
        return nullptr;
    if (auto kept = m_mocksWithIntactOriginal.find(record->module()); kept != m_mocksWithIntactOriginal.end())
        kept->value.namespaceIsHeld = true;
    return loaded->namespaceBeforeMockInPlace.get();
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
    ModuleID withExports = modules.moduleWithExportsOf(modules.table().moduleBehindFacade(module).value_or(module));
    for (const SyntheticExportData& syntheticExport : modules.table().syntheticExports(withExports)) {
        if (syntheticExport.hasVariable && modules.identifier(syntheticExport.name).impl() == name)
            return { syntheticExport.variable, nullptr, nullptr };
    }
    return { std::nullopt, identityOfModule(modules, withExports), name };
}

bool ModuleRecord::isSameBinding(const Resolution& first, const Resolution& second)
{
    auto* firstRecord = uncheckedDowncast<ModuleRecord>(first.moduleRecord);
    if (&firstRecord->m_instance != &uncheckedDowncast<ModuleRecord>(second.moduleRecord)->m_instance)
        return false;
    Modules& modules = firstRecord->modules();
    if (firstRecord->takesNamesFromModuleItself() != uncheckedDowncast<ModuleRecord>(second.moduleRecord)->takesNamesFromModuleItself())
        return false;
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
    if (modules.activeMock(m_module) || m_mockOfView) [[unlikely]]
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

bool ModuleNamespace::keepsToModule() const
{
    return m_keepsToModule || modules().isBypassingMocks();
}

JSObject* ModuleNamespace::activeMock() const
{
    if (modules().isBypassingMocks()) [[unlikely]]
        return nullptr;
    if (JSObject* ofView = m_mockOfView.get()) [[unlikely]]
        return ofView;
    return keepsToModule() ? nullptr : modules().activeMock(m_module);
}

bool ModuleNamespace::isAnsweredByMock() const
{
    return keepsToModule() || !!activeMock();
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
    if (!table || m_mayNeedEvaluation || parseIndex(*name) || !isDynamicExport(name) || modules().activeMock(m_module))
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

JSValue Modules::readOffNamespaceThatListsName(ModuleID reader, JSObject* namespaceOfReader, JSObject* lister, const Identifier& name)
{
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::pair<JSObject*, UniquedStringImpl*> read { namespaceOfReader, name.impl() };
    if (m_namesBeingReadOffListers.contains(read) && (m_isBypassingMocks || (!m_mocks && m_removedMocksInUse.isEmpty()))) [[unlikely]] {
        AbstractModuleRecord* record = m_isBypassingMocks && activeMock(reader) ? ensureDisplacedRecord(reader) : recordForResolution(reader);
        RETURN_IF_EXCEPTION(scope, { });
        AbstractModuleRecord::Resolution resolution = record->resolveExport(realm, name);
        RETURN_IF_EXCEPTION(scope, { });
        auto* provider = resolution.type == AbstractModuleRecord::Resolution::Type::Resolved ? dynamicDowncast<ModuleRecord>(resolution.moduleRecord) : nullptr;
        if (provider && provider->module() != reader && resolution.localName != m_instance.vm->propertyNames->starNamespacePrivateName) {
            JSObject* namespaceOfProvider = m_isBypassingMocks ? originalNamespace(provider->module()) : moduleNamespace(provider->module(), ModulePhase::Evaluation);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, namespaceOfProvider->get(realm, resolution.localName));
        }
    }
    m_namesBeingReadOffListers.append(read);
    auto endRead = makeScopeExit([&] {
        m_namesBeingReadOffListers.removeLast();
    });
    auto* readingNamespace = dynamicDowncast<ModuleNamespace>(namespaceOfReader);
    auto* listingNamespace = dynamicDowncast<ModuleNamespace>(lister);
    bool shouldBypassMocks = listingNamespace && (m_isBypassingMocks || (readingNamespace && readingNamespace->keepsToModule()) || (listingNamespace->activeMock() && importsOriginalDespiteMock(listingNamespace->module(), reader)));
    SetForScope isBypassingMocks(m_isBypassingMocks, shouldBypassMocks);
    RELEASE_AND_RETURN(scope, lister->get(realm, name));
}

JSValue ModuleNamespace::readDynamicExport(JSGlobalObject* globalObject, PropertyName name)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Identifier identifier = Identifier::fromUid(globalObject->vm(), name.uid());
    if (JSObject* liveSources = m_liveSources.get()) {
        JSValue source = Modules::storedValue(globalObject, liveSources, identifier);
        RETURN_IF_EXCEPTION(scope, { });
        if (source && !globalObject->vm().isSafeToRecurseSoft()) [[unlikely]] {
            throwStackOverflowError(globalObject, scope);
            return { };
        }
        if (source)
            RELEASE_AND_RETURN(scope, modules().readOffNamespaceThatListsName(m_module, this, asObject(source), identifier));
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

JSC_DEFINE_CUSTOM_GETTER(refuseToHandOutExport, (JSGlobalObject*, EncodedJSValue thisValue, PropertyName name))
{
    uncheckedDowncast<ModuleNamespace>(JSValue::decode(thisValue))->refuseToShowExport(name);
    return { };
}

void ModuleNamespace::refuseToShowExport(PropertyName name)
{
    modules().refuseToHandOutExportOf(m_module, StringView(name.publicName()));
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
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->activeMock()) [[unlikely]]
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
        if (auto kept = index && method == InternalMethodType::GetOwnProperty && !thisObject->keepsToModule() ? thisObject->modules().exportThatStaysInBehindIndirectEntries(thisObject->m_module, *index) : std::nullopt) {
            thisObject->modules().readExport(kept->first, kept->second);
            RETURN_IF_EXCEPTION(scope, false);
            slot.setCustom(thisObject, static_cast<unsigned>(PropertyAttribute::DontDelete), refuseToHandOutExport);
            return true;
        }
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
    if (JSObject* mock = thisObject->activeMock()) [[unlikely]] {
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
            isAnsweredByMock = !!modules.findMockForReexport(thisObject->m_module, entries[ownIndex++]);
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
    if (JSObject* mock = thisObject->activeMock()) [[unlikely]] {
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
    if (JSObject* mock = thisObject->activeMock()) [[unlikely]] {
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
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->activeMock()) [[unlikely]] {
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
    if (JSObject* mock = thisObject->isSymbolLikeKey(name.uid()) ? nullptr : thisObject->activeMock()) [[unlikely]] {
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
