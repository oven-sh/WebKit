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
#include "AsyncContextSwapScope.h"
#include "JSCInlines.h"
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleRegistryEntry.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"

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

void Modules::tellHostOfImport(JSPromise* promise, const String& path)
{
    if (auto* didStartImport = host().didStartImport; didStartImport && promise->status() == JSPromise::Status::Pending)
        didStartImport(m_instance.globalObject, promise, path);
}

bool Modules::isTextReadOnOtherThread(ModuleID file)
{
    return host().callInLaterTurn && isFileOfProgram(file) && !m_instance.vm->synchronousModuleQueue() && !isFetchedInsideCall(file);
}

bool Modules::isTestRun() const
{
    return host().isInPreload && host().isInPreload(m_instance.globalObject).has_value();
}

bool Modules::loadsAsExecutableDoes() const
{
    return host().holdsTextsOfProgram && host().holdsTextsOfProgram(m_instance.globalObject);
}

bool Modules::hasTextAtHand() const
{
    return !m_innermostFilesBeingRun && m_textsOnTheirWay.isEmpty() && loadsAsExecutableDoes() && (!isTestRun() || m_isInJobsBehindDelivery);
}

bool Modules::textOfImportComesInLaterTurn(ModuleID module, AskedBy askedBy)
{
    ModuleID file = fileOf(module);
    if (!isTextReadOnOtherThread(file) || mockInPlace(module) || mockInPlace(file))
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
    if (m_innermostFilesBeingRun || !m_textsOnTheirWay.isEmpty()) {
        std::optional<ModuleID> fileThatCalls = m_innermostFilesBeingRun ? fileOrFaceEvaluatedIn(*m_innermostFilesBeingRun) : std::nullopt;
        holdText(promise, module, requested, referrerAsyncOrder, fileThatCalls ? turnInWhichTextCame(*fileThatCalls) : m_turnOfTexts);
        return;
    }
    if (m_textsThatWait.isEmpty()) {
        m_jobsBehindFirstCall = 0;
        queueJobOfRound(module);
    }
    uint64_t place = static_cast<uint64_t>(m_turnOfTexts) << 32 | m_numberOfTextsAskedFor++;
    m_textsThatWait.append(TextThatWaits { place, m_turnOfDeliveries, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder, caller, m_jobsBehindFirstCall, vm.aotState().momentOfLoads, requested ? Strong<Unknown>() : currentAsyncContext() });
    host().callInLaterTurn(m_instance.globalObject);
}

void Modules::putCallsThatWaitApartInLine()
{
    if (m_textsThatWait.isEmpty()) [[likely]]
        return;
    for (TextThatWaits& text : textsThatWait()) {
        if (!text.requestedRecord)
            m_modulesOfQueuedImports.set(text.module.index());
        m_textsOnTheirWay.appendAndBubble(TextOnItsWay { text.place, WTF::move(text.promise), text.module, WTF::move(text.requestedRecord), text.referrerAsyncOrder, std::nullopt, text.momentOfCall, WTF::move(text.asyncContext) }, [&](const TextOnItsWay& inLine) {
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
    m_isInJobsBehindDelivery = true;
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
    SetForScope momentOfCall(m_momentOfCallWhoseTextHasCome, text.momentOfCall);
    AsyncContextSwapScope asyncContextOfCall(vm, m_instance.globalObject, text.asyncContext.get());
    ModuleEvaluationAsyncContextScope jobOfImport(vm, m_instance.globalObject, text.asyncContext.get());
    JSPromise* loaded = startImport(text.module, text.referrerAsyncOrder, text.caller, TextOfImport::HasCome, promise);
    if (scope.exception()) [[unlikely]] {
        promise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    if (loaded != promise)
        settleAsLoaderDoesInImportTurns(m_instance.globalObject, promise, loaded);
}

void Modules::forgetMockLaidBehindCallWhoseTextHasCome(ModuleID file)
{
    std::optional<uint32_t> momentOfCall = std::exchange(m_momentOfCallWhoseTextHasCome, std::nullopt);
    if (!momentOfCall || !m_modulesWithMockToRun.get(file.index()))
        return;
    auto laid = m_momentsWaitingMocksWereLaid.find(file);
    if (laid == m_momentsWaitingMocksWereLaid.end() || laid->value.moment <= *momentOfCall)
        return;
    forgetMockThatWaits(file);
    forgetCallOfMock(file);
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

void Modules::queueJobOfRound(ModuleID moduleOfJob)
{
    if (!loadsAsExecutableDoes() || std::exchange(m_isJobOfRoundQueued, true))
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

void deliverTextThatWaits(VM& vm)
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

void didRunJobsBehindDelivery(VM& vm)
{
    for (Instance* instance : vm.aotState().instances) {
        if (Modules* modules = modulesOfInstanceInImportTurns(instance))
            modules->didRunJobsBehindDelivery();
    }
}

bool textOfFetchedModuleComesInLaterTurn(JSGlobalObject* realm, ModuleID module)
{
    if (!realm->vm().aotState().frameOfImportThatAsksLoader) [[likely]]
        return false;
    Modules* modules = modulesOf(realm);
    return modules && modules->isTestRun() && !modules->hasTextAtHand() && modules->textOfImportComesInLaterTurn(module, Modules::AskedBy::Loader);
}

void holdTextOfFetchedModule(JSGlobalObject* realm, ModuleID module, JSPromise* fetched, AbstractModuleRecord* record)
{
    modulesOf(realm)->holdTextOfCall(fetched, module, uncheckedDowncast<ModuleRecord>(record), std::nullopt, Modules::ImportCaller::CompiledCode);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
