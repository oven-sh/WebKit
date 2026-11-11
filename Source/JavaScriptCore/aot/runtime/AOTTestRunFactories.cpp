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

bool Modules::forgetLoadThatFailed(ModuleID file)
{
    resetPackageFailureIfNeeded(file);
    if (!data(file).runsAgainAfterThrow())
        return false;
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
        return false;
    discardFailedEvaluation(file);
    Vector<Identifier, 4> failedWithIt;
    for (auto& [key, entry] : loader()->moduleMap()) {
        if (entry->status() >= ModuleRegistryEntry::Status::FetchFailed)
            failedWithIt.append(Identifier::fromUid(*m_instance.vm, key.first));
    }
    for (const Identifier& key : failedWithIt)
        loader()->removeEntry(key);
    return true;
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
            if (isLinkedToReleasedInstance(importer, imported))
                continue;
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

std::optional<ModuleID> Modules::findUnlinkableModule(ModuleID root) const
{
    if (std::optional<ModuleFailureID> id = failureByAbsentExport(root))
        return m_table.failure(*id).referrerModule();
    auto failure = m_linkFailures.find(root);
    return failure == m_linkFailures.end() ? std::nullopt : std::optional(failure->value);
}

bool Modules::wasLinkedAheadOfFailure(ModuleID root, ModuleID module) const
{
    std::optional<ModuleID> referrer = findUnlinkableModule(root);
    if (!referrer)
        return false;
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
            if (step.importer == *referrer)
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
    if (!hasLinkFailures() || !data(module).isESModuleOfProgram())
        return false;
    if (status(module) == ModuleStatus::Failed)
        return !!failureByAbsentExport(module);
    if (status(module) > ModuleStatus::Linked)
        return false;
    bool wasPulledIn = false;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (!findUnlinkableModule(root) || !modulesInLoadGraph(root).get(module.index()))
            continue;
        if (wasLinkedAheadOfFailure(root, module))
            return false;
        wasPulledIn = true;
    }
    return wasPulledIn;
}

bool Modules::wasFetchedByFailedLoad(ModuleID module) const
{
    if (!hasLinkFailures()) [[likely]]
        return false;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (root != module && findUnlinkableModule(root) && modulesInLoadGraph(root).get(module.index()))
            return true;
    }
    return false;
}

bool Modules::wasLinkedBeforeLinkFailure(ModuleID module) const
{
    if (!hasLinkFailures())
        return false;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (findUnlinkableModule(root) && modulesInLoadGraph(root).get(module.index()) && wasLinkedAheadOfFailure(root, module))
            return true;
    }
    return false;
}

bool Modules::isLinkedForPendingImporter(ModuleID module, std::optional<ModuleID> asking) const
{
    if (!hasLinkFailures() || status(module) > ModuleStatus::Linked || !wasLinkedBeforeLinkFailure(module))
        return false;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID importer(index);
        if (importer == module || importer == asking || !data(importer).isESModuleOfProgram() || data(importer).isFacade() || status(importer) > ModuleStatus::Linked || isOfPackage(importer))
            continue;
        bool importsModule = std::ranges::any_of(m_table.imports(importer), [&](const ModuleRequestData& request) {
            return !request.isHoisted && moduleLoadedBy(request) == module;
        });
        if (importsModule && wasLinkedBeforeLinkFailure(importer))
            return true;
    }
    return false;
}

bool Modules::hasImporterLinkedBeforeFailure(ModuleID module)
{
    tryLinkFailedLoads();
    if (!activeMock(module) && !m_linkedInstances.get(module.index()))
        return false;
    listImportersAndRequirers();
    bool isPackage = isOfPackage(module);
    bool onlyByFailedLoads = isGraphRunning();
    return std::ranges::any_of(m_importersByStatement[module.index()], [&](uint32_t importer) {
        if (isPackage && isOfPackage(ModuleID(importer)))
            return false;
        if (onlyByFailedLoads && !m_linkedByFailedLoads.get(importer))
            return false;
        if (status(ModuleID(importer)) > ModuleStatus::Linked || !isLinkedToCurrentEntry(ModuleID(importer), module).value_or(false))
            return false;
        return data(ModuleID(importer)).needsEvaluation() || m_linkedByFailedLoads.get(importer) || (wasLinkedByFailedGraph(ModuleID(importer), false) && !wasReachedByFailedGraph(ModuleID(importer)));
    });
}

bool Modules::wasLinkedByFailedGraph(ModuleID module, bool byEarlierTestFile)
{
    return data(module).isESModuleOfProgram() && !data(module).isFacade() && status(module) <= ModuleStatus::Linked && !hasRun(module) && !didCaptureExports(module) && (!byEarlierTestFile || !wasLoadedByTestFile(module)) && isImportedByFailedRoot(module, byEarlierTestFile);
}

bool Modules::wasReachedByFailedGraph(ModuleID module) const
{
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID root(index);
        if (root == module || status(root) != ModuleStatus::Failed || !importsStatically(root, module))
            continue;
        for (size_t each : staticImportClosure(root, MockTraversal::Stop)) {
            ModuleID failed(each);
            if (status(failed) != ModuleStatus::Failed)
                continue;
            bool hasThrown = std::ranges::none_of(m_table.imports(failed), [&](const ModuleRequestData& request) {
                return status(request.targetModule()) == ModuleStatus::Failed;
            });
            if (hasThrown && findModulesLinkedBefore(root, failed).get(module.index()))
                return true;
        }
    }
    return false;
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

