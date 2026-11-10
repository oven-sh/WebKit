/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "ModuleRegistryEntry.h"
#include "TopExceptionScope.h"
#include <ranges>
#include <wtf/Scope.h>

namespace JSC { namespace AOT {

ModuleID Modules::moduleLoadedBy(const ModuleRequestData& request) const
{
    return moduleLoadedUnder(request.targetModule());
}

ModuleID Modules::moduleLoadedUnder(ModuleID target) const
{
    if (!data(target).isUncompiledCode()) [[likely]]
        return target;
    const LoadedModule* loaded = loadedIfExists(target);
    std::optional<ModuleID> found = loaded && !loaded->keyOfLatestLoad.isNull() ? findByKey(loaded->keyOfLatestLoad) : std::nullopt;
    return found ? moduleWithExportsOf(fileOf(*found)) : target;
}

bool Modules::wasLoadedWhenMockWasMade(ModuleID module)
{
    if (!m_modulesWithDeferredMock.get(module.index()))
        return wasLoadedWithImporter(module);
    m_modulesWithDeferredMock.clear(module.index());
    return m_modulesLoadedBeforeMockThatWaited.get(module.index());
}

void Modules::forgetLoadThatFailed(ModuleID file)
{
    resetPackageFailureIfNeeded(file);
    if (!data(file).runsAgainAfterThrow())
        return;
    bool hasFailed = false;
    for (ModuleID each : { moduleWithExportsOf(file), file }) {
        ModuleRecord* record = m_records.get(each);
        if (!record || !record->evaluationError())
            continue;
        hasFailed = true;
        loader()->removeEntry(moduleKey(each));
        takeOutRecord(each);
        setStoredStatus(each, StoredModuleStatus::Linked);
    }
    if (!hasFailed)
        return;
    discardFailedEvaluation(file);
    Vector<Identifier, 4> failedWithIt;
    for (auto& [key, entry] : loader()->moduleMap()) {
        if (entry->status() >= ModuleRegistryEntry::Status::FetchFailed)
            failedWithIt.append(Identifier::fromUid(*m_instance.vm, key.first));
    }
    for (const Identifier& key : failedWithIt)
        loader()->removeEntry(key);
}

bool Modules::importsModuleWithPendingMock(ModuleID root)
{
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        if (isSeen.quickSet(importer.index()))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID imported = moduleLoadedBy(request);
            if (m_modulesWithPendingMock.get(m_table.moduleBehindFacade(imported).value_or(imported).index()))
                return true;
            toVisit.append(imported);
        }
    }
    return false;
}

bool Modules::isImportedByGraphUnderEvaluation(ModuleID module, bool forIntactOriginal)
{
    auto isUnevaluated = [&](ModuleID each) {
        ModuleStatus current = status(each);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    };
    bool shouldCountFailedImporters = !m_modulesWithResetFailure.get(module.index());
    BitVector isReached(m_table.size());
    Vector<ModuleID, 16> importers;
    for (unsigned index = 0; index < m_table.size(); ++index) {
        ModuleStatus current = status(ModuleID(index));
        if (current == ModuleStatus::Evaluating || current == ModuleStatus::EvaluatingAsync || (shouldCountFailedImporters && current == ModuleStatus::Failed && !failureByAbsentExport(ModuleID(index)) && !((forIntactOriginal || !isOfPackage(module)) && wasReleasedByLoader(ModuleID(index)))) || (wasMockedAfterFailure(ModuleID(index)) && !wasReleasedByLoader(ModuleID(index))))
            importers.append(ModuleID(index));
    }
    while (!importers.isEmpty()) {
        ModuleID importer = importers.takeLast();
        if (isReached.quickSet(importer.index()))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.isHoisted || request.isDeferred)
                continue;
            ModuleID imported = moduleLoadedBy(request);
            if (imported == module && (!forIntactOriginal || status(importer) != ModuleStatus::Failed))
                return true;
            if (imported != module && isUnevaluated(imported) && (!forIntactOriginal || (!isMocked(imported) && !m_modulesWithPendingMock.get(imported.index()))))
                importers.append(imported);
        }
    }
    return false;
}

std::optional<ModuleFailureID> Modules::failureByAbsentExport(ModuleID module) const
{
    if (!m_errorsOfExportsThatBuildFoundAbsent || status(module) != ModuleStatus::Failed)
        return std::nullopt;
    ModuleRecord* record = m_records.get(module);
    JSValue error = record ? record->evaluationError() : JSValue();
    for (unsigned index = 0; error && index < m_failuresByExportsThatBuildFoundAbsent.size(); ++index) {
        if (m_errorsOfExportsThatBuildFoundAbsent->getDirectIndex(m_instance.globalObject, index) == error)
            return m_failuresByExportsThatBuildFoundAbsent[index];
    }
    return std::nullopt;
}

bool Modules::wasLinkedAheadOfFailure(ModuleID root, ModuleID module) const
{
    std::optional<ModuleFailureID> id = failureByAbsentExport(root);
    if (!id)
        return false;
    ModuleID referrer = m_table.failure(*id).referrerModule();
    struct Step {
        ModuleID importer;
        size_t nextRequest;
        uint32_t place;
        uint32_t earliestPlaceReached;
    };
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> places;
    BitVector waitsForHeadOfRing(m_table.size());
    Vector<ModuleID, 16> filesThatWait { root };
    Vector<Step, 16> way { { root, 0, 0, 0 } };
    places.add(root.index(), 0);
    waitsForHeadOfRing.quickSet(root.index());
    while (!way.isEmpty()) {
        Step& step = way.last();
        auto requests = m_table.requests(step.importer);
        if (step.nextRequest == requests.size() || isMocked(step.importer)) {
            if (step.importer == referrer)
                return false;
            Step done = way.takeLast();
            if (done.earliestPlaceReached == done.place) {
                for (bool isHead = false; !isHead;) {
                    ModuleID linked = filesThatWait.takeLast();
                    waitsForHeadOfRing.quickClear(linked.index());
                    if (linked == module)
                        return true;
                    isHead = linked == done.importer;
                }
            }
            if (!way.isEmpty())
                way.last().earliestPlaceReached = std::min(way.last().earliestPlaceReached, done.earliestPlaceReached);
            continue;
        }
        const ModuleRequestData& request = requests[step.nextRequest++];
        if (request.isHoisted || request.isDeferred)
            continue;
        ModuleID imported = moduleLoadedBy(request);
        uint32_t place = places.size();
        if (auto entered = places.add(imported.index(), place); !entered.isNewEntry) {
            if (waitsForHeadOfRing.quickGet(imported.index()))
                step.earliestPlaceReached = std::min(step.earliestPlaceReached, entered.iterator->value);
            continue;
        }
        waitsForHeadOfRing.quickSet(imported.index());
        filesThatWait.append(imported);
        way.append({ imported, 0, place, place });
    }
    return false;
}

bool Modules::wasRegisteredByFailedLoad(ModuleID module) const
{
    if (!m_errorsOfExportsThatBuildFoundAbsent || !data(module).isESModuleOfProgram())
        return false;
    if (status(module) == ModuleStatus::Failed)
        return !!failureByAbsentExport(module);
    if (status(module) > ModuleStatus::Linked)
        return false;
    bool wasPulledIn = false;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (!failureByAbsentExport(root) || !modulesInLoadGraph(root).get(module.index()))
            continue;
        if (wasLinkedAheadOfFailure(root, module))
            return false;
        wasPulledIn = true;
    }
    return wasPulledIn;
}

bool Modules::wasLinkedBeforeLinkFailure(ModuleID module) const
{
    if (!m_errorsOfExportsThatBuildFoundAbsent)
        return false;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (failureByAbsentExport(root) && modulesInLoadGraph(root).get(module.index()) && wasLinkedAheadOfFailure(root, module))
            return true;
    }
    return false;
}

bool Modules::wasLinkedByFailedGraph(ModuleID module, bool byEarlierTestFile)
{
    return data(module).isESModuleOfProgram() && !data(module).isFacade() && status(module) <= ModuleStatus::Linked && !hasRun(module) && !didCaptureExports(module) && (!byEarlierTestFile || !wasLoadedByTestFile(module)) && isImportedByFailedRoot(module, byEarlierTestFile);
}

bool Modules::isImportedByFailedRoot(ModuleID module, bool ofEarlierTestFile) const
{
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (root != module && status(root) == ModuleStatus::Failed && !failureByAbsentExport(root) && !wasReleasedByLoader(root) && (!ofEarlierTestFile || !wasLoadedByTestFile(root)) && importsStatically(root, module))
            return true;
    }
    return false;
}

void Modules::forgetLoadsRefusedByCheckOfNames(ModuleID mocked)
{
    if (!wasRegisteredByFailedLoad(mocked))
        return;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (status(root) != ModuleStatus::Failed)
            continue;
        for (const ModuleRequestData& request : m_table.requests(root)) {
            ModuleID check = request.targetModule();
            if (!request.isHoisted || data(check).kind != ModuleKind::SourceText || status(check) != ModuleStatus::Failed)
                continue;
            for (ModuleID refused : { check, root }) {
                loader()->removeEntry(moduleKey(refused));
                takeOutRecord(refused);
                setStoredStatus(refused, StoredModuleStatus::Unlinked);
            }
        }
    }
    forgetFailuresOfExportsThatBuildFoundAbsent();
}

