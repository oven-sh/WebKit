/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTModuleRecord.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "InternalFieldTuple.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "ModuleLoaderPayload.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleRegistryEntry.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static void queueStepOfImportInImportTurns(JSPromise* importPromise, ModuleID module, uint8_t stepsLeft)
{
    JSGlobalObject* realm = importPromise->realm();
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImportStep, stepsLeft, importPromise, jsNumber(module.index()), JSValue());
}

static void settleAsLoaderDoesInImportTurns(JSGlobalObject* realm, JSPromise* promiseOfProgram, JSPromise* loaded)
{
    VM& vm = realm->vm();
    if (loaded->status() == JSPromise::Status::Fulfilled)
        promiseOfProgram->fulfill(vm, loaded->result());
    else
        promiseOfProgram->resolve(realm, vm, loaded);
}

void Modules::tellHostOfImport(JSPromise* promise, const String& path, std::optional<ModuleID> file)
{
    auto* didStartImport = host().didStartImport;
    if (!didStartImport || promise->status() != JSPromise::Status::Pending)
        return;
    didStartImport(m_instance.globalObject, promise, path, [&] {
        return file ? modulesInLoadGraph(*file) : BitVector();
    });
}

bool Modules::isTextReadOnOtherThread(ModuleID file)
{
    return host().scheduleFetchFulfillment && isFileOfProgram(file) && !m_instance.vm->synchronousModuleQueue() && !isFetchedInsideCall(file);
}

bool Modules::isTestRun() const
{
    return host().isInPreload && host().isInPreload(m_instance.globalObject).has_value();
}

bool Modules::hostHasProgramSources() const
{
    return host().hasProgramSources && host().hasProgramSources(m_instance.globalObject);
}

bool Modules::hasTextAtHand() const
{
    return !m_innermostEagerLoad && m_queuedFetches.isEmpty() && hostHasProgramSources() && !isTestRun();
}

bool Modules::isFetchedBesideFactory(ModuleID module)
{
    if (!isTestRun() || !host().hasRunningFactory(m_instance.globalObject) || isLinkedAndUnevaluated(module)) [[likely]]
        return false;
    for (size_t index : modulesInLoadGraph(module)) {
        if (isPatchPending(fileOf(ModuleID(index))))
            return true;
    }
    return false;
}

bool Modules::textOfImportComesInLaterTurn(ModuleID module, AskedBy askedBy)
{
    ModuleID file = fileOf(module);
    if (!isTextReadOnOtherThread(file) || activeMock(module) || activeMock(file))
        return false;
    if (isCommonJSModuleOrFacade(module)) {
        for (ModuleID moduleOrFacade : { module, file }) {
            if (status(moduleOrFacade) != ModuleStatus::Unlinked && status(moduleOrFacade) != ModuleStatus::Linked)
                return false;
        }
    } else if (!isNotLoaded(module) || (hasInstances(file) && m_linkedInstances.get(file.index())))
        return false;
    if (askedBy == AskedBy::Import) {
        for (ModuleID moduleOrFacade : { module, file }) {
            if (loader()->registryEntry(moduleKey(moduleOrFacade)))
                return false;
        }
    }
    return true;
}

void Modules::holdTextOfCall(JSPromise* promise, ModuleID module, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller)
{
    VM& vm = *m_instance.vm;
    if (m_innermostEagerLoad || !m_queuedFetches.isEmpty()) {
        std::optional<ModuleID> evaluatingCaller = m_innermostEagerLoad ? evaluatingModuleOrFacade(*m_innermostEagerLoad) : std::nullopt;
        enqueueFetch(promise, module, requested, referrerAsyncOrder, evaluatingCaller ? turnInWhichTextCame(*evaluatingCaller) : m_sourceArrivalCount);
        return;
    }
    auto fetch = makeUnique<BatchedFetch>();
    fetch->place = static_cast<uint64_t>(m_sourceArrivalCount) << 32 | m_numberOfTextsAskedFor++;
    fetch->batchNumber = m_batchBeingFulfilled.value_or(m_currentBatchNumber);
    fetch->promise.set(vm, promise);
    fetch->module = module;
    fetch->file = fileOf(module);
    if (requested)
        fetch->requestedRecord.set(vm, requested);
    fetch->referrerAsyncOrder = referrerAsyncOrder;
    fetch->caller = caller;
    fetch->callSequenceNumber = vm.aotState().momentOfLoads;
    fetch->precedingDiskReads = host().scheduleFetchFulfillment(m_instance.globalObject);
    m_batchedFetches.addCall(WTF::move(fetch));
}

bool Modules::BatchedFetches::containsCallOf(ModuleID module, ModuleID file) const
{
    auto found = m_callsByFile.find(file);
    return found != m_callsByFile.end() && found->value.containsIf([&](const BatchedFetch* call) { return call->module == module; });
}

