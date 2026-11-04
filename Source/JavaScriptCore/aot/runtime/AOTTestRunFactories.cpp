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
#include "ModuleRegistryEntry.h"
#include <ranges>

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
    if (!m_modulesWithMockThatWaited.get(module.index()))
        return wasLoadedWithImporter(module);
    m_modulesWithMockThatWaited.clear(module.index());
    return m_modulesLoadedBeforeMockThatWaited.get(module.index());
}

// A graph is loaded before a module of it begins, so a helper that mocks finds what its importer imports beside it linked. The body does not run ahead of an import whose own body is up the stack.
// True: the module is linked and no load has come to it: whatever imports it met a mock in its own place.
bool Modules::runBodyOfLinkedModuleAheadOfMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    auto hasYetToRun = [&](ModuleID each) {
        ModuleStatus current = status(each);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    };
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || !hasYetToRun(module) || isMocked(module) || m_modulesWithMockToRun.get(module.index()))
        return false;
    // Only a graph whose evaluation is under way, or was ended by an error, has linked what it did not come to. A load that met a mock in place of the module never fetched it.
    auto isImportedByGraphUnderEvaluation = [&] {
        BitVector isReached(m_table.size());
        Vector<ModuleID, 16> importers;
        for (unsigned index = 0; index < m_table.size(); ++index) {
            ModuleStatus current = status(ModuleID(index));
            if (current == ModuleStatus::Evaluating || current == ModuleStatus::EvaluatingAsync || current == ModuleStatus::Failed)
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
                if (imported == module)
                    return true;
                if (hasYetToRun(imported) && !isMocked(imported) && !m_modulesWithMockToRun.get(imported.index()))
                    importers.append(imported);
            }
        }
        return false;
    };
    if (!isImportedByGraphUnderEvaluation())
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
            if (hasYetToRun(moduleLoadedBy(request)))
                toVisit.append(moduleLoadedBy(request));
        }
    }
    JSPromise* evaluation = evaluate(module, std::nullopt, nullptr);
    RETURN_IF_EXCEPTION(scope, false);
    if (evaluation->status() == JSPromise::Status::Rejected) {
        evaluation->markAsHandled();
        throwException(m_instance.globalObject, scope, evaluation->result());
    }
    return false;
}

bool Modules::runBodyOfImportedModuleAheadOfMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || hasBegunToLoad(module))
        return false;
    VisitedModuleRecords seen;
    bool isReady = readyForSyncExecution(module, seen);
    RETURN_IF_EXCEPTION(scope, false);
    if (!isReady)
        return false;
    forgetMockThatWaits(module);
    RETURN_IF_EXCEPTION(scope, false);
    evaluateSynchronously(module, SynchronousEvaluationReason::Require);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

void Modules::runBodyOfRequiredFileAheadOfMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    forgetMockThatWaits(module);
    RETURN_IF_EXCEPTION(scope, void());
    m_filesThatRanAheadOfMock.set(module.index());
    scope.release();
    require(module, ModuleID(), RequiredBy::Program);
}

// The loader has asked for a module and has yet to run it.
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

// Compiled code that requires a module again reads the status of that module and asks nobody where it has run: no load would come by.
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

void Modules::noteCallOfMock(ModuleID fileOrModule)
{
    m_momentsWaitingMocksWereLaid.set(fileOrModule, momentOfNow());
}

void Modules::forgetCallOfMock(ModuleID fileOrModule)
{
    m_momentsWaitingMocksWereLaid.remove(fileOrModule);
}

void Modules::deferMock(ModuleID module, AfterFailure afterFailure)
{
    // Its factory has failed and it waits again. The load that came to it has left the loader of `bun test`, which makes it anew.
    if (m_modulesWithMockThatWaited.get(module.index()) && !mockInPlace(module) && !hasMockLaidOver(module))
        letGoOfImportersOf(module, ImportersLetGo::All);
    // Whoever was linked against the earlier mock reads it until the load that this one waits for.
    m_modulesWithMockToRun.set(module.index());
    m_modulesWithMockThatAsksAgain.set(module.index(), afterFailure == AfterFailure::AsksAgain);
    dropErrorsOfLoadsEndedByMockOf(module);
    if (data(module).kind == ModuleKind::CommonJs)
        ensureRecord(module);
    if (data(module).isOfLibrary())
        willMockLibraryModule(module);
    if (!data(module).isOfLibrary() && m_table.hasFilePath(module)) {
        m_modulesWithMockThatWaited.set(module.index());
        m_modulesLoadedBeforeMockThatWaited.set(module.index(), wasLoadedWithImporter(module));
        willMock(module);
        noteCallOfMock(module);
    }
}

