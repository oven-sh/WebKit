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
#include "ModuleRegistryEntry.h"
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
    if (!data(module).isESModuleOfProgram() || data(module).isFacade() || (m_originalsRestoredAtUnmock.get(module.index()) && hasInstances(module)) || isSomeLoadUnderWay() || isLoaderInMiddleOfGraph() || isImportedByModuleThatRequireAnswered(module)) {
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
    if (LoadedModule* loaded = isLibraryOriginalDisplaced(module) ? loadedIfExists(module) : nullptr; loaded && loaded->exportsBeforeMock)
        return false;
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
    if (auto fetched = m_failedLoadTestFiles.find(module); fetched != m_failedLoadTestFiles.end()) [[unlikely]]
        return fetched->value == momentOfTestFile;
    if (m_hasBegunTestFile && isLinkedAndUnevaluated(module)) [[unlikely]]
        return m_linkTestFiles[module.index()] == momentOfTestFile;
    if (!m_momentsOfLoads.isEmpty() && m_momentsOfLoads[module.index()] > momentOfTestFile)
        return true;
    if (!m_momentsOfLoads.isEmpty() && !m_momentsOfLoads[module.index()] && m_hasBegunTestFile && m_linkedInstances.get(module.index()))
        return m_linkTestFiles[module.index()] == momentOfTestFile;
    auto anew = m_momentsLoadedAnew.find(module);
    return anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == momentOfTestFile;
}

bool Modules::wasFileLoadedByTest(ModuleID file) const
{
    ModuleKind kind = data(file).kind;
    bool hasOtherVariants = kind == ModuleKind::Data && !m_table.importAttributeVariants(m_table.fullPath(file)).isEmpty();
    if ((kind != ModuleKind::CommonJs && kind != ModuleKind::Data) || hasOtherVariants)
        return wasLoadedByTestFile(moduleWithExportsOf(file));
    if (!(kind == ModuleKind::Data ? m_dataModulesLoadedBeforeTestFile : m_filesThatRanBeforeTestFile).get(file.index()))
        return true;
    auto anew = m_momentsLoadedAnew.find(moduleWithExportsOf(file));
    return anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == m_instance.vm->aotState().momentOfTestFile;
}

void Modules::releaseMockedCycleMember(ModuleID module)
{
    if (isMocked(module) || activeMock(module) || !currentInstance(module))
        return;
    if (!tryReleaseModule(module))
        tellThatFileDidNotRunAgain(module);
}

void Modules::releaseDisplacedMockImporters(ModuleID mocked)
{
    SetForScope isDisplacingOriginal(m_isDisplacingOriginal, true);
    releaseImportersOf(mocked, ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule);
}

bool Modules::isEvictedWithOriginal(ModuleID module) const
{
    if (!currentInstance(module) || !isUnderActualKey(module) || status(module) != ModuleStatus::Evaluated || m_loadedInPreload.get(module.index()))
        return false;
    return m_originalsFetchedByTestFiles.get(module.index()) || (host().wasFetchedByTestFile && host().wasFetchedByTestFile(m_instance.globalObject, makeString(m_table.fullPath(module), "?actual"_s)));
}