void Modules::BatchedFetches::addCall(std::unique_ptr<BatchedFetch> call)
{
    m_callsByFile.add(call->file, Vector<BatchedFetch*, 1> { }).iterator->value.append(call.get());
    m_inOrder.append(call.release());
}

void Modules::BatchedFetches::addImport(std::unique_ptr<BatchedFetch> fetch)
{
    m_importersByFile.add(fetch->file, Vector<BatchedFetch*, 1> { });
    m_inOrder.append(fetch.release());
}

void Modules::BatchedFetches::addImporter(BatchedFetch& call, ModuleID imported)
{
    m_importersByFile.find(imported)->value.append(&call);
    ++call.missingImports;
}

std::unique_ptr<Modules::BatchedFetch> Modules::BatchedFetches::takeFirst()
{
    BatchedFetch& fetch = *m_inOrder.begin();
    fetch.remove();
    return std::unique_ptr<BatchedFetch>(&fetch);
}

void Modules::BatchedFetches::letWait(std::unique_ptr<BatchedFetch> call)
{
    m_waiting.append(call.release());
}

std::unique_ptr<Modules::BatchedFetch> Modules::BatchedFetches::takeWaiting(BatchedFetch& call)
{
    call.remove();
    return std::unique_ptr<BatchedFetch>(&call);
}

void Modules::BatchedFetches::didTakeCall(BatchedFetch& call)
{
    auto calls = m_callsByFile.find(call.file);
    calls->value.removeFirst(&call);
    if (calls->value.isEmpty())
        m_callsByFile.remove(calls);
}

Vector<std::unique_ptr<Modules::BatchedFetch>> Modules::BatchedFetches::takeCalls()
{
    Vector<std::unique_ptr<BatchedFetch>> calls;
    for (auto* fetches : { &m_inOrder, &m_waiting }) {
        while (!fetches->isEmpty()) {
            BatchedFetch& fetch = *fetches->begin();
            fetch.remove();
            std::unique_ptr<BatchedFetch> owned(&fetch);
            if (owned->isCall())
                calls.append(WTF::move(owned));
        }
    }
    m_callsByFile.clear();
    m_importersByFile.clear();
    return calls;
}

void Modules::BatchedFetches::clear()
{
    takeCalls();
}

bool Modules::hasImportsToFetch(ModuleID module)
{
    for (const ModuleRequestData& request : m_table.requests(module)) {
        if (!request.isDeferred && !request.isRequire && !request.isHoisted && textOfImportComesInLaterTurn(request.targetModule(), AskedBy::Import))
            return true;
    }
    return false;
}

bool Modules::hasDeferredFetchOf(ModuleID file)
{
    if (m_batchedFetches.containsCallOf(file))
        return true;
    for (const QueuedFetch& text : m_queuedFetches) {
        if (fileOf(text.module) == file)
            return true;
    }
    return false;
}

bool Modules::hasDeferredFetchToEvict()
{
    for (size_t file : m_filesToEvictOnceFetched) {
        if (hasDeferredFetchOf(ModuleID(file)))
            return true;
    }
    m_filesToEvictOnceFetched.clearAll();
    return false;
}

void Modules::willRunTestFileAgain(ModuleID file)
{
    if (m_filesToEvictOnceFetched.isEmpty()) [[likely]]
        return;
    bool hasReleasedImport = std::ranges::any_of(m_table.imports(file), [&](const ModuleRequestData& request) { return m_modulesReleasedByLoader.get(moduleLoadedBy(request).index()); });
    if (!hasReleasedImport && !hasImportsToFetch(file))
        m_filesToEvictOnceFetched.clearAll();
}

void Modules::putCallsThatWaitApartInLine()
{
    if (m_batchedFetches.isEmpty()) [[likely]]
        return;
    Vector<std::unique_ptr<BatchedFetch>> calls = m_batchedFetches.takeCalls();
    m_filesWithSource.clearAll();
    std::ranges::sort(calls, { }, [](const auto& call) { return call->place; });
    for (auto& fetch : calls) {
        if (!fetch->requestedRecord)
            m_modulesOfQueuedImports.set(fetch->module.index());
        m_queuedFetches.appendAndBubble(QueuedFetch { fetch->place, WTF::move(fetch->promise), fetch->module, WTF::move(fetch->requestedRecord), fetch->referrerAsyncOrder, std::nullopt, fetch->callSequenceNumber, { }, { }, 0, fetch->precedingDiskReads }, [&](const QueuedFetch& inLine) {
            return inLine.place > fetch->place;
        });
    }
}

void Modules::clearBatchedFetches()
{
    m_batchedFetches.clear();
    m_filesWithSource.clearAll();
    m_promiseOfJobInWhichLoadAsksForOriginal.clear();
}

