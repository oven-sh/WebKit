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
#include "JSMicrotask.h"
#include "JSModuleLoader.h"
#include "MicrotaskQueueInlines.h"
#include "ModuleRegistryEntry.h"
#include "TopExceptionScope.h"
#include "VMInlines.h"

namespace JSC { namespace AOT {

// queueStepOfImport() of AOTModules.cpp.
static void queueStepOfImportInImportTurns(JSPromise* importPromise, ModuleID module, uint8_t stepsLeft)
{
    JSGlobalObject* realm = importPromise->realm();
    realm->queueMicrotask(realm->vm(), InternalMicrotask::CompiledModuleImportStep, stepsLeft, importPromise, jsNumber(module.index()), JSValue());
}

// settleAsLoaderDoes() of AOTModules.cpp.
static void settleAsLoaderDoesInImportTurns(JSGlobalObject* realm, JSPromise* promiseOfProgram, JSPromise* loaded)
{
    VM& vm = realm->vm();
    if (loaded->status() == JSPromise::Status::Fulfilled)
        promiseOfProgram->fulfill(vm, loaded->result());
    else
        promiseOfProgram->resolve(realm, vm, loaded);
}

// `import("./slow"); await vi.dynamicImportSettled()`
void Modules::tellHostOfImport(JSPromise* promise, const String& path)
{
    if (auto* didStartImport = host().didStartImport; didStartImport && promise->status() == JSPromise::Status::Pending)
        didStartImport(m_instance.globalObject, promise, path);
}

// As the host decides where it is asked to fetch a file for an import. While a require() loads a graph it reads every text itself, also for an import() that is called meanwhile.
bool Modules::isTextReadOnOtherThread(ModuleID file)
{
    return host().callInLaterTurn && isFileOfProgram(file) && !m_instance.vm->synchronousModuleQueue() && !hostLoadsSynchronously(file);
}

bool Modules::isTestRun() const
{
    return host().isInPreload && host().isInPreload(m_instance.globalObject).has_value();
}

bool Modules::loadsAsExecutableDoes() const
{
    return host().holdsTextsOfProgram && host().holdsTextsOfProgram(m_instance.globalObject);
}

// `bun test` reads a text on another thread, so what a call asks for comes a turn later: behind the `describe` callbacks of a test file, behind the end of a test that does not wait for it.
bool Modules::hasTextAtHand() const
{
    return !m_innermostFilesBeingRun && m_textsOnTheirWay.isEmpty() && loadsAsExecutableDoes() && (!isTestRun() || m_isInJobsBehindDelivery);
}

bool Modules::textOfImportComesInLaterTurn(ModuleID module, AskedBy askedBy)
{
    ModuleID file = fileOf(module);
    if (!isTextReadOnOtherThread(file))
        return false;
    // The loader has the text of whatever it has come to, and what a require() has begun is not begun by this call.
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
    // What a file asks for while a graph is loaded stands in line, and so does whatever is asked for while a text is on its way.
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
    m_textsThatWait.append(TextThatWaits { place, m_turnOfDeliveries, Strong<JSPromise>(vm, promise), module, Strong<JSCell>(vm, requested), referrerAsyncOrder, caller, m_jobsBehindFirstCall });
    host().callInLaterTurn(m_instance.globalObject);
}

// A text is held in line. The calls that wait for a turn alone came ahead of it, and no call overtakes a text that is on its way.
void Modules::putCallsThatWaitApartInLine()
{
    if (m_textsThatWait.isEmpty()) [[likely]]
        return;
    for (TextThatWaits& text : textsThatWait()) {
        size_t index = m_textsOnTheirWay.size();
        while (index && m_textsOnTheirWay[index - 1].place > text.place)
            --index;
        // As startImportInLaterTurn().
        if (!text.requestedRecord)
            m_modulesOfQueuedImports.set(text.module.index());
        m_textsOnTheirWay.insert(index, TextOnItsWay { text.place, WTF::move(text.promise), text.module, WTF::move(text.requestedRecord), text.referrerAsyncOrder, std::nullopt });
    }
    m_textsThatWait.clear();
    m_numberOfTextsThatHaveCome = 0;
}

void Modules::forgetTextsThatWait()
{
    m_textsThatWait.clear();
    m_numberOfTextsThatHaveCome = 0;
    m_callsToMakeAgain.clear();
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
    auto scope = DECLARE_THROW_SCOPE(*m_instance.vm);
    JSPromise* promise = text.promise.get();
    // The host says when they have run: no job of the run time's own stands among those of the program meanwhile.
    m_isInJobsBehindDelivery = true;
    if (text.jobsBehindFirstCall) {
        m_callsToMakeAgain.append(WTF::move(text));
        makeCallAgain(promise);
        return true;
    }
    scope.release();
    makeCallAgain(text);
    return true;
}

bool Modules::deliverFirstTextInLine()
{
    if (m_instance.globalObject->microtaskRunnability() == QueuedTaskResult::Discard) {
        m_textsOnTheirWay.removeAt(0);
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
    JSPromise* loaded = startImport(text.module, text.referrerAsyncOrder, text.caller, TextOfImport::HasCome, promise);
    if (scope.exception()) [[unlikely]] {
        promise->rejectWithCaughtException(vm, scope);
        return;
    }
    scope.release();
    if (loaded != promise)
        settleAsLoaderDoesInImportTurns(m_instance.globalObject, promise, loaded);
}

bool Modules::makeCallAgain(JSPromise* promise)
{
    size_t index = m_callsToMakeAgain.findIf([&](const TextThatWaits& text) { return text.promise.get() == promise; });
    if (index == notFound)
        return false;
    // A step counts its jobs in a byte.
    if (uint32_t jobs = std::min<uint32_t>(m_callsToMakeAgain[index].jobsBehindFirstCall, 255)) {
        m_callsToMakeAgain[index].jobsBehindFirstCall -= jobs;
        queueStepOfImportInImportTurns(promise, m_callsToMakeAgain[index].module, jobs);
        return true;
    }
    TextThatWaits text = WTF::move(m_callsToMakeAgain[index]);
    m_callsToMakeAgain.removeAt(index);
    makeCallAgain(text);
    return true;
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
        // Only a request to terminate is left behind, and the jobs find it.
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

// A call has asked the loader by a key, and the key is that of a file of the program.
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
