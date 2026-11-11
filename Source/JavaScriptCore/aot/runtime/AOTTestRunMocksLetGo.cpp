/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTRealmStartup.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>

namespace JSC { namespace AOT {

bool Modules::isSomeLoadUnderWay() const
{
    if (m_instance.vm->synchronousModuleQueue() || m_rootWhoseFilesRun || m_rootAwaitingLoader || m_moduleWithDeferredJobs || !m_queuedFetches.isEmpty())
        return true;
    if (std::ranges::any_of(m_levelsOfLoads.values(), [&](auto& level) { return !hasGraphRun(*level); }))
        return true;
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return true;
    for (const LoadInFlight& load : m_loadsInFlight) {
        if (load.promise->status() == JSPromise::Status::Pending)
            return true;
    }
    return false;
}

bool Modules::willReloadAfterMockRemoval(ModuleID module) const
{
    return hasRemovedMock(module) || data(module).kind == ModuleKind::CommonJs || data(module).kind == ModuleKind::Data || isImportedByModuleThatRequireAnswered(module);
}

void Modules::bringBackMockOfPreloadThatWasPatched(ModuleID fileOrModule)
{
    if (!m_modulesWhoseMockOfPreloadIsPatched.get(fileOrModule.index()))
        return;
    LoadedModule& held = ensureLoaded(fileOrModule);
    setActiveMock(fileOrModule, held.preloadMock.get(), held.preloadMockRequireValue.get());
}

void Modules::releaseMockAtNextLoad(ModuleID module)
{
    finishLoadsAwaitingLoader(module);
    if (!data(module).isESModuleOfProgram() || data(module).isFacade() || isSomeLoadUnderWay() || isLoaderInMiddleOfGraph() || isImportedByModuleThatRequireAnswered(module)) {
        setActiveMock(module, nullptr, { });
        return;
    }
    m_modulesWithRemovedMock.set(module.index());
    m_modulesWithNothingOwedBelow.clearAll();
    m_modulesWithNoNamesOwedBelow.clearAll();
    m_testFilesInWhichMocksEnded.set(module, m_instance.vm->aotState().momentOfTestFile);
    if (ModuleRecord* record = recordIfExists(module); record && record->hasLiveExports())
        loader()->removeEntriesForRecord(record);
}

bool Modules::releaseLibraryMockOnLoad(ModuleID module)
{
    if (!m_libraryModulesWithMock.get(module.index()) || !exportsWhatMockNamesAlone(module) || m_modulesWhoseMockOfPreloadIsPatched.get(module.index()) || isSomeLoadUnderWay() || isLoaderInMiddleOfGraph())
        return false;
    m_modulesWithRemovedMock.set(module.index());
    m_modulesWithNothingOwedBelow.clearAll();
    m_modulesWithNoNamesOwedBelow.clearAll();
    return true;
}

void Modules::releaseRemovedMockSlow(ModuleID module)
{
    if (data(module).isOfLibrary() || !m_table.hasFilePath(module)) {
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        m_modulesWithRemovedMock.clear(module.index());
        if (activeMock(module)) {
            setActiveMock(module, nullptr, { });
            RETURN_IF_EXCEPTION(scope, void());
        }
        RELEASE_AND_RETURN(scope, endMockOfLibraryModule(module));
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    bool wasEvaluatedUnderMock = m_originalsEvaluatedUnderMock.get(module.index());
    setActiveMock(module, nullptr, { });
    scope.assertNoExceptionExceptTermination();
    if (m_modulesWithPendingMock.get(module.index())) {
        willMock(module);
        if (wasEvaluatedUnderMock)
            m_originalsEvaluatedUnderMock.set(module.index());
    }
}

void Modules::releaseRemovedMocks()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    removeLingeringMocks();
    RETURN_IF_EXCEPTION(scope, void());
    while (!m_modulesWithRemovedMock.isEmpty()) {
        releaseRemovedMock(ModuleID(*m_modulesWithRemovedMock.begin()));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool Modules::wasLoadedByTestFile(ModuleID module) const
{
    uint32_t momentOfTestFile = m_instance.vm->aotState().momentOfTestFile;
    if (!m_momentsOfLoads.isEmpty() && m_momentsOfLoads[module.index()] > momentOfTestFile)
        return true;
    auto anew = m_momentsLoadedAnew.find(module);
    return anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == momentOfTestFile;
}

void Modules::releaseImportersOf(ModuleID mocked, ReleasedImporters which)
{
    if (m_momentsOfLoads.isEmpty())
        return;
    bool keepsImportersOfTestFile = which == ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule;
    if (keepsImportersOfTestFile && wasLoadedByTestFile(mocked))
        return;
    listImportersAndRequirers();
    BitVector asImported(m_table.size());
    asImported.quickSet(mocked.index());
    asImported.quickSet(moduleWithExportsOf(mocked).index());
    BitVector seen = asImported;
    Vector<uint32_t, 32> work;
    for (size_t imported : seen)
        work.append(imported);
    while (!work.isEmpty()) {
        uint32_t imported = work.takeLast();
        for (uint32_t index : m_importersByStatement[imported]) {
            ModuleID importer(index);
            if (asImported.quickGet(imported) && keepsImportersOfTestFile && wasLoadedByTestFile(importer))
                continue;
            if (asImported.quickGet(imported) && isLinkedToRetainedMock(m_table.moduleBehindFacade(mocked).value_or(mocked), importer))
                continue;
            if (asImported.quickGet(imported) && isLinkedToOriginalOf(mocked, importer))
                continue;
            if (seen.quickSet(index))
                continue;
            bool isLoaded = status(importer) > ModuleStatus::Linked || (data(importer).isESModuleOfProgram() && wasLoadedWithGraph(importer));
            if (!isLoaded || m_loadedInPreload.get(index) || activeMock(importer) || hasIntactOriginal(importer))
                continue;
            m_modulesReleasedByLoader.set(index);
            if (asImported.quickGet(imported) ? !hasIntactOriginal(ModuleID(imported)) || seesMockOf(ModuleID(imported), importer) : m_importersOfMockThatEnded.get(imported))
                m_importersOfMockThatEnded.set(index);
            work.append(index);
        }
    }
    for (size_t imported : seen) {
        if (asImported.quickGet(imported) || m_modulesReleasedByLoader.get(imported))
            work.append(imported);
    }
    auto leftAlone = m_mocksWithIntactOriginal.find(m_table.moduleBehindFacade(mocked).value_or(mocked));
    const BitVector* holdOriginal = leftAlone != m_mocksWithIntactOriginal.end() && !wasRequiredUnderActiveMock(leftAlone->key) ? &leftAlone->value.filesThatHadRun : nullptr;
    while (!work.isEmpty()) {
        uint32_t imported = work.takeLast();
        bool isAboveRequirer = m_importersRetainedAboveMock.get(imported);
        auto visit = [&](uint32_t index) {
            ModuleID importer(index);
            if (asImported.quickGet(imported) && keepsImportersOfTestFile && wasLoadedByTestFile(importer))
                return;
            if (auto bypassing = m_requirersBypassingMock.find(static_cast<uint64_t>(mocked.index()) << 32 | index); asImported.quickGet(imported) && bypassing != m_requirersBypassingMock.end() && bypassing->value == m_timesMocked.get(mocked))
                return;
            if (asImported.quickGet(imported) && holdOriginal && holdOriginal->get(index))
                return;
            if (m_modulesReleasedByLoader.get(index) || status(importer) <= ModuleStatus::Linked || m_loadedInPreload.get(index) || activeMock(importer))
                return;
            if (m_importersRetainedAboveMock.set(index))
                return;
            ensureRecord(importer);
            work.append(index);
            if (ModuleID facade = moduleWithExportsOf(importer); facade != importer && !m_importersRetainedAboveMock.set(facade.index()))
                work.append(facade.index());
        };
        for (uint32_t index : m_requirers[imported])
            visit(index);
        if (auto answered = m_requirersThatRunTimeAnswered.find(imported); answered != m_requirersThatRunTimeAnswered.end()) {
            for (uint32_t index : answered->value)
                visit(index);
        }
        if (!isAboveRequirer)
            continue;
        for (uint32_t index : m_importersByStatement[imported])
            visit(index);
    }
}

void Modules::noteRequireThatRunTimeAnswers(ModuleID target, ModuleID referrer)
{
    if (m_requiresThatRunTimeAnswered.add((static_cast<uint64_t>(referrer.index()) + 1) << 32 | target.index()).isNewEntry)
        m_requirersThatRunTimeAnswered.add(target.index(), Vector<uint32_t, 2>()).iterator->value.append(referrer.index());
}

void Modules::listImportersAndRequirers()
{
    if (!m_importersByStatement.isEmpty())
        return;
    m_importersByStatement.grow(m_table.size());
    m_requirers.grow(m_table.size());
    m_takersOfNames.grow(m_table.size());
    for (uint32_t importer = 0; importer < m_table.size(); ++importer) {
        for (const ModuleRequestData& request : m_table.requests(ModuleID(importer))) {
            if (!request.isHoisted && !request.isDeferred)
                (request.isRequire ? m_requirers : m_importersByStatement)[moduleLoadedBy(request).index()].append(importer);
        }
        for (ModuleRequestData star : m_table.dynamicStars(ModuleID(importer)))
            m_takersOfNames[moduleLoadedBy(star).index()].append(importer);
        if (!data(ModuleID(importer)).hasExportTable())
            continue;
        for (const ModuleExportData& entry : m_table.exports(ModuleID(importer))) {
            if (entry.kind() == ModuleExportKind::Indirect)
                m_takersOfNames[m_table.indirectExport(entry).targetModule().index()].append(importer);
        }
    }
}

BitVector Modules::transitiveImportersOf(std::span<const ModuleID> files)
{
    listImportersAndRequirers();
    BitVector reaching(m_table.size());
    Vector<uint32_t, 32> work;
    auto enter = [&](ModuleID module) {
        for (ModuleID each : { module, moduleWithExportsOf(module), m_table.moduleBehindFacade(module).value_or(module) }) {
            if (!reaching.quickSet(each.index()))
                work.append(each.index());
        }
    };
    for (ModuleID file : files)
        enter(file);
    while (!work.isEmpty()) {
        uint32_t reached = work.takeLast();
        for (uint32_t taker : m_takersOfNames[reached])
            enter(ModuleID(taker));
    }
    return reaching;
}

void Modules::releaseFailedESModules()
{
    if (m_momentsOfLoads.isEmpty())
        return;
    for (uint32_t index = 0; index < m_table.size(); ++index) {
        ModuleID module(index);
        if (isFailedESModule(module) && !isMocked(module) && !activeMock(module)) {
            m_modulesReleasedByLoader.set(index);
            forgetFailedLoad(module);
        }
    }
}

void Modules::tellThatFileDidNotRunAgain(ModuleID module)
{
    if (status(module) <= ModuleStatus::Linked || !m_table.hasFilePath(module) || m_table.moduleBehindFacade(module))
        return;
    if (!m_instance.vm->aotState().filesReportedAsKept.set(module.index()))
        notifyRejectedChangeListener(m_instance.globalObject, makeString("Kept the run of '"_s, m_table.fullPath(module), '\''));
}

void Modules::reportRetainedFailure(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (LoadedModule* loaded = loadedIfExists(file); loaded && loaded->wasDisplacedAfterFailure) [[unlikely]]
        tellThatFileDidNotRunAgain(file);
}

void Modules::tellOfFilesThatStayedLoadedBelow(ModuleID root)
{
    if (m_importersRetainedAboveMock.isEmpty()) [[likely]]
        return;
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        if (isSeen.quickSet(module.index()))
            continue;
        if (m_importersRetainedAboveMock.clear(module.index()) && !(m_retainedFactoryImporters.clear(module.index()) && isEvicted(module)))
            tellThatFileDidNotRunAgain(module);
        if (isMocked(module) || activeMock(module))
            continue;
        if (std::optional<ModuleID> file = m_table.moduleBehindFacade(module))
            toVisit.append(*file);
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (!request.isHoisted && !request.isDeferred)
                toVisit.append(moduleLoadedBy(request));
        }
    }
}

void Modules::bringBackWhatLoaderLetGoBelow(ModuleID root)
{
    tellOfFilesThatStayedLoadedBelow(root);
    if (m_modulesReleasedByLoader.isEmpty()) [[likely]]
        return;
    auto take = [&](BitVector& list, BitVector LinksBeforeLoad::* taken, ModuleID module) {
        if (!list.clear(module.index()))
            return false;
        if (m_linksBeforeLoadInFlight)
            (m_linksBeforeLoadInFlight->*taken).set(module.index());
        return true;
    };
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        if (isSeen.quickSet(module.index()) || isMocked(module) || activeMock(module))
            continue;
        std::optional<ModuleID> file = m_table.moduleBehindFacade(module);
        bool wasLetGo = take(m_modulesReleasedByLoader, &LinksBeforeLoad::modulesReleasedByLoader, module);
        if (file)
            wasLetGo |= take(m_modulesReleasedByLoader, &LinksBeforeLoad::modulesReleasedByLoader, *file);
        if (!wasLetGo && status(module) > ModuleStatus::Linked)
            continue;
        if (wasLetGo) {
            tellThatFileDidNotRunAgain(module);
            if (m_hasBegunTestFile && isFailedESModule(module))
                m_refetchedFailures.set(module.index());
        }
        bool sawMockThatEnded = wasLetGo && take(m_importersOfMockThatEnded, &LinksBeforeLoad::importersOfMockThatEnded, module);
        if (wasLetGo && file)
            sawMockThatEnded |= take(m_importersOfMockThatEnded, &LinksBeforeLoad::importersOfMockThatEnded, *file);
        if (sawMockThatEnded)
            noteLoadedAnew(module, root);
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !request.isDeferred)
                toVisit.append(moduleLoadedBy(request));
        }
    }
}