void Modules::fetchImports(BatchedFetch& call, ModuleID importer, const BatchedFetch& arrived)
{
    Vector<ModuleID, 16> work { importer };
    while (!work.isEmpty()) {
        ModuleID module = work.takeLast();
        for (const ModuleRequestData& request : m_table.requests(module)) {
            if (request.isDeferred || request.isRequire || request.isHoisted)
                continue;
            ModuleID imported = request.targetModule();
            ModuleID file = fileOf(imported);
            if (file == fileOf(module) || !call.importedFiles.add(file).isNewEntry)
                continue;
            if (m_filesWithSource.get(file.index())) {
                work.append(imported);
                continue;
            }
            if (!m_batchedFetches.isFetchedForImporters(file)) {
                if (!textOfImportComesInLaterTurn(imported, AskedBy::Import))
                    continue;
                auto fetch = makeUnique<BatchedFetch>();
                fetch->batchNumber = arrived.batchNumber;
                fetch->module = imported;
                fetch->file = file;
                bool isBehindLaterBatch = !m_batchedFetches.isEmpty() && m_batchedFetches.last().batchNumber > arrived.batchNumber;
                fetch->precedingDiskReads = isBehindLaterBatch ? host().scheduleFetchFulfillment(m_instance.globalObject) : arrived.precedingDiskReads;
                m_batchedFetches.addImport(WTF::move(fetch));
            }
            m_batchedFetches.addImporter(call, file);
        }
    }
}

bool Modules::tryFulfillFirstBatchedFetch()
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    std::unique_ptr<BatchedFetch> arrived = m_batchedFetches.takeFirst();
    uint32_t batchNumber = arrived->batchNumber;
    bool isDiscarded = m_instance.globalObject->microtaskRunnability() == QueuedTaskResult::Discard;
    Vector<std::unique_ptr<BatchedFetch>, 1> calls;
    if (!arrived->isCall()) {
        m_filesWithSource.set(arrived->file.index());
        for (BatchedFetch* call : m_batchedFetches.takeImportersOf(arrived->file)) {
            --call->missingImports;
            fetchImports(*call, arrived->module, *arrived);
            if (!call->missingImports)
                calls.append(m_batchedFetches.takeWaiting(*call));
        }
        if (calls.size() > 2)
            std::stable_partition(calls.mutableSpan().begin() + 1, calls.mutableSpan().end(), [](const auto& call) { return call->joinsEarlierCall; });
    } else {
        if (!arrived->requestedRecord && !isDiscarded) {
            if (m_batchedFetches.isFetchedForImporters(arrived->file)) {
                arrived->joinsEarlierCall = true;
                arrived->importedFiles.add(arrived->file);
                m_batchedFetches.addImporter(*arrived, arrived->file);
            } else {
                arrived->joinsEarlierCall = m_filesWithSource.get(arrived->file.index());
                fetchImports(*arrived, arrived->module, *arrived);
            }
        }
        if (arrived->missingImports) {
            m_filesWithSource.set(arrived->file.index(), !m_batchedFetches.isFetchedForImporters(arrived->file));
            m_batchedFetches.letWait(WTF::move(arrived));
        } else
            calls.append(WTF::move(arrived));
    }
    for (auto& call : calls) {
        m_batchedFetches.didTakeCall(*call);
        m_filesWithSource.clear(call->file.index());
        for (ModuleID file : call->importedFiles)
            m_filesWithSource.clear(file.index());
    }
    if (isDiscarded)
        return false;
    bool isFollowedInBatch = firstBatchNumber() == batchNumber;
    m_batchBeingFulfilled = isFollowedInBatch ? std::optional(batchNumber) : std::nullopt;
    m_isInJobsAfterArrival = !isFollowedInBatch;
    JSArray* line = nullptr;
    bool settlesInLine = !m_mocks && m_modulesWithPendingMock.isEmpty() && calls.containsIf([&](const auto& call) { return call->joinsEarlierCall; }) && calls.containsIf([&](const auto& call) { return call->file != calls.first()->file; });
    for (auto& call : calls) {
        JSPromise* promise = call->promise.get();
        EnsureStillAliveScope keepsPromise(promise);
        if (settlesInLine && line) {
            JSPromise* loaded = JSPromise::create(*m_instance.vm, m_instance.globalObject->promiseStructure());
            settleInLine(line, promise, loaded);
            RETURN_IF_EXCEPTION(scope, true);
            call->promise.set(*m_instance.vm, loaded);
        }
        makeCallAgain(*call);
        RETURN_IF_EXCEPTION(scope, true);
        if (settlesInLine && !line) {
            bool wasHandled = promise->isHandled();
            line = createLineBehindCall(promise);
            RETURN_IF_EXCEPTION(scope, true);
            if (!wasHandled)
                promise->setFlags(promise->flags() & ~JSPromise::isHandledFlag);
        }
    }
    return true;
}

