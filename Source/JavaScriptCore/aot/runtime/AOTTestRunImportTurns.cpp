/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTModules.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTModuleNamespace.h"
#include "AOTState.h"
#include "AOTVMState.h"
#include "AsyncContextSwapScope.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
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
    std::optional<BitVector> reached;
    didStartImport(m_instance.globalObject, promise, path, [&](ModuleID module) {
        if (file && !reached)
            reached = modulesInLoadGraph(*file);
        return reached && reached->quickGet(module.index());
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
    return !m_innermostEagerLoad && m_queuedFetches.isEmpty() && hostHasProgramSources() && (!isTestRun() || m_isDrainingDeferredFetchJobs);
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
    } else if (!isNotLoaded(module))
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
    if (m_batchedFetches.isEmpty()) {
        m_jobsBehindFirstCall = 0;
        queueJobOfRound(module);
    }
    uint64_t place = static_cast<uint64_t>(m_sourceArrivalCount) << 32 | m_numberOfTextsAskedFor++;
    m_batchedFetches.append(BatchedFetch { place, m_currentBatchNumber, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder, caller, m_jobsBehindFirstCall, vm.aotState().momentOfLoads, requested ? Strong<Unknown>() : currentAsyncContext() });
    m_batchedFetches.last().precedingDiskReads = host().scheduleFetchFulfillment(m_instance.globalObject);
}

bool Modules::hasDeferredFetchOf(ModuleID file)
{
    for (const BatchedFetch& text : batchedFetches()) {
        if (fileOf(text.module) == file)
            return true;
    }
    for (const BatchedFetch& text : m_callsToMakeAgain) {
        if (fileOf(text.module) == file)
            return true;
    }
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

void Modules::putCallsThatWaitApartInLine()
{
    if (m_batchedFetches.isEmpty()) [[likely]]
        return;
    for (BatchedFetch& text : batchedFetches()) {
        if (!text.requestedRecord)
            m_modulesOfQueuedImports.set(text.module.index());
        m_queuedFetches.appendAndBubble(QueuedFetch { text.place, WTF::move(text.promise), text.module, WTF::move(text.requestedRecord), text.referrerAsyncOrder, std::nullopt, text.callSequenceNumber, WTF::move(text.asyncContext), { }, 0, text.precedingDiskReads }, [&](const QueuedFetch& inLine) {
            return inLine.place > text.place;
        });
    }
    m_batchedFetches.clear();
    m_fulfilledBatchedFetchCount = 0;
}

void Modules::clearBatchedFetches()
{
    m_batchedFetches.clear();
    m_fulfilledBatchedFetchCount = 0;
    m_callsToMakeAgain.clear();
    m_promiseOfJobThatMakesCallsAgain.clear();
    m_promiseOfJobInWhichLoadAsksForOriginal.clear();
    m_promiseOfJobOfRound.clear();
    m_isJobOfRoundQueued = false;
}

bool Modules::tryFulfillFirstBatchedFetch()
{
    BatchedFetch text = WTF::move(m_batchedFetches[m_fulfilledBatchedFetchCount]);
    if (++m_fulfilledBatchedFetchCount == m_batchedFetches.size()) {
        m_batchedFetches.clear();
        m_fulfilledBatchedFetchCount = 0;
    }
    if (m_instance.globalObject->microtaskRunnability() == QueuedTaskResult::Discard)
        return false;
    m_isDrainingDeferredFetchJobs = true;
    if (!text.jobsBehindFirstCall) {
        makeCallAgain(text);
        return true;
    }
    if (m_callsToMakeAgain.isEmpty()) {
        VM& vm = *m_instance.vm;
        if (!m_promiseOfJobThatMakesCallsAgain)
            m_promiseOfJobThatMakesCallsAgain.set(vm, JSPromise::create(vm, m_instance.globalObject->promiseStructure()));
        m_jobsBehindDelivery = 0;
        queueStepOfImportInImportTurns(m_promiseOfJobThatMakesCallsAgain.get(), text.module, 1);
    }
    m_callsToMakeAgain.append(WTF::move(text));
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
    AsyncContextSwapScope asyncContextOfCall(vm, m_instance.globalObject, text.asyncContext.get());
    ModuleEvaluationAsyncContextScope jobOfImport(vm, m_instance.globalObject, text.asyncContext.get());
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
    std::optional<uint32_t> callSequenceNumber = std::exchange(m_resumingCallSequenceNumber, std::nullopt);
    if (!callSequenceNumber || !m_modulesWithPendingMock.get(file.index()))
        return;
    auto laid = m_pendingMockCalls.find(file);
    if (laid == m_pendingMockCalls.end() || laid->value.moment <= *callSequenceNumber)
        return;
    if (m_importsAwaitingSource.containsIf([&](const ImportAwaitingSource& each) { return each.isMockImport && !each.missingMockFailure && fileOf(each.module) == file; }))
        return;
    if (m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == file && above.original; }))
        return;
    forgetMockThatWaits(file);
    forgetCallOfMock(file);
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
    for (const BatchedFetch& text : batchedFetches()) {
        if (leadsToFile(text.module))
            return true;
    }
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
    for (const BatchedFetch& text : batchedFetches()) {
        if (!promiseOfCall && !text.requestedRecord && isFetchForCall(text))
            promiseOfCall = text.promise.get();
    }
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

void Modules::runJobThatMakesCallsAgain(ModuleID moduleOfJob)
{
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    ++m_jobsBehindDelivery;
    while (!m_callsToMakeAgain.isEmpty() && m_callsToMakeAgain.first().jobsBehindFirstCall <= m_jobsBehindDelivery) {
        BatchedFetch text = m_callsToMakeAgain.takeFirst();
        makeCallAgain(text);
        if (scope.exception()) [[unlikely]] {
            m_callsToMakeAgain.clear();
            return;
        }
    }
    if (!m_callsToMakeAgain.isEmpty())
        queueStepOfImportInImportTurns(m_promiseOfJobThatMakesCallsAgain.get(), moduleOfJob, 1);
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

void Modules::queueJobOfRound(ModuleID moduleOfJob)
{
    if (!hostHasProgramSources() || std::exchange(m_isJobOfRoundQueued, true))
        return;
    VM& vm = *m_instance.vm;
    if (!m_promiseOfJobOfRound)
        m_promiseOfJobOfRound.set(vm, JSPromise::create(vm, m_instance.globalObject->promiseStructure()));
    queueStepOfImportInImportTurns(m_promiseOfJobOfRound.get(), moduleOfJob, 1);
}

void Modules::runJobOfRound(ModuleID moduleOfJob)
{
    m_isJobOfRoundQueued = false;
    ++m_jobsBehindFirstCall;
    if (!m_batchedFetches.isEmpty() && !m_instance.vm->defaultMicrotaskQueue().isEmpty())
        queueJobOfRound(moduleOfJob);
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
        while (batchNumber && first->firstBatchNumber() == batchNumber) {
            isDelivered |= first->tryFulfillFirstBatchedFetch();
            if (scope.exception()) [[unlikely]]
                return true;
        }
    }
    return true;
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

void deferFetchFulfillment(JSGlobalObject* realm, ModuleID module, JSPromise* fetched, AbstractModuleRecord* record)
{
    modulesOf(realm)->holdTextOfCall(fetched, module, uncheckedDowncast<ModuleRecord>(record), std::nullopt, Modules::ImportCaller::CompiledCode);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