// What deferMock() did is taken back. The record of a CommonJS file stays: its code goes on asking the run time for its status.
void Modules::forgetMockThatWaits(ModuleID module)
{
    m_modulesWithMockToRun.clear(module.index());
    if (!m_modulesWithMockThatWaited.get(module.index()))
        return;
    m_modulesWithMockThatWaited.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didEndMock(module);
}

// It does not wait while its factory runs, which may load the module. A factory that fails leaves nothing in place of the module: the next load asks the mock again, as under `bun test`.
bool Modules::runMockThatWaitsSlow(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockDoor door(this, module, "runMock"_s);
    m_modulesWithMockToRun.clear(module.index());
    bool ranAlready = m_modulesWhoseFactoryRuns.set(module.index());
    bool factoryHasNotSettled = host().runMock(m_instance.globalObject, module);
    if (!ranAlready)
        m_modulesWhoseFactoryRuns.clear(module.index());
    if (Exception* exception = scope.exception()) [[unlikely]] {
        m_modulesWithMockToRun.set(module.index());
        m_loaderMayHoldLoadsThatFactoryEnded = true;
        if (m_importsMadeByOwnFactory.contains(module) && !m_instance.vm->isTerminationException(exception)) {
            m_mockedModulesWithErrorKeptByLoader.set(module.index());
            ensureLoaded(module).errorOfMockKeptByLoader.set(*this, exception->value());
        }
        return false;
    }
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
        if (isMocked(module) || hasBegunToLoad(module))
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
            if (m_modulesWithLoadEndedByMock.set(importer.index()))
                continue;
            ensureLoaded(importer).errorKeptByLoader.set(*this, error);
        }
    }
}

void Modules::dropErrorOfLoadEndedByMock(ModuleID module)
{
    if (m_modulesWithLoadEndedByMock.clear(module.index()))
        ensureLoaded(module).errorKeptByLoader.set(*this, JSValue());
    dropErrorOfMockKeptByLoader(module);
}

void Modules::dropErrorOfMockKeptByLoader(ModuleID module)
{
    if (m_mockedModulesWithErrorKeptByLoader.clear(module.index())) [[unlikely]]
        ensureLoaded(module).errorOfMockKeptByLoader.set(*this, JSValue());
}

// A new mock takes the record of the module out of the loader, and with it the records of all that import the module.
void Modules::dropErrorsOfLoadsEndedByMockOf(ModuleID mocked)
{
    dropErrorOfMockKeptByLoader(mocked);
    dropErrorsOfLoadsThatCameTo([&](ModuleID module) { return module == mocked; });
    // A factory that failed waits anew in the middle of the load that it ended: what else of that load asks for the module shares the error.
    if (!m_loaderMayHoldLoadsThatFactoryEnded || m_modulesWhoseFactoryRuns.get(mocked.index()) || isLoaderInMiddleOfGraph()) [[likely]]
        return;
    m_loaderMayHoldLoadsThatFactoryEnded = false;
    takeFailedLoadsOutOfLoader([&](ModuleID module) { return module == mocked; });
}

// Where the loader itself was asked for the graph, its entries hold the error.
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