bool Modules::tryFulfillFirstQueuedFetch()
{
    if (m_instance.globalObject->microtaskRunnability() == QueuedTaskResult::Discard) {
        m_queuedFetches.removeFirst();
        return false;
    }
    fulfillFirstQueuedFetch();
    return true;
}

void Modules::makeCallAgain(BatchedFetch& text)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* promise = text.promise.get();
    if (text.requestedRecord) {
        promise->fulfill(vm, text.requestedRecord.get());
        return;
    }
    SetForScope callSequenceNumber(m_resumingCallSequenceNumber, text.callSequenceNumber);
    didRefetchFile(text.file);
    bool wasHeldBeforeWalk = holdsLinksOfLoadThatMayFail(text.module);
    JSPromise* loaded = startImport(text.module, text.referrerAsyncOrder, text.caller, ImportSourceState::Arrived, promise);
    bool walkFiledLinks = !wasHeldBeforeWalk && holdsLinksOfLoadThatMayFail(text.module);
    if (scope.exception()) [[unlikely]] {
        promise->rejectWithCaughtException(vm, scope);
        if (walkFiledLinks)
            bringBackLinksOfLoadThatFailed(text.module);
        return;
    }
    if (walkFiledLinks) [[unlikely]] {
        if (loaded->status() == JSPromise::Status::Rejected)
            bringBackLinksOfLoadThatFailed(text.module);
        else
            hearWhetherImportThatLinkedAnewFails(text.module, loaded);
    }
    scope.release();
    if (loaded != promise)
        settleAsLoaderDoesInImportTurns(m_instance.globalObject, promise, loaded);
}

void Modules::forgetMockLaidBehindCallWhoseTextHasCome(ModuleID file)
{
    if (std::optional<uint32_t> callSequenceNumber = std::exchange(m_resumingCallSequenceNumber, std::nullopt))
        forgetMockRegisteredAfter(file, *callSequenceNumber);
}

bool hasBatchedFetches(JSGlobalObject* realm)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->hasBatchedFetches();
}

uint32_t momentOfLoads(JSGlobalObject* realm)
{
    return realm->vm().aotState().momentOfLoads;
}

void forgetMockRegisteredAfter(JSGlobalObject* realm, ModuleID module, uint32_t moment)
{
    if (Modules* modules = modulesOf(realm))
        modules->forgetMockRegisteredAfter(module, moment);
}

void Modules::forgetMockRegisteredAfter(ModuleID module, uint32_t moment)
{
    ModuleID file = fileOf(module);
    if (!m_modulesWithPendingMock.get(file.index()))
        return;
    auto laid = m_pendingMockCalls.find(file);
    if (laid == m_pendingMockCalls.end() || laid->value.moment <= moment || pendingFactoryCallID(file))
        return;
    if (m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return each.isMockImport && !each.missingMockFailure && fileOf(each.module) == file; }))
        return;
    if (m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == file && above.original; }))
        return;
    forgetMockThatWaits(file);
    forgetCallOfMock(file);
    m_unseenMocks.set(file.index());
}

void Modules::addImportAwaitingSource(ModuleID module)
{
    VM& vm = *m_instance.vm;
    bool isMockImport = needsOriginalBeforeFactory(module) || needsOriginalBeforeFactory(fileOf(module));
    m_importsAwaitingSource.append({ module, isMockImport });
    m_importsAwaitingSource.last().callSequenceNumber = vm.aotState().momentOfLoads;
    if (!isMockImport)
        return;
    if (hasLoaderSettledOriginalOf(fileOf(module))) {
        m_importsAwaitingSource.last().hasRequestedOriginal = true;
        return;
    }
    if (!m_promiseOfJobInWhichLoadAsksForOriginal)
        m_promiseOfJobInWhichLoadAsksForOriginal.set(vm, JSPromise::create(vm, m_instance.globalObject->promiseStructure()));
    queueStepOfImportInImportTurns(m_promiseOfJobInWhichLoadAsksForOriginal.get(), module, 4);
}

JSPromise* Modules::takeMissingMockFailure(ModuleID module)
{
    size_t place = m_importsAwaitingSource.findIf([&](const ImportAwaitingSource& each) { return each.module == module && each.missingMockFailure && (!m_resumingCallSequenceNumber || each.callSequenceNumber == *m_resumingCallSequenceNumber); });
    if (place == notFound)
        return m_didFinishTests ? nullptr : findMissingMockFailure(module);
    JSPromise* failed = m_importsAwaitingSource[place].missingMockFailure.get();
    m_importsAwaitingSource.removeAt(place);
    return failed;
}

bool Modules::takeImportWithRemovedMock(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(file.index()) || activeMock(module) || activeMock(file) || hasPendingMock(file))
        return false;
    return m_importsAwaitingSource.removeFirstMatching([&](const ImportAwaitingSource& each) { return each.module == module && each.isMockImport && each.hasRequestedOriginal && !each.didRunMocksBeforeSource && !each.missingMockFailure; });
}