bool Modules::releaseImportersOf(ModuleID mocked, ReleasedImporters which, std::optional<ModuleID> patched)
{
    if (m_momentsOfLoads.isEmpty())
        return false;
    bool keepsImportersOfTestFile = which == ReleasedImporters::AllButThoseThatThisTestFileLinkedToModule;
    bool hasSettledFactory = data(fileOf(mocked)).kind == ModuleKind::CommonJs && m_moduleWithSettledFactory == fileOf(mocked);
    if (keepsImportersOfTestFile && !activeMock(fileOf(mocked)) && (hasSettledFactory ? wasLoadedByTestFile(mocked) : wasFileLoadedByTest(fileOf(mocked))))
        return false;
    listImportersAndRequirers();
    BitVector released(m_table.size());
    BitVector asImported(m_table.size());
    asImported.quickSet(mocked.index());
    asImported.quickSet(moduleWithExportsOf(mocked).index());
    BitVector seen = asImported;
    Vector<uint32_t, 32> work;
    for (size_t imported : seen)
        work.append(imported);
    Vector<ModuleID, 16> toRelease;
    Vector<ModuleID, 2> originals;
    Vector<ModuleID, 2> retainedParents;
    Vector<ModuleID, 2> commonJSOriginals;
    bool importsReleasedImporter = false;
    ModuleIndexMap<Vector<ModuleID, 2>> retainedRequirers;
    auto isLinkedTo = [&](ModuleID importer, Environment* instance, ModuleID target, Environment* linked) {
        const ModuleLinkData* link = findModuleLink(importer, target);
        return link && link->hasLink && !mockWord(*link, instance) && readLink(instance, link->link) == linked;
    };
    auto releaseOriginal = [&](ModuleID file, Environment* instance) {
        Vector<std::pair<ModuleID, Environment*>, 2> found { { file, instance } };
        while (!found.isEmpty()) {
            auto [module, original] = found.takeLast();
            if (originals.contains(module))
                continue;
            originals.append(module);
            for (uint32_t index : m_importersByStatement[module.index()]) {
                ModuleID importer(index);
                if (Environment* other = asImported.quickGet(index) || hasIntactOriginal(importer) ? nullptr : originalBesideMock(importer); other && isLinkedTo(importer, other, module, original))
                    found.append({ importer, other });
                if (importer == m_replacedTestFile || importer == patched || m_loadedInPreload.get(index) || activeMock(importer) || hasIntactOriginal(importer))
                    continue;
                Environment* current = m_linkedInstances.get(index) ? currentInstance(importer) : nullptr;
                if (!current || !isLinkedTo(importer, current, module, original) || !mayReleaseModule(importer) || seen.quickSet(index))
                    continue;
                toRelease.append(importer);
                released.quickSet(index);
                work.append(index);
            }
        }
    };
    bool keepsPackages = isOfPackage(mocked) && !isPatchedByMock(fileOf(mocked)) && (m_isDisplacingOriginal || hasIntactOriginal(fileOf(mocked)) || wasOriginalEvaluated(fileOf(mocked)));
    bool keepsPackageParents = isOfPackage(mocked) && !isPatchedByMock(fileOf(mocked));
    while (!work.isEmpty()) {
        uint32_t imported = work.takeLast();
        for (uint32_t index : m_importersByStatement[imported]) {
            ModuleID importer(index);
            if (importer == m_replacedTestFile || m_modulesWithoutTestFile.get(index) || (asImported.quickGet(imported) && keepsImportersOfTestFile && wasLoadedByTestFile(importer)))
                continue;
            bool keepsImporter = keepsPackages && asImported.quickGet(imported) && isExternalizedPackage(importer);
            if (Environment* original = keepsImporter ? nullptr : originalBesideMock(importer); original && isLinkedToCurrentEntry(importer, ModuleID(imported), original).value_or(false)) [[unlikely]]
                releaseOriginal(importer, original);
            std::optional<bool> isLinked = isLinkedToCurrentEntry(importer, ModuleID(imported));
            if (isLinked && (!*isLinked || (keepsImporter && (m_isDisplacingOriginal || !activeMock(fileOf(mocked))))))
                continue;
            if (!isLinked && asImported.quickGet(imported) && isLinkedToRetainedMock(m_table.moduleBehindFacade(mocked).value_or(mocked), importer))
                continue;
            if (!isLinked && asImported.quickGet(imported) && isLinkedToOriginalOf(mocked, importer))
                continue;
            if (isDisplacedRunImporter(fileOf(ModuleID(imported)), importer)) [[unlikely]]
                continue;
            if (which == ReleasedImporters::WithInstances && !isLinked)
                continue;
            if (isLinked && hasIntactOriginal(importer) && isEvictedWithOriginal(importer) && !originals.contains(importer)) [[unlikely]] {
                releaseOriginal(importer, currentInstance(importer));
                m_evictedOriginals.set(importer.index());
            }
            importsReleasedImporter |= importer == mocked && released.quickGet(imported);
            if (seen.quickSet(index))
                continue;
            bool isLoaded = isLinked || status(importer) > ModuleStatus::Linked || (data(importer).isESModuleOfProgram() && wasLoadedWithGraph(importer));
            if (!isLoaded || m_loadedInPreload.get(index) || importer == patched || activeMock(importer) || hasIntactOriginal(importer) || (m_originalsRestoredAfterPass && m_originalsRestoredAfterPass->get(index)))
                continue;
            if (mayReleaseModule(importer) || mayReleasePatchedModule(importer)) {
                toRelease.append(importer);
                released.quickSet(index);
                work.append(index);
                continue;
            }
            m_modulesReleasedByLoader.set(index);
            if (asImported.quickGet(imported) ? !hasIntactOriginal(ModuleID(imported)) || seesMockOf(ModuleID(imported), importer) : m_importersOfMockThatEnded.get(imported))
                m_importersOfMockThatEnded.set(index);
            work.append(index);
        }
        if (auto parent = retainedEntryParent(ModuleID(imported)); parent && !(asImported.quickGet(imported) && keepsImportersOfTestFile && wasLoadedByTestFile(*parent)) && !(asImported.quickGet(imported) && keepsPackageParents && isOfPackage(*parent)))
            retainedParents.append(*parent);
        auto releaseRequirer = [&](ModuleID requirer) {
            bool wasLoadedByThisTestFile = data(requirer).kind == ModuleKind::CommonJs ? wasFileLoadedByTest(requirer) : wasLoadedByTestFile(requirer);
            if ((asImported.quickGet(imported) && keepsImportersOfTestFile && wasLoadedByThisTestFile) || seen.quickGet(requirer.index()))
                return;
            if (asImported.quickGet(imported) && ((keepsPackages && isOfPackage(requirer)) || requiresOriginalUnderMock(fileOf(mocked), requirer)))
                return;
            toRelease.append(requirer);
            for (ModuleID each : { requirer, moduleWithExportsOf(requirer) }) {
                if (seen.quickSet(each.index()))
                    continue;
                released.quickSet(each.index());
                work.append(each.index());
            }
        };
        auto evictTestFile = [&](ModuleID testFile) {
            if (!(asImported.quickGet(imported) && keepsImportersOfTestFile && wasFileLoadedByTest(testFile)))
                m_evictedTestFiles.set(testFile.index());
        };
        if (auto parent = m_entryParents.getOptional(ModuleID(imported)); parent && *parent && isStartedByHost(*parent))
            evictTestFile(*parent);
        if (std::optional<ModuleID> requirer = entryParentToRelease(ModuleID(imported)))
            releaseRequirer(*requirer);
        ModuleID required = fileOf(ModuleID(imported));
        const ModuleIndexSet* requirers = requirersOfEntry(required);
        if (!requirers)
            continue;
        for (uint32_t index : *requirers) {
            if (index >= m_table.size()) [[unlikely]] {
                ModuleID file(index - m_table.size());
                uint32_t testFile = m_testFilesOfCommonJSOriginals.get(file.index());
                bool isKept = asImported.quickGet(imported) && ((keepsImportersOfTestFile && testFile == m_instance.vm->aotState().momentOfTestFile) || (keepsPackages && isOfPackage(file)));
                if (!testFile || isKept || commonJSOriginals.contains(file))
                    continue;
                commonJSOriginals.append(file);
                if (!host().evictOriginal(m_instance.globalObject, m_table.fullPath(file)) || !activeMock(file))
                    continue;
                for (ModuleID each : { file, moduleWithExportsOf(file) }) {
                    if (!seen.quickSet(each.index()))
                        work.append(each.index());
                }
                continue;
            }
            ModuleID requirer(index);
            if (requirer == m_replacedTestFile || seen.quickGet(index))
                continue;
            if (isStartedByHost(requirer))
                evictTestFile(requirer);
            else if (mayReleaseCommonJSFile(requirer)) {
                releaseRequirer(requirer);
                if (released.quickGet(requirer.index()))
                    m_mockedFilesWithReleasedRequirers.add(fileOf(mocked), false);
                if (required == fileOf(mocked) && released.quickGet(requirer.index()))
                    m_mockedFilesWithReleasedRequirers.set(required, true);
            }
            else if (!(asImported.quickGet(imported) && ((keepsPackages && isOfPackage(requirer)) || requiresOriginalUnderMock(fileOf(mocked), requirer))))
                retainedRequirers.add(imported, Vector<ModuleID, 2>()).iterator->value.append(requirer);
        }
    }
    bool mayHaveImportersWithoutInstances = areReadByImportersWithoutInstances(toRelease.span());
    for (ModuleID importer : toRelease) {
        if (!tryReleaseModule(importer) && !tryReleasePatchedModule(importer))
            continue;
        if (data(fileOf(importer)).isReloadable() && runsTestFilesAgain())
            m_requirersThatRunAgain.set(fileOf(importer).index());
        if (!mayHaveImportersWithoutInstances || !hasEvaluatedImporterWithoutInstances(importer))
            continue;
        m_modulesOwedToImportersThatRan.set(importer.index());
        m_modulesWithNothingOwedBelow.clearAll();
        followImportsAnewAtNextQuestion();
    }
    for (ModuleID importer : originals) {
        if (!hasIntactOriginal(importer))
            forgetUnmockedOriginal(importer);
    }
    for (ModuleID file : commonJSOriginals)
        forgetCommonJSOriginal(file);
    for (ModuleID parent : retainedParents) {
        if (m_modulesReleasedByLoader.get(parent.index()) || m_importersRetainedAboveMock.set(parent.index()))
            continue;
        ensureRecord(parent);
        work.append(parent.index());
        if (ModuleID facade = moduleWithExportsOf(parent); facade != parent && !m_importersRetainedAboveMock.set(facade.index()))
            work.append(facade.index());
    }
    if (importsReleasedImporter && keepsImportersOfTestFile && !m_isDisplacingOriginal)
        m_patchedModuleOfCycle = mocked;
    if (which == ReleasedImporters::WithInstances)
        return importsReleasedImporter;
    for (size_t imported : seen) {
        if (asImported.quickGet(imported) || released.quickGet(imported) || m_modulesReleasedByLoader.get(imported))
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
            if (isDisplacedRunImporter(fileOf(ModuleID(imported)), importer)) [[unlikely]]
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
        auto visitRequirer = [&](uint32_t index) {
            bool requiredOriginalUnderMock = hasRequiredOriginal(ModuleID(index), fileOf(ModuleID(imported))) && (activeMock(fileOf(mocked)) || isPatchedByMock(fileOf(mocked)));
            if (!data(ModuleID(index)).isReloadable() || requiredOriginalUnderMock || m_filesRequiredBesidePatch.get(fileOf(ModuleID(imported)).index()) || runsTestFilesAgain())
                return visit(index);
            if (asImported.quickGet(imported) && status(ModuleID(index)) == ModuleStatus::Evaluated && !m_loadedInPreload.get(index)) {
                auto released = m_mockedFilesWithReleasedRequirers.add(fileOf(mocked), false);
                released.iterator->value |= evaluatedBy(fileOf(mocked)) == LoadedBy::EarlierTestFile && !isOfPackage(fileOf(mocked));
            }
        };
        if (auto retained = retainedRequirers.find(imported); retained != retainedRequirers.end()) {
            for (ModuleID requirer : retained->value)
                visit(requirer.index());
        }
        bool hasRegisteredRequirers = !data(ModuleID(imported)).isOfLibrary() || isKeptInRequireCache(ModuleID(imported));
        if (hasRegisteredRequirers) {
            for (uint32_t index : m_requirers[imported])
                visitRequirer(index);
        }
        if (auto answered = m_requirersThatRunTimeAnswered.find(imported); hasRegisteredRequirers && answered != m_requirersThatRunTimeAnswered.end()) {
            for (uint32_t index : answered->value)
                visitRequirer(index);
        }
        if (!isAboveRequirer)
            continue;
        for (uint32_t index : m_importersByStatement[imported])
            visit(index);
    }
    return importsReleasedImporter;
}

