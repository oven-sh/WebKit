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
    return !m_innermostEagerLoad && m_textsOnTheirWay.isEmpty() && hostHasProgramSources() && (!isTestRun() || m_isDrainingDeferredFetchJobs);
}

bool Modules::textOfImportComesInLaterTurn(ModuleID module, AskedBy askedBy)
{
    ModuleID file = fileOf(module);
    if (!isTextReadOnOtherThread(file) || activeMock(module) || activeMock(file))
        return false;
    if (isCommonJSFileOrFace(module)) {
        for (ModuleID fileOrFace : { module, file }) {
            if (status(fileOrFace) != ModuleStatus::Unlinked && status(fileOrFace) != ModuleStatus::Linked)
                return false;
        }
    } else if (!isNotLoaded(module))
        return false;
    if (askedBy == AskedBy::Import) {
        for (ModuleID fileOrFace : { module, file }) {
            if (loader()->registryEntry(moduleKey(fileOrFace)))
                return false;
        }
    }
    return true;
}

void Modules::holdTextOfCall(JSPromise* promise, ModuleID module, ModuleRecord* requested, std::optional<int64_t> referrerAsyncOrder, ImportCaller caller)
{
    VM& vm = *m_instance.vm;
    if (m_innermostEagerLoad || !m_textsOnTheirWay.isEmpty()) {
        std::optional<ModuleID> fileThatCalls = m_innermostEagerLoad ? fileOrFaceEvaluatedIn(*m_innermostEagerLoad) : std::nullopt;
        holdText(promise, module, requested, referrerAsyncOrder, fileThatCalls ? turnInWhichTextCame(*fileThatCalls) : m_turnOfTexts);
        return;
    }
    if (m_textsThatWait.isEmpty()) {
        m_jobsBehindFirstCall = 0;
        queueJobOfRound(module);
    }
    uint64_t place = static_cast<uint64_t>(m_turnOfTexts) << 32 | m_numberOfTextsAskedFor++;
    m_textsThatWait.append(TextThatWaits { place, m_turnOfDeliveries, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder, caller, m_jobsBehindFirstCall, vm.aotState().momentOfLoads, requested ? Strong<Unknown>() : currentAsyncContext() });
    host().scheduleFetchFulfillment(m_instance.globalObject);
}

void Modules::putCallsThatWaitApartInLine()
{
    if (m_textsThatWait.isEmpty()) [[likely]]
        return;
    for (TextThatWaits& text : textsThatWait()) {
        if (!text.requestedRecord)
            m_modulesOfQueuedImports.set(text.module.index());
        m_textsOnTheirWay.appendAndBubble(TextOnItsWay { text.place, WTF::move(text.promise), text.module, WTF::move(text.requestedRecord), text.referrerAsyncOrder, std::nullopt, text.callSequenceNumber, WTF::move(text.asyncContext) }, [&](const TextOnItsWay& inLine) {
            return inLine.place > text.place;
        });
    }
    m_textsThatWait.clear();
    m_numberOfTextsThatHaveCome = 0;
}

void Modules::forgetTextsThatWait()
{
    m_textsThatWait.clear();
    m_numberOfTextsThatHaveCome = 0;
    m_callsToMakeAgain.clear();
    m_promiseOfJobThatMakesCallsAgain.clear();
    m_promiseOfJobInWhichLoadAsksForOriginal.clear();
    m_promiseOfJobOfRound.clear();
    m_isJobOfRoundQueued = false;
}