void Modules::forgetFailuresOfExportsThatBuildFoundAbsent()
{
    JSObject* kept = std::exchange(m_errorsOfExportsThatBuildFoundAbsent, nullptr);
    if (!kept)
        return;
    UncheckedKeyHashSet<JSCell*> errorsOfAbsentExports;
    for (unsigned index = std::exchange(m_failuresByExportsThatBuildFoundAbsent, { }).size(); index--;)
        errorsOfAbsentExports.add(kept->getDirectIndex(m_instance.globalObject, index).asCell());
    startLoadingModulesIndividually();
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID refused(index);
        if (status(refused) != ModuleStatus::Failed)
            continue;
        ModuleRecord* record = m_records.get(refused);
        JSValue error = record ? record->evaluationError() : JSValue();
        if (!error || !error.isCell() || !errorsOfAbsentExports.contains(error.asCell()))
            continue;
        loader()->removeEntry(moduleKey(refused));
        takeOutRecord(refused);
        setStoredStatus(refused, StoredModuleStatus::Unlinked);
        m_modulesLoadedBesideGroups.clear(refused.index());
    }
    ensureStillAliveHere(kept);
}

bool Modules::evaluateLinkedModuleBeforeMock(ModuleID module, bool keepsOriginalIntact)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_moduleWhoseBodyMockCallRan = std::nullopt;
    forgetLoadsRefusedByCheckOfNames(module);
    auto isUnevaluated = [&](ModuleID each) {
        ModuleStatus current = status(each);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    };
    if (hasFailedWithoutMock(module) && !isMocked(module) && !m_modulesWithPendingMock.get(module.index()))
        return !didCaptureExports(module);
    if (!data(module).isESModuleOfProgram() || data(module).isFacade() || !isUnevaluated(module) || isMocked(module) || m_modulesWithPendingMock.get(module.index()))
        return false;
    if (!isImportedByGraphUnderEvaluation(module, keepsOriginalIntact) && (keepsOriginalIntact || !wasLinkedBeforeLinkFailure(module)))
        return !hasRun(module) && !didCaptureExports(module);
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { module };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        if (isSeen.quickSet(importer.index()))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.isDeferred)
                continue;
            if (status(moduleLoadedBy(request)) == ModuleStatus::Evaluating)
                return false;
            if (isUnevaluated(moduleLoadedBy(request)))
                toVisit.append(moduleLoadedBy(request));
        }
    }
    JSPromise* evaluation = evaluate(module, std::nullopt, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    if (evaluation->status() == JSPromise::Status::Rejected) {
        evaluation->markAsHandled();
        throwException(m_instance.globalObject, scope, evaluation->result());
        return false;
    }
    m_moduleWhoseBodyMockCallRan = module;
    return false;
}

JSPromise* Modules::pendingEvaluation(ModuleID module, bool hasAlreadyWaited)
{
    if (!data(module).isESModuleOfProgram() || data(module).isFacade())
        return nullptr;
    for (const DetachedLoad& left : m_detachedLoads) {
        if (left.isForOriginal && left.required == module && left.wait->status() == JSPromise::Status::Pending)
            return left.wait.get();
    }
    if (status(module) == ModuleStatus::EvaluatingAsync) {
        JSPromise* evaluation = evaluate(module, std::nullopt, nullptr, nullptr, true);
        return evaluation && evaluation->status() == JSPromise::Status::Pending ? behindStepsOfImportThatWaitsFor(evaluation, module) : nullptr;
    }
    if (hasAlreadyWaited || status(module) != ModuleStatus::Linked || !isImportedByGraphUnderEvaluation(module))
        return nullptr;
    constexpr unsigned jobsThatLoaderTakesToAnswerWithModuleThatItHolds = 6;
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSPromise* turn = JSPromise::resolvedPromise(realm, jsUndefined());
    for (unsigned job = 0; job < jobsThatLoaderTakesToAnswerWithModuleThatItHolds; ++job) {
        JSPromise* next = JSPromise::create(vm, realm->promiseStructure());
        turn->performPromiseThen(vm, realm, jsUndefined(), jsUndefined(), next);
        turn = next;
    }
    return turn;
}

JSPromise* Modules::pendingEvaluationInGraph(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { file };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        if (isSeen.quickSet(importer.index()))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            if (request.isDeferred)
                continue;
            ModuleID imported = moduleLoadedBy(request);
            JSPromise* evaluation = pendingEvaluation(imported, true);
            RETURN_IF_EXCEPTION(scope, nullptr);
            if (evaluation)
                return evaluation;
            toVisit.append(imported);
        }
    }
    return nullptr;
}

bool Modules::tryEvaluateESModuleBeforeMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).isESModuleOfProgram() || data(module).isFacade() || hasStartedLoading(module) || isEvicted(module))
        return false;
    VisitedModuleRecords seen;
    bool isReady = readyForSyncExecution(module, seen);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isReady)
        return false;
    forgetMockThatWaits(module);
    RETURN_IF_EXCEPTION(scope, false);
    SetForScope shouldContinuePastPendingFactories(m_shouldContinuePastPendingFactories, true);
    evaluateSynchronously(module, SynchronousEvaluationReason::Require);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool Modules::startDetachedOriginalLoad(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!isStillBlockedOnFactories(module))
        return false;
    startDetachedLoad(module, true);
    RETURN_IF_EXCEPTION(scope, false);
    return m_detachedLoads.containsIf([&](const DetachedLoad& left) { return left.isForOriginal && left.required == module; });
}

JSObject* Modules::evaluateOriginalUnderMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if ((!data(module).isESModuleOfProgram() && data(module).kind != ModuleKind::Data) || data(module).isFacade() || !isMocked(module) || hasIntactOriginal(module) || isEvicted(module))
        return nullptr;
    if (!wasOriginalEvaluated(module)) {
        if (hasStartedLoading(module) && !hasOnlyBeenLinked(module))
            return nullptr;
        VisitedModuleRecords seen;
        bool isReady = readyForSyncExecution(module, seen);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!isReady)
            return nullptr;
        didRemoveMock(module);
        bool wasMockPending = m_modulesWithPendingMock.get(module.index());
        m_modulesWithPendingMock.clear(module.index());
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        m_modulesWithPendingMock.set(module.index(), wasMockPending);
        willMock(module);
        if (status(module) == ModuleStatus::Evaluated || status(module) == ModuleStatus::Failed)
            m_originalsEvaluatedUnderMock.set(module.index());
        if (scope.exception()) [[unlikely]] {
            setAsideErrorOfModuleThatRanBesideMock(module);
            return nullptr;
        }
    }
    JSValue own = requireOriginalUnderMock(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return own ? own.getObject() : nullptr;
}

void Modules::evaluateCommonJSModuleBeforeMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    forgetMockThatWaits(module);
    RETURN_IF_EXCEPTION(scope, void());
    m_modulesEvaluatedBeforeMock.set(module.index());
    scope.release();
    require(module, ModuleID(), RequiredBy::Program);
}

bool Modules::isLoaderInMiddleOfGraph() const
{
    for (auto& entry : loader()->moduleMap().values()) {
        switch (entry->status()) {
        case ModuleRegistryEntry::Status::New:
        case ModuleRegistryEntry::Status::Fetching:
            return true;
        case ModuleRegistryEntry::Status::Fetched:
            if (auto* record = dynamicDowncast<CyclicModuleRecord>(entry->record()); record && record->status() != CyclicModuleRecord::Status::Evaluated)
                return true;
            break;
        case ModuleRegistryEntry::Status::FetchFailed:
        case ModuleRegistryEntry::Status::InstantiationFailed:
        case ModuleRegistryEntry::Status::EvaluationFailed:
            break;
        }
    }
    return false;
}

bool Modules::isImportedByModuleThatRequireAnswered(ModuleID imported) const
{
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        const LoadedModule* loaded = loadedIfExists(ModuleID(index));
        if (loaded && loaded->requiredValue && index != imported.index())
            work.append(ModuleID(index));
    }
    while (!work.isEmpty()) {
        for (const ModuleRequestData& request : m_table.imports(work.takeLast())) {
            if (moduleLoadedBy(request) == imported)
                return true;
            if (!seen.quickSet(moduleLoadedBy(request).index()))
                work.append(moduleLoadedBy(request));
        }
    }
    return false;
}

void Modules::noteCallOfMock(ModuleID fileOrModule, bool keepsActiveMock)
{
    MomentMockWasLaid call = momentOfNow();
    call.hadNamespace = !!namespaceIfCreated(moduleWithExportsOf(fileOrModule), ModulePhase::Evaluation);
    call.keepsActiveMock = keepsActiveMock;
    m_pendingMockCalls.set(fileOrModule, call);
}

void Modules::forgetCallOfMock(ModuleID fileOrModule)
{
    m_pendingMockCalls.remove(fileOrModule);
}

void Modules::deferMock(ModuleID module, FactoryFailurePolicy failurePolicy)
{
    finishLoadsAwaitingLoader(module);
    keepMockForPendingImports(module, MockChange::Replaced);
    noteDeedOnMockUnderCallsOfImport(module, DeedOnMock::LaysAnother);
    bool ranAheadOfThisTestFile = data(module).kind == ModuleKind::CommonJs && wasLoadedBeforeTestFile(module);
    if ((m_modulesWithDeferredMock.get(module.index()) || ranAheadOfThisTestFile) && !activeMock(module) && !isPatchedByMock(module))
        releaseImportersOf(module, ReleasedImporters::All);
    if (hasDeferredFetchOf(fileOf(module)))
        m_filesToEvictOnceFetched.set(fileOf(module).index());
    if (m_delayedImportTargets.contains(module))
        m_mockEntriesToEvict.set(module.index());
    m_modulesWithPendingMock.set(module.index());
    m_modulesWhoseFactoryWasCalled.clear(module.index());
    m_missingMockFailures.removeAllMatching([&](const auto& load) { return fileOf(load.first) == fileOf(module); });
    m_failuresOfFactories.removeAllMatching([&](const FailureOfFactory& failure) { return failure.mocked == module; });
    m_mocksRetriedAfterFailure.set(module.index(), failurePolicy == FactoryFailurePolicy::RetryFactory);
    clearLoadErrorsForMock(module);
    if (data(module).kind == ModuleKind::CommonJs)
        ensureRecord(module);
    if (data(module).isOfLibrary())
        willMockLibraryModule(module);
    if (!data(module).isOfLibrary() && m_table.hasFilePath(module)) {
        m_modulesWithDeferredMock.set(module.index());
        m_modulesLoadedBeforeMockThatWaited.set(module.index(), wasLoadedWithImporter(module));
        willMock(module);
        auto noted = m_pendingMockCalls.find(module);
        noteCallOfMock(module, noted != m_pendingMockCalls.end() && noted->value.keepsActiveMock);
    }
}