bool Modules::isInAnotherLoadGraph(ModuleID file)
{
    auto leadsToFile = [&](ModuleID root) {
        return fileOf(root) != file && modulesInLoadGraph(root, LoadGraphMode::IgnoreMocks).get(file.index());
    };
    if (m_batchedFetches.containsCallIf([&](const BatchedFetch& call) { return leadsToFile(call.module); }))
        return true;
    for (const QueuedFetch& text : m_queuedFetches) {
        if (leadsToFile(text.module))
            return true;
    }
    for (size_t queued : m_modulesOfQueuedImports) {
        if (leadsToFile(ModuleID(queued)))
            return true;
    }
    return false;
}

bool Modules::needsOriginalDespiteMock(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(file.index()) || hasPendingMock(file) || (!activeMock(module) && !activeMock(file)))
        return false;
    if (wasOriginalEvaluated(file) || hasLoaderSettledOriginalOf(file))
        return false;
    return m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return each.module == module && each.wasOvertakenByRequire && each.isMockImport && each.hasRequestedOriginal && !each.didRunMocksBeforeSource && !each.missingMockFailure; });
}

void Modules::letLoadInFlightAskForOriginal(ModuleID module)
{
    auto isBookingThatAsksNow = [&](const ImportAwaitingSource& each) { return each.module == module && each.isMockImport && !each.hasRequestedOriginal; };
    size_t place = m_importsAwaitingSource.findIf(isBookingThatAsksNow);
    if (place == notFound)
        return;
    if (m_importsAwaitingSource[place].didRunMocksBeforeSource) {
        m_importsAwaitingSource[place].hasRequestedOriginal = true;
        return;
    }
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* failed = findMissingMockFailure(module);
    if (!failed) {
        Identifier file = moduleKey(fileOf(module));
        loader()->resolve(realm, Identifier::fromString(vm, makeString('\0', file.string())), file, nullptr, true);
        if (scope.exception()) {
            failed = rejectedPromiseWithPendingException(realm, scope);
            failed->markAsHandled();
            m_missingMockFailures.append({ module, Strong<JSPromise>(vm, failed) });
        }
        place = m_importsAwaitingSource.findIf(isBookingThatAsksNow);
        if (place == notFound)
            return;
    }
    m_importsAwaitingSource[place].hasRequestedOriginal = true;
    if (!failed)
        return;
    m_importsAwaitingSource[place].missingMockFailure.set(vm, failed);
    auto isFetchForCall = [&, callSequenceNumber = m_importsAwaitingSource[place].callSequenceNumber](const auto& text) { return text.module == module && text.callSequenceNumber == callSequenceNumber && !(text.promise->flags() & JSPromise::isFirstResolvingFunctionCalledFlag); };
    JSPromise* promiseOfCall = nullptr;
    for (const QueuedFetch& text : m_queuedFetches) {
        if (!promiseOfCall && !text.requested && isFetchForCall(text))
            promiseOfCall = text.promise.get();
    }
    const BatchedFetch* fetchOfCall = nullptr;
    m_batchedFetches.forEachCall([&](const BatchedFetch& call) {
        if (!call.requestedRecord && isFetchForCall(call) && (!fetchOfCall || call.place < fetchOfCall->place))
            fetchOfCall = &call;
    });
    if (!promiseOfCall && fetchOfCall)
        promiseOfCall = fetchOfCall->promise.get();
    if (promiseOfCall)
        RELEASE_AND_RETURN(scope, promiseOfCall->resolve(realm, vm, failed));
}

void Modules::walkInPlaceOfCallsHeldForTextOf(ModuleID file)
{
    size_t place = m_importsAwaitingSource.findIf([&](const ImportAwaitingSource& each) { return each.isMockImport && !each.didRunMocksBeforeSource && fileOf(each.module) == file; });
    if (place == notFound)
        return;
    ModuleID asked = m_importsAwaitingSource[place].module;
    if (!needsOriginalBeforeFactory(asked) && !needsOriginalBeforeFactory(file))
        return;
    if (!host().shouldBypassMock || !host().shouldBypassMock(m_instance.globalObject, m_table.fullPath(file), String(), false)) {
        for (ImportAwaitingSource& each : m_importsAwaitingSource)
            each.wasOvertakenByRequire |= fileOf(each.module) == file;
        return;
    }
    for (ImportAwaitingSource& each : m_importsAwaitingSource)
        each.didRunMocksBeforeSource |= fileOf(each.module) == file;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    {
        SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, std::optional(asked));
        SetForScope currentImportTarget(m_currentImportTarget, asked);
        SetForScope currentImportWaitState(m_currentImportWaitState, JSValue());
        runMocksInGraph(asked);
    }
    Exception* exception = scope.exception();
    if (!exception)
        return;
    JSValue error = exception->value();
    if (!scope.tryClearException())
        return;
    noteImportFailedUnderMock(asked, error);
    recordFactoryFailure(file, error);
}