bool Modules::noteEntryParent(ModuleID target, ModuleID referrer)
{
    if (referrer && requiresOriginalUnderMock(fileOf(target), referrer)) [[unlikely]]
        return false;
    m_modulesWithKnownParent.set(target.index());
    if (!hasInstances(target) && !m_hasBegunTestFile && data(target).kind != ModuleKind::CommonJs)
        return false;
    ModuleID parent = referrer ? referrer : callingModule().value_or(ModuleID());
    if (parent && m_instance.vm->aotState().filesStartedByHost.get(parent.index())) {
        Vector<ModuleID, 1>& testFiles = m_testFilesRequiringESModules.add(target, Vector<ModuleID, 1>()).iterator->value;
        if (!testFiles.contains(parent))
            testFiles.append(parent);
    }
    if (!hasInstances(target) && data(target).kind != ModuleKind::CommonJs)
        return false;
    if (parent && requirerIndex(parent) != parent.index()) [[unlikely]]
        parent = ModuleID();
    m_entryParents.add(target, parent);
    return true;
}

std::optional<ModuleID> Modules::forgetEntryParent(ModuleID target)
{
    m_modulesWithKnownParent.clear(target.index());
    return m_entryParents.takeOptional(target);
}

void Modules::didDisplaceOriginal(ModuleID module)
{
    if (auto first = forgetEntryParent(module))
        m_displacedEntryParents.set(module, *first);
}