void Modules::removeUnusedLingeringMocksSlow()
{
    if (m_hasImportsBlockedOnFactories || !m_modulesWithRunningFactory.isEmpty() || m_loadsInFlight.numberBeingLoaded())
        return;
    auto leadsToMockThatStands = [&](ModuleID root) {
        BitVector reached = modulesInLoadGraph(root);
        reached.filter(m_modulesWithLingeringMock);
        return !reached.isEmpty();
    };
    for (ModuleID root : m_factoryBlockedImportRoots) {
        if (leadsToMockThatStands(root))
            return;
    }
    for (const BatchedFetch& text : batchedFetches()) {
        if (leadsToMockThatStands(text.module))
            return;
    }
    for (const QueuedFetch& text : m_queuedFetches) {
        if (leadsToMockThatStands(text.module))
            return;
    }
    for (size_t queued : m_modulesOfQueuedImports) {
        if (leadsToMockThatStands(ModuleID(queued)))
            return;
    }
    removeLingeringMocks();
}

void Modules::removeLingeringMocks()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope isRemovingLingeringMocks(m_isRemovingLingeringMocks, true);
    m_hasImportsBlockedOnFactories = false;
    while (!m_modulesWithLingeringMock.isEmpty()) {
        removeLingeringMock(ModuleID(*m_modulesWithLingeringMock.begin()));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::removeLingeringMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope isRemovingLingeringMocks(m_isRemovingLingeringMocks, true);
    m_modulesWithLingeringMock.clear(module.index());
    bool anotherWaits = m_modulesWithPendingMock.get(module.index());
    bool asksAgain = m_mocksRetriedAfterFailure.get(module.index());
    bool wasInstalledOnPreloadMock = !m_modulesWhoseEndPutOffBringsNoMockOfPreloadBack.get(module.index());
    m_modulesWhoseEndPutOffBringsNoMockOfPreloadBack.clear(module.index());
    AOT::didRemoveMock(m_instance.globalObject, module, RetainedOriginalPolicy::DiscardWithMock, wasInstalledOnPreloadMock);
    scope.exception();
    if (anotherWaits)
        deferMock(module, asksAgain ? FactoryFailurePolicy::RetryFactory : FactoryFailurePolicy::CacheError);
}

JSValue Modules::takeErrorOfFactoryThatFailedWhileCallWasHeldForItsText(ModuleID asked)
{
    if (!m_resumingCallSequenceNumber)
        return { };
    for (const FailureOfFactory& failure : m_failuresOfFactories) {
        if (failure.moment <= *m_resumingCallSequenceNumber || fileOf(failure.mocked) != fileOf(asked))
            continue;
        m_resumingCallSequenceNumber = std::nullopt;
        m_importsAwaitingSource.removeFirstMatching([&](const ImportAwaitingSource& each) { return each.module == asked; });
        return failure.error.get();
    }
    return { };
}

void Modules::recordFactoryFailure(ModuleID mocked, JSValue error)
{
    if (isSomeCallHeldForItsText())
        m_failuresOfFactories.append(FailureOfFactory { mocked, Strong<Unknown>(*m_instance.vm, error), momentOfNow().moment, { } });
}

JSValue Modules::errorOfFactoryThatFailedWhileCallWaited(ModuleID root, uint32_t momentCallBeganToWait) const
{
    for (const FailureOfFactory& failure : m_failuresOfFactories) {
        if (failure.moment > momentCallBeganToWait && failure.rootsAwaitingOriginal.contains(root))
            return failure.error.get();
    }
    return { };
}

void Modules::didResumeAfterFactories(ModuleID root)
{
    didResumeAfterFactories();
    auto numberOf = [&](const Vector<ModuleID, 2>& roots) { return std::ranges::count(roots, root); };
    if (numberOf(m_rootsOfImportsThatHaveGoneOn) < numberOf(m_factoryBlockedImportRoots))
        m_rootsOfImportsThatHaveGoneOn.append(root);
}

void Modules::didSettleImportBehindFactories(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_factoryBlockedImportRoots.removeFirst(root);
    m_factoryBlockedImports.removeAllMatching([](const auto& call) { return call.promise->status() != JSPromise::Status::Pending; });
    if (m_factoryBlockedImportRoots.isEmpty() && !isSomeCallHeldForItsText())
        m_failuresOfFactories.clear();
    if (!m_rootsOfImportsThatHaveGoneOn.removeFirst(root) || m_modulesWithLingeringMock.isEmpty() || m_loadsInFlight.numberBeingLoaded())
        return;
    BitVector taken = modulesInLoadGraph(root);
    taken.filter(m_modulesWithLingeringMock);
    for (ModuleID other : m_rootsOfImportsThatHaveGoneOn)
        taken.exclude(modulesInLoadGraph(other));
    for (const BatchedFetch& text : batchedFetches())
        taken.exclude(modulesInLoadGraph(text.module));
    for (const QueuedFetch& text : m_queuedFetches)
        taken.exclude(modulesInLoadGraph(text.module));
    for (size_t queued : m_modulesOfQueuedImports)
        taken.exclude(modulesInLoadGraph(ModuleID(queued)));
    for (size_t module : taken) {
        removeLingeringMock(ModuleID(module));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool Modules::isResolvedForInFlightLoads(ModuleID mocked) const
{
    return !m_factoryBlockedImportRoots.containsIf([&](ModuleID root) {
        if (fileOf(root) == fileOf(mocked))
            return !namespaceIfCreated(root, ModulePhase::Evaluation);
        return status(root) < ModuleStatus::EvaluatingAsync && modulesInLoadGraph(root).get(mocked.index());
    });
}

void Modules::forgetMockThatWaits(ModuleID module)
{
    auto wasAskedForByLoadThatFailed = [&] {
        for (size_t kept : m_modulesWithMockLoadError) {
            for (const ModuleRequestData& request : m_table.imports(ModuleID(kept))) {
                if (!request.isHoisted && !request.isDeferred && fileOf(moduleLoadedBy(request)) == fileOf(module))
                    return true;
            }
        }
        return false;
    };
    if (m_modulesWithPendingMock.clear(module.index()) && !activeMock(module) && !m_mocksWhoseEntryProgramDeleted.get(module.index()) && wasAskedForByLoadThatFailed()) [[unlikely]]
        clearMockLoadErrors();
    releaseNamespaceBeforeMock(module);
    if (!m_modulesWithDeferredMock.get(module.index()))
        return;
    m_modulesWithDeferredMock.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didRemoveMock(module);
}

bool Modules::runPendingMockSlow(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockDoor door(this, module, "runMock"_s);
    if (isMockLingering(module)) {
        removeLingeringMock(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    m_modulesWithPendingMock.clear(module.index());
    m_modulesWhoseFactoryWasCalled.set(module.index());
    m_failuresOfFactories.removeAllMatching([&](const FailureOfFactory& failure) { return failure.mocked == module; });
    bool ranAlready = m_modulesWithRunningFactory.set(module.index());
    SetForScope factoryCallImportTarget(m_factoryCallImportTarget, m_currentImportTarget);
    if (m_currentImportTarget && !m_importsAwaitingFirstStep.isEmpty()) [[unlikely]]
        advanceMockedImportsThrough(m_lastMockedImportSerial);
    bool factoryHasNotSettled = host().runMock(m_instance.globalObject, module);
    if (!ranAlready)
        m_modulesWithRunningFactory.clear(module.index());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        m_modulesWithPendingMock.set(module.index());
        m_loaderMayHoldLoadsThatFactoryEnded = true;
        Vector<ModuleID, 2> rootsAwaitingOriginal;
        for (const ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
            if (fileOf(above.mocked) == fileOf(module) && above.original && above.mockHasFactory)
                rootsAwaitingOriginal.append(above.root);
        }
        if ((!rootsAwaitingOriginal.isEmpty() || hasInFlightMockImport(module)) && !isCurrentRequireTarget(module) && !m_instance.vm->isTerminationException(exception))
            m_failuresOfFactories.append(FailureOfFactory { module, Strong<Unknown>(*m_instance.vm, exception->value()), momentOfNow().moment, WTF::move(rootsAwaitingOriginal) });
        if (m_importsMadeByOwnFactory.contains(module) && !m_instance.vm->isTerminationException(exception)) {
            m_modulesWithCachedMockError.set(module.index());
            ensureLoaded(module).cachedMockError.set(*this, exception->value());
        }
        return false;
    }
    if (!factoryHasNotSettled)
        m_importsMadeByOwnFactory.removeAll(module);
    return factoryHasNotSettled;
}

void Modules::keepErrorOfLoad(ModuleID root, JSValue error, const ScopedLambda<bool(ModuleID)>& endedLoad)
{
    UncheckedKeyHashMap<ModuleID, Vector<ModuleID, 2>> importers;
    BitVector seen(m_table.size());
    BitVector ended(m_table.size());
    seen.quickSet(root.index());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (endedLoad(module)) {
            ended.quickSet(module.index());
            continue;
        }
        if (isMocked(module) || (module != root && hasStartedLoading(module)))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (request.isHoisted)
                continue;
            importers.add(moduleLoadedBy(request), Vector<ModuleID, 2>()).iterator->value.append(module);
            if (!seen.quickSet(moduleLoadedBy(request).index()))
                work.append(moduleLoadedBy(request));
        }
    }
    BitVector above(m_table.size());
    for (size_t module : ended)
        work.append(ModuleID(module));
    while (!work.isEmpty()) {
        ModuleID imported = work.takeLast();
        auto found = importers.find(imported);
        if (found == importers.end())
            continue;
        for (ModuleID importer : found->value) {
            if (above.quickSet(importer.index()))
                continue;
            work.append(importer);
            if (m_modulesWithMockLoadError.set(importer.index()))
                continue;
            ensureLoaded(importer).cachedLoadError.set(*this, error);
        }
    }
}

void Modules::noteImportFailedUnderMock(ModuleID root, JSValue error)
{
    if (status(root) >= ModuleStatus::Evaluating || m_modulesWithMockLoadError.get(root.index()))
        return;
    m_rootsOfImportsThatFailedUnderMocks.set(root.index());
    ensureLoaded(root).errorOfImportThatFailedUnderMock.set(*this, error);
    ensureLoaded(root).momentImportFailedUnderMock = momentOfNow().moment;
}

bool Modules::hasFailedUnderMockThatStands(ModuleID root)
{
    if (!m_rootsOfImportsThatFailedUnderMocks.get(root.index())) [[likely]]
        return false;
    uint32_t momentOfFailure = ensureLoaded(root).momentImportFailedUnderMock;
    auto wasMockedBeforeFailure = [&](ModuleID mocked) {
        for (const auto* moments : { &m_pendingMockCalls, &m_activeMockCalls }) {
            if (auto made = moments->find(mocked); made != moments->end() && made->value.moment < momentOfFailure)
                return true;
        }
        return false;
    };
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if ((isMocked(module) || hasPendingMock(fileOf(module))) && (wasMockedBeforeFailure(module) || wasMockedBeforeFailure(fileOf(module))))
            return true;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !seen.quickSet(moduleLoadedBy(request).index()))
                work.append(moduleLoadedBy(request));
        }
    }
    return false;
}