bool Modules::deliverFirstTextThatWaits()
{
    TextThatWaits text = WTF::move(m_textsThatWait[m_numberOfTextsThatHaveCome]);
    if (++m_numberOfTextsThatHaveCome == m_textsThatWait.size()) {
        m_textsThatWait.clear();
        m_numberOfTextsThatHaveCome = 0;
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

bool Modules::deliverFirstTextInLine()
{
    if (m_instance.globalObject->microtaskRunnability() == QueuedTaskResult::Discard) {
        m_textsOnTheirWay.removeFirst();
        return false;
    }
    deliverFirstTextOnItsWay();
    return true;
}

void Modules::makeCallAgain(TextThatWaits& text)
{
    VM& vm = *m_instance.vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* promise = text.promise.get();
    if (text.requestedRecord) {
        promise->fulfill(vm, text.requestedRecord.get());
        return;
    }
    SetForScope callSequenceNumber(m_momentOfCallWhoseTextHasCome, text.callSequenceNumber);
    AsyncContextSwapScope asyncContextOfCall(vm, m_instance.globalObject, text.asyncContext.get());
    ModuleEvaluationAsyncContextScope jobOfImport(vm, m_instance.globalObject, text.asyncContext.get());
    bool wasHeldBeforeWalk = holdsLinksOfLoadThatMayFail(text.module);
    JSPromise* loaded = startImport(text.module, text.referrerAsyncOrder, text.caller, TextOfImport::HasCome, promise);
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
    std::optional<uint32_t> callSequenceNumber = std::exchange(m_momentOfCallWhoseTextHasCome, std::nullopt);
    if (!callSequenceNumber || !m_modulesWithPendingMock.get(file.index()))
        return;
    auto laid = m_momentsWaitingMocksWereLaid.find(file);
    if (laid == m_momentsWaitingMocksWereLaid.end() || laid->value.moment <= *callSequenceNumber)
        return;
    if (m_importsWhoseMocksRunWhenTheirTextsHaveCome.containsIf([&](const ImportWhoseMocksRunWhenItsTextHasCome& each) { return each.asksForMockOfFileItself && !each.failureForWantOfMockThatTakesOriginal && fileOf(each.module) == file; }))
        return;
    if (m_importersEnteredAboveMocks.containsIf([&](const ImportersEnteredAboveMock& above) { return above.mocked == file && above.original; }))
        return;
    forgetMockThatWaits(file);
    forgetCallOfMock(file);
}

void Modules::bookImportWhoseMocksRunWhenItsTextHasCome(ModuleID module)
{
    VM& vm = *m_instance.vm;
    bool asksForMockOfFileItself = needsOriginalAheadOfFactory(module) || needsOriginalAheadOfFactory(fileOf(module));
    m_importsWhoseMocksRunWhenTheirTextsHaveCome.append({ module, asksForMockOfFileItself });
    m_importsWhoseMocksRunWhenTheirTextsHaveCome.last().callSequenceNumber = vm.aotState().momentOfLoads;
    if (!asksForMockOfFileItself)
        return;
    if (hasLoaderSettledOriginalOf(fileOf(module))) {
        m_importsWhoseMocksRunWhenTheirTextsHaveCome.last().hasLoadAskedForOriginal = true;
        return;
    }
    if (!m_promiseOfJobInWhichLoadAsksForOriginal)
        m_promiseOfJobInWhichLoadAsksForOriginal.set(vm, JSPromise::create(vm, m_instance.globalObject->promiseStructure()));
    queueStepOfImportInImportTurns(m_promiseOfJobInWhichLoadAsksForOriginal.get(), module, 4);
}

JSPromise* Modules::takeFailureOfLoadForWantOfMock(ModuleID module)
{
    size_t place = m_importsWhoseMocksRunWhenTheirTextsHaveCome.findIf([&](const ImportWhoseMocksRunWhenItsTextHasCome& each) { return each.module == module && each.failureForWantOfMockThatTakesOriginal && (!m_momentOfCallWhoseTextHasCome || each.callSequenceNumber == *m_momentOfCallWhoseTextHasCome); });
    if (place == notFound)
        return m_didFinishTests ? nullptr : loadThatFailedForWantOfMock(module);
    JSPromise* failed = m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].failureForWantOfMockThatTakesOriginal.get();
    m_importsWhoseMocksRunWhenTheirTextsHaveCome.removeAt(place);
    return failed;
}

bool Modules::takeBookingOfLoadWhoseMockHasEnded(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(file.index()) || activeMock(module) || activeMock(file) || hasPendingMock(file))
        return false;
    return m_importsWhoseMocksRunWhenTheirTextsHaveCome.removeFirstMatching([&](const ImportWhoseMocksRunWhenItsTextHasCome& each) { return each.module == module && each.asksForMockOfFileItself && each.hasLoadAskedForOriginal && !each.hasWalkedAheadOfItsText && !each.failureForWantOfMockThatTakesOriginal; });
}