bool Modules::evaluateModuleOwed(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!m_modulesOwedToImportersThatRan.get(module.index()))
        return false;
    if (ModuleID file = fileOf(module); m_modulesWithPendingMock.get(file.index())) {
        scope.release();
        bool factoryHasNotSettled = runMocksInGraph(file, LoadRootKind::ImportStatement);
        m_isDueModuleBlocked |= factoryHasNotSettled;
        return factoryHasNotSettled;
    }
    if (ModuleID file = fileOf(module); !activeMock(file) && pendingFactoryCallID(file)) {
        m_isDueModuleBlocked = true;
        return true;
    }
    m_modulesOwedToImportersThatRan.clear(module.index());
    auto staysOwedWhereItFails = makeScopeExit([&] {
        if (scope.exception()) [[unlikely]]
            m_modulesOwedToImportersThatRan.set(module.index());
    });
    followImportsAnewAtNextQuestion();
    ModuleStatus statusAtStart = status(module);
    if ((statusAtStart >= ModuleStatus::Evaluating && statusAtStart != ModuleStatus::Failed) || activeMock(module))
        return false;
    const ModuleData& moduleData = data(module);
    if (moduleData.capturesExportsBehindEvaluation()) {
        captureExportsBehindEvaluation(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!moduleData.hasExportTable() || (statusAtStart == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode() && !moduleData.hasTopLevelAwait()))
        RELEASE_AND_RETURN(scope, (evaluateSynchronously(module, SynchronousEvaluationReason::Require), false));
    JSPromise* evaluation = evaluate(module, std::nullopt, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    if (evaluation->status() == JSPromise::Status::Rejected) {
        evaluation->markAsHandled();
        throwException(m_instance.globalObject, scope, evaluation->result());
    }
    return false;
}

void Modules::passModuleThatHasRun(ModuleID root, BitVector& isPassed)
{
    if (!isSomethingDueWhereModulesArePassed()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    isPassed.ensureSize(m_table.size());
    struct ModuleOnPath {
        ModuleID module;
        uint32_t nextRequest;
    };
    Vector<ModuleOnPath, 16> path;
    bool factoryHasNotSettled = false;
    auto enter = [&](ModuleID module) {
        if (isPassed.quickSet(module.index()) || isEvicted(module))
            return;
        if (isMocked(module) || activeMock(module)) {
            if (!m_mockedModulesPassedOverByWalks.contains(module))
                m_mockedModulesPassedOverByWalks.append(module);
            return;
        }
        path.append({ module, data(module).firstRequest });
    };
    enter(root);
    while (!path.isEmpty()) {
        ModuleID module = path.last().module;
        if (path.last().nextRequest == m_table.endOfRequests(module)) {
            path.removeLast();
            reinstallBodyMocks(module);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        const ModuleRequestData& request = m_table.allRequests()[path.last().nextRequest++];
        if ((request.isRequire && !data(module).isFacade()) || request.isDeferred)
            continue;
        ModuleID imported = moduleLoadedBy(request);
        if (!data(module).isFacade() && isLinkedToRetainedMock(fileOf(imported), module))
            continue;
        if (keepsOriginalAfterRemoval(fileOf(imported), module))
            continue;
        releaseRemovedMock(imported);
        RETURN_IF_EXCEPTION(scope, void());
        if (!data(module).isFacade() && seesEndedMockLaidInPreload(fileOf(imported), module)) {
            bool isOwed = m_modulesOwedToImportersThatRan.get(imported.index()) || m_modulesOwedToImportersThatRan.get(fileOf(imported).index());
            if (isOwed && !isImportedByModuleLinkedInThisTestFile(fileOf(imported), imported)) {
                m_modulesOwedToImportersThatRan.clear(imported.index());
                m_modulesOwedToImportersThatRan.clear(fileOf(imported).index());
            }
            continue;
        }
        if (m_modulesOwedToImportersThatRan.get(imported.index())) {
            if (factoryHasNotSettled && !m_shouldContinuePastPendingFactories)
                continue;
            factoryHasNotSettled = evaluateModuleOwed(imported);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        if (ModuleStatus current = status(imported); current != ModuleStatus::Linking && current != ModuleStatus::Evaluating)
            enter(imported);
    }
}

void Modules::releaseRemovedMocksBelow(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty() && !m_modulesWithRemovedMock.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        if (isSeen.quickSet(module.index()) || isMocked(module) || activeMock(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (request.isDeferred)
                continue;
            ModuleID imported = moduleLoadedBy(request);
            if (m_letsGoOfMocksAtLinkFromSource && ((!data(module).isFacade() && isLinkedToRetainedMock(fileOf(imported), module)) || keepsOriginalAfterRemoval(fileOf(imported), module)))
                continue;
            toVisit.append(imported);
            releaseRemovedMock(imported);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

void Modules::releaseRemovedMocksFrom(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope letsGoOfMocksAtLinkFromSource(m_letsGoOfMocksAtLinkFromSource, true);
    releaseRemovedMock(root);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, releaseRemovedMocksBelow(root));
}

void Modules::letStartsAskWhereSomethingIsDueBelow(ModuleID root)
{
    auto isFollowed = [&](ModuleID module, const ModuleRequestData& request) {
        return !request.isDeferred && !isMocked(module) && !activeMock(module);
    };
    Vector<ModuleID, 64> modulesBelow;
    struct ModuleOnPath {
        ModuleID module;
        uint32_t nextRequest;
    };
    Vector<ModuleOnPath, 16> path { { root, data(root).firstRequest } };
    BitVector isSeen(m_table.size());
    isSeen.quickSet(root.index());
    while (!path.isEmpty()) {
        ModuleID module = path.last().module;
        if (path.last().nextRequest == m_table.endOfRequests(module)) {
            modulesBelow.append(path.takeLast().module);
            continue;
        }
        const ModuleRequestData& request = m_table.allRequests()[path.last().nextRequest++];
        if (ModuleID target = moduleLoadedBy(request); isFollowed(module, request) && !isSeen.quickSet(target.index()))
            path.append({ target, data(target).firstRequest });
    }
    BitVector isDueAtOrBelow = m_modulesNeedingMockReinstall;
    isDueAtOrBelow.merge(m_modulesOwedToImportersThatRan);
    isDueAtOrBelow.ensureSize(m_table.size());
    for (bool hasGrown = true; std::exchange(hasGrown, false);) {
        for (ModuleID module : modulesBelow) {
            if (isDueAtOrBelow.quickGet(module.index()))
                continue;
            for (const ModuleRequestData& request : m_table.requests(module)) {
                if (isFollowed(module, request) && isDueAtOrBelow.quickGet(moduleLoadedBy(request).index())) {
                    isDueAtOrBelow.quickSet(module.index());
                    hasGrown = true;
                    break;
                }
            }
        }
    }
    isSeen.clearAll();
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        if (isSeen.quickSet(module.index()) || !isDueAtOrBelow.quickGet(module.index()))
            continue;
        if (ModuleStatus current = status(module); current == ModuleStatus::Evaluated || current == ModuleStatus::EvaluatingAsync) {
            ensureRecord(module);
            continue;
        }
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (isFollowed(module, request))
                toVisit.append(moduleLoadedBy(request));
        }
    }
}

void Modules::evaluateModulesOwedBelow(ModuleID root, bool hasImporterThatRan)
{
    bringBackWhatLoaderLetGoBelow(root);
    if (!isSomethingDueWhereModulesArePassed()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    releaseRemovedMock(root);
    RETURN_IF_EXCEPTION(scope, void());
    if (hasMockEndedThatWalksPassedOver()) {
        m_modulesWithNothingOwedBelow.clearAll();
        m_mockedModulesPassedOverByWalks.clear();
    }
    if (hasImporterThatRan) {
        for (ModuleID each : { root, moduleWithExportsOf(root) }) {
            evaluateModuleOwed(each);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    ModuleStatus statusOfRoot = status(root);
    if (statusOfRoot != ModuleStatus::Evaluated && statusOfRoot != ModuleStatus::EvaluatingAsync && !hasRun(root)) {
        releaseRemovedMocksBelow(root);
        RETURN_IF_EXCEPTION(scope, void());
        letStartsAskWhereSomethingIsDueBelow(root);
        return;
    }
    BitVector isPassed;
    passModuleThatHasRun(root, isPassed);
    RETURN_IF_EXCEPTION(scope, void());
    m_modulesWithNothingOwedBelow.set(root.index());
}

void Modules::evaluateDueDependencies(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    for (const ModuleRequestData& request : m_table.imports(file)) {
        if (request.isDeferred)
            continue;
        evaluateModulesOwedBelow(moduleLoadedBy(request), true);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool wasReleasedByLoader(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasReleasedByLoader(module);
}

bool willReloadAfterMockRemoval(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->willReloadAfterMockRemoval(module);
}

void releaseRemovedMocks(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->releaseRemovedMocks();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