void Modules::letRootsOfFailedImportsGoWithWhatIsReset()
{
    for (size_t root : m_rootsOfImportsThatFailedUnderMocks)
        ensureLoaded(ModuleID(root)).momentImportFailedUnderMock = 0;
}

void Modules::forgetImportFailedUnderMock(ModuleID root)
{
    if (m_rootsOfImportsThatFailedUnderMocks.clear(root.index()))
        ensureLoaded(root).errorOfImportThatFailedUnderMock.set(*this, JSValue());
}

void Modules::settleFailedImportsUnder(ModuleID mocked, FailedImports failedImports)
{
    if (failedImports == FailedImports::MakeUnevictable && (m_mocksRetriedAfterFailure.get(mocked.index()) || m_mocksRetriedAfterFailure.get(fileOf(mocked).index())))
        return;
    if (failedImports == FailedImports::MakeUnevictable) {
        clearCachedMockError(mocked);
        m_mocksWhoseEntryProgramDeleted.set(mocked.index());
        for (ModuleID module : evictableFailedLoads([&](ModuleID each) { return fileOf(each) == fileOf(mocked); }))
            m_modulesWithUnevictableFailure.set(module.index());
    }
    if (m_rootsOfImportsThatFailedUnderMocks.isEmpty()) [[likely]]
        return;
    m_filesAboveNoEvictedFile.clearAll();
    ModuleID file = fileOf(mocked);
    BitVector roots = m_rootsOfImportsThatFailedUnderMocks;
    for (size_t index : roots) {
        ModuleID root(index);
        BitVector keptBefore = m_modulesWithMockLoadError;
        JSValue error = ensureLoaded(root).errorOfImportThatFailedUnderMock.get();
        keepErrorOfLoad(root, error, [&](ModuleID each) { return fileOf(each) == file; });
        BitVector kept = m_modulesWithMockLoadError;
        kept.exclude(keptBefore);
        if (kept.isEmpty())
            continue;
        m_rootsOfImportsThatFailedUnderMocks.clear(index);
        ensureLoaded(root).errorOfImportThatFailedUnderMock.set(*this, JSValue());
        if (failedImports == FailedImports::MakeUnevictable) {
            m_modulesWithUnevictableFailure.merge(kept);
            continue;
        }
        for (size_t module : kept)
            clearMockLoadError(ModuleID(module));
    }
}

void Modules::didFailMockImportRoot(ModuleID mocked)
{
    bool hasImporterErrors = !evictableFailedLoads([&](ModuleID each) { return fileOf(each) == fileOf(mocked); }).isEmpty();
    if (hasImporterErrors || !m_modulesWithCachedMockError.get(mocked.index()))
        settleFailedImportsUnder(mocked, FailedImports::MakeUnevictable);
}

void Modules::clearMockLoadError(ModuleID module)
{
    m_modulesWithUnevictableFailure.clear(module.index());
    if (m_modulesWithMockLoadError.clear(module.index()))
        ensureLoaded(module).cachedLoadError.set(*this, JSValue());
    clearCachedMockError(module);
}

void Modules::clearCachedMockError(ModuleID module)
{
    if (m_modulesWithCachedMockError.clear(module.index())) [[unlikely]]
        ensureLoaded(module).cachedMockError.set(*this, JSValue());
}

void Modules::clearLoadErrorsForMock(ModuleID mocked)
{
    if (m_modulesWithCachedMockError.get(mocked.index())) [[unlikely]]
        clearMockLoadErrors();
    clearCachedMockError(mocked);
    dropErrorsOfLoadsThatCameTo([&](ModuleID module) { return module == mocked; });
    if (!m_loaderMayHoldLoadsThatFactoryEnded || m_modulesWithRunningFactory.get(mocked.index()) || isLoaderInMiddleOfGraph()) [[likely]]
        return;
    m_loaderMayHoldLoadsThatFactoryEnded = false;
    takeFailedLoadsOutOfLoader([&](ModuleID module) { return module == mocked; });
}

void Modules::takeFailedLoadsOutOfLoader(const ScopedLambda<bool(ModuleID)>& isMocked)
{
    Vector<Identifier, 4> failed;
    for (auto& entry : loader()->moduleMap().values()) {
        if (entry->status() < ModuleRegistryEntry::Status::FetchFailed)
            continue;
        std::optional<ModuleID> root = findByKey(entry->key());
        if (!root) {
            m_loaderMayHoldLoadsThatFactoryEnded = true;
            continue;
        }
        bool cameToMock = false;
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { *root };
        while (!work.isEmpty() && !cameToMock) {
            ModuleID module = work.takeLast();
            cameToMock = isMocked(module);
            for (const ModuleRequestData& request : m_table.imports(module)) {
                if (!request.isHoisted && !seen.quickSet(moduleLoadedBy(request).index()))
                    work.append(moduleLoadedBy(request));
            }
        }
        if (cameToMock)
            failed.append(entry->key());
        else
            m_loaderMayHoldLoadsThatFactoryEnded = true;
    }
    for (const Identifier& key : failed)
        loader()->removeEntry(key);
}

Vector<ModuleID, 8> Modules::evictableFailedLoads(const ScopedLambda<bool(ModuleID)>& isMocked)
{
    Vector<ModuleID, 8> above;
    for (size_t kept : m_modulesWithMockLoadError) {
        if (m_modulesWithUnevictableFailure.get(kept))
            continue;
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { ModuleID(kept) };
        while (!work.isEmpty()) {
            ModuleID module = work.takeLast();
            if (isMocked(module)) {
                above.append(ModuleID(kept));
                break;
            }
            for (const ModuleRequestData& request : m_table.imports(module)) {
                if (!request.isHoisted && !seen.quickSet(moduleLoadedBy(request).index()))
                    work.append(moduleLoadedBy(request));
            }
        }
    }
    return above;
}

void Modules::dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked)
{
    for (ModuleID module : evictableFailedLoads(isMocked))
        clearMockLoadError(module);
}

void Modules::discardDetachedLoadErrorsReaching(ModuleID module)
{
    for (DetachedLoad& left : m_detachedLoads) {
        if (left.keepsError && staticImportClosure(left.required, MockTraversal::Stop).get(module.index()))
            left.keepsError = false;
    }
}

bool Modules::hasPermanentFailureInPackage(ModuleID root)
{
    if (status(root) != ModuleStatus::Failed)
        return false;
    for (size_t below : staticImportClosure(root, MockTraversal::Stop)) {
        if (below != root.index() && (m_modulesWithResetFailure.get(below) || hasFailedWithoutMock(ModuleID(below))))
            return true;
    }
    return false;
}

bool Modules::hasPermanentFailureInProject(ModuleID root)
{
    if (status(root) != ModuleStatus::Failed || failureByAbsentExport(root) || !wasLoadedByTestFile(root) || wasReleasedByLoader(root))
        return false;
    for (size_t below : staticImportClosure(root, MockTraversal::Stop)) {
        if (below != root.index() && status(ModuleID(below)) == ModuleStatus::Failed && isFailedESModule(ModuleID(below)) && !isMocked(ModuleID(below)) && !m_modulesWithPendingMock.get(below))
            return true;
    }
    return false;
}