bool Modules::doesAnotherLoadInFlightLeadTo(ModuleID file)
{
    auto leadsToFile = [&](ModuleID root) {
        return fileOf(root) != file && modulesInLoadGraph(root, LoadGraphMode::IgnoreMocks).get(file.index());
    };
    for (const TextThatWaits& text : textsThatWait()) {
        if (leadsToFile(text.module))
            return true;
    }
    for (const TextOnItsWay& text : m_textsOnTheirWay) {
        if (leadsToFile(text.module))
            return true;
    }
    for (size_t queued : m_modulesOfQueuedImports) {
        if (leadsToFile(ModuleID(queued)))
            return true;
    }
    return false;
}

bool Modules::owesOriginalUnderMockThatHasSettled(ModuleID module)
{
    ModuleID file = fileOf(module);
    if (m_modulesWithPendingMock.get(module.index()) || m_modulesWithPendingMock.get(file.index()) || hasPendingMock(file) || (!activeMock(module) && !activeMock(file)))
        return false;
    if (wasOriginalEvaluated(file) || hasLoaderSettledOriginalOf(file))
        return false;
    return m_importsWhoseMocksRunWhenTheirTextsHaveCome.containsIf([&](const ImportWhoseMocksRunWhenItsTextHasCome& each) { return each.module == module && each.wasPassedByRequireOfItsFile && each.asksForMockOfFileItself && each.hasLoadAskedForOriginal && !each.hasWalkedAheadOfItsText && !each.failureForWantOfMockThatTakesOriginal; });
}

void Modules::letLoadInFlightAskForOriginal(ModuleID module)
{
    auto isBookingThatAsksNow = [&](const ImportWhoseMocksRunWhenItsTextHasCome& each) { return each.module == module && each.asksForMockOfFileItself && !each.hasLoadAskedForOriginal; };
    size_t place = m_importsWhoseMocksRunWhenTheirTextsHaveCome.findIf(isBookingThatAsksNow);
    if (place == notFound)
        return;
    if (m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].hasWalkedAheadOfItsText) {
        m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].hasLoadAskedForOriginal = true;
        return;
    }
    VM& vm = *m_instance.vm;
    JSGlobalObject* realm = m_instance.globalObject;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSPromise* failed = loadThatFailedForWantOfMock(module);
    if (!failed) {
        Identifier file = moduleKey(fileOf(module));
        loader()->resolve(realm, Identifier::fromString(vm, makeString('\0', file.string())), file, nullptr, true);
        if (scope.exception()) {
            failed = rejectedPromiseWithPendingException(realm, scope);
            failed->markAsHandled();
            m_loadsThatFailedForWantOfMock.append({ module, Strong<JSPromise>(vm, failed) });
        }
        place = m_importsWhoseMocksRunWhenTheirTextsHaveCome.findIf(isBookingThatAsksNow);
        if (place == notFound)
            return;
    }
    m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].hasLoadAskedForOriginal = true;
    if (!failed)
        return;
    m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].failureForWantOfMockThatTakesOriginal.set(vm, failed);
    auto isTextOfCall = [&, callSequenceNumber = m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].callSequenceNumber](const auto& text) { return text.module == module && text.callSequenceNumber == callSequenceNumber && !(text.promise->flags() & JSPromise::isFirstResolvingFunctionCalledFlag); };
    JSPromise* promiseOfCall = nullptr;
    for (const TextOnItsWay& text : m_textsOnTheirWay) {
        if (!promiseOfCall && !text.requested && isTextOfCall(text))
            promiseOfCall = text.promise.get();
    }
    for (const TextThatWaits& text : textsThatWait()) {
        if (!promiseOfCall && !text.requestedRecord && isTextOfCall(text))
            promiseOfCall = text.promise.get();
    }
    if (promiseOfCall)
        RELEASE_AND_RETURN(scope, promiseOfCall->resolve(realm, vm, failed));
}