bool ranDuringMockThatOnlyWaited(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->ranDuringMockThatOnlyWaited(module);
}

void reportIfFileWasReleased(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->reportIfFileWasReleased(module);
}

void reportMockedFileKept(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->reportMockedFileKept(module);
}

void reportReloadableFileKept(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->reportReloadableFileKept(module);
}

void didRepeatMockOfKeptFile(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRepeatMockOfKeptFile(module);
}

void noteKeptOriginal(JSGlobalObject* realm, ModuleID module, bool isAutomock)
{
    if (Modules* modules = modulesOf(realm))
        modules->noteKeptOriginal(module, isAutomock);
}

void displaceEntryParent(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didDisplaceOriginal(module);
}

void Modules::didEndMockOfEntry(ModuleID module, bool restoresOriginal)
{
    forgetEntryParent(module);
    if (auto first = m_displacedEntryParents.takeOptional(module); first && restoresOriginal) {
        m_modulesWithKnownParent.set(module.index());
        m_entryParents.add(module, *first);
    }
}

std::optional<ModuleID> Modules::entryParentToRelease(ModuleID target)
{
    auto first = m_entryParents.getOptional(target);
    if (!first || !*first || status(*first) != ModuleStatus::Evaluated || m_instance.vm->aotState().filesStartedByHost.get(first->index()) || m_loadedInPreload.get(first->index()) || !mayReleaseModule(*first))
        return std::nullopt;
    return first;
}

