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
    if (!m_modulesWithMockThatWaited.get(module.index()))
        return wasLoadedWithImporter(module);
    m_modulesWithMockThatWaited.clear(module.index());
    return m_modulesLoadedBeforeMockThatWaited.get(module.index());
}

void Modules::forgetLoadThatFailed(ModuleID file)
{
    forgetFailedRunOfESModuleThatMayRunAgain(file);
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
    forgetRunThatThrew(file);
    Vector<Identifier, 4> failedWithIt;
    for (auto& [key, entry] : loader()->moduleMap()) {
        if (entry->status() >= ModuleRegistryEntry::Status::FetchFailed)
            failedWithIt.append(Identifier::fromUid(*m_instance.vm, key.first));
    }
    for (const Identifier& key : failedWithIt)
        loader()->removeEntry(key);
}

bool Modules::importsModuleWhoseMockWaits(ModuleID root)
{
    BitVector isSeen(m_table.size());
    Vector<ModuleID, 16> toVisit { root };
    while (!toVisit.isEmpty()) {
        ModuleID importer = toVisit.takeLast();
        if (isSeen.quickSet(importer.index()))
            continue;
        for (const ModuleRequestData& request : m_table.imports(importer)) {
            ModuleID imported = moduleLoadedBy(request);
            if (m_modulesWithMockToRun.get(m_table.fileBehindFace(imported).value_or(imported).index()))
                return true;
            toVisit.append(imported);
        }
    }
    return false;
}

bool Modules::isImportedByGraphUnderEvaluation(ModuleID module)
{
    auto hasYetToRun = [&](ModuleID each) {
        ModuleStatus current = status(each);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    };
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
}

void Modules::forgetLoadsRefusedByCheckOfNames()
{
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
    JSObject* kept = std::exchange(m_errorsOfExportsThatBuildFoundAbsent, nullptr);
    if (!kept)
        return;
    UncheckedKeyHashSet<JSCell*> errorsOfAbsentExports;
    for (unsigned index = std::exchange(m_numberOfErrorsOfExportsThatBuildFoundAbsent, 0); index--;)
        errorsOfAbsentExports.add(kept->getDirectIndex(m_instance.globalObject, index).asCell());
    goModuleByModuleFromNowOn();
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

bool Modules::runBodyOfLinkedModuleAheadOfMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    forgetLoadsRefusedByCheckOfNames();
    auto hasYetToRun = [&](ModuleID each) {
        ModuleStatus current = status(each);
        return current == ModuleStatus::Unlinked || current == ModuleStatus::Linked;
    };
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || !hasYetToRun(module) || isMocked(module) || m_modulesWithMockToRun.get(module.index()))
        return false;
    if (!isImportedByGraphUnderEvaluation(module))
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

JSPromise* Modules::evaluationUnderWay(ModuleID module, bool hasWaitedForTurn)
{
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile())
        return nullptr;
    if (status(module) == ModuleStatus::EvaluatingAsync) {
        JSPromise* evaluation = evaluate(module, std::nullopt, nullptr, nullptr, true);
        return evaluation && evaluation->status() == JSPromise::Status::Pending ? evaluation : nullptr;
    }
    if (hasWaitedForTurn || status(module) != ModuleStatus::Linked || !isImportedByGraphUnderEvaluation(module))
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

bool Modules::runBodyOfImportedModuleAheadOfMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || hasBegunToLoad(module) || isEvicted(module))
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