void Modules::resetChecksOfNames()
{
    if (!m_checksOfNames) {
        m_checksOfNames = Vector<ModuleID>();
        for (const ModuleRequestData& request : m_table.allRequests()) {
            const ModuleData& target = data(request.targetModule());
            if (request.isHoisted && target.kind == ModuleKind::SourceText && !target.isFacade())
                m_checksOfNames->append(request.targetModule());
        }
    }
    for (ModuleID check : *m_checksOfNames) {
        if (status(check) != ModuleStatus::Evaluated)
            continue;
        startLoadingModulesIndividually();
        takeOutRecord(check);
        setStoredStatus(check, StoredModuleStatus::Unlinked);
    }
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
    for (ModuleID root : m_linkFailures.keys()) {
        if (!hasInstances(root) || status(root) > ModuleStatus::Linked || isMocked(root))
            continue;
        startLoadingModulesIndividually();
        loader()->removeEntry(moduleKey(root));
        takeOutRecord(root);
        setStoredStatus(root, StoredModuleStatus::Unlinked);
        m_linkedInstances.clear(root.index());
        didChangeLinkState();
        forgetLoad(root);
    }
    forgetFailuresOfExportsThatBuildFoundAbsent();
}

void Modules::forgetFailuresOfExportsThatBuildFoundAbsent()
{
    m_linkFailures.clear();
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
    if (!data(module).isESModuleOfProgram() || data(module).isFacade() || !isUnevaluated(module) || isMocked(module) || m_modulesWithPendingMock.get(module.index()))
        return false;
    bool endsBeforeLink = m_table.hasLoadFailures() && loadFailure(module);
    auto waitsForFactories = [&] {
        for (size_t index : staticImportClosure(module, MockTraversal::Continue)) {
            if (ModuleID below(index); isMockPending(fileOf(below)) || (isMocked(below) && !activeMock(below)))
                return true;
        }
        return false;
    };
    bool isInSweptGraph = m_rootWhoseLoadIsSwept && m_rootWhoseLoadIsSwept != module && staticImportClosure(*m_rootWhoseLoadIsSwept, MockTraversal::Stop).get(module.index()) && !waitsForFactories();
    if (endsBeforeLink || (!isInSweptGraph && !isImportedByGraphUnderEvaluation(module, keepsOriginalIntact) && (keepsOriginalIntact || (!wasLinkedBeforeLinkFailure(module) && !hasImporterLinkedBeforeFailure(module) && !(isLinkedAndUnevaluated(module) && m_linkedByFailedLoads.get(module.index()))))))
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

bool Modules::tryStartAsyncOriginal(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!hasInstances(module) || hasStartedLoading(module) || isEvicted(module))
        return false;
    forgetMockThatWaits(module);
    RETURN_IF_EXCEPTION(scope, false);
    SetForScope shouldContinuePastPendingFactories(m_shouldContinuePastPendingFactories, true);
    evaluateSynchronously(module, SynchronousEvaluationReason::Require);
    if (scope.exception() && !scope.tryClearException()) [[unlikely]]
        return false;
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
    }
    if (!wasOriginalEvaluated(module) && !tryRestoreCompiledOriginalUnderMock(module)) {
        bool awaitsFetch = importsUnloadedCode(module);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (awaitsFetch)
            return nullptr;
        VisitedModuleRecords seen;
        bool isReady = readyForSyncExecution(module, seen);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!isReady)
            return nullptr;
        didRemoveMock(module);
        bool wasMockPending = m_modulesWithPendingMock.get(module.index());
        m_modulesWithPendingMock.clear(module.index());
        {
            SetForScope moduleEvaluatedAsOriginal(m_moduleEvaluatedAsOriginal, module);
            evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        }
        m_modulesWithPendingMock.set(module.index(), wasMockPending);
        willMock(module);
        if (status(module) == ModuleStatus::Evaluated || status(module) == ModuleStatus::Failed)
            didEvaluateOriginalUnderMock(module);
        m_failedInstancesBesideMocks.set(module.index(), !!scope.exception());
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
    auto earlier = m_pendingMockCalls.find(fileOrModule);
    call.wasStoredDuringImport = isAwaitedByImport(fileOrModule) || !m_importsAwaitingFirstStep.isEmpty() || (earlier != m_pendingMockCalls.end() && earlier->value.wasStoredDuringImport);
    if (call.wasStoredDuringImport)
        noteCompiledOriginal(fileOrModule, OriginalRun::MockedDuringImport);
    for (unsigned index = 0; index < m_table.size() && !call.wasStoredByImportedFile; ++index)
        call.wasStoredByImportedFile = status(ModuleID(index)) == ModuleStatus::Evaluating && !data(ModuleID(index)).isEntryPoint();
    m_pendingMockCalls.set(fileOrModule, call);
}