std::optional<ModuleID> Modules::retainedEntryParent(ModuleID target)
{
    auto first = m_entryParents.getOptional(target);
    if (!first || !*first || !hasInstances(target) || hasInstances(*first) || data(fileOf(*first)).kind != ModuleKind::CommonJs || status(*first) != ModuleStatus::Evaluated || m_instance.vm->aotState().filesStartedByHost.get(first->index()) || m_loadedInPreload.get(first->index()) || activeMock(*first) || mayReleaseModule(*first))
        return std::nullopt;
    return first;
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
            if (!request.isHoisted && (!request.isDeferred || hasInstances(ModuleID(importer))))
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

void Modules::noteMockEnd(ModuleID mocked)
{
    if (m_hasBegunTestFile && (activeMock(mocked) || isPatchedByMock(mocked) || hasMockOfLibraryModule(mocked)))
        m_mockEndMoments.set(mocked, ++m_instance.vm->aotState().momentOfLoads);
}

bool Modules::wasEvictedFromRequireCache(ModuleID file)
{
    uint32_t startMoment = m_startMoments.getOptional(file).value_or(0);
    if (m_momentOfModuleReset > startMoment || m_evictedTestFiles.get(file.index()))
        return true;
    auto originals = m_originalsByRequirer.find(file.index());
    if (m_evictedFiles.isEmpty() && (originals == m_originalsByRequirer.end() || originals->value.isEmpty()))
        return false;
    listImportersAndRequirers();
    BitVector seen(m_table.size());
    Vector<uint32_t, 32> work;
    auto visit = [&](ModuleID module) {
        for (ModuleID each : { module, moduleWithExportsOf(module), m_table.moduleBehindFacade(module).value_or(module) }) {
            if (!seen.quickSet(each.index()))
                work.append(each.index());
        }
    };
    for (auto& [mocked, moment] : m_mockEndMoments) {
        if (moment > startMoment)
            visit(mocked);
    }
    while (!work.isEmpty()) {
        uint32_t module = work.takeLast();
        if (auto testFiles = m_testFilesRequiringESModules.find(ModuleID(module)); testFiles != m_testFilesRequiringESModules.end() && testFiles->value.contains(file))
            return true;
        auto answered = m_requirersThatRunTimeAnswered.find(module);
        std::span<const uint32_t> answeredRequirers = answered == m_requirersThatRunTimeAnswered.end() ? std::span<const uint32_t>() : answered->value.span();
        for (std::span<const uint32_t> importers : { std::span<const uint32_t>(m_importersByStatement[module].span()), std::span<const uint32_t>(m_requirers[module].span()), answeredRequirers }) {
            for (uint32_t importer : importers) {
                if (importer == file.index() || importer == moduleWithExportsOf(file).index())
                    return true;
                visit(ModuleID(importer));
            }
        }
    }
    return false;
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

void Modules::releaseFailedESModules(std::optional<ModuleID> patched)
{
    if (m_momentsOfLoads.isEmpty())
        return;
    BitVector importedByPatched;
    if (patched && isFailedESModule(*patched) && !m_loadedInPreload.get(patched->index()) && !wasLoadedByTestFile(*patched))
        importedByPatched = staticImportClosure(*patched, MockTraversal::Continue);
    bool importsReleasedModule = false;
    constexpr auto inRecord = static_cast<uint8_t>(StoredModuleStatus::InRecord);
    std::span<const uint8_t> statuses = m_storedStatuses;
    BitVector candidates = m_originalsEvaluatedUnderMock;
    candidates.merge(m_modulesThatRanBesideMockAlone);
    for (size_t index = WTF::find(statuses, inRecord); index != notFound; index = WTF::find(statuses, inRecord, index + 1))
        candidates.set(index);
    candidates.forEachSetBit([&](size_t index) {
        if (statuses[index] != inRecord && !m_originalsEvaluatedUnderMock.get(index) && !m_modulesThatRanBesideMockAlone.get(index))
            return;
        ModuleID module(static_cast<uint32_t>(index));
        if (errorOfModuleThatRanBesideMock(module) && originalBesideMock(module)) [[unlikely]]
            forgetUnmockedOriginal(module);
        if (!isFailedESModule(module) || module == patched || isMocked(module) || activeMock(module))
            return;
        importsReleasedModule |= importedByPatched.get(index);
        m_loadRoots.clear(index);
        if (currentInstance(module))
            releaseImportersOf(module, ReleasedImporters::WithInstances, patched);
        if (!tryReleaseModule(module))
            m_modulesReleasedByLoader.set(index);
    });
    if (importsReleasedModule)
        tellThatFileDidNotRunAgain(*patched);
}

void Modules::evictFailedRecords()
{
    Vector<ModuleID, 4> failed;
    for (auto& [module, record] : m_records) {
        if (record->status() > CyclicModuleRecord::Status::Unlinked || !data(module).isESModuleOfProgram() || data(module).isFacade())
            continue;
        ModuleRegistryEntry* entry = loader()->registryEntry(moduleKey(module));
        if (entry && entry->record() == record && entry->status() >= ModuleRegistryEntry::Status::FetchFailed)
            failed.append(module);
    }
    for (ModuleID module : failed) {
        loader()->removeEntriesForRecord(m_records.get(module));
        takeOutRecord(module);
        if (storedStatus(module) == StoredModuleStatus::InRecord)
            setStoredStatus(module, StoredModuleStatus::Unlinked);
        m_linkFailures.remove(module);
        if (!hasInstances(module))
            continue;
        startLoadingModulesIndividually();
        m_linkedInstances.clear(module.index());
        m_failedLoadTestFiles.remove(module);
        if (!m_momentsOfLoads.isEmpty())
            didGiveUpMomentLinked(std::exchange(m_momentsOfLoads[module.index()], 0));
        didChangeLinkState();
    }
}

void Modules::willFetchESModules(ModuleID root)
{
    if (m_fetchedESModules.get(root.index()))
        return;
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> fetched;
    Vector<ModuleID, 4> failed;
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        ModuleID file = fileOf(module);
        if (isSeen.quickSet(file.index()))
            continue;
        if (data(file).kind == ModuleKind::CommonJs) {
            if (data(file).runsAgainAfterThrow())
                failed.append(file);
            continue;
        }
        if (isMocked(module) || !data(module).isESModuleOfProgram() || status(module) > ModuleStatus::Linked)
            continue;
        fetched.append(module);
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted)
                work.append(request.targetModule());
        }
    }
    failed.removeAllMatching([&](ModuleID file) {
        return m_timesMocked.isEmpty() || isMocked(file) || activeMock(file) || !forgetLoadThatFailed(file);
    });
    if (!failed.isEmpty()) [[unlikely]] {
        BitVector evicted(m_table.size());
        importersOfModules().addModulesAbove(Vector<ModuleID, 32>(failed.span()), evicted);
        m_fetchedESModules.exclude(evicted);
    }
    for (ModuleID module : fetched)
        m_fetchedESModules.set(module.index());
}