void Modules::clearMockLoadErrors()
{
    BitVector kept = m_modulesWithMockLoadError;
    kept.merge(m_modulesWithCachedMockError);
    for (size_t module : kept)
        clearMockLoadError(ModuleID(module));
    BitVector roots = m_rootsOfImportsThatFailedUnderMocks;
    for (size_t root : roots)
        forgetImportFailedUnderMock(ModuleID(root));
}

bool Modules::isFetchedForImportStatement(ModuleID module) const
{
    ModuleRecord* record = m_records.get(module);
    ModuleRegistryEntry* entry = loader()->registryEntry(record ? record->moduleKey() : Identifier::fromString(*m_instance.vm, m_table.fullPath(module)));
    return entry && entry->status() == ModuleRegistryEntry::Status::New;
}

bool Modules::runMocksInFetchedGraphSlow(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockDoor door(this, module, "the loader fetches it"_s);
    if (m_originalBeingEvaluated == module) [[unlikely]]
        return false;
    evaluateModulesOwedBelow(module);
    RETURN_IF_EXCEPTION(scope, false);
    scope.release();
    if (m_modulesWithPendingMock.isEmpty()) [[likely]]
        return false;
    bool isAskedForByImportCall = m_instance.vm->aotState().frameOfImportThatAsksLoader;
    if (isAskedForByImportCall)
        clearCachedMockError(module);
    m_mocksWhoseFactoriesRanInsideTheirFetch.set(module.index(), m_modulesWithPendingMock.get(module.index()));
    SetForScope moduleBeingFetched(m_moduleBeingFetched, loadOfFetch(module));
    bool wasFetchedForStatement = m_statementFetchesDelayedByTick.get(module.index());
    m_statementFetchesDelayedByTick.clear(module.index());
    return runMocksInGraph(module, wasFetchedForStatement || (!isAskedForByImportCall && isFetchedForImportStatement(module)) ? LoadRootKind::ImportStatement : LoadRootKind::StartsLoad);
}

void Modules::willFetchAfterTick(ModuleID module)
{
    m_statementFetchesDelayedByTick.set(module.index(), !m_instance.vm->aotState().frameOfImportThatAsksLoader && isFetchedForImportStatement(module));
}

bool Modules::didMockFailForImportStatement(ModuleID module)
{
    return m_modulesWithPendingMock.get(module.index()) && isFetchedForImportStatement(module);
}

void Modules::reinstallBodyMocks(ModuleID module)
{
    if (!hasMocksToReinstall() || !host().reinstallMocks)
        return;
    host().reinstallMocks(m_instance.globalObject, [&](ModuleID each) {
        return each == module;
    });
}

bool Modules::hasMocksToReinstall() const
{
    return !m_modulesNeedingMockReinstall.isEmpty();
}

BitVector Modules::staticImportClosure(ModuleID root, MockTraversal atMock) const
{
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()) || (atMock == MockTraversal::Stop && isMocked(module)))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module))
            work.append(moduleLoadedBy(request));
    }
    return reached;
}

bool Modules::importsStatically(ModuleID importer, ModuleID imported) const
{
    return staticImportClosure(importer, MockTraversal::Continue).quickGet(imported.index());
}

static bool importsItselfByStatementsInTestRunFactories(const ModuleTable& table, ModuleID module)
{
    BitVector seen(table.size());
    Vector<ModuleID, 32> work { module };
    while (!work.isEmpty()) {
        for (const ModuleRequestData& request : table.imports(work.takeLast())) {
            if (request.isHoisted)
                continue;
            if (request.targetModule() == module)
                return true;
            if (!seen.quickSet(request.target()))
                work.append(request.targetModule());
        }
    }
    return false;
}

void Modules::deferMockAfterAsyncFailure(ModuleID module)
{
    leaveImportersAbove(module);
    m_modulesWithPendingMock.set(module.index());
    if (m_importsMadeByOwnFactory.removeAll(module) && !importsItselfByStatementsInTestRunFactories(m_table, module))
        m_originalsDueUnderMock.set(module.index());
}

bool Modules::isOriginalDelegatedToHost(ModuleID mocked, const BitVector& held)
{
    if (!data(mocked).isESModuleOfProgram() || data(mocked).isFacade() || !host().evaluateOriginalBeforeFactory)
        return true;
    BitVector seen(m_table.size());
    seen.quickSet(mocked.index());
    Vector<ModuleID, 16> work { mocked };
    while (!work.isEmpty()) {
        for (uint32_t number : m_table.evaluationRequests(work.takeLast())) {
            ModuleID imported(number);
            if (held.quickGet(number) || imported == mocked)
                continue;
            if (hasPendingMock(fileOf(imported)) || m_modulesWithPendingMock.get(imported.index()))
                return true;
            if (!seen.quickSet(number) && status(imported) <= ModuleStatus::Linked && !isMocked(imported))
                work.append(imported);
        }
    }
    return false;
}

Identifier Modules::actualKey(ModuleID file)
{
    return Identifier::fromString(*m_instance.vm, makeString(m_table.fullPath(file), "?actual"_s));
}

JSObject* Modules::actualKeyNamespace(ModuleID mocked)
{
    ModuleRegistryEntry* entry = loader()->registryEntry(actualKey(mocked));
    auto* record = entry && entry->record() ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
    if (!record || record->status() != CyclicModuleRecord::Status::Evaluated || record->evaluationError())
        return nullptr;
    return record->getModuleNamespace(m_instance.globalObject);
}

bool Modules::hasLoaderSettledOriginalOf(ModuleID mocked)
{
    ModuleRegistryEntry* entry = loader()->registryEntry(actualKey(mocked));
    auto* record = entry && entry->record() ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
    return record && record->status() == CyclicModuleRecord::Status::Evaluated;
}

void Modules::pruneActualKeyLoads()
{
    m_pendingActualKeyLoads.removeAllMatching([&](const auto& copy) {
        if (copy.second->status() != JSPromise::Status::Pending)
            return true;
        ModuleRegistryEntry* entry = loader()->registryEntry(actualKey(copy.first));
        auto* record = entry && entry->record() ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
        return record && record->status() >= CyclicModuleRecord::Status::Evaluating;
    });
}

void Modules::didFinishTests()
{
    m_didFinishTests = true;
    for (ModuleID root : m_factoryBlockedImportRoots)
        m_factoryBlockedRootsSnapshot.set(root.index());
    for (const ImportAwaitingSource& each : m_importsAwaitingSource) {
        if (each.isMockImport)
            m_mockImportTargetsSnapshot.set(fileOf(each.module).index());
    }
    for (const auto& held : std::exchange(m_factoryBlockedFetches, { })) {
        if (held.second->status() == JSPromise::Status::Pending)
            m_modulesWithAbandonedFactory.set(held.first.index());
    }
}

bool Modules::isBlockedOnAbandonedFactory(ModuleID file) const
{
    if (m_modulesWithAbandonedFactory.isEmpty()) [[likely]]
        return false;
    BitVector reached = modulesInLoadGraph(file, LoadGraphMode::IgnoreMocks);
    reached.quickClear(file.index());
    reached.filter(m_modulesWithAbandonedFactory);
    return !reached.isEmpty();
}

bool Modules::hasOriginalGraphStartedEvaluating(ModuleID file)
{
    for (size_t reached : modulesInLoadGraph(file, LoadGraphMode::IgnoreMocks)) {
        if (ModuleStatus own = status(ModuleID(reached)); own == ModuleStatus::Evaluating || own == ModuleStatus::EvaluatingAsync)
            return true;
        ModuleRegistryEntry* entry = loader()->registryEntry(actualKey(fileOf(ModuleID(reached))));
        auto* record = entry && entry->record() ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
        if (!record || record->status() < CyclicModuleRecord::Status::Evaluating)
            continue;
        if (ModuleID(reached) == file || record->status() <= CyclicModuleRecord::Status::EvaluatingAsync)
            return true;
    }
    return false;
}

bool Modules::hasLateImportAwaitingSource()
{
    return m_didFinishTests && m_factoryBlockedImportRoots.containsIf([&](ModuleID root) {
        ModuleID file = fileOf(root);
        if (!m_mockImportTargetsSnapshot.get(file.index()) || m_factoryBlockedRootsSnapshot.get(root.index()) || wasOriginalEvaluated(file) || isBlockedOnAbandonedFactory(file))
            return false;
        return !hasOriginalGraphStartedEvaluating(file);
    });
}

bool Modules::hasUnstartedActualKeyLoad()
{
    pruneActualKeyLoads();
    return m_pendingActualKeyLoads.containsIf([&](const auto& copy) { return !isBlockedOnAbandonedFactory(copy.first) && !hasOriginalGraphStartedEvaluating(copy.first); });
}

auto Modules::runOriginalInSweep(ModuleID mocked) -> OriginalInSweep
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue thrownByBody = errorOfModuleThatRanBesideMock(mocked)) [[unlikely]] {
        if (wasDisplacedAfterFailure(mocked))
            return OriginalInSweep::DelegatedToHost;
        throwException(m_instance.globalObject, scope, thrownByBody);
        return OriginalInSweep::Failed;
    }
    if (wasOriginalEvaluated(mocked) || hasStartedLoading(mocked))
        return OriginalInSweep::Evaluated;
    bool hasFactory = false;
    JSPromise* evaluation;
    {
        SetForScope originalBeingEvaluated(m_originalBeingEvaluated, mocked);
        evaluation = host().evaluateOriginalBeforeFactory(m_instance.globalObject, mocked, hasFactory);
    }
    for (ImportersEnteredAboveMock& each : m_importersEnteredAboveMocks) {
        if (each.mocked == mocked)
            endPassing(each);
    }
    RETURN_IF_EXCEPTION(scope, OriginalInSweep::Failed);
    if (!evaluation)
        return OriginalInSweep::DelegatedToHost;
    evaluation->markAsHandled();
    if (evaluation->status() == JSPromise::Status::Rejected) {
        throwException(m_instance.globalObject, scope, evaluation->result());
        return OriginalInSweep::Failed;
    }
    if (!hasFactory && evaluation->status() == JSPromise::Status::Fulfilled) {
        bool hasNotSettled = runPendingMock(mocked);
        RETURN_IF_EXCEPTION(scope, OriginalInSweep::Failed);
        if (!hasNotSettled)
            leaveImportersAbove(mocked);
        return OriginalInSweep::Evaluated;
    }
    for (ImportersEnteredAboveMock& each : m_importersEnteredAboveMocks) {
        if (each.mocked == mocked)
            each.original = Strong<JSPromise>(vm, evaluation);
        if (each.mocked == mocked)
            each.mockHasFactory = hasFactory;
    }
    return OriginalInSweep::Evaluated;
}