void Modules::walkInPlaceOfCallsHeldForTextOf(ModuleID file)
{
    size_t place = m_importsWhoseMocksRunWhenTheirTextsHaveCome.findIf([&](const ImportWhoseMocksRunWhenItsTextHasCome& each) { return each.asksForMockOfFileItself && !each.hasWalkedAheadOfItsText && fileOf(each.module) == file; });
    if (place == notFound)
        return;
    ModuleID asked = m_importsWhoseMocksRunWhenTheirTextsHaveCome[place].module;
    if (!needsOriginalAheadOfFactory(asked) && !needsOriginalAheadOfFactory(file))
        return;
    if (!host().shouldBypassMock || !host().shouldBypassMock(m_instance.globalObject, m_table.pathText(file), String(), false)) {
        for (ImportWhoseMocksRunWhenItsTextHasCome& each : m_importsWhoseMocksRunWhenTheirTextsHaveCome)
            each.wasPassedByRequireOfItsFile |= fileOf(each.module) == file;
        return;
    }
    for (ImportWhoseMocksRunWhenItsTextHasCome& each : m_importsWhoseMocksRunWhenTheirTextsHaveCome)
        each.hasWalkedAheadOfItsText |= fileOf(each.module) == file;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(*m_instance.vm);
    {
        SetForScope moduleThatProgramImportsNow(m_moduleThatProgramImportsNow, std::optional(asked));
        SetForScope moduleThatImportAsksFor(m_currentImportTarget, asked);
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
        TextThatWaits text = m_callsToMakeAgain.takeFirst();
        makeCallAgain(text);
        if (scope.exception()) [[unlikely]] {
            m_callsToMakeAgain.clear();
            return;
        }
    }
    if (!m_callsToMakeAgain.isEmpty())
        queueStepOfImportInImportTurns(m_promiseOfJobThatMakesCallsAgain.get(), moduleOfJob, 1);
}

void Modules::noteChangeOfMockUnderCallsOfImport(ModuleID module)
{
    bool isChangedByImport = m_currentImportTarget && (m_modulesWhoseFactoryRuns.isEmpty() || m_modulesWhoseFactoryRuns.get(module.index()) || m_currentImportTarget != m_moduleThatImportAskedForWhenFactoryBegan);
    if (!isChangedByImport)
        m_lastDeedsOnMocksUnderCallsOfImport.remove(fileOf(module));
    if (m_numberOfCallsOfImportWithNamespaceKept == (isChangedByImport ? m_callsOfImportUnderMocks.size() : 0))
        return;
    JSObject* namespaceHeld = isChangedByImport ? namespaceIfCreated(module, ModulePhase::Evaluation) : nullptr;
    for (CallOfImportUnderMock& call : m_callsOfImportUnderMocks.values()) {
        if (call.module != module)
            continue;
        if (!isChangedByImport) {
            if (call.namespaceBeforeImportChangedMock)
                --m_numberOfCallsOfImportWithNamespaceKept;
            call.namespaceBeforeImportChangedMock.clear();
        } else if (!call.namespaceBeforeImportChangedMock) {
            if (JSObject* namespaceObject = namespaceHeld) {
                ++m_numberOfCallsOfImportWithNamespaceKept;
                call.namespaceBeforeImportChangedMock.set(*m_instance.vm, namespaceObject);
            }
        }
    }
}

void Modules::noteCallOfImportUnderMock(JSPromise* promise, ModuleID module)
{
    unsigned serial = ++m_numberOfCallsOfImportMadeUnderMocks;
    m_callsOfImportUnderMocks.add(promise, CallOfImportUnderMock { Strong<JSPromise>(*m_instance.vm, promise), moduleWithExportsOf(module), { }, serial });
    m_callsOfImportThatHaveTakenNoStep.append({ serial, promise });
}

void Modules::didTakeFirstStepOfCallOfImportSlow(JSPromise* promise)
{
    auto call = m_callsOfImportUnderMocks.find(promise);
    if (call != m_callsOfImportUnderMocks.end())
        takeFirstStepsOfCallsOfImportThrough(call->value.serial);
}

void Modules::takeFirstStepsOfCallsOfImportThrough(unsigned serial)
{
    while (!m_callsOfImportThatHaveTakenNoStep.isEmpty() && m_callsOfImportThatHaveTakenNoStep.first().first <= serial) {
        auto found = m_callsOfImportUnderMocks.find(m_callsOfImportThatHaveTakenNoStep.takeFirst().second);
        if (found == m_callsOfImportUnderMocks.end())
            continue;
        CallOfImportUnderMock& call = found->value;
        auto deed = m_lastDeedsOnMocksUnderCallsOfImport.find(fileOf(call.module));
        if (deed == m_lastDeedsOnMocksUnderCallsOfImport.end() || call.serial > deed->value.lastCallInFrontOfIt)
            continue;
        if (call.namespaceBeforeImportChangedMock) {
            call.namespaceBeforeImportChangedMock.clear();
            --m_numberOfCallsOfImportWithNamespaceKept;
        }
        if (deed->value.bringsBackFileThatHadRun || !deed->value.mock)
            continue;
        if (!deed->value.viewOfMock) {
            ModuleNamespace* view = ModuleNamespace::create(*m_instance.vm, namespaceStructure(true), call.module, ModulePhase::Evaluation, { });
            view->setMockOfView(*m_instance.vm, deed->value.mock.get());
            deed->value.viewOfMock.set(*m_instance.vm, view);
        }
        call.namespaceBeforeImportChangedMock.set(*m_instance.vm, deed->value.viewOfMock.get());
        ++m_numberOfCallsOfImportWithNamespaceKept;
    }
    if (m_callsOfImportThatHaveTakenNoStep.isEmpty())
        m_lastDeedsOnMocksUnderCallsOfImport.clear();
}

