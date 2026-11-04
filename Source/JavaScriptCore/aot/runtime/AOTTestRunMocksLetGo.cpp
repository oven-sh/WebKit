/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "TopExceptionScope.h"

namespace JSC { namespace AOT {

// A graph that is being loaded holds the records of its modules, and goes on by what the table says of them.
bool Modules::isSomeLoadUnderWay() const
{
    if (m_instance.vm->synchronousModuleQueue() || m_rootWhoseFilesRun || m_rootWhoseFilesRanAhead || m_fileWhoseJobsWaitForLoader || !m_levelsOfLoads.isEmpty() || !m_textsOnTheirWay.isEmpty())
        return true;
    // The host loads the graph of the entry point.
    if (!m_firstModuleRequestedByHost || status(*m_firstModuleRequestedByHost) < ModuleStatus::Evaluating)
        return true;
    for (const LoadInFlight& load : m_loadsInFlight) {
        if (load.promise->status() == JSPromise::Status::Pending)
            return true;
    }
    return false;
}

// The code of a CommonJS file whose mock was let go asks the run time for its status again.
bool Modules::loadComesToModuleWhoseMockEnded(ModuleID module) const
{
    return hasMockThatEnded(module) || (data(module).kind == ModuleKind::CommonJs && !isImportedByModuleThatRequireAnswered(module));
}

// Compiled code calls the body of a CommonJS file by itself once the status says so, and asks nobody.
void Modules::letGoOfMockAtNextLoad(ModuleID module)
{
    // A graph that is being loaded holds the record of the module, and goes on by what the table says of the module.
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || isSomeLoadUnderWay() || isLoaderInMiddleOfGraph() || isImportedByModuleThatRequireAnswered(module)) {
        setMockInPlace(module, nullptr, { });
        return;
    }
    m_modulesWithMockToLetGo.set(module.index());
    m_testFilesInWhichMocksEnded.set(module, m_instance.vm->aotState().momentOfTestFile);
    // The loader asks for the module anew, and so comes by.
    if (ModuleRecord* record = recordIfExists(module); record && record->hasLiveExports())
        loader()->removeEntryOfRecord(record);
}

bool Modules::letGoOfMockOfLibraryModuleAtNextLoad(ModuleID module)
{
    // A mock that was laid over the names of a loaded module goes at once, as it came. A graph that is being loaded goes on by what the table says of the module.
    if (!m_libraryModulesWithMock.get(module.index()) || !exportsWhatMockNamesAlone(module) || isSomeLoadUnderWay() || isLoaderInMiddleOfGraph())
        return false;
    m_modulesWithMockToLetGo.set(module.index());
    return true;
}

void Modules::letGoOfMockThatEndedSlow(ModuleID module)
{
    if (data(module).isOfLibrary() || !m_table.hasFilePath(module)) {
        auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
        m_modulesWithMockToLetGo.clear(module.index());
        if (mockInPlace(module)) {
            setMockInPlace(module, nullptr, { });
            RETURN_IF_EXCEPTION(scope, void());
        }
        RELEASE_AND_RETURN(scope, endMockOfLibraryModule(module));
    }
    // Nothing is asked of a mock that is taken away.
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    setMockInPlace(module, nullptr, { });
    scope.assertNoExceptionExceptTermination();
    // The next mock waits already: deferMock().
    if (m_modulesWithMockToRun.get(module.index()))
        willMock(module);
}

void Modules::letGoOfMocksThatEnded()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    while (!m_modulesWithMockToLetGo.isEmpty()) {
        letGoOfMockThatEnded(ModuleID(*m_modulesWithMockToLetGo.begin()));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// By its first load, or by the load that linked it anew where `bun test` makes it anew.
bool Modules::wasLoadedInThisTestFile(ModuleID module) const
{
    uint32_t momentOfTestFile = m_instance.vm->aotState().momentOfTestFile;
    if (!m_momentsOfLoads.isEmpty() && m_momentsOfLoads[module.index()] > momentOfTestFile)
        return true;
    auto anew = m_momentsLoadedAnew.find(module);
    return anew != m_momentsLoadedAnew.end() && anew->value.momentOfTestFile == momentOfTestFile;
}

void Modules::letGoOfImportersOf(ModuleID mocked, ImportersLetGo which)
{
    // No moment of a load is noted outside a test run.
    if (m_momentsOfLoads.isEmpty())
        return;
    bool keepsImportersOfTestFile = which == ImportersLetGo::AllButThoseThatThisTestFileLinkedToModule;
    if (keepsImportersOfTestFile && wasLoadedInThisTestFile(mocked))
        return;
    if (m_importersByStatement.isEmpty()) {
        m_importersByStatement.grow(m_table.size());
        for (uint32_t importer = 0; importer < m_table.size(); ++importer) {
            for (const ModuleRequestData& request : m_table.imports(ModuleID(importer))) {
                if (!request.isHoisted && !request.isDeferred)
                    m_importersByStatement[moduleLoadedBy(request).index()].append(importer);
            }
        }
    }
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
            if (asImported.quickGet(imported) && keepsImportersOfTestFile && wasLoadedInThisTestFile(importer))
                continue;
            if (seen.quickSet(index))
                continue;
            // A module with nothing to run stays as the loader left it.
            bool isLoaded = status(importer) > ModuleStatus::Linked || (data(importer).isESModuleOfProgram() && wasLoadedWithGraph(importer));
            // Nothing would load a module of a preload again. A mock that stands in place of a body that never ran imports nothing.
            if (!isLoaded || m_loadedInPreload.get(index) || (mockInPlace(importer) && !isLeftAloneByMock(importer)))
                continue;
            m_modulesThatLoaderLetGo.set(index);
            // `bun test` lets go of what was linked to the mock. Whoever kept the module under a mock that left it alone stays as it is.
            if (asImported.quickGet(imported) ? !isLeftAloneByMock(ModuleID(imported)) || seesMockOf(ModuleID(imported), importer) : m_importersOfMockThatEnded.get(imported))
                m_importersOfMockThatEnded.set(index);
            work.append(index);
        }
    }
}