bool Modules::sweepAheadOfFactories(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    struct Entered {
        ModuleID module;
        ModuleNumbers requests;
        size_t next { 0 };
        Vector<ModuleID, 4> finished { };
        bool isHeld { false };
    };
    SetForScope rootWhoseLoadIsSwept(m_rootWhoseLoadIsSwept, root);
    if (std::exchange(m_lastSweptRoot, root) != root)
        m_modulesPendingAfterSweep.clearAll();
    auto leavesImportersWhenSweepEnds = makeScopeExit([&] {
        leaveImportersWhere([&](const ImportersEnteredAboveMock& above) { return above.isLeftWhenSweepEnds; });
    });
    SetForScope currentImportTarget(m_currentImportTarget, ModuleID());
    SetForScope currentImportWaitState(m_currentImportWaitState, JSValue());
    SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, std::nullopt);
    SetForScope moduleBeingFetched(m_moduleBeingFetched, std::nullopt);
    BitVector seen(m_table.size());
    BitVector held(m_table.size());
    BitVector isOnPath(m_table.size());
    seen.quickSet(root.index());
    isOnPath.quickSet(root.index());
    Vector<Entered, 16> path { Entered { root, m_table.evaluationRequests(root) } };

    auto evaluateNow = [&](ModuleID module) {
        if (status(module) <= ModuleStatus::Linked) {
            JSPromise* evaluation = evaluate(module, std::nullopt, nullptr, nullptr);
            RETURN_IF_EXCEPTION(scope, false);
            evaluation->markAsHandled();
            if (status(module) == ModuleStatus::EvaluatingAsync)
                m_modulesPendingAfterSweep.set(module.index());
        }
        return status(module) != ModuleStatus::Failed;
    };
    auto holdPath = [&] {
        for (Entered& each : path) {
            each.isHeld = true;
            held.quickSet(each.module.index());
        }
        for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
            if (above.root == root)
                above.entered.merge(held);
        }
        for (size_t index = 0; index < path.size(); ++index) {
            for (ModuleID finished : std::exchange(path[index].finished, { })) {
                bool hasRun = evaluateNow(finished);
                RETURN_IF_EXCEPTION(scope, false);
                if (!hasRun)
                    return false;
            }
        }
        return true;
    };
    auto fail = [&] {
        leaveImportersWhere([&](const ImportersEnteredAboveMock& above) { return above.root == root; });
        return false;
    };
    bool hasPassedMock = false;
    auto endsBehindMockThatWasPassed = [&](std::optional<ModuleID> mockNotReached) {
        if (!hasPassedMock || (scope.exception() && !scope.tryClearException()))
            return false;
        if (mockNotReached)
            leaveImportersWhere([&](const ImportersEnteredAboveMock& above) { return above.root == root && above.mocked == *mockNotReached; });
        return true;
    };
    auto noteMock = [&](ModuleID mocked) {
        if (m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == mocked && above.root == root; }))
            return false;
        size_t ofAnotherLoad = m_importersEnteredAboveMocks.findIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == mocked && above.original; });
        Strong<JSPromise> original = ofAnotherLoad == notFound ? Strong<JSPromise>() : m_importersEnteredAboveMocks[ofAnotherLoad].original;
        m_importersEnteredAboveMocks.append({ mocked, root, BitVector(m_table.size()), BitVector(m_table.size()), WTF::move(original) });
        m_importersEnteredAboveMocks.last().mockHasFactory = ofAnotherLoad != notFound && m_importersEnteredAboveMocks[ofAnotherLoad].mockHasFactory;
        return true;
    };

    if (needsOriginalBeforeFactory(root)) {
        if (isOriginalDelegatedToHost(root, held) || !noteMock(root))
            return true;
        OriginalInSweep original = runOriginalInSweep(root);
        EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::Failed);
        return original == OriginalInSweep::Failed ? fail() : true;
    }
    while (!path.isEmpty()) {
        if (path.last().next == path.last().requests.size()) {
            Entered finished = path.takeLast();
            isOnPath.quickClear(finished.module.index());
            if (path.isEmpty() || finished.isHeld)
                continue;
            if (!path.last().isHeld) {
                path.last().finished.append(finished.module);
                continue;
            }
            bool hasRun = evaluateNow(finished.module);
            if (scope.exception())
                return fail();
            if (!hasRun)
                break;
            continue;
        }
        ModuleID imported(path.last().requests[path.last().next++]);
        if (data(imported).isFacade() && needsOriginalBeforeFactory(fileOf(imported)))
            break;
        bool isSeen = seen.quickSet(imported.index());
        if (needsOriginalBeforeFactory(imported)) {
            noteMock(imported);
            bool hasRun = holdPath();
            if (scope.exception())
                return fail();
            if (!hasRun) {
                if (endsBehindMockThatWasPassed(imported))
                    break;
                return fail();
            }
            if (!needsOriginalBeforeFactory(imported)) {
                fail();
                RELEASE_AND_RETURN(scope, sweepAheadOfFactories(root));
            }
            if (isOriginalDelegatedToHost(imported, held))
                break;
            bool hasBeenEvaluated = m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == imported && above.original; });
            OriginalInSweep original = hasBeenEvaluated ? OriginalInSweep::Evaluated : runOriginalInSweep(imported);
            EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::Failed);
            if (original == OriginalInSweep::DelegatedToHost)
                break;
            if (original == OriginalInSweep::Failed) {
                if (endsBehindMockThatWasPassed(imported))
                    break;
                return fail();
            }
            hasPassedMock = true;
            continue;
        }
        if (isSeen) {
            if (!held.quickGet(imported.index()) || isOnPath.quickGet(imported.index()))
                continue;
            bool hasRun = holdPath();
            if (scope.exception())
                return fail();
            if (!hasRun) {
                if (endsBehindMockThatWasPassed(std::nullopt))
                    break;
                return fail();
            }
            continue;
        }
        if (status(imported) == ModuleStatus::Failed) {
            if (path.last().isHeld)
                break;
            path.last().finished.append(imported);
            continue;
        }
        if (isMocked(imported) || status(imported) > ModuleStatus::Linked || !data(imported).isESModuleOfProgram())
            continue;
        isOnPath.quickSet(imported.index());
        path.append(Entered { imported, m_table.evaluationRequests(imported) });
    }
    for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks)
        endPassing(above);
    return true;
}

bool Modules::loadWaitsForOriginalBelow(ModuleID root) const
{
    return m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) {
        return above.root == root && above.original && (!above.loadHasWaitedForOriginal || above.original->status() == JSPromise::Status::Pending);
    });
}

JSPromise* Modules::originalThatLoadWaitsForBelow(ModuleID root)
{
    JSPromise* original = nullptr;
    for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
        if (above.root != root || !above.original)
            continue;
        bool isUnderWay = above.original->status() == JSPromise::Status::Pending;
        if ((isUnderWay || !above.loadHasWaitedForOriginal) && (!original || isUnderWay))
            original = above.original.get();
        above.loadHasWaitedForOriginal = true;
    }
    return original;
}

void Modules::evaluateImportersBehindModuleThatHasRun(ModuleID module, BitVector& seen)
{
    if (m_importersEnteredAboveMocks.isEmpty() || m_evaluatesImportersBehindModulesThatHaveRun || m_rootWhoseLoadIsSwept) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope evaluatesImporters(m_evaluatesImportersBehindModulesThatHaveRun, true);
    BitVector importers(m_table.size());
    for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
        endPassing(above);
        importers.merge(above.entered);
    }
    seen.ensureSize(m_table.size());
    Vector<ModuleID, 16> work { module };
    while (!work.isEmpty()) {
        ModuleID next = work.takeLast();
        if (seen.quickSet(next.index()) || isMocked(next))
            continue;
        if (status(next) > ModuleStatus::Linked) {
            ModuleNumbers requests = m_table.evaluationRequests(next);
            for (size_t index = requests.size(); index--;)
                work.append(ModuleID(requests[index]));
            continue;
        }
        if (!importers.quickGet(next.index()))
            continue;
        JSPromise* evaluation = evaluate(next, std::nullopt, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        evaluation->markAsHandled();
    }
}

void Modules::evaluateImportersBehindModuleThatHasRun(ModuleID module, ModuleID root, const ModuleEvaluationStack& stack, BitVector& seen)
{
    bool passesImporters = m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) {
        return isWalkOfOriginal(above.mocked, root, stack);
    });
    if (!passesImporters)
        evaluateImportersBehindModuleThatHasRun(module, seen);
}

bool Modules::isDeferredByCurrentSweep(ModuleID module) const
{
    return m_rootWhoseLoadIsSwept && m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) {
        return above.root == m_rootWhoseLoadIsSwept && above.entered.quickGet(module.index());
    });
}