void Modules::noteDeedOnMockUnderCallsOfImportSlow(ModuleID module, DeedOnMock kind)
{
    LastDeedOnMock& deed = m_lastDeedsOnMocksUnderCallsOfImport.ensure(fileOf(module), [] { return LastDeedOnMock { }; }).iterator->value;
    if (!deed.mock) {
        if (JSObject* mock = activeMock(moduleWithExportsOf(module)))
            deed.mock.set(*m_instance.vm, mock);
    }
    deed.lastCallInFrontOfIt = m_numberOfCallsOfImportMadeUnderMocks;
    deed.bringsBackFileThatHadRun = kind == DeedOnMock::Ends && (hasIntactOriginal(module) || wasPatchedAfterEvaluation(module));
}

void Modules::forgetNamespacesKeptForCallsOfImport()
{
    m_lastDeedsOnMocksUnderCallsOfImport.clear();
    if (!m_numberOfCallsOfImportWithNamespaceKept)
        return;
    m_numberOfCallsOfImportWithNamespaceKept = 0;
    for (CallOfImportUnderMock& call : m_callsOfImportUnderMocks.values())
        call.namespaceBeforeImportChangedMock.clear();
}

JSObject* Modules::takeNamespaceKeptForCallOfImport(JSPromise* importPromise)
{
    auto call = m_callsOfImportUnderMocks.find(importPromise);
    if (call == m_callsOfImportUnderMocks.end())
        return nullptr;
    JSObject* namespaceObject = call->value.namespaceBeforeImportChangedMock.get();
    if (namespaceObject)
        --m_numberOfCallsOfImportWithNamespaceKept;
    m_callsOfImportUnderMocks.remove(call);
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
    if (!m_textsThatWait.isEmpty() && !m_instance.vm->defaultMicrotaskQueue().isEmpty())
        queueJobOfRound(moduleOfJob);
}

static Modules* modulesOfInstanceInImportTurns(Instance* instance)
{
    return instance->state ? instance->state->modules.get() : nullptr;
}

void fulfillDeferredFetches(VM& vm)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    VMState& state = vm.aotState();
    for (Instance* instance : state.instances) {
        if (Modules* modules = modulesOfInstanceInImportTurns(instance))
            modules->willDeliverText();
    }
    bool isDelivered = false;
    while (!isDelivered) {
        Modules* first = nullptr;
        for (Instance* instance : state.instances) {
            Modules* modules = modulesOfInstanceInImportTurns(instance);
            if (modules && modules->hasTextThatWaits()) {
                first = modules;
                break;
            }
        }
        if (!first)
            return;
        std::optional<uint32_t> turnOfCall = first->turnOfFirstTextThatWaits();
        isDelivered = turnOfCall ? first->deliverFirstTextThatWaits() : first->deliverFirstTextInLine();
        if (scope.exception()) [[unlikely]]
            return;
        while (turnOfCall && first->turnOfFirstTextThatWaits() == turnOfCall) {
            isDelivered |= first->deliverFirstTextThatWaits();
            if (scope.exception()) [[unlikely]]
                return;
        }
    }
}

void didDrainDeferredFetchJobs(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        if (Modules* modules = modulesOfInstanceInImportTurns(instance))
            modules->didDrainDeferredFetchJobs();
    }
}

bool shouldDeferFetchFulfillment(JSGlobalObject* realm, ModuleID module)
{
    if (!realm->vm().aotState().frameOfImportThatAsksLoader) [[likely]]
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