void Modules::dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked)
{
    if (m_modulesWithLoadEndedByMock.isEmpty()) [[likely]]
        return;
    Vector<ModuleID, 8> above;
    for (size_t kept : m_modulesWithLoadEndedByMock) {
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
    for (ModuleID module : above)
        dropErrorOfLoadEndedByMock(module);
}

// The loader has made an entry for what a statement asks for, and none yet for what an import() or a require() asks for.
bool Modules::isFetchedForImportStatement(ModuleID module) const
{
    ModuleRecord* record = m_records.get(module);
    ModuleRegistryEntry* entry = loader()->registryEntry(record ? record->moduleKey() : Identifier::fromString(*m_instance.vm, m_table.pathText(module)));
    return entry && entry->status() == ModuleRegistryEntry::Status::New;
}

bool Modules::runMocksBelowModuleThatLoaderFetchesSlow(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockDoor door(this, module, "the loader fetches it"_s);
    if (mocksWaitToBeLaidAgain()) [[unlikely]] {
        layMocksAgainBelow(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    scope.release();
    if (m_modulesWithMockToRun.isEmpty()) [[likely]] {
        evaluateModulesOwedBelow(module);
        return false;
    }
    // The entry that a statement left when the factory failed is still there.
    bool isAskedForByImportCall = m_instance.vm->aotState().frameOfImportThatAsksLoader;
    if (isAskedForByImportCall)
        dropErrorOfMockKeptByLoader(module);
    return runMocksBelow(module, !isAskedForByImportCall && isFetchedForImportStatement(module) ? RootOfLoad::IsImportedByStatement : RootOfLoad::BeginsLoad);
}

bool Modules::mockFailedBehindImportStatement(ModuleID module)
{
    return m_modulesWithMockToRun.get(module.index()) && isFetchedForImportStatement(module);
}

// Ahead of what is owed below: a module that is mocked again stays owed.
void Modules::layMocksAgainBelow(ModuleID root)
{
    if (!host().layMocksAgain)
        return;
    BitVector reached = reachedByStatements(root, AtMock::Stops);
    host().layMocksAgain(m_instance.globalObject, [&](ModuleID module) {
        return reached.quickGet(module.index());
    });
}

bool Modules::mocksWaitToBeLaidAgain() const
{
    return !m_modulesThatLayMocksAgain.isEmpty();
}

BitVector Modules::reachedByStatements(ModuleID root, AtMock atMock) const
{
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()) || (atMock == AtMock::Stops && isMocked(module)))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module))
            work.append(moduleLoadedBy(request));
    }
    return reached;
}

bool Modules::importsByStatements(ModuleID importer, ModuleID imported) const
{
    return reachedByStatements(importer, AtMock::GoesOn).quickGet(imported.index());
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

bool Modules::runMocksBelowSlow(ModuleID root, RootOfLoad rootOfLoad)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (mocksWaitToBeLaidAgain()) [[unlikely]] {
        layMocksAgainBelow(root);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || !m_modulesThatLoaderLetGo.isEmpty()) [[unlikely]] {
        evaluateModulesOwedBelow(root);
        RETURN_IF_EXCEPTION(scope, false);
    }
    if (!m_endedMocks.isEmpty()) [[unlikely]]
        linkAnewWhoeverReadsEndedMockBelow(root);
    if (m_modulesWithMockToRun.isEmpty()) [[likely]]
        return false;
    // The walk does nothing where it comes to none of them.
    MocksBelowRoot mocks = mocksBelow(root);
    bool mayComeToOne = false;
    for (size_t module : m_modulesWithMockToRun)
        mayComeToOne |= !isMocked(ModuleID(module)) || !mocks.hides(ModuleID(module));
    if (!mayComeToOne)
        return false;
    // An import() of a CommonJS file begins at the face of the file.
    std::optional<ModuleID> fileOfRoot = m_table.fileBehindFace(root);
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()))
            continue;
        bool isImportedByStatement = (module != root && module != fileOfRoot) || rootOfLoad == RootOfLoad::IsImportedByStatement;
        JSValue kept = errorOfLoadEndedByMock(module);
        if (!kept && isImportedByStatement && m_mockedModulesWithErrorKeptByLoader.get(module.index()))
            kept = loadedIfExists(module)->errorOfMockKeptByLoader.get();
        bool isAskedForByOwnFactory = !kept && !isImportedByStatement && m_moduleThatProgramImportsNow != module && m_mockedModulesWithErrorKeptByLoader.get(module.index()) && m_importsMadeByOwnFactory.removeFirst(module);
        // The factory gets the module itself. A statement below that asks for the module comes to the mock, which failed.
        if (isAskedForByOwnFactory && importsItselfByStatementsInTestRunFactories(m_table, module)) [[unlikely]]
            kept = loadedIfExists(module)->errorOfMockKeptByLoader.get();
        else if (isAskedForByOwnFactory) [[unlikely]]
            m_modulesOwedBesideMock.set(module.index());
        if (kept) [[unlikely]] {
            keepErrorOfLoad(root, kept, [&](ModuleID each) { return each == module; });
            throwException(m_instance.globalObject, scope, kept);
            return false;
        }
        if (m_modulesWithMockToRun.get(module.index()) && (isImportedByStatement || !m_modulesOwedBesideMock.get(module.index()))) {
            bool factoryHasNotSettled = runMockThatWaits(module);
            if (Exception* exception = scope.exception()) [[unlikely]] {
                if (vm.isTerminationException(exception) || m_modulesWithMockThatAsksAgain.get(module.index()))
                    return false;
                keepErrorOfLoad(root, exception->value(), [&](ModuleID each) { return each == module; });
                if (isImportedByStatement) {
                    m_mockedModulesWithErrorKeptByLoader.set(module.index());
                    ensureLoaded(module).errorOfMockKeptByLoader.set(*this, exception->value());
                }
                return false;
            }
            if (factoryHasNotSettled)
                return true;
        }
        if (isMocked(module))
            continue;
        // Depth first, in the order of the text: what is taken first is appended last.
        for (const ModuleRequestData& request : m_table.requests(module) | std::views::reverse) {
            // The load of a module that keeps the module itself does not come to the mock. A require() runs the factory of its own file where it stands; the face of a CommonJS file asks for the file as an import does.
            if (!request.isHoisted && (!request.isRequire || data(module).isFaceOfFile()) && seesMockOf(moduleLoadedBy(request), module))
                work.append(moduleLoadedBy(request));
        }
    }
    return false;
}