bool Modules::isWalkOfOriginal(ModuleID mocked, ModuleID root, const ModuleEvaluationStack& stack) const
{
    if (root == mocked)
        return true;
    return stack.containsIf([&](AbstractModuleRecord* record) {
        return originalNamedBy(record->moduleKey()) == mocked;
    });
}

bool Modules::passImporterEnteredAboveMock(ModuleID module, ModuleID root, const ModuleEvaluationStack& stack)
{
    if (m_importersEnteredAboveMocks.isEmpty() || status(module) != ModuleStatus::Linked) [[likely]]
        return false;
    for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
        bool isMockOfOriginalUnderWay = module == above.mocked && m_originalBeingEvaluated == module;
        if (!isMockOfOriginalUnderWay && (!above.entered.quickGet(module.index()) || (above.root != m_rootWhoseLoadIsSwept && !isWalkOfOriginal(above.mocked, root, stack))))
            continue;
        above.passed.quickSet(module.index());
        setStatus(module, ModuleStatus::Evaluating);
        if (ModuleRecord* record = recordIfExists(module))
            record->setEvaluationStack(nullptr);
        return true;
    }
    return false;
}

void Modules::endPassing(ImportersEnteredAboveMock& above)
{
    for (size_t importer : above.passed) {
        if (status(ModuleID(importer)) == ModuleStatus::Evaluating)
            setStatus(ModuleID(importer), ModuleStatus::Linked);
    }
    above.passed.clearAll();
}

template<typename Matches>
void Modules::leaveImportersWhere(const Matches& matches)
{
    m_importersEnteredAboveMocks.removeAllMatching([&](ImportersEnteredAboveMock& above) {
        if (!matches(above))
            return false;
        endPassing(above);
        return true;
    });
}

void Modules::leaveImportersAbove(ModuleID mocked)
{
    if (m_importersEnteredAboveMocks.isEmpty()) [[likely]]
        return;
    leaveImportersWhere([&](ImportersEnteredAboveMock& above) {
        if (above.mocked != mocked)
            return false;
        above.isLeftWhenSweepEnds = above.root == m_rootWhoseLoadIsSwept;
        return !above.isLeftWhenSweepEnds;
    });
}

void Modules::didBeginWalkOnEmptyStack(ModuleID root)
{
    if (m_importersEnteredAboveMocks.isEmpty()) [[likely]]
        return;
    leaveImportersWhere([&](const ImportersEnteredAboveMock& above) { return above.root == root; });
    for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks)
        endPassing(above);
}

bool Modules::isUnmockedInDetachedLoad(ModuleID root, ModuleID module) const
{
    return m_detachedLoads.containsIf([&](const DetachedLoad& left) {
        return left.required == root && left.wait->status() == JSPromise::Status::Pending && left.unmockedModules.get(module.index());
    });
}

bool Modules::isFetchedByPendingFactoryCall(ModuleID root, ModuleID mocked) const
{
    if (m_resumingDetachedLoad != root || !host().isFactoryCallPending || (m_modulesBeingRetried && m_modulesBeingRetried->get(mocked.index())))
        return false;
    return m_detachedLoads.containsIf([&](const DetachedLoad& left) {
        return left.required == root && left.wait->status() == JSPromise::Status::Pending && left.awaitedFactoryCalls.containsIf([&](const auto& call) {
            return call.first == mocked && host().isFactoryCallPending(m_instance.globalObject, call.second);
        });
    });
}

bool Modules::runMocksInGraphSlow(ModuleID root, LoadRootKind rootOfLoad)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!m_linksOfLoadsThatMayFail.isEmpty()) [[unlikely]]
        meetLinksOfLoadsThatMayFail(root);
    bool isRequireBlocked = false;
    auto bringsBackWhereLoadFails = makeScopeExit([&] {
        if (scope.exception()) [[unlikely]]
            bringBackLinksOfLoadThatFailed(root);
    });
    {
        LinksBeforeLoad before;
        before.outer = m_linksBeforeLoadInFlight;
        SetForScope keepsLinksBefore(m_linksBeforeLoadInFlight, &before);
        if (hasMocksToReinstall() || !m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesWithRemovedMock.isEmpty() || !m_modulesReleasedByLoader.isEmpty() || !m_importersRetainedAboveMock.isEmpty()) [[unlikely]] {
            SetForScope isDueModuleBlocked(m_isDueModuleBlocked, false);
            evaluateModulesOwedBelow(root);
            isRequireBlocked = m_isDueModuleBlocked && vm.aotState().requireRunsMocks;
            if (Exception* exception = scope.exception()) [[unlikely]] {
                bringBackLinks(before, LinksOfLoad::AreForgotten);
                if (!vm.isTerminationException(exception))
                    keepErrorOfLoad(root, exception->value(), [&](ModuleID each) { return isMocked(fileOf(each)) && m_modulesWithCachedMockError.get(fileOf(each).index()); });
                return false;
            }
        }
        if (!m_removedMocksInUse.isEmpty()) [[unlikely]]
            linkAnewWhoeverReadsEndedMockBelow(root);
        if (!m_modulesLoadedAnewWhoseNamesAreOwed.isEmpty()) [[unlikely]] {
            checkRequiredNamesOfModulesLoadedAnewBelow(root);
            if (scope.exception()) [[unlikely]] {
                bringBackLinks(before, LinksOfLoad::AreMetByLaterLoads);
                return false;
            }
        }
        if (!before.moments.isEmpty()) [[unlikely]] {
            LinksOfLoadThatMayFail* links = linksOfLoadWalkedFor(root);
            if (!links) {
                m_linksOfLoadsThatMayFail.append(LinksOfLoadThatMayFail { root, { }, { { root, status(root) == ModuleStatus::Evaluated } } });
                links = &m_linksOfLoadsThatMayFail.last();
            }
            links->before.moments.appendVector(before.moments);
            links->before.modulesReleasedByLoader.merge(before.modulesReleasedByLoader);
            links->before.importersOfMockThatEnded.merge(before.importersOfMockThatEnded);
        }
        if (!m_linksOfLoadsThatMayFail.isEmpty()) [[unlikely]]
            meetLinksOfLoadsThatMayFail(root);
    }
    if (m_modulesWithPendingMock.isEmpty()) [[likely]]
        return isRequireBlocked;
    MocksInGraph mocks = mocksInGraph(root);
    bool mayComeToOne = false;
    for (size_t module : m_modulesWithPendingMock)
        mayComeToOne |= !isMocked(ModuleID(module)) || !mocks.hides(ModuleID(module));
    if (!mayComeToOne)
        return isRequireBlocked;
    std::optional<ModuleID> fileOfRoot = m_table.moduleBehindFacade(root);
    bool walksInFrontOfMocks = rootOfLoad == LoadRootKind::StartsLoad && m_modulesWithRunningFactory.isEmpty() && !m_evaluatesSynchronously && !m_rootWhoseLoadIsSwept && !vm.aotState().requireRunsMocks && !vm.synchronousModuleQueue();
    bool hasCalledOrdinaryFactories = !walksInFrontOfMocks;
    bool hasPutMockAside = false;
    bool hasPendingFactory = isRequireBlocked;
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty() || std::exchange(hasPutMockAside, false)) {
        if (work.isEmpty()) {
            hasCalledOrdinaryFactories = true;
            reached.clearAll();
            work.append(root);
        }
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()) || isDeferredByCurrentSweep(module) || m_originalBeingEvaluated == module)
            continue;
        bool isImportedByStatement = (module != root && module != fileOfRoot) || rootOfLoad == LoadRootKind::ImportStatement;
        JSValue kept = mockLoadError(module);
        if (!kept && isImportedByStatement && m_modulesWithCachedMockError.get(module.index()))
            kept = loadedIfExists(module)->cachedMockError.get();
        bool isRequestedByOwnFactory = !kept && !isImportedByStatement && m_moduleThatProgramImportsNow != module && m_modulesWithCachedMockError.get(module.index()) && m_importsMadeByOwnFactory.removeFirst(module);
        if (isRequestedByOwnFactory && importsItselfByStatementsInTestRunFactories(m_table, module)) [[unlikely]]
            kept = loadedIfExists(module)->cachedMockError.get();
        else if (isRequestedByOwnFactory) [[unlikely]]
            m_originalsDueUnderMock.set(module.index());
        if (kept) [[unlikely]] {
            keepErrorOfLoad(root, kept, [&](ModuleID each) { return fileOf(each) == fileOf(module); });
            throwException(m_instance.globalObject, scope, kept);
            return false;
        }
        bool runsMock = m_modulesWithPendingMock.get(module.index()) && !isMockLingering(module) && (isImportedByStatement || !m_originalsDueUnderMock.get(module.index())) && (m_isResumingFactoryChainImport || !isUnmockedInDetachedLoad(root, module)) && !isFetchedByPendingFactoryCall(root, module);
        bool isPutAside = runsMock && isImportedByStatement && !hasCalledOrdinaryFactories && m_mocksRetriedAfterFailure.get(module.index());
        hasPutMockAside |= isPutAside;
        if (runsMock && !isPutAside) {
            if (walksInFrontOfMocks && m_mocksRetriedAfterFailure.get(module.index())) {
                bool hasRun = sweepAheadOfFactories(root);
                RETURN_IF_EXCEPTION(scope, false);
                if (!hasRun)
                    return false;
                if (loadWaitsForOriginalBelow(root))
                    return true;
                if (!m_modulesWithPendingMock.get(module.index())) {
                    reached.quickClear(module.index());
                    work.append(module);
                    continue;
                }
            }
            bool factoryHasNotSettled = runPendingMock(module);
            if (!factoryHasNotSettled)
                leaveImportersAbove(module);
            if (Exception* exception = scope.exception()) [[unlikely]] {
                if (vm.isTerminationException(exception) || m_mocksRetriedAfterFailure.get(module.index()))
                    return false;
                keepErrorOfLoad(root, exception->value(), [&](ModuleID each) { return fileOf(each) == fileOf(module); });
                if (isImportedByStatement) {
                    m_modulesWithCachedMockError.set(module.index());
                    ensureLoaded(module).cachedMockError.set(*this, exception->value());
                }
                return false;
            }
            if (factoryHasNotSettled) {
                if (!m_shouldContinuePastPendingFactories)
                    return true;
                hasPendingFactory = true;
            }
        }
        if (isMocked(module) && !isPutAside && (module != root || !mocks.loadsRootOriginal()))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module) | std::views::reverse) {
            if (!request.isHoisted && (!request.isRequire || data(module).isFacade()) && leadsToWhatIsImported(moduleLoadedBy(request), module) && !keepsMockThatStandsWhileAnotherWaits(moduleLoadedBy(request), module) && (data(module).isFacade() || !isLinkedToRetainedMock(fileOf(moduleLoadedBy(request)), module)))
                work.append(moduleLoadedBy(request));
        }
    }
    return hasPendingFactory;
}