JSObject* Modules::packageItselfBesideMock(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    if (!data(module).isESModuleOfProgram() || data(module).isFaceOfFile() || !isMocked(module) || isLeftAloneByMock(module) || isEvicted(module))
        return nullptr;
    if (!hasRunItself(module)) {
        if (hasBegunToLoad(module))
            return nullptr;
        VisitedModuleRecords seen;
        bool isReady = readyForSyncExecution(module, seen);
        RETURN_IF_EXCEPTION(scope, nullptr);
        if (!isReady)
            return nullptr;
        didEndMock(module);
        evaluateSynchronously(module, SynchronousEvaluationReason::Require);
        willMock(module);
        if (status(module) == ModuleStatus::Evaluated || status(module) == ModuleStatus::Failed)
            m_modulesThatRanBesideMock.set(module.index());
        if (scope.exception()) [[unlikely]] {
            setAsideErrorOfModuleThatRanBesideMock(module);
            return nullptr;
        }
    }
    JSValue own = requireModuleItselfBesideMock(module);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return own ? own.getObject() : nullptr;
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

void Modules::noteCallOfMock(ModuleID fileOrModule, bool importersKeepMockThatStands)
{
    MomentMockWasLaid call = momentOfNow();
    call.hadNamespace = !!namespaceIfCreated(moduleWithExportsOf(fileOrModule), ModulePhase::Evaluation);
    call.importersKeepMockThatStands = importersKeepMockThatStands;
    m_momentsWaitingMocksWereLaid.set(fileOrModule, call);
}

void Modules::forgetCallOfMock(ModuleID fileOrModule)
{
    m_momentsWaitingMocksWereLaid.remove(fileOrModule);
}

void Modules::deferMock(ModuleID module, AfterFailure afterFailure)
{
    if (m_modulesWithMockThatWaited.get(module.index()) && !mockInPlace(module) && !hasMockLaidOver(module))
        letGoOfImportersOf(module, ImportersLetGo::All);
    m_modulesWithMockToRun.set(module.index());
    m_modulesWhoseFactoryWasCalled.clear(module.index());
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
        auto noted = m_momentsWaitingMocksWereLaid.find(module);
        noteCallOfMock(module, noted != m_momentsWaitingMocksWereLaid.end() && noted->value.importersKeepMockThatStands);
    }
}

void Modules::endMocksThatStoodForLoadsInFlightSlow()
{
    if (m_importsBehindFactoriesHaveYetToGoOn || !m_modulesWhoseFactoryRuns.isEmpty() || m_loadsInFlight.numberBeingLoaded())
        return;
    auto leadsToMockThatStands = [&](ModuleID root) {
        BitVector reached = modulesThatLoadLeadsTo(root);
        reached.filter(m_modulesWithMockThatStandsForLoadsInFlight);
        return !reached.isEmpty();
    };
    for (ModuleID root : m_rootsOfImportsBehindFactories) {
        if (leadsToMockThatStands(root))
            return;
    }
    for (const TextThatWaits& text : textsThatWait()) {
        if (leadsToMockThatStands(text.module))
            return;
    }
    for (const TextOnItsWay& text : m_textsOnTheirWay) {
        if (leadsToMockThatStands(text.module))
            return;
    }
    for (size_t queued : m_modulesOfQueuedImports) {
        if (leadsToMockThatStands(ModuleID(queued)))
            return;
    }
    endMocksThatStandForLoadsInFlight();
}