// A module that is still with the loader of `bun test` has there all that it imports.
void Modules::bringBackWhatLoaderLetGoBelow(ModuleID root)
{
    if (m_modulesThatLoaderLetGo.isEmpty()) [[likely]]
        return;
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        if (isSeen.quickSet(module.index()))
            continue;
        bool wasLetGo = m_modulesThatLoaderLetGo.clear(module.index());
        // An import of a CommonJS file comes to its face, and loads the file with it.
        if (std::optional<ModuleID> file = m_table.fileBehindFace(module))
            wasLetGo |= m_modulesThatLoaderLetGo.clear(file->index());
        if (!wasLetGo && status(module) > ModuleStatus::Linked)
            continue;
        if (wasLetGo && m_importersOfMockThatEnded.clear(module.index())) {
            VMState& state = m_instance.vm->aotState();
            m_momentsLoadedAnew.set(module, MomentLoadedAnew { ++state.momentOfLoads, state.momentOfTestFile, root });
        }
        // What only a mocked module imports is not loaded.
        if (isMocked(module) || mockInPlace(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (!request.isHoisted && !request.isDeferred)
                toVisit.append(moduleLoadedBy(request));
        }
    }
}

// A mock kept the body from running while an importer ran, and has ended.
void Modules::evaluateModuleOwed(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!m_modulesOwedToImportersThatRan.get(module.index()))
        return;
    // The mock that waits for the module comes in place of the body, which stays owed for when that mock ends.
    if (ModuleID file = fileOf(module); m_modulesWithMockToRun.get(file.index())) {
        scope.release();
        runMocksBelow(file, RootOfLoad::IsImportedByStatement);
        return;
    }
    m_modulesOwedToImportersThatRan.clear(module.index());
    followImportsAnewAtNextQuestion();
    ModuleStatus statusAtStart = status(module);
    if (statusAtStart >= ModuleStatus::Evaluating || mockInPlace(module))
        return;
    const ModuleData& moduleData = data(module);
    // The importers were linked while the mock stood, so no link has seen to it that the names are taken behind the body.
    if (moduleData.capturesExportsBehindEvaluation()) {
        captureExportsBehindEvaluation(module);
        RETURN_IF_EXCEPTION(scope, void());
    }
    // The mock kept the loader from what the module imports from outside the image.
    if (!moduleData.hasExportTable() || (statusAtStart == ModuleStatus::Unlinked && moduleData.reachesUncompiledCode() && !moduleData.hasTopLevelAwait()))
        RELEASE_AND_RETURN(scope, evaluateSynchronously(module, SynchronousEvaluationReason::Require));
    // A module that waits at an `await` goes on in its own jobs.
    JSPromise* evaluation = evaluate(module, std::nullopt, nullptr);
    RETURN_IF_EXCEPTION(scope, void());
    if (evaluation->status() == JSPromise::Status::Rejected) {
        evaluation->markAsHandled();
        throwException(m_instance.globalObject, scope, evaluation->result());
    }
}