bool Modules::leadsToWhatIsImported(ModuleID imported, ModuleID importer) const
{
    if (seesMockOf(imported, importer)) [[likely]]
        return true;
    ModuleID file = fileOf(imported);
    return !isMocked(file) && !activeMock(file) && !m_modulesWithPendingMock.get(file.index()) && keepsOriginalAfterRemoval(file, importer);
}

BitVector Modules::modulesInLoadGraph(ModuleID root, LoadGraphMode graphMode) const
{
    BitVector reached(m_table.size());
    bool loadsRootOriginal = graphMode != LoadGraphMode::RespectMocks || mocksInGraph(root).loadsRootOriginal();
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID next = work.takeLast();
        if (reached.quickSet(next.index()) || (graphMode != LoadGraphMode::IgnoreMocks && isMocked(next) && !needsOriginalBeforeFactory(next) && (next != root || !loadsRootOriginal)))
            continue;
        for (const ModuleRequestData& request : m_table.requests(next)) {
            if (!request.isHoisted && leadsToWhatIsImported(moduleLoadedBy(request), next))
                work.append(moduleLoadedBy(request));
        }
    }
    return reached;
}

BitVector Modules::modulesInRequireGraph(ModuleID root) const
{
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID next = work.takeLast();
        if (reached.quickSet(next.index()) || isMocked(next) || hasStartedLoading(next))
            continue;
        for (const ModuleRequestData& request : m_table.requests(next)) {
            if (!request.isHoisted && !request.isRequire)
                work.append(moduleLoadedBy(request));
        }
    }
    return reached;
}

std::optional<ModuleID> Modules::loadOfFetch(ModuleID fetched)
{
    ModuleID file = fileOf(fetched);
    bool isOrWillBeMocked = isMocked(fetched) || isMocked(file) || m_modulesWithPendingMock.get(fetched.index()) || m_modulesWithPendingMock.get(file.index());
    return isOrWillBeMocked ? std::nullopt : std::optional(file);
}

JSPromise* Modules::pendingFactoryForFetch(ModuleID fetched, JSValue& state)
{
    SetForScope moduleBeingFetched(m_moduleBeingFetched, loadOfFetch(fetched));
    return pendingFactoryInGraph(fetched, PendingFactoryRequester::Loader, state);
}

bool Modules::tellHostOfRequire(ModuleID required)
{
    if (!m_hasBegunTestFile) [[likely]]
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    JSValue state;
    JSPromise* factory = pendingFactoryInGraph(required, PendingFactoryRequester::Require, state);
    scope.assertNoExceptionExceptTermination();
    return factory;
}

JSPromise* Modules::pendingFactoryInGraph(ModuleID root, PendingFactoryRequester askedBy, JSValue& state)
{
    if (JSPromise* original = originalThatLoadWaitsForBelow(root)) [[unlikely]]
        return original;
    if (!host().nextPendingFactory)
        return nullptr;
    std::optional<BitVector> reached;
    JSPromise* factory = host().nextPendingFactory(m_instance.globalObject, [&](ModuleID module) {
        if (!reached)
            reached = askedBy == PendingFactoryRequester::Require ? modulesInRequireGraph(root) : modulesInLoadGraph(root);
        return reached->quickGet(module.index());
    }, askedBy, state);
    if (factory && askedBy == PendingFactoryRequester::Loader && !m_didFinishTests) [[unlikely]] {
        m_factoryBlockedFetches.removeAllMatching([](const auto& held) { return held.second->status() != JSPromise::Status::Pending; });
        m_factoryBlockedFetches.append({ root, Strong<JSPromise>(*m_instance.vm, factory) });
    }
    return factory;
}

void evaluateCommonJSModuleBeforeMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "evaluateCommonJSModuleBeforeMock"_s);
    modules->evaluateCommonJSModuleBeforeMock(module);
}

AbstractModuleRecord* originalModuleRecord(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules || !modules->wasOriginalEvaluated(module))
        return nullptr;
    ModuleRecord* record = modules->ensureDisplacedRecord(module);
    record->setKeepsToModule();
    return record;
}

JSObject* evaluateOriginalUnderMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return nullptr;
    MockDoor door(modules, module, "evaluateOriginalUnderMock"_s);
    return modules->evaluateOriginalUnderMock(module);
}

bool startDetachedOriginalLoad(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->startDetachedOriginalLoad(module);
}

bool wasOriginalEvaluated(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasOriginalEvaluated(module);
}

bool tryEvaluateESModuleBeforeMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "tryEvaluateESModuleBeforeMock"_s);
    return modules && modules->tryEvaluateESModuleBeforeMock(module);
}

JSPromise* pendingEvaluation(JSGlobalObject* realm, ModuleID module, bool hasAlreadyWaited)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->pendingEvaluation(module, hasAlreadyWaited) : nullptr;
}

JSPromise* pendingEvaluationInGraph(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->pendingEvaluationInGraph(file) : nullptr;
}

bool evaluateLinkedModuleBeforeMock(JSGlobalObject* realm, ModuleID module, bool displacesEarlierMock, bool displacesModule, bool keepsOriginalIntact)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the mock call"_s);
    if (!modules)
        return false;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    if (modules->isMockLingering(module))
        modules->removeLingeringMocks();
    RETURN_IF_EXCEPTION(scope, false);
    modules->removeUnusedLingeringMocks();
    RETURN_IF_EXCEPTION(scope, false);
    if (displacesModule || !modules->hasFailedWithoutMock(module))
        modules->forgetLoadThatFailed(module);
    modules->countMocksAnewBehindLoad(module);
    modules->noteCallOfMock(module);
    modules->noteWhetherFileHasThrownWhereMockIsMade(module, displacesEarlierMock, displacesModule);
    modules->clearLoadErrorsForMock(module);
    RELEASE_AND_RETURN(scope, modules->evaluateLinkedModuleBeforeMock(module, keepsOriginalIntact));
}

void markMockLingering(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the mock is gone behind its factory"_s);
    if (modules && modules->activeMock(module))
        modules->markMockLingering(module);
}

bool isMockLingering(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isMockLingering(module);
}

bool isRequestedByDetachedLoad(JSGlobalObject* realm, ModuleID mocked)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isRequestedByDetachedLoad(mocked);
}

void Modules::retryMockInDetachedLoads(ModuleID mocked)
{
    for (DetachedLoad& left : m_detachedLoads) {
        if (left.awaitedFactoryCalls.containsIf([&](const auto& call) { return call.first == mocked; }))
            left.modulesToRetry.set(mocked.index());
    }
}

void retryMockInDetachedLoads(JSGlobalObject* realm, ModuleID mocked)
{
    if (Modules* modules = modulesOf(realm))
        modules->retryMockInDetachedLoads(mocked);
}

bool needsRetryInDetachedLoad(JSGlobalObject* realm, ModuleID mocked)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->needsRetryInDetachedLoad(mocked);
}

bool hasDelayedImport(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasDelayedImport(module);
}

void removeUnusedLingeringMocks(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->removeUnusedLingeringMocks();
}

void clearMockLoadErrors(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->clearEvictableFailures();
}

void finishLoadsAwaitingLoader(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->finishLoadsAwaitingLoader(std::nullopt);
}

void discardDetachedLoadErrors(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->discardDetachedLoadErrors();
}

void retainFactoryLoadedImporters(JSGlobalObject* realm, ModuleID mocked)
{
    if (Modules* modules = modulesOf(realm))
        modules->retainFactoryLoadedImporters(mocked);
}

void deferMockAfterAsyncFailure(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the factory failed behind an await"_s);
    if (modules)
        modules->deferMockAfterAsyncFailure(module);
}

bool importsStatically(JSGlobalObject* realm, ModuleID importer, ModuleID imported)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->importsStatically(importer, imported);
}

void setNeedsMockReinstall(JSGlobalObject* realm, ModuleID module, bool waits)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, waits ? "the mocks of its body wait for the next load"_s : "the mocks of its body are laid again"_s);
    if (modules)
        modules->setNeedsMockReinstall(module, waits);
}

JSPromise* pendingFactoryInGraph(JSGlobalObject* realm, ModuleID module, JSValue& state)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->pendingFactoryForFetch(module, state) : nullptr;
}

void willFetchAfterTick(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->willFetchAfterTick(module);
}

void didResumeFetchAfterFactory(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->didResumeAfterFactories();
}

bool didMockFailForImportStatement(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->didMockFailForImportStatement(module);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