void Modules::clearFailedCommonJSFiles()
{
    m_fetchedESModules.clearAll();
    Vector<ModuleID, 4> failed;
    for (auto& [module, record] : m_records) {
        ModuleID file = fileOf(module);
        if (record->evaluationError() && data(file).runsAgainAfterThrow() && !isMocked(file) && !activeMock(file) && !m_loadedInPreload.get(module.index()))
            failed.append(file);
    }
    for (ModuleID file : failed)
        forgetLoadThatFailed(file);
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
    if (m_importersRetainedAboveMock.isEmpty() && m_originalsKeptUnderMock.isEmpty()) [[likely]]
        return;
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        if (isSeen.quickSet(module.index()))
            continue;
        if (m_importersRetainedAboveMock.clear(module.index()) && !(m_retainedFactoryImporters.clear(module.index()) && isEvicted(module)))
            tellThatFileDidNotRunAgain(module);
        if (m_originalsKeptUnderMock.clear(fileOf(module).index()))
            tellThatFileDidNotRunAgain(fileOf(module));
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
    bool isNotedRoot = m_loadRoots.get(root.index());
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
        if (wasLetGo)
            tellOfReleaseWhereTestFilesRunAgain(fileOf(module));
        bool isReleased = wasLetGo && tryReleaseModule(module);
        if (isReleased && m_linksBeforeLoadInFlight)
            m_linksBeforeLoadInFlight->modulesReleasedByLoader.clear(module.index());
        if (wasLetGo && !isReleased && !m_mockedEvictedDataFiles.clear(module.index()))
            tellThatFileDidNotRunAgain(file.value_or(module));
        bool sawMockThatEnded = wasLetGo && take(m_importersOfMockThatEnded, &LinksBeforeLoad::importersOfMockThatEnded, module);
        if (wasLetGo && file)
            sawMockThatEnded |= take(m_importersOfMockThatEnded, &LinksBeforeLoad::importersOfMockThatEnded, *file);
        if (sawMockThatEnded && !isReleased)
            noteLoadedAnew(module, root);
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !request.isDeferred)
                toVisit.append(moduleLoadedBy(request));
        }
    }
    if (isNotedRoot)
        m_loadRoots.set(root.index());
}

bool Modules::evaluateModuleOwed(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!m_modulesOwedToImportersThatRan.get(module.index()))
        return false;
    if (m_filesThatRanInPreload.get(fileOf(module).index())) [[unlikely]]
        tellOfKeptRun(fileOf(module));
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
    auto isDue = [&] {
        ModuleStatus current = status(module);
        return (current < ModuleStatus::Evaluating || current == ModuleStatus::Failed) && !activeMock(module);
    };
    if (hasInstances(module) && !m_modulesWithPendingMock.isEmpty() && isDue()) [[unlikely]] {
        bool factoryHasNotSettled = runMocksInGraph(module, LoadRootKind::ImportStatement);
        RETURN_IF_EXCEPTION(scope, false);
        if (factoryHasNotSettled) {
            m_isDueModuleBlocked = true;
            return true;
        }
    }
    m_modulesOwedToImportersThatRan.clear(module.index());
    auto staysOwedWhereItFails = makeScopeExit([&] {
        if (scope.exception()) [[unlikely]]
            m_modulesOwedToImportersThatRan.set(module.index());
    });
    followImportsAnewAtNextQuestion();
    if (!isDue())
        return false;
    ModuleStatus statusAtStart = status(module);
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