// Another load may have come to the mock first: `await Promise.all([import("./a"), import("./b")])`.
JSPromise* Modules::factoryThatWaitsBelow(ModuleID root, FactoryAskedForBy askedBy, JSValue& state)
{
    if (!host().factoryThatWaits)
        return nullptr;
    std::optional<BitVector> reached;
    return host().factoryThatWaits(m_instance.globalObject, [&](ModuleID module) {
        if (!reached) {
            reached.emplace(m_table.size());
            Vector<ModuleID, 32> work { root };
            while (!work.isEmpty()) {
                ModuleID next = work.takeLast();
                if (reached->quickSet(next.index()) || isMocked(next))
                    continue;
                for (const ModuleRequestData& request : m_table.requests(next)) {
                    if (!request.isHoisted)
                        work.append(moduleLoadedBy(request));
                }
            }
        }
        return reached->quickGet(module.index());
    }, askedBy, state);
}

void runBodyOfRequiredFileAheadOfMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return;
    MockDoor door(modules, module, "runBodyOfRequiredFileAheadOfMock"_s);
    modules->runBodyOfRequiredFileAheadOfMock(module);
}

bool runBodyOfImportedModuleAheadOfMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "runBodyOfImportedModuleAheadOfMock"_s);
    return modules && modules->runBodyOfImportedModuleAheadOfMock(module);
}

bool runBodyOfLinkedModuleAheadOfMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the mock call"_s);
    // A mock without a factory is made of the original, and may come into its place long behind its call.
    if (modules)
        modules->noteCallOfMock(module);
    return modules && modules->runBodyOfLinkedModuleAheadOfMock(module);
}

bool importsByStatements(JSGlobalObject* realm, ModuleID importer, ModuleID imported)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->importsByStatements(importer, imported);
}

void layMocksAgainAtNextLoad(JSGlobalObject* realm, ModuleID module, bool waits)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, waits ? "the mocks of its body wait for the next load"_s : "the mocks of its body are laid again"_s);
    if (modules)
        modules->layMocksAgainAtNextLoad(module, waits);
}

JSPromise* factoryThatWaitsBelow(JSGlobalObject* realm, ModuleID module, JSValue& state)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->factoryThatWaitsBelow(module, FactoryAskedForBy::Loader, state) : nullptr;
}

bool mockFailedBehindImportStatement(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mockFailedBehindImportStatement(module);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