void Modules::endMocksThatStandForLoadsInFlight()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope endsMocksThatStandForLoadsInFlight(m_endsMocksThatStandForLoadsInFlight, true);
    m_importsBehindFactoriesHaveYetToGoOn = false;
    while (!m_modulesWithMockThatStandsForLoadsInFlight.isEmpty()) {
        endMockThatStandsForLoadsInFlight(ModuleID(*m_modulesWithMockThatStandsForLoadsInFlight.begin()));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void Modules::endMockThatStandsForLoadsInFlight(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    SetForScope endsMocksThatStandForLoadsInFlight(m_endsMocksThatStandForLoadsInFlight, true);
    m_modulesWithMockThatStandsForLoadsInFlight.clear(module.index());
    bool anotherWaits = m_modulesWithMockToRun.get(module.index());
    bool asksAgain = m_modulesWithMockThatAsksAgain.get(module.index());
    bool wasLaidOverMockOfPreload = !m_modulesWhoseEndPutOffBringsNoMockOfPreloadBack.get(module.index());
    m_modulesWhoseEndPutOffBringsNoMockOfPreloadBack.clear(module.index());
    AOT::didEndMock(m_instance.globalObject, module, OriginalBehindEntryOfProgram::GoesWithPatch, wasLaidOverMockOfPreload);
    scope.exception();
    if (anotherWaits)
        deferMock(module, asksAgain ? AfterFailure::AsksAgain : AfterFailure::KeepsError);
}

void Modules::didGoOnBehindFactories(ModuleID root)
{
    didGoOnBehindFactories();
    auto numberOf = [&](const Vector<ModuleID, 2>& roots) { return std::ranges::count(roots, root); };
    if (numberOf(m_rootsOfImportsThatHaveGoneOn) < numberOf(m_rootsOfImportsBehindFactories))
        m_rootsOfImportsThatHaveGoneOn.append(root);
}

void Modules::didSettleImportBehindFactories(ModuleID root)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    m_rootsOfImportsBehindFactories.removeFirst(root);
    if (!m_rootsOfImportsThatHaveGoneOn.removeFirst(root) || m_modulesWithMockThatStandsForLoadsInFlight.isEmpty() || m_loadsInFlight.numberBeingLoaded())
        return;
    BitVector taken = modulesThatLoadLeadsTo(root);
    taken.filter(m_modulesWithMockThatStandsForLoadsInFlight);
    for (ModuleID other : m_rootsOfImportsThatHaveGoneOn)
        taken.exclude(modulesThatLoadLeadsTo(other));
    for (const TextThatWaits& text : textsThatWait())
        taken.exclude(modulesThatLoadLeadsTo(text.module));
    for (const TextOnItsWay& text : m_textsOnTheirWay)
        taken.exclude(modulesThatLoadLeadsTo(text.module));
    for (size_t queued : m_modulesOfQueuedImports)
        taken.exclude(modulesThatLoadLeadsTo(ModuleID(queued)));
    for (size_t module : taken) {
        endMockThatStandsForLoadsInFlight(ModuleID(module));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

bool Modules::haveLoadsInFlightTakenAnswerThatStandsOn(ModuleID mocked) const
{
    return !m_rootsOfImportsBehindFactories.containsIf([&](ModuleID root) {
        if (fileOf(root) == fileOf(mocked))
            return !namespaceIfCreated(root, ModulePhase::Evaluation);
        return status(root) < ModuleStatus::Evaluated && modulesThatLoadLeadsTo(root).get(mocked.index());
    });
}

void Modules::forgetMockThatWaits(ModuleID module)
{
    auto wasAskedForByLoadThatFailed = [&] {
        for (size_t kept : m_modulesWithLoadEndedByMock) {
            for (const ModuleRequestData& request : m_table.imports(ModuleID(kept))) {
                if (!request.isHoisted && !request.isDeferred && fileOf(moduleLoadedBy(request)) == fileOf(module))
                    return true;
            }
        }
        return false;
    };
    if (m_modulesWithMockToRun.clear(module.index()) && !mockInPlace(module) && !m_mocksWhoseEntryProgramDeleted.get(module.index()) && wasAskedForByLoadThatFailed()) [[unlikely]]
        dropErrorsOfLoadsEndedByMocks();
    letGoOfNamespaceOfFileThatRanAheadOfMock(module);
    if (!m_modulesWithMockThatWaited.get(module.index()))
        return;
    m_modulesWithMockThatWaited.clear(module.index());
    m_modulesLoadedBeforeMockThatWaited.clear(module.index());
    didEndMock(module);
}

bool Modules::runMockThatWaitsSlow(ModuleID module)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    MockDoor door(this, module, "runMock"_s);
    if (standsForLoadsInFlight(module)) {
        endMockThatStandsForLoadsInFlight(module);
        RETURN_IF_EXCEPTION(scope, false);
    }
    m_modulesWithMockToRun.clear(module.index());
    m_modulesWhoseFactoryWasCalled.set(module.index());
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

void Modules::noteImportFailedUnderMock(ModuleID root, JSValue error)
{
    if (status(root) >= ModuleStatus::Evaluating || m_modulesWithLoadEndedByMock.get(root.index()))
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
    auto wasMadeBefore = [&](ModuleID mocked) {
        for (const auto* moments : { &m_momentsWaitingMocksWereLaid, &m_momentsStandingMocksWereLaid }) {
            if (auto made = moments->find(mocked); made != moments->end() && made->value.moment < momentOfFailure)
                return true;
        }
        return false;
    };
    BitVector seen(m_table.size());
    Vector<ModuleID, 16> work { root };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        if ((isMocked(module) || hasMockThatWaitsOrRuns(fileOf(module))) && (wasMadeBefore(module) || wasMadeBefore(fileOf(module))))
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

void Modules::settleImportsThatFailedUnder(ModuleID mocked, FailedImports failedImports)
{
    if (failedImports == FailedImports::LoseEntryThatEvictsThem) {
        dropErrorOfMockKeptByLoader(mocked);
        m_mocksWhoseEntryProgramDeleted.set(mocked.index());
    }
    if (m_rootsOfImportsThatFailedUnderMocks.isEmpty()) [[likely]]
        return;
    m_filesAboveNoEvictedFile.clearAll();
    ModuleID file = fileOf(mocked);
    BitVector roots = m_rootsOfImportsThatFailedUnderMocks;
    for (size_t index : roots) {
        ModuleID root(index);
        BitVector keptBefore = m_modulesWithLoadEndedByMock;
        JSValue error = ensureLoaded(root).errorOfImportThatFailedUnderMock.get();
        keepErrorOfLoad(root, error, [&](ModuleID each) { return fileOf(each) == file; });
        BitVector kept = m_modulesWithLoadEndedByMock;
        kept.exclude(keptBefore);
        if (kept.isEmpty())
            continue;
        m_rootsOfImportsThatFailedUnderMocks.clear(index);
        ensureLoaded(root).errorOfImportThatFailedUnderMock.set(*this, JSValue());
        if (failedImports == FailedImports::LoseEntryThatEvictsThem) {
            m_modulesWhoseFailedLoadNothingEvicts.merge(kept);
            continue;
        }
        for (size_t module : kept)
            dropErrorOfLoadEndedByMock(ModuleID(module));
    }
}

void Modules::dropErrorOfLoadEndedByMock(ModuleID module)
{
    m_modulesWhoseFailedLoadNothingEvicts.clear(module.index());
    if (m_modulesWithLoadEndedByMock.clear(module.index()))
        ensureLoaded(module).errorKeptByLoader.set(*this, JSValue());
    dropErrorOfMockKeptByLoader(module);
}

void Modules::dropErrorOfMockKeptByLoader(ModuleID module)
{
    if (m_mockedModulesWithErrorKeptByLoader.clear(module.index())) [[unlikely]]
        ensureLoaded(module).errorOfMockKeptByLoader.set(*this, JSValue());
}

void Modules::dropErrorsOfLoadsEndedByMockOf(ModuleID mocked)
{
    if (m_mockedModulesWithErrorKeptByLoader.get(mocked.index())) [[unlikely]]
        dropErrorsOfLoadsEndedByMocks();
    dropErrorOfMockKeptByLoader(mocked);
    dropErrorsOfLoadsThatCameTo([&](ModuleID module) { return module == mocked; });
    if (!m_loaderMayHoldLoadsThatFactoryEnded || m_modulesWhoseFactoryRuns.get(mocked.index()) || isLoaderInMiddleOfGraph()) [[likely]]
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

void Modules::dropErrorsOfLoadsThatCameTo(const ScopedLambda<bool(ModuleID)>& isMocked)
{
    if (m_modulesWithLoadEndedByMock.isEmpty()) [[likely]]
        return;
    Vector<ModuleID, 8> above;
    for (size_t kept : m_modulesWithLoadEndedByMock) {
        if (m_modulesWhoseFailedLoadNothingEvicts.get(kept))
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
    for (ModuleID module : above)
        dropErrorOfLoadEndedByMock(module);
}

void Modules::dropErrorsOfLoadsEndedByMocks()
{
    BitVector kept = m_modulesWithLoadEndedByMock;
    kept.merge(m_mockedModulesWithErrorKeptByLoader);
    for (size_t module : kept)
        dropErrorOfLoadEndedByMock(ModuleID(module));
    BitVector roots = m_rootsOfImportsThatFailedUnderMocks;
    for (size_t root : roots)
        forgetImportFailedUnderMock(ModuleID(root));
}

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
    if (m_mockWhoseOriginalIsEvaluated == module) [[unlikely]]
        return false;
    evaluateModulesOwedBelow(module);
    RETURN_IF_EXCEPTION(scope, false);
    scope.release();
    if (m_modulesWithMockToRun.isEmpty()) [[likely]]
        return false;
    bool isAskedForByImportCall = m_instance.vm->aotState().frameOfImportThatAsksLoader;
    if (isAskedForByImportCall)
        dropErrorOfMockKeptByLoader(module);
    m_mocksWhoseFactoriesRanInsideTheirFetch.set(module.index(), m_modulesWithMockToRun.get(module.index()));
    SetForScope moduleThatLoaderFetches(m_moduleThatLoaderFetches, loadOfFetch(module));
    bool wasFetchedForStatement = m_modulesFetchedForStatementsBehindTurns.get(module.index());
    m_modulesFetchedForStatementsBehindTurns.clear(module.index());
    return runMocksBelow(module, wasFetchedForStatement || (!isAskedForByImportCall && isFetchedForImportStatement(module)) ? RootOfLoad::IsImportedByStatement : RootOfLoad::BeginsLoad);
}

void Modules::willFetchBehindTurn(ModuleID module)
{
    m_modulesFetchedForStatementsBehindTurns.set(module.index(), !m_instance.vm->aotState().frameOfImportThatAsksLoader && isFetchedForImportStatement(module));
}

bool Modules::mockFailedBehindImportStatement(ModuleID module)
{
    return m_modulesWithMockToRun.get(module.index()) && isFetchedForImportStatement(module);
}

void Modules::layMocksOfBodyAgain(ModuleID module)
{
    if (!mocksWaitToBeLaidAgain() || !host().layMocksAgain)
        return;
    host().layMocksAgain(m_instance.globalObject, [&](ModuleID each) {
        return each == module;
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

void Modules::letMockWaitBehindLateFailure(ModuleID module)
{
    leaveImportersAbove(module);
    m_modulesWithMockToRun.set(module.index());
    if (m_importsMadeByOwnFactory.removeAll(module) && !importsItselfByStatementsInTestRunFactories(m_table, module))
        m_modulesOwedBesideMock.set(module.index());
}

bool Modules::isOriginalLeftToHost(ModuleID mocked, const BitVector& held)
{
    if (!data(mocked).isESModuleOfProgram() || data(mocked).isFaceOfFile() || !host().evaluateOriginalOfMock)
        return true;
    BitVector seen(m_table.size());
    seen.quickSet(mocked.index());
    Vector<ModuleID, 16> work { mocked };
    while (!work.isEmpty()) {
        for (uint32_t number : m_table.evaluationRequests(work.takeLast())) {
            ModuleID imported(number);
            if (held.quickGet(number) || imported == mocked)
                continue;
            if (hasMockThatWaitsOrRuns(fileOf(imported)) || m_modulesWithMockToRun.get(imported.index()))
                return true;
            if (!seen.quickSet(number) && status(imported) <= ModuleStatus::Linked && !isMocked(imported))
                work.append(imported);
        }
    }
    return false;
}

JSObject* Modules::originalThatLoaderHolds(ModuleID mocked)
{
    ModuleRegistryEntry* entry = loader()->registryEntry(Identifier::fromString(*m_instance.vm, makeString(m_table.pathText(mocked), "?actual"_s)));
    auto* record = entry && entry->record() ? dynamicDowncast<CyclicModuleRecord>(entry->record()) : nullptr;
    if (!record || record->status() != CyclicModuleRecord::Status::Evaluated || record->evaluationError())
        return nullptr;
    return record->getModuleNamespace(m_instance.globalObject);
}

auto Modules::runOriginalInSweep(ModuleID mocked) -> OriginalInSweep
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue thrownByBody = errorOfModuleThatRanBesideMock(mocked)) [[unlikely]] {
        throwException(m_instance.globalObject, scope, thrownByBody);
        return OriginalInSweep::HasFailed;
    }
    if (hasRunItself(mocked) || hasBegunToLoad(mocked))
        return OriginalInSweep::HasRun;
    bool hasFactory = false;
    JSPromise* evaluation;
    {
        SetForScope mockWhoseOriginalIsEvaluated(m_mockWhoseOriginalIsEvaluated, mocked);
        evaluation = host().evaluateOriginalOfMock(m_instance.globalObject, mocked, hasFactory);
    }
    for (ImportersEnteredAboveMock& each : m_importersEnteredAboveMocks) {
        if (each.mocked == mocked)
            endPassing(each);
    }
    RETURN_IF_EXCEPTION(scope, OriginalInSweep::HasFailed);
    if (!evaluation)
        return OriginalInSweep::IsLeftToHost;
    evaluation->markAsHandled();
    if (evaluation->status() == JSPromise::Status::Rejected) {
        throwException(m_instance.globalObject, scope, evaluation->result());
        return OriginalInSweep::HasFailed;
    }
    if (!hasFactory && evaluation->status() == JSPromise::Status::Fulfilled) {
        bool hasNotSettled = runMockThatWaits(mocked);
        RETURN_IF_EXCEPTION(scope, OriginalInSweep::HasFailed);
        if (!hasNotSettled)
            leaveImportersAbove(mocked);
        return OriginalInSweep::HasRun;
    }
    for (ImportersEnteredAboveMock& each : m_importersEnteredAboveMocks) {
        if (each.mocked == mocked)
            each.original = Strong<JSPromise>(vm, evaluation);
    }
    return OriginalInSweep::HasRun;
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
    SetForScope moduleThatImportAsksFor(m_moduleThatImportAsksFor, ModuleID());
    SetForScope stateOfWaitOfImportThatAsks(m_stateOfWaitOfImportThatAsks, JSValue());
    SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, std::nullopt);
    SetForScope moduleThatLoaderFetches(m_moduleThatLoaderFetches, std::nullopt);
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
        return true;
    };

    if (needsOriginalAheadOfFactory(root)) {
        if (isOriginalLeftToHost(root, held) || !noteMock(root))
            return true;
        OriginalInSweep original = runOriginalInSweep(root);
        EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::HasFailed);
        return original == OriginalInSweep::HasFailed ? fail() : true;
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
        if (data(imported).isFaceOfFile() && needsOriginalAheadOfFactory(fileOf(imported)))
            break;
        bool isSeen = seen.quickSet(imported.index());
        if (needsOriginalAheadOfFactory(imported)) {
            noteMock(imported);
            bool hasRun = holdPath();
            if (scope.exception())
                return fail();
            if (!hasRun) {
                if (endsBehindMockThatWasPassed(imported))
                    break;
                return fail();
            }
            if (!needsOriginalAheadOfFactory(imported)) {
                fail();
                RELEASE_AND_RETURN(scope, sweepAheadOfFactories(root));
            }
            if (isOriginalLeftToHost(imported, held))
                break;
            bool hasBeenEvaluated = m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == imported && above.original; });
            OriginalInSweep original = hasBeenEvaluated ? OriginalInSweep::HasRun : runOriginalInSweep(imported);
            EXCEPTION_ASSERT(!scope.exception() || original == OriginalInSweep::HasFailed);
            if (original == OriginalInSweep::IsLeftToHost)
                break;
            if (original == OriginalInSweep::HasFailed) {
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

bool Modules::isHeldBySweepUnderWay(ModuleID module) const
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
        bool isMockOfOriginalUnderWay = module == above.mocked && m_mockWhoseOriginalIsEvaluated == module;
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
    if (!m_importersEnteredAboveMocks.isEmpty()) [[unlikely]]
        leaveImportersWhere([&](const ImportersEnteredAboveMock& above) { return above.mocked == mocked; });
}

void Modules::didBeginWalkOnEmptyStack(ModuleID root)
{
    if (m_importersEnteredAboveMocks.isEmpty()) [[likely]]
        return;
    leaveImportersWhere([&](const ImportersEnteredAboveMock& above) { return above.root == root; });
    for (ImportersEnteredAboveMock& above : m_importersEnteredAboveMocks)
        endPassing(above);
}

bool Modules::runMocksBelowSlow(ModuleID root, RootOfLoad rootOfLoad)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (m_linksOfLoadThatMayFail && !isWalkedForLoadThatMayFail(root) && (status(m_linksOfLoadThatMayFail->root) < ModuleStatus::Evaluating || reachesWhatLoadThatMayFailLinkedAnew(root))) [[unlikely]]
        m_linksOfLoadThatMayFail = std::nullopt;
    auto bringsBackWhereLoadFails = makeScopeExit([&] {
        if (scope.exception()) [[unlikely]]
            bringBackLinksOfLoadThatFailed(root);
    });
    {
        LinksBeforeLoad before;
        SetForScope keepsLinksBefore(m_linksBeforeLoadInFlight, &before);
        if (mocksWaitToBeLaidAgain() || !m_modulesOwedToImportersThatRan.isEmpty() || !m_modulesWithMockToLetGo.isEmpty() || !m_modulesThatLoaderLetGo.isEmpty()) [[unlikely]] {
            evaluateModulesOwedBelow(root);
            if (scope.exception()) [[unlikely]] {
                bringBackLinks(before, LinksOfLoad::AreForgotten);
                return false;
            }
        }
        if (!m_endedMocks.isEmpty()) [[unlikely]]
            linkAnewWhoeverReadsEndedMockBelow(root);
        if (!m_modulesLoadedAnewWhoseNamesAreOwed.isEmpty()) [[unlikely]] {
            checkRequiredNamesOfModulesLoadedAnewBelow(root);
            if (scope.exception()) [[unlikely]] {
                bringBackLinks(before, LinksOfLoad::AreMetByLaterLoads);
                return false;
            }
        }
        if (!before.moments.isEmpty()) [[unlikely]] {
            if (!isWalkedForLoadThatMayFail(root))
                m_linksOfLoadThatMayFail = LinksOfLoadThatMayFail { root, { } };
            m_linksOfLoadThatMayFail->before.moments.appendVector(before.moments);
            m_linksOfLoadThatMayFail->before.modulesThatLoaderLetGo.merge(before.modulesThatLoaderLetGo);
            m_linksOfLoadThatMayFail->before.importersOfMockThatEnded.merge(before.importersOfMockThatEnded);
        }
    }
    if (m_modulesWithMockToRun.isEmpty()) [[likely]]
        return false;
    MocksBelowRoot mocks = mocksBelow(root);
    bool mayComeToOne = false;
    for (size_t module : m_modulesWithMockToRun)
        mayComeToOne |= !isMocked(ModuleID(module)) || !mocks.hides(ModuleID(module));
    if (!mayComeToOne)
        return false;
    std::optional<ModuleID> fileOfRoot = m_table.fileBehindFace(root);
    bool walksInFrontOfMocks = rootOfLoad == RootOfLoad::BeginsLoad && m_modulesWhoseFactoryRuns.isEmpty() && !m_evaluatesSynchronously && !m_rootWhoseLoadIsSwept && !vm.aotState().requireRunsMocks && !vm.synchronousModuleQueue();
    bool hasCalledOrdinaryFactories = !walksInFrontOfMocks;
    bool hasPutMockAside = false;
    BitVector reached(m_table.size());
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty() || std::exchange(hasPutMockAside, false)) {
        if (work.isEmpty()) {
            hasCalledOrdinaryFactories = true;
            reached.clearAll();
            work.append(root);
        }
        ModuleID module = work.takeLast();
        if (reached.quickSet(module.index()) || isHeldBySweepUnderWay(module) || m_mockWhoseOriginalIsEvaluated == module)
            continue;
        bool isImportedByStatement = (module != root && module != fileOfRoot) || rootOfLoad == RootOfLoad::IsImportedByStatement;
        JSValue kept = errorOfLoadEndedByMock(module);
        if (!kept && isImportedByStatement && m_mockedModulesWithErrorKeptByLoader.get(module.index()))
            kept = loadedIfExists(module)->errorOfMockKeptByLoader.get();
        bool isAskedForByOwnFactory = !kept && !isImportedByStatement && m_moduleThatProgramImportsNow != module && m_mockedModulesWithErrorKeptByLoader.get(module.index()) && m_importsMadeByOwnFactory.removeFirst(module);
        if (isAskedForByOwnFactory && importsItselfByStatementsInTestRunFactories(m_table, module)) [[unlikely]]
            kept = loadedIfExists(module)->errorOfMockKeptByLoader.get();
        else if (isAskedForByOwnFactory) [[unlikely]]
            m_modulesOwedBesideMock.set(module.index());
        if (kept) [[unlikely]] {
            keepErrorOfLoad(root, kept, [&](ModuleID each) { return each == module; });
            throwException(m_instance.globalObject, scope, kept);
            return false;
        }
        bool runsMock = m_modulesWithMockToRun.get(module.index()) && !standsForLoadsInFlight(module) && (isImportedByStatement || !m_modulesOwedBesideMock.get(module.index()));
        bool isPutAside = runsMock && isImportedByStatement && !hasCalledOrdinaryFactories && m_modulesWithMockThatAsksAgain.get(module.index());
        hasPutMockAside |= isPutAside;
        if (runsMock && !isPutAside) {
            if (walksInFrontOfMocks && m_modulesWithMockThatAsksAgain.get(module.index())) {
                bool hasRun = sweepAheadOfFactories(root);
                RETURN_IF_EXCEPTION(scope, false);
                if (!hasRun)
                    return false;
                if (loadWaitsForOriginalBelow(root))
                    return true;
                if (!m_modulesWithMockToRun.get(module.index())) {
                    reached.quickClear(module.index());
                    work.append(module);
                    continue;
                }
            }
            bool factoryHasNotSettled = runMockThatWaits(module);
            if (!factoryHasNotSettled)
                leaveImportersAbove(module);
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
        if (isMocked(module) && !isPutAside && (module != root || !mocks.rootRunsBesideItsMock()))
            continue;
        for (const ModuleRequestData& request : m_table.requests(module) | std::views::reverse) {
            if (!request.isHoisted && (!request.isRequire || data(module).isFaceOfFile()) && seesMockOf(moduleLoadedBy(request), module) && !keepsMockThatStandsWhileAnotherWaits(moduleLoadedBy(request), module) && (data(module).isFaceOfFile() || !keepsEndedMockWhereItIsAskedForAgain(fileOf(moduleLoadedBy(request)), module)))
                work.append(moduleLoadedBy(request));
        }
    }
    return false;
}

BitVector Modules::modulesThatLoadLeadsTo(ModuleID root) const
{
    BitVector reached(m_table.size());
    bool rootRunsBesideItsMock = mocksBelow(root).rootRunsBesideItsMock();
    Vector<ModuleID, 32> work { root };
    while (!work.isEmpty()) {
        ModuleID next = work.takeLast();
        if (reached.quickSet(next.index()) || (isMocked(next) && !needsOriginalAheadOfFactory(next) && (next != root || !rootRunsBesideItsMock)))
            continue;
        for (const ModuleRequestData& request : m_table.requests(next)) {
            if (!request.isHoisted && seesMockOf(moduleLoadedBy(request), next))
                work.append(moduleLoadedBy(request));
        }
    }
    return reached;
}

std::optional<ModuleID> Modules::loadOfFetch(ModuleID fetched)
{
    ModuleID file = fileOf(fetched);
    bool isOrWillBeMocked = isMocked(fetched) || isMocked(file) || m_modulesWithMockToRun.get(fetched.index()) || m_modulesWithMockToRun.get(file.index());
    return isOrWillBeMocked ? std::nullopt : std::optional(file);
}

JSPromise* Modules::factoryThatFetchWaitsFor(ModuleID fetched, JSValue& state)
{
    SetForScope moduleThatLoaderFetches(m_moduleThatLoaderFetches, loadOfFetch(fetched));
    return factoryThatWaitsBelow(fetched, FactoryAskedForBy::Loader, state);
}

bool Modules::tellHostOfRequire(ModuleID required)
{
    if (!m_hasBegunTestFile) [[likely]]
        return false;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    JSValue state;
    JSPromise* factory = factoryThatWaitsBelow(required, FactoryAskedForBy::Require, state);
    scope.assertNoExceptionExceptTermination();
    return factory;
}

JSPromise* Modules::factoryThatWaitsBelow(ModuleID root, FactoryAskedForBy askedBy, JSValue& state)
{
    if (JSPromise* original = originalThatLoadWaitsForBelow(root)) [[unlikely]]
        return original;
    if (!host().factoryThatWaits)
        return nullptr;
    std::optional<BitVector> reached;
    return host().factoryThatWaits(m_instance.globalObject, [&](ModuleID module) {
        if (!reached)
            reached = modulesThatLoadLeadsTo(root);
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

JSObject* packageItselfBesideMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    if (!modules)
        return nullptr;
    MockDoor door(modules, module, "packageItselfBesideMock"_s);
    return modules->packageItselfBesideMock(module);
}

bool runBodyOfImportedModuleAheadOfMock(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "runBodyOfImportedModuleAheadOfMock"_s);
    return modules && modules->runBodyOfImportedModuleAheadOfMock(module);
}

JSPromise* evaluationUnderWay(JSGlobalObject* realm, ModuleID module, bool hasWaitedForTurn)
{
    Modules* modules = modulesOf(realm);
    return modules ? modules->evaluationUnderWay(module, hasWaitedForTurn) : nullptr;
}

bool runBodyOfLinkedModuleAheadOfMock(JSGlobalObject* realm, ModuleID module, bool displacesEarlierMock, bool setsModuleAside)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the mock call"_s);
    if (!modules)
        return false;
    auto scope = DECLARE_THROW_SCOPE(realm->vm());
    if (modules->standsForLoadsInFlight(module))
        modules->endMocksThatStandForLoadsInFlight();
    RETURN_IF_EXCEPTION(scope, false);
    modules->endMocksThatStoodForLoadsInFlight();
    RETURN_IF_EXCEPTION(scope, false);
    modules->forgetLoadThatFailed(module);
    modules->countMocksAnewBehindLoad(module);
    modules->noteCallOfMock(module);
    modules->noteWhetherFileHasThrownWhereMockIsMade(module, displacesEarlierMock, setsModuleAside);
    modules->dropErrorsOfLoadsEndedByMockOf(module);
    RELEASE_AND_RETURN(scope, modules->runBodyOfLinkedModuleAheadOfMock(module));
}

void letMockStandForLoadsInFlight(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the mock is gone behind its factory"_s);
    if (modules && modules->mockInPlace(module))
        modules->letMockStandForLoadsInFlight(module);
}

bool standsForLoadsInFlight(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->standsForLoadsInFlight(module);
}

bool isImportHeld(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isImportHeld(module);
}

void endMocksThatStoodForLoadsInFlight(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->endMocksThatStoodForLoadsInFlight();
}

void dropErrorsOfLoadsEndedByMocks(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->dropErrorsOfLoadsEndedByMocks();
}

void letMockWaitBehindLateFailure(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    MockDoor door(modules, module, "the factory failed behind an await"_s);
    if (modules)
        modules->letMockWaitBehindLateFailure(module);
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
    return modules ? modules->factoryThatFetchWaitsFor(module, state) : nullptr;
}

void willFetchBehindTurn(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->willFetchBehindTurn(module);
}

void fetchDidGoOnBehindFactory(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->didGoOnBehindFactories();
}

bool mockFailedBehindImportStatement(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->mockFailedBehindImportStatement(module);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