void Modules::updateRetainedImportNamespaces(ModuleID module)
{
    bool isChangedByImport = m_currentImportTarget && (m_modulesWithRunningFactory.isEmpty() || m_modulesWithRunningFactory.get(module.index()) || m_currentImportTarget != m_factoryCallImportTarget);
    if (!isChangedByImport)
        m_lastDeedsOnMocksUnderCallsOfImport.remove(fileOf(module));
    if (m_retainedImportNamespaceCount == (isChangedByImport ? m_mockedImports.size() : 0))
        return;
    JSObject* namespaceHeld = isChangedByImport ? namespaceIfCreated(module, ModulePhase::Evaluation) : nullptr;
    for (MockedImport& call : m_mockedImports.values()) {
        if (call.module != module)
            continue;
        if (!isChangedByImport) {
            if (call.namespaceBeforeImportChangedMock)
                --m_retainedImportNamespaceCount;
            call.namespaceBeforeImportChangedMock.clear();
        } else if (!call.namespaceBeforeImportChangedMock) {
            if (JSObject* namespaceObject = namespaceHeld) {
                ++m_retainedImportNamespaceCount;
                call.namespaceBeforeImportChangedMock.set(*m_instance.vm, namespaceObject);
            }
        }
    }
}

void Modules::recordMockedImport(JSPromise* promise, ModuleID module)
{
    unsigned serial = ++m_lastMockedImportSerial;
    m_mockedImports.add(promise, MockedImport { Strong<JSPromise>(*m_instance.vm, promise), moduleWithExportsOf(module), { }, serial });
    m_importsAwaitingFirstStep.append({ serial, promise });
}

void Modules::didRunFirstImportStepSlow(JSPromise* promise)
{
    auto call = m_mockedImports.find(promise);
    if (call != m_mockedImports.end())
        advanceMockedImportsThrough(call->value.serial);
}

void Modules::advanceMockedImportsThrough(unsigned serial)
{
    while (!m_importsAwaitingFirstStep.isEmpty() && m_importsAwaitingFirstStep.first().first <= serial) {
        auto found = m_mockedImports.find(m_importsAwaitingFirstStep.takeFirst().second);
        if (found == m_mockedImports.end())
            continue;
        MockedImport& call = found->value;
        scheduleImportBinding(call.promise.get());
        auto deed = m_lastDeedsOnMocksUnderCallsOfImport.find(fileOf(call.module));
        if (deed == m_lastDeedsOnMocksUnderCallsOfImport.end() || call.serial > deed->value.lastCallInFrontOfIt)
            continue;
        if (call.namespaceBeforeImportChangedMock) {
            call.namespaceBeforeImportChangedMock.clear();
            --m_retainedImportNamespaceCount;
        }
        if (deed->value.bringsBackFileThatHadRun || !deed->value.mock)
            continue;
        if (!deed->value.viewOfMock) {
            ModuleNamespace* view = ModuleNamespace::create(*m_instance.vm, namespaceStructure(true), call.module, ModulePhase::Evaluation, { });
            view->setMockOfView(*m_instance.vm, deed->value.mock.get());
            deed->value.viewOfMock.set(*m_instance.vm, view);
        }
        call.namespaceBeforeImportChangedMock.set(*m_instance.vm, deed->value.viewOfMock.get());
        ++m_retainedImportNamespaceCount;
    }
    if (m_importsAwaitingFirstStep.isEmpty())
        m_lastDeedsOnMocksUnderCallsOfImport.clear();
}

void Modules::noteDeedOnMockUnderCallsOfImportSlow(ModuleID module, DeedOnMock kind)
{
    LastDeedOnMock& deed = m_lastDeedsOnMocksUnderCallsOfImport.ensure(fileOf(module), [] { return LastDeedOnMock { }; }).iterator->value;
    if (!deed.mock) {
        if (JSObject* mock = activeMock(moduleWithExportsOf(module)))
            deed.mock.set(*m_instance.vm, mock);
    }
    deed.lastCallInFrontOfIt = m_lastMockedImportSerial;
    deed.bringsBackFileThatHadRun = kind == DeedOnMock::Ends && (hasIntactOriginal(module) || wasPatchedAfterEvaluation(module));
}