// `bun test` runs a module again for the next test file that imports it, where its body laid a mock or it was linked to one: at its turn among the bodies of the graph. A compiled body runs once.
// No walk of an evaluation goes below a module that has run. This one does, where an evaluation passes such a module and where a load begins at one, and does what is left of the second run:
// depth first, in the order of the text, a body that a mock kept from running runs, and the calls that a body made are made again. Each module once for each evaluation.
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
    auto enter = [&](ModuleID module) {
        if (isPassed.quickSet(module.index()))
            return;
        // What only a mocked module imports is not loaded.
        if (isMocked(module) || mockInPlace(module)) {
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
            layMocksOfBodyAgain(module);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        const ModuleRequestData& request = m_table.allRequests()[path.last().nextRequest++];
        // What a body requires runs inside it. The face of a CommonJS file asks for the file as an import does.
        if ((request.isRequire && !data(module).isFaceOfFile()) || request.isDeferred)
            continue;
        ModuleID imported = moduleLoadedBy(request);
        letGoOfMockThatEnded(imported);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_modulesOwedToImportersThatRan.get(imported.index())) {
            evaluateModuleOwed(imported);
            RETURN_IF_EXCEPTION(scope, void());
            continue;
        }
        // An evaluation that has begun it goes on with it.
        if (ModuleStatus current = status(imported); current != ModuleStatus::Linking && current != ModuleStatus::Evaluating)
            enter(imported);
    }
}

void Modules::letGoOfMocksThatEndedBelow(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty() && !m_modulesWithMockToLetGo.isEmpty()) {
        ModuleID module = toVisit.takeLast();
        // What only a mocked module imports is not loaded.
        if (isSeen.quickSet(module.index()) || isMocked(module) || mockInPlace(module))
            continue;
        for (const ModuleRequestData& request : m_table.imports(module)) {
            if (request.isDeferred)
                continue;
            toVisit.append(moduleLoadedBy(request));
            letGoOfMockThatEnded(moduleLoadedBy(request));
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
}

// A start passes what has run and asks nobody. It asks the run time for a module whose status is in a record: beginModuleInRecord().
void Modules::letStartsAskWhereSomethingIsDueBelow(ModuleID root)
{
    auto isFollowed = [&](ModuleID module, const ModuleRequestData& request) {
        return !request.isDeferred && !isMocked(module) && !mockInPlace(module);
    };
    // As a walk leaves them: what a module asks for stands in front of it, but for a cycle.
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
    BitVector isDueAtOrBelow = m_modulesThatLayMocksAgain;
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
    // No start goes below a module that has run.
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

// A load begins at the root.
void Modules::evaluateModulesOwedBelow(ModuleID root, bool hasImporterThatRan)
{
    bringBackWhatLoaderLetGoBelow(root);
    if (!isSomethingDueWhereModulesArePassed()) [[likely]]
        return;
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    letGoOfMockThatEnded(root);
    RETURN_IF_EXCEPTION(scope, void());
    // What lay behind the mock may be owed.
    if (hasMockEndedThatWalksPassedOver()) {
        m_modulesWithNothingOwedBelow.clearAll();
        m_mockedModulesPassedOverByWalks.clear();
    }
    // The importer is one that no walk from here comes to: evaluateModulesOwedBelowOriginal(). A CommonJS file runs before the module that takes its exports.
    if (hasImporterThatRan) {
        for (ModuleID each : { root, moduleWithExportsOf(root) }) {
            evaluateModuleOwed(each);
            RETURN_IF_EXCEPTION(scope, void());
        }
    }
    // A module with nothing to run keeps no status. It has run with its importer.
    ModuleStatus statusOfRoot = status(root);
    if (statusOfRoot != ModuleStatus::Evaluated && statusOfRoot != ModuleStatus::EvaluatingAsync && !hasRun(root)) {
        // Its evaluation passes what has run below it. The link in front of that finds no mock that has ended.
        letGoOfMocksThatEndedBelow(root);
        RETURN_IF_EXCEPTION(scope, void());
        letStartsAskWhereSomethingIsDueBelow(root);
        return;
    }
    BitVector isPassed;
    passModuleThatHasRun(root, isPassed);
    RETURN_IF_EXCEPTION(scope, void());
    m_modulesWithNothingOwedBelow.set(root.index());
}

// `bun test` loads the original with all that it imports. A walk from the module itself ends at once: it counts as mocked.
void Modules::evaluateModulesOwedBelowOriginal(ModuleID file)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    for (const ModuleRequestData& request : m_table.imports(file)) {
        if (request.isDeferred)
            continue;
        evaluateModulesOwedBelow(moduleLoadedBy(request), true);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool wasLetGoByLoader(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasLetGoByLoader(module);
}

bool loadComesToModuleWhoseMockEnded(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->loadComesToModuleWhoseMockEnded(module);
}

void letGoOfMocksThatEnded(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->letGoOfMocksThatEnded();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