void Modules::tellIfMockedBelowImport(ModuleID file, std::optional<ModuleID> caller)
{
    bool isLoaded = status(file) > ModuleStatus::Unlinked;
    bool isOver = !m_loadsInFlight.isEmpty() && status(file) == ModuleStatus::Evaluated && !isMocked(file) && !activeMock(file);
    bool isBesideHostImport = !(m_hostImportCalls.isEmpty() && m_importsInFlight.isEmpty()) && (isLoaded || isMocked(file)) && !isGraphRunning();
    bool ranDuringOwnFetch = status(file) == ModuleStatus::Evaluated && !isMocked(file) && !activeMock(file) && std::ranges::any_of(batchedFetches(), [&](const BatchedFetch& fetch) { return fileOf(fetch.module) == file; });
    bool isOriginalAwaited = needsOriginalBeforeFactory(file) && data(moduleWithExportsOf(file)).isImportedByModule() && m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& waiting) { return waiting.isMockImport && fileOf(waiting.module) == file; });
    if ((!isOver && !isBesideHostImport && !ranDuringOwnFetch && !isOriginalAwaited) || !host().didRunOriginal) [[likely]]
        return;
    auto leadsToFile = [&](ModuleID root) {
        bool found = false;
        BitVector seen(m_table.size());
        Vector<ModuleID, 16> work { root };
        while (!work.isEmpty()) {
            ModuleID module = work.takeLast();
            if (seen.quickSet(module.index()))
                continue;
            if (caller && fileOf(module) == fileOf(*caller))
                return false;
            found |= fileOf(module) == file;
            for (const ModuleRequestData& request : m_table.requests(module)) {
                if (!request.isHoisted)
                    work.append(moduleLoadedBy(request));
            }
        }
        return found;
    };
    auto isBelowImport = [&] {
        for (LoadInFlight& load : m_loadsInFlight) {
            if (isOver && load.promise->status() == JSPromise::Status::Pending && leadsToFile(load.root))
                return true;
        }
        for (auto& [module, promise] : m_hostImportCalls) {
            if (isBesideHostImport && promise->status() == JSPromise::Status::Pending && leadsToFile(module))
                return true;
        }
        for (auto& [promise, import] : m_importsInFlight) {
            if (isBesideHostImport && promise->status() == JSPromise::Status::Pending && leadsToFile(import.module))
                return true;
        }
        return false;
    };
    if (ranDuringOwnFetch || isOriginalAwaited || isBelowImport())
        host().didRunOriginal(m_instance.globalObject, m_table.fullPath(file), OriginalRun::MockedDuringImport);
}

void Modules::noteHostImportCall(ModuleID module, JSPromise* promise)
{
    m_hostImportCalls.removeAllMatching([](const auto& call) { return call.second->status() != JSPromise::Status::Pending; });
    if (promise->status() == JSPromise::Status::Pending)
        m_hostImportCalls.append({ module, Strong<JSPromise>(*m_instance.vm, promise) });
}

void Modules::tellIfFactoryImportsAbsentExport(ModuleID requested)
{
    if (!host().didRunOriginal || status(requested) > ModuleStatus::Linked)
        return;
    for (size_t index : modulesInRequireGraph(requested)) {
        bool importsAbsentExport = m_table.importsAbsentExport(ModuleID(index));
        for (const ModuleRequestData& request : m_table.imports(ModuleID(index))) {
            ModuleID mocked = moduleLoadedBy(request);
            if (m_modulesWithRunningFactory.get(mocked.index()) && m_table.hasFilePath(mocked) && (importsAbsentExport || m_table.exports(mocked).empty()))
                host().didRunOriginal(m_instance.globalObject, m_table.fullPath(mocked), OriginalRun::RunsAmongLoads);
        }
    }
}

void Modules::forgetCallOfMock(ModuleID fileOrModule)
{
    m_pendingMockCalls.remove(fileOrModule);
}

void Modules::deferMock(ModuleID module, FactoryFailurePolicy failurePolicy, bool isFromPreload)
{
    m_unseenMocks.clear(fileOf(module).index());
    finishLoadsAwaitingLoader(module);
    keepMockForPendingImports(module, MockChange::Replaced);
    noteDeedOnMockUnderCallsOfImport(module, DeedOnMock::LaysAnother);
    bool wasEvaluatedBeforeTestFile = data(module).kind == ModuleKind::CommonJs && wasLoadedBeforeTestFile(module) && !wasLoadedByTestFile(moduleWithExportsOf(module));
    bool isOriginalOutsideRegistry = hasInstances(module) && m_originalsEvaluatedUnderMock.get(module.index()) && !m_modulesWhoseFactoryWasCalled.get(module.index());
    if ((m_modulesWithDeferredMock.get(module.index()) || wasEvaluatedBeforeTestFile) && !isOriginalOutsideRegistry && !activeMock(module) && !isPatchedByMock(module))
        releaseImportersOf(module, isOfPackage(module) && hasIntactOriginal(module) ? ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule : ReleasedImporters::All);
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
        m_pendingMockCalls.find(module)->value.isInPreload |= isFromPreload;
    }
}

void Modules::removeUnusedLingeringMocksSlow(ModuleID requested)
{
    if (m_hasImportsBlockedOnFactories || !m_modulesWithRunningFactory.isEmpty() || m_loadsInFlight.numberBeingLoaded())
        return;
    auto leadsToMockThatStands = [&](ModuleID root) {
        BitVector reached = modulesInLoadGraph(root);
        reached.filter(m_modulesWithLingeringMock);
        return !reached.isEmpty();
    };
    for (const DetachedLoad& left : m_detachedLoads) {
        bool joinsLoad = requested && !m_modulesWithLingeringMock.get(fileOf(requested).index()) && !m_modulesWithLingeringMock.get(requested.index()) && modulesInLoadGraph(left.required).get(requested.index());
        if (left.wait->status() == JSPromise::Status::Pending && leadsToMockThatStands(left.required) && !(requested && !joinsLoad && leadsToMockThatStands(requested)))
            return;
    }
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
    AOT::didRemoveMock(m_instance.globalObject, module, RetainedOriginalPolicy::DiscardWithMock, wasInstalledOnPreloadMock, m_originalsRestoredAtUnmock.get(module.index()));
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
    clearPendingMockState(module);
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
        retainFactoryLoadedImporters(module, FactoryOutcome::Threw);
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
        didChangeLinkState();
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

bool Modules::hidesFailureOfLoad(ModuleID mocked, ModuleID root)
{
    bool comesToMock = false;
    BitVector reached(m_table.size());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()))
            continue;
        comesToMock |= module == mocked;
        if (module == mocked || isMocked(module))
            continue;
        if (m_failedLoadModules.get(module.index()))
            return false;
        for (const ModuleRequestData& request : m_table.requests(module))
            work.append(moduleLoadedBy(request));
    }
    BitVector hidden = staticImportClosure(mocked, MockTraversal::Continue);
    hidden.filter(m_failedLoadModules);
    return comesToMock && !hidden.isEmpty();
}