void Modules::keepMockForPendingImports(ModuleID module, MockChange change)
{
    if (m_mockedImports.isEmpty()) [[likely]]
        return;
    if (change == MockChange::Replaced) {
        for (MockedImport& call : m_mockedImports.values()) {
            if (call.module == moduleWithExportsOf(module))
                call.hasProvisionalView = false;
        }
    }
    ModuleRecord* original = displacedRecord(module);
    if (!original || (original->isSCCEvaluated() && !original->evaluationError()) || wasDisplacedAfterFailure(module) || wasMockedAfterFailure(module))
        return;
    ModuleID mocked = moduleWithExportsOf(module);
    JSObject* mock = activeMock(mocked);
    if (!mock)
        return;
    ModuleNamespace* view = nullptr;
    for (MockedImport& call : m_mockedImports.values()) {
        if (call.module != mocked || call.namespaceBeforeImportChangedMock || m_importsAwaitingFirstStep.containsIf([&](const auto& waiting) { return waiting.first == call.serial; }))
            continue;
        if (!view) {
            view = ModuleNamespace::create(*m_instance.vm, namespaceStructure(true), mocked, ModulePhase::Evaluation, { });
            view->setMockOfView(*m_instance.vm, mock);
        }
        call.namespaceBeforeImportChangedMock.set(*m_instance.vm, view);
        call.hasProvisionalView = change == MockChange::Removed && !call.isBoundToMock;
        ++m_retainedImportNamespaceCount;
    }
}

static JSC_DECLARE_HOST_FUNCTION(importBindingJob);

JSC_DEFINE_HOST_FUNCTION(importBindingJob, (JSGlobalObject* realm, CallFrame* callFrame))
{
    if (Modules* modules = modulesOf(realm))
        modules->didBindImportToMock(uncheckedDowncast<JSPromise>(callFrame->argument(1)));
    return JSValue::encode(jsUndefined());
}

void Modules::scheduleImportBinding(JSPromise* importPromise)
{
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    JSPromise* tick = JSPromise::create(vm, realm->promiseStructure());
    tick->fulfill(vm, jsUndefined());
    JSFunction* didBind = JSFunction::create(vm, realm, 2, String(), importBindingJob, ImplementationVisibility::Private);
    tick->performPromiseThenWithContext(vm, realm, didBind, didBind, jsUndefined(), importPromise);
}

void Modules::didBindImportToMock(JSPromise* promise)
{
    auto found = m_mockedImports.find(promise);
    if (found == m_mockedImports.end())
        return;
    MockedImport& call = found->value;
    call.isBoundToMock = true;
    if (!std::exchange(call.hasProvisionalView, false) || !call.namespaceBeforeImportChangedMock)
        return;
    call.namespaceBeforeImportChangedMock.clear();
    --m_retainedImportNamespaceCount;
}

void Modules::clearRetainedImportNamespaces()
{
    m_lastDeedsOnMocksUnderCallsOfImport.clear();
    if (!m_retainedImportNamespaceCount)
        return;
    m_retainedImportNamespaceCount = 0;
    for (MockedImport& call : m_mockedImports.values())
        call.namespaceBeforeImportChangedMock.clear();
}

JSObject* Modules::takeRetainedImportNamespace(JSPromise* importPromise)
{
    auto call = m_mockedImports.find(importPromise);
    if (call == m_mockedImports.end())
        return nullptr;
    JSObject* namespaceObject = call->value.namespaceBeforeImportChangedMock.get();
    if (namespaceObject)
        --m_retainedImportNamespaceCount;
    m_mockedImports.remove(call);
    return namespaceObject;
}

static JSC_DECLARE_HOST_FUNCTION(resumeLoadAfterFactory);

JSC_DEFINE_HOST_FUNCTION(resumeLoadAfterFactory, (JSGlobalObject* realm, CallFrame* callFrame))
{
    auto* load = uncheckedDowncast<InternalFieldTuple>(callFrame->argument(1));
    auto* root = uncheckedDowncast<ModuleRecord>(load->getInternalField(0));
    auto* payload = uncheckedDowncast<ModuleLoaderPayload>(load->getInternalField(1));
    root->setMayMeetNewMocks();
    InternalMicrotask job = payload->deferred() ? InternalMicrotask::DynamicImportDeferLoadSettled : InternalMicrotask::DynamicImportLoadSettled;
    realm->queueMicrotask(realm->vm(), job, static_cast<uint8_t>(JSPromise::Status::Fulfilled), root, jsUndefined(), payload);
    return JSValue::encode(jsUndefined());
}

bool Modules::tryDeferForFactories(ModuleRecord& root, JSCell* payload)
{
    if (status(root.module()) > ModuleStatus::Linked)
        return false;
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSValue state;
    bool factoryHasNotSettled = runMocksInGraph(root.module());
    JSPromise* factory = factoryHasNotSettled && !scope.exception() ? pendingFactoryInGraph(root.module(), PendingFactoryRequester::ImportCall, state) : nullptr;
    if (Exception* exception = scope.exception()) [[unlikely]] {
        JSValue error = exception->value();
        if (scope.tryClearException())
            uncheckedDowncast<ModuleLoaderPayload>(payload)->promise()->reject(vm, error);
        return true;
    }
    if (!factory)
        return false;
    auto* load = InternalFieldTuple::create(vm, realm->internalFieldTupleStructure(), &root, payload);
    JSFunction* resume = JSFunction::create(vm, realm, 2, String(), resumeLoadAfterFactory, ImplementationVisibility::Private);
    factory->performPromiseThenWithContext(vm, realm, resume, resume, jsUndefined(), load);
    return true;
}