void Modules::forgetFailedLoadsAt(ModuleID mocked)
{
    if (m_failedLoadModules.isEmpty()) [[likely]]
        return;
    Vector<std::pair<ModuleID, Identifier>, 4> failed;
    for (auto& entry : loader()->moduleMap().values()) {
        std::optional<ModuleID> root = findByKey(entry->key());
        if (!root)
            continue;
        ModuleRecord* record = m_records.get(*root);
        bool hasFailed = entry->status() >= ModuleRegistryEntry::Status::FetchFailed || failureByAbsentExport(*root) || (record && record->hasUnresolvableRequest());
        if (hasFailed && hidesFailureOfLoad(mocked, *root))
            failed.append({ *root, entry->key() });
    }
    if (failed.isEmpty())
        return;
    startLoadingModulesIndividually();
    for (auto& [root, key] : failed) {
        loader()->removeEntry(key);
        if (!m_records.contains(root) || status(root) == ModuleStatus::Evaluating)
            continue;
        takeOutRecord(root);
        setStoredStatus(root, StoredModuleStatus::Unlinked);
        m_linkedInstances.clear(root.index());
        forgetLoad(root);
    }
}

void Modules::clearLoadErrorsForMock(ModuleID mocked)
{
    forgetFailedLoadsAt(mocked);
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
    bool isAskedForByImportCall = m_instance.vm->aotState().importFrameInLoader;
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
    m_statementFetchesDelayedByTick.set(module.index(), !m_instance.vm->aotState().importFrameInLoader && isFetchedForImportStatement(module));
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
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (ModuleID target = moduleLoadedBy(request); !isLinkedToReleasedInstance(module, target))
                work.append(target);
        }
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

bool Modules::isOriginalDelegatedToHost(ModuleID mocked, const BitVector& held, const BitVector& isOnPath)
{
    bool isOtherFile = data(mocked).mayRunAsOriginal() || isDataFile(mocked);
    if ((!data(mocked).isESModuleOfProgram() && !isOtherFile) || data(mocked).isFacade() || !host().evaluateOriginalBeforeFactory)
        return true;
    BitVector seen(m_table.size());
    seen.quickSet(mocked.index());
    Vector<ModuleID, 16> work { mocked };
    while (!work.isEmpty()) {
        for (uint32_t number : m_table.evaluationRequests(work.takeLast())) {
            ModuleID imported(number);
            if (isOnPath.quickGet(number) || imported == mocked)
                continue;
            if (held.quickGet(number))
                return true;
            if (hasPendingMock(fileOf(imported)) || m_modulesWithPendingMock.get(imported.index())) {
                if (!needsOriginalBeforeFactory(imported) || !host().hasSynchronousFactory || !host().hasSynchronousFactory(m_instance.globalObject, m_table.fullPath(fileOf(imported)), data(imported).isSynchronous() && !importsStatically(imported, mocked)))
                    return true;
                continue;
            }
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

bool Modules::displacesRequiredMock(ModuleID mocked)
{
    ModuleID file = fileOf(mocked);
    if (!activeMock(file) || !hasInstances(file) || wasOriginalEvaluatedUnderMock(file))
        return false;
    auto settlesSynchronously = [&](bool isOriginalSynchronous) {
        return host().hasSynchronousFactory && host().hasSynchronousFactory(m_instance.globalObject, m_table.fullPath(file), isOriginalSynchronous);
    };
    if (settlesSynchronously(true) != settlesSynchronously(false))
        return false;
    if (ensureLoaded(moduleWithExportsOf(file)).nextRequestReloadsActiveMock)
        return true;
    auto waiting = m_pendingMockCalls.find(file);
    return waiting != m_pendingMockCalls.end() && waiting->value.keepsActiveMock && ensureLoaded(file).isKeptByRequire && !ensureLoaded(moduleWithExportsOf(file)).wasImportedUnderActiveMock;
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
    if (wasOriginalEvaluated(mocked) || (hasStartedLoading(mocked) && !displacesRequiredMock(mocked))) {
        if (!host().isSpy || !host().isSpy(m_instance.globalObject, m_table.fullPath(fileOf(mocked)))) [[likely]]
            return OriginalInSweep::Evaluated;
        bool hasNotSettled = runPendingMock(mocked);
        RETURN_IF_EXCEPTION(scope, OriginalInSweep::Failed);
        if (!hasNotSettled)
            leaveImportersAbove(mocked);
        return OriginalInSweep::Evaluated;
    }
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

Vector<ModuleID, 4> Modules::commonJSOriginalsInGraph(ModuleID root)
{
    Vector<ModuleID, 4> originals;
    BitVector seen(m_table.size());
    seen.quickSet(root.index());
    Deque<ModuleID> requested;
    requested.append(root);
    while (!requested.isEmpty()) {
        ModuleID importer = requested.takeFirst();
        ModuleID file = fileOf(importer);
        if (data(file).mayRunAsOriginal() && needsOriginalBeforeFactory(file)) {
            if (!originals.contains(file))
                originals.append(file);
            continue;
        }
        if (isMocked(importer) || status(importer) > ModuleStatus::Linked || !data(importer).isESModuleOfProgram())
            continue;
        for (const ModuleRequestData& request : m_table.requests(importer)) {
            if (!request.isRequire && !request.isHoisted && !seen.quickSet(request.target()))
                requested.append(request.targetModule());
        }
    }
    return originals;
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
    SetForScope importDepths(m_importDepthsInSweep, Vector<uint32_t>());
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

    BitVector notedMocks;
    BitVector mocksWithOriginal;
    bool areMocksListed = false;
    bool knowWhatIsHeld = false;
    auto didRunCode = [&] {
        areMocksListed = false;
        knowWhatIsHeld = false;
    };
    auto listMocks = [&] {
        if (std::exchange(areMocksListed, true))
            return;
        notedMocks.clearAll();
        mocksWithOriginal.clearAll();
        for (const ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
            if (above.root == root)
                notedMocks.set(above.mocked.index());
            if (above.original)
                mocksWithOriginal.set(above.mocked.index());
        }
    };
    auto evaluateNow = [&](ModuleID module) {
        if (status(module) <= ModuleStatus::Linked) {
            didRunCode();
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
            knowWhatIsHeld &= held.quickSet(each.module.index());
        }
        if (!std::exchange(knowWhatIsHeld, true)) {
            for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks) {
                if (above.root == root)
                    above.entered.merge(held);
            }
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
        listMocks();
        if (notedMocks.set(mocked.index()))
            return false;
        knowWhatIsHeld = false;
        size_t ofAnotherLoad = m_importersEnteredAboveMocks.findIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == mocked && above.original; });
        Strong<JSPromise> original = ofAnotherLoad == notFound ? Strong<JSPromise>() : m_importersEnteredAboveMocks[ofAnotherLoad].original;
        m_importersEnteredAboveMocks.append({ mocked, root, BitVector(m_table.size()), BitVector(m_table.size()), WTF::move(original) });
        m_importersEnteredAboveMocks.last().mockHasFactory = ofAnotherLoad != notFound && m_importersEnteredAboveMocks[ofAnotherLoad].mockHasFactory;
        return true;
    };

    auto mockedModule = [&](ModuleID requested) {
        ModuleID file = fileOf(requested);
        return data(file).mayRunAsOriginal() && needsOriginalBeforeFactory(file) ? file : requested;
    };
    if (ModuleID mocked = mockedModule(root); needsOriginalBeforeFactory(mocked)) {
        if (isOriginalDelegatedToHost(mocked, held, isOnPath) || !noteMock(mocked))
            return true;
        OriginalInSweep original = runOriginalInSweep(mocked);
        EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::Failed);
        return original == OriginalInSweep::Failed ? fail() : true;
    }
    for (ModuleID mocked : commonJSOriginalsInGraph(root)) {
        if (isOriginalDelegatedToHost(mocked, held, isOnPath) || !noteMock(mocked))
            continue;
        didRunCode();
        OriginalInSweep original = runOriginalInSweep(mocked);
        EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::Failed);
        if (original == OriginalInSweep::Failed)
            return fail();
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
        if (hasPassedMock && data(imported).isESModuleOfProgram() && !data(imported).isFacade() && m_table.fullPath(imported).startsWith('#'))
            continue;
        ModuleID mocked = mockedModule(imported);
        if (data(mocked).isFacade() && needsOriginalBeforeFactory(fileOf(mocked)))
            break;
        bool isSeen = seen.quickSet(imported.index());
        if (needsOriginalBeforeFactory(mocked)) {
            noteMock(mocked);
            bool hasRun = holdPath();
            if (scope.exception())
                return fail();
            if (!hasRun) {
                if (endsBehindMockThatWasPassed(mocked))
                    break;
                return fail();
            }
            if (!needsOriginalBeforeFactory(mocked)) {
                fail();
                RELEASE_AND_RETURN(scope, sweepAheadOfFactories(root));
            }
            if (isOriginalDelegatedToHost(mocked, held, isOnPath))
                break;
            listMocks();
            OriginalInSweep original = OriginalInSweep::Evaluated;
            if (!mocksWithOriginal.get(mocked.index())) {
                didRunCode();
                original = runOriginalInSweep(mocked);
            }
            EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::Failed);
            if (original == OriginalInSweep::DelegatedToHost)
                break;
            if (original == OriginalInSweep::Failed) {
                if (endsBehindMockThatWasPassed(mocked))
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
        if (data(imported).isUncompiledCode() && status(imported) <= ModuleStatus::Linked) {
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

const Vector<uint32_t>& Modules::importDepthsInSweep()
{
    if (!m_importDepthsInSweep.isEmpty())
        return m_importDepthsInSweep;
    m_importDepthsInSweep.fill(0, m_table.size());
    BitVector seen(m_table.size());
    seen.quickSet(m_rootWhoseLoadIsSwept->index());
    Vector<ModuleID, 16> level { *m_rootWhoseLoadIsSwept };
    for (uint32_t depth = 1; !level.isEmpty(); ++depth) {
        Vector<ModuleID, 16> next;
        for (ModuleID importer : level) {
            if (isMocked(importer) || isCommonJSModuleOrFacade(importer))
                continue;
            for (const ModuleRequestData& request : m_table.requests(importer)) {
                if (request.isRequire || request.isHoisted)
                    continue;
                if (uint32_t& known = m_importDepthsInSweep[fileOf(request.targetModule()).index()]; !known)
                    known = depth;
                if (!seen.quickSet(request.target()))
                    next.append(request.targetModule());
            }
        }
        level = WTF::move(next);
    }
    return m_importDepthsInSweep;
}

bool Modules::isOriginalLoadedBySweep(ModuleID mocked)
{
    const Vector<uint32_t>& depths = importDepthsInSweep();
    uint32_t depth = depths[fileOf(mocked).index()];
    if (!depth || !m_commonJSModuleBeforeFactories)
        return depth;
    uint32_t depthOfRequirer = depths[fileOf(*m_commonJSModuleBeforeFactories).index()];
    return !depthOfRequirer || depth <= depthOfRequirer + 1;
}

void Modules::rejectRequireInSweep(ModuleID target)
{
    if (!m_rootWhoseLoadIsSwept || !m_modulesWithPendingMock.get(target.index()) || !isMockedByFactoryThatTakesOriginal(target))
        return;
    if (data(target).isESModuleOfProgram() ? wasOriginalEvaluated(target) : !data(target).mayRunAsOriginal() || m_filesWithCompiledOriginal.get(fileOf(target).index()))
        return;
    if (!isOriginalLoadedBySweep(target) || !host().shouldBypassMock || !host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(target), String(), false))
        return;
    m_filesWithRejectedRequire.set(fileOf(target).index());
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    throwException(m_instance.globalObject, scope, host().createAsyncModuleRequiredError(m_instance.globalObject, m_table.fullPath(target)));
}

std::optional<bool> Modules::evaluateCommonJSModulesBeforeFactories(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope currentImportTarget(m_currentImportTarget, ModuleID());
    SetForScope currentImportWaitState(m_currentImportWaitState, JSValue());
    SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, std::nullopt);
    SetForScope moduleBeingFetched(m_moduleBeingFetched, std::nullopt);
    SetForScope rootWhoseLoadIsSwept(m_rootWhoseLoadIsSwept, root);
    SetForScope importDepths(m_importDepthsInSweep, Vector<uint32_t>());
    MocksInGraph mocks = mocksInGraph(root);
    bool hasRun = false;
    for (const ModuleRequestData& request : m_table.requests(root)) {
        ModuleID imported = request.targetModule();
        if (!request.isHoisted || request.isRequire || !isCommonJSModuleOrFacade(imported) || isMocked(fileOf(imported)) || mocks.hides(imported))
            continue;
        if (status(imported) > ModuleStatus::Linked)
            continue;
        hasRun = true;
        SetForScope commonJSModule(m_commonJSModuleBeforeFactories, imported);
        JSPromise* evaluation = evaluate(imported, std::nullopt, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        evaluation->markAsHandled();
        if (status(imported) == ModuleStatus::Failed)
            return std::nullopt;
    }
    return hasRun;
}

void Modules::evaluatePrecedingCommonJSModules(std::span<const ModuleID> files)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope currentImportTarget(m_currentImportTarget, ModuleID());
    SetForScope currentImportWaitState(m_currentImportWaitState, JSValue());
    SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, std::nullopt);
    SetForScope moduleBeingFetched(m_moduleBeingFetched, std::nullopt);
    for (ModuleID file : files) {
        if (status(file) > ModuleStatus::Linked || isMocked(fileOf(file)))
            continue;
        JSPromise* evaluation = evaluate(file, std::nullopt, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        evaluation->markAsHandled();
        if (status(file) == ModuleStatus::Failed)
            return;
    }
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

const BitVector& Modules::modulesRequestedBefore(FactoryCaller& caller)
{
    if (caller.requested)
        return *caller.requested;
    caller.requested.emplace(m_table.size());
    BitVector& requested = *caller.requested;
    Vector<ModuleID, 32> work { caller.root };
    for (size_t next = 0; next < work.size() && !requested.get(caller.mocked.index()); ++next) {
        ModuleID importer = work[next];
        if ((next && isMocked(importer)) || hasRun(importer))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID target = moduleLoadedBy(request);
            if (!request.isDeferred && !requested.quickSet(target.index()))
                work.append(target);
        }
    }
    return requested;
}

bool Modules::wasRequestedByFactoryCaller(ModuleID mocked)
{
    for (FactoryCaller* caller = m_innermostFactoryCaller; caller; caller = caller->outer) {
        if (caller->root != mocked && caller->mocked != mocked && modulesRequestedBefore(*caller).get(mocked.index()))
            return true;
    }
    return false;
}

bool Modules::awaitsGeneratedModule(ModuleID mocked)
{
    bool isAwaited = m_instance.vm->aotState().requireRunsMocks && wasRequestedByFactoryCaller(mocked);
    m_awaitedGeneratedModules.set(mocked.index(), isAwaited);
    return isAwaited;
}

bool Modules::isHeldByDetachedLoad(ModuleID mocked) const
{
    if (!host().isFactoryCallPending)
        return false;
    return m_detachedLoads.containsIf([&](const DetachedLoad& left) {
        return left.wait->status() == JSPromise::Status::Pending && !left.modulesToRetry.get(mocked.index()) && left.awaitedFactoryCalls.containsIf([&](const auto& call) {
            return call.first == mocked && !host().isFactoryCallPending(m_instance.globalObject, call.second);
        });
    });
}

bool Modules::hasUnresolvableRequestInGraph(ModuleID root)
{
    if (m_table.modulesOfUncompiledCode().empty()) [[likely]]
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (seen.quickSet(module.index()))
            continue;
        if (data(module).isUncompiledCode()) {
            resolvedKey(module);
            if (!scope.exception()) [[likely]]
                continue;
            static_cast<void>(scope.tryClearException());
            return true;
        }
        if (!data(module).isESModuleOfProgram() || status(module) > ModuleStatus::Linked || (module != root && isMocked(module) && !needsOriginalBeforeFactory(module)))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !request.isDeferred)
                work.append(moduleLoadedBy(request));
        }
    }
    return false;
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
    bool isRequired = vm.aotState().requireRunsMocks && m_currentRequireTarget == root;
    std::optional<bool> endsBeforeLink;
    bool hasCalledOrdinaryFactories = !walksInFrontOfMocks;
    bool hasPutMockAside = false;
    bool hasEvaluatedCommonJSModules = false;
    bool hasPendingFactory = isRequireBlocked;
    BitVector reached(m_table.size());
    bool isBreadthFirst = isRequired || m_evaluatesSynchronously;
    struct FailedRequest {
        ModuleID mocked;
        bool isRetried;
        JSValue error;
    };
    std::optional<FailedRequest> failedRequest;
    struct Request {
        ModuleID module;
        ModuleID importer;
        uint32_t depth { 0 };
    };
    Deque<Request, 32> work;
    Vector<std::pair<ModuleID, uint32_t>, 4> precedingCommonJSModules;
    auto addRequest = [&](ModuleID requested, ModuleID requester, ModuleID importerOfRequester, uint32_t depth) {
        if (isBreadthFirst && data(requester).isFacade())
            work.prepend(Request { requested, importerOfRequester, depth });
        else
            work.append({ requested, requester, depth + 1 });
    };
    work.append({ root, ModuleID() });
    while (!work.isEmpty() || std::exchange(hasPutMockAside, false)) {
        if (work.isEmpty()) {
            hasCalledOrdinaryFactories = true;
            reached.clearAll();
            work.append({ root, ModuleID() });
        }
        auto [module, importer, depth] = isBreadthFirst ? work.takeFirst() : work.takeLast();
        if (reached.quickSet(module.index()) || (!isRequired && isDeferredByCurrentSweep(module)) || m_originalBeingEvaluated == module)
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
        bool isLeftToHost = isEvicted(fileOf(root)) && hasPendingMockAfterReset(module);
        bool runsMock = m_modulesWithPendingMock.get(module.index()) && !isLeftToHost && !isMockLingering(module) && (isImportedByStatement || !m_originalsDueUnderMock.get(module.index())) && (m_isResumingFactoryChainImport || !isUnmockedInDetachedLoad(root, module)) && !isFetchedByPendingFactoryCall(root, module);
        bool isPutAside = runsMock && isImportedByStatement && m_mocksRetriedAfterFailure.get(module.index());
        if (isPutAside) {
            if (!endsBeforeLink)
                endsBeforeLink = hasUnresolvableRequestInGraph(root);
            isPutAside = !hasCalledOrdinaryFactories || *endsBeforeLink;
            hasPutMockAside |= isPutAside && !*endsBeforeLink;
        }
        if (runsMock && !isPutAside) {
            if (walksInFrontOfMocks && isImportedByStatement && !hasEvaluatedCommonJSModules) [[unlikely]] {
                hasEvaluatedCommonJSModules = true;
                std::optional<bool> hasRun = evaluateCommonJSModulesBeforeFactories(root);
                RETURN_IF_EXCEPTION(scope, false);
                if (!hasRun) {
                    if (needsOriginalBeforeFactory(module)) {
                        runOriginalInSweep(module);
                        if (scope.exception() && !scope.tryClearException()) [[unlikely]]
                            return false;
                    }
                    return false;
                }
                if (*hasRun) {
                    reached.clearAll();
                    work.clear();
                    work.append({ root, ModuleID() });
                    continue;
                }
            }
            if (walksInFrontOfMocks && m_mocksRetriedAfterFailure.get(module.index())) {
                bool hasRun = sweepAheadOfFactories(root);
                RETURN_IF_EXCEPTION(scope, false);
                if (!hasRun)
                    return false;
                if (loadWaitsForOriginalBelow(root))
                    return true;
                if (!m_modulesWithPendingMock.get(module.index())) {
                    reached.quickClear(module.index());
                    work.append({ module, importer, depth });
                    continue;
                }
            }
            if (m_rootWhoseLoadIsSwept && m_rootWhoseLoadIsSwept != root && module == root && m_mocksRetriedAfterFailure.get(module.index()) && needsOriginalBeforeFactory(module)) [[unlikely]] {
                runOriginalInSweep(module);
                RETURN_IF_EXCEPTION(scope, false);
                if (!m_modulesWithPendingMock.get(module.index()))
                    continue;
            }
            m_mocksCalledAgain.set(module.index(), m_mocksCalledInTestFile.set(module.index()));
            FactoryCaller caller { root, module, m_innermostFactoryCaller };
            bool isCalledByRequire = vm.aotState().requireRunsMocks || m_evaluatesSynchronously;
            if (!precedingCommonJSModules.isEmpty() && precedingCommonJSModules.first().second < depth) {
                Vector<ModuleID, 4> inFront;
                precedingCommonJSModules.removeAllMatching([&](const auto& each) {
                    if (each.second >= depth)
                        return false;
                    inFront.append(each.first);
                    return true;
                });
                evaluatePrecedingCommonJSModules(inFront);
                RETURN_IF_EXCEPTION(scope, false);
                if (!m_modulesWithPendingMock.get(module.index()))
                    continue;
            }
            if (!isCalledByRequire) [[likely]]
                didCallFactoryOfLoad(fileOf(module));
            SetForScope innermostFactoryCaller(m_innermostFactoryCaller, isCalledByRequire ? m_innermostFactoryCaller : &caller);
            SetForScope loadCallingFactory(m_loadCallingFactory, module != root && module != fileOfRoot ? std::optional(root) : std::nullopt);
            bool factoryHasNotSettled = runPendingMock(module);
            if (m_mocksImportedByFactories.clear(module.index()) && !factoryHasNotSettled && !scope.exception()) [[unlikely]]
                host().didRunOriginal(m_instance.globalObject, m_table.fullPath(module), OriginalRun::RunsAmongLoads);
            if (!factoryHasNotSettled)
                leaveImportersAbove(module);
            if (Exception* exception = scope.exception()) [[unlikely]] {
                if (vm.isTerminationException(exception))
                    return false;
                bool isRetried = m_mocksRetriedAfterFailure.get(module.index());
                if (isImportedByStatement && !isRetried) {
                    m_modulesWithCachedMockError.set(module.index());
                    ensureLoaded(module).cachedMockError.set(*this, exception->value());
                }
                if (isBreadthFirst) {
                    if (!failedRequest)
                        failedRequest = FailedRequest { module, isRetried, exception->value() };
                    if (!scope.tryClearException())
                        return false;
                    work.removeAllMatching([&](const Request& request) { return request.importer == importer; });
                    continue;
                }
                if (!isRetried)
                    keepErrorOfLoad(root, exception->value(), [&](ModuleID each) { return fileOf(each) == fileOf(module); });
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
        if (isRequired && module != root && data(module).isFacade() && data(root).kind == ModuleKind::SourceText && isCommonJSModuleOrFacade(module) && !mocks.hides(module) && status(module) <= ModuleStatus::Linked)
            precedingCommonJSModules.append({ module, depth });
        auto requests = m_table.requests(module);
        for (size_t index = 0; index < requests.size(); ++index) {
            const ModuleRequestData& request = requests[isBreadthFirst ? index : requests.size() - 1 - index];
            if (!request.isHoisted && (!request.isRequire || data(module).isFacade()) && leadsToWhatIsImported(moduleLoadedBy(request), module) && !(m_modulesWithPendingMock.get(request.target()) && isEvaluatedWithOriginal(module, request.targetModule())) && !keepsMockThatStandsWhileAnotherWaits(moduleLoadedBy(request), module) && (data(module).isFacade() || !isLinkedToRetainedMock(fileOf(moduleLoadedBy(request)), module)))
                addRequest(moduleLoadedBy(request), module, importer, depth);
        }
    }
    if (failedRequest) [[unlikely]] {
        if (!failedRequest->isRetried)
            keepErrorOfLoad(root, failedRequest->error, [&](ModuleID each) { return fileOf(each) == fileOf(failedRequest->mocked); });
        throwException(m_instance.globalObject, scope, failedRequest->error);
        return false;
    }
    return hasPendingFactory;
}

bool Modules::leadsToWhatIsImported(ModuleID imported, ModuleID importer) const
{
    if (isLinkedToReleasedInstance(importer, imported)) [[unlikely]]
        return false;
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

bool tryStartAsyncOriginal(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "tryStartAsyncOriginal"_s);
    return modules && modules->tryStartAsyncOriginal(module);
}

void restoreOriginalForRequire(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->restoreOriginalForRequire(module);
}

bool isOriginalEvaluatedOutsideRegistry(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasInstances(module) && modules->wasOriginalEvaluatedUnderMock(module) && modules->status(module) == ModuleStatus::Evaluated;
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
    modules->tryLinkFailedLoads();
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    if (modules->isMockLingering(module) && !modules->isHeldByDetachedLoad(module))
        modules->removeLingeringMocks();
    RETURN_IF_EXCEPTION(scope, false);
    modules->removeUnusedLingeringMocks();
    RETURN_IF_EXCEPTION(scope, false);
    if (displacesModule ? !modules->hasInstances(module) : !modules->hasFailedWithoutMock(module))
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

uint64_t Modules::factoryCallOfResumedLoad(ModuleID mocked) const
{
    if (!m_resumingDetachedLoad)
        return 0;
    for (const DetachedLoad& left : m_detachedLoads) {
        if (left.required != *m_resumingDetachedLoad || left.wait->status() != JSPromise::Status::Pending)
            continue;
        for (auto& [awaited, call] : left.awaitedFactoryCalls) {
            if (awaited == mocked)
                return call;
        }
    }
    return 0;
}

uint64_t factoryCallOfResumedLoad(JSGlobalObject* realm, ModuleID mocked)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->factoryCallOfResumedLoad(mocked) : 0;
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
        modules->retainFactoryLoadedImporters(mocked, Modules::FactoryOutcome::Settled);
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

bool wasLoadedByPreloadHook(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLoadedByPreloadHook(module);
}

bool isImportedByStatement(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isImportedByStatement(module);
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