static Modules* modulesIfInitialized(Instance* instance)
{
    return instance->state ? instance->state->modules.get() : nullptr;
}

bool fulfillDeferredFetches(VM& vm, uint64_t completedDiskReads)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    VMState& state = vm.aotState();
    for (Instance* instance : state.instances) {
        Modules* modules = modulesIfInitialized(instance);
        if (!modules || !modules->hasDeferredFetches())
            continue;
        if (!modules->isFirstFetchDue(completedDiskReads))
            return false;
        break;
    }
    for (Instance* instance : state.instances) {
        if (Modules* modules = modulesIfInitialized(instance))
            modules->willFulfillDeferredFetches();
    }
    bool isDelivered = false;
    while (!isDelivered) {
        Modules* first = nullptr;
        for (Instance* instance : state.instances) {
            Modules* modules = modulesIfInitialized(instance);
            if (modules && modules->hasDeferredFetches()) {
                first = modules;
                break;
            }
        }
        if (!first)
            return true;
        std::optional<uint32_t> batchNumber = first->firstBatchNumber();
        isDelivered = batchNumber ? first->tryFulfillFirstBatchedFetch() : first->tryFulfillFirstQueuedFetch();
        if (scope.exception()) [[unlikely]]
            return true;
    }
    return true;
}

std::optional<uint32_t> firstDeferredBatchNumber(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        if (Modules* modules = modulesIfInitialized(instance); modules && modules->hasDeferredFetches())
            return modules->firstBatchNumber();
    }
    return std::nullopt;
}

void didDrainDeferredFetchJobs(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        if (Modules* modules = modulesIfInitialized(instance))
            modules->didDrainDeferredFetchJobs();
    }
}

bool shouldDeferFetchFulfillment(JSGlobalObject* realm, ModuleID module)
{
    if (!realm->vm().aotState().importFrameInLoader) [[likely]]
        return false;
    Modules* modules = modulesOf(realm);
    return modules && modules->isTestRun() && !modules->hasTextAtHand() && modules->textOfImportComesInLaterTurn(module, Modules::AskedBy::Loader);
}

bool tryEvictRequirerOfOriginal(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->tryEvictRequirerOfOriginal(module);
}

bool takeRequirerOfOwnOriginal(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->takeRequirerOfOwnOriginal(module);
}

void didRequireOwnOriginal(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->didRequireOwnOriginal(module);
}

void willEndMocksOfTestFile(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->willEndMocksOfTestFile();
}

void didEndMocksOfTestFile(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->didEndMocksOfTestFile();
}

bool isOriginalEvicted(JSGlobalObject* realm, ModuleID mocked)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isOriginalEvicted(mocked);
}

bool wasEvictedOnceLoaded(JSGlobalObject* realm, ModuleID module)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasEvictedOnceLoaded(module);
}

void didPatchPendingMock(JSGlobalObject* realm, ModuleID mocked)
{
    Modules* modules = modulesOf(realm);
    if (modules && modules->isAwaitedByDetachedLoad(mocked) && !modules->hasIntactOriginal(mocked))
        modules->didPatchPendingMock(mocked);
}

void evictAbandonedModules(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->evictAbandonedModules();
}

void evictAtNextLoad(JSGlobalObject* realm, ModuleID module)
{
    if (Modules* modules = modulesOf(realm))
        modules->evictAtNextLoad(module);
}

void evictLoadedModules(JSGlobalObject* realm)
{
    if (Modules* modules = modulesOf(realm))
        modules->evictLoadedModules();
}

bool wasOriginalRequiredFirst(JSGlobalObject* realm, ModuleID file)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->wasOriginalRequiredFirst(file);
}

bool isMockUnseen(JSGlobalObject* realm, ModuleID mocked)
{
    Modules* modules = modulesOf(realm);
    return modules && modules->isMockUnseen(mocked);
}

void didFetch(JSGlobalObject* realm, AbstractModuleRecord* record)
{
    if (auto* compiled = dynamicDowncast<ModuleRecord>(record))
        compiled->setMomentFetched(realm->vm().aotState().momentOfLoads);
}

void deferFetchFulfillment(JSGlobalObject* realm, ModuleID module, JSPromise* fetched, AbstractModuleRecord* record)
{
    modulesOf(realm)->holdTextOfCall(fetched, module, uncheckedDowncast<ModuleRecord>(record), std::nullopt, Modules::ImportCaller::CompiledCode);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
