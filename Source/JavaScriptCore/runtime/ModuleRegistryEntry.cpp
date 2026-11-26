/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ModuleRegistryEntry.h"

#if ENABLE(AOT)
#include "AOTModuleRecord.h"
#endif
#include "ImportOrderWalk.h"
#include "JSCInlines.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "JSPromise.h"
#include "Microtask.h"
#include "SyntheticModuleRecord.h"

namespace JSC {

const ClassInfo ModuleRegistryEntry::s_info = { "ModuleRegistryEntry"_s, nullptr, nullptr, nullptr, CREATE_METHOD_TABLE(ModuleRegistryEntry) };

ModuleRegistryEntry::ModuleRegistryEntry(VM& vm, Structure* structure, JSModuleLoader* loader, Identifier key, ScriptFetchParameters::Type type, RefPtr<ScriptFetcher> scriptFetcher)
    : Base(vm, structure)
    , m_key(WTF::move(key))
    , m_type(type)
    , m_scriptFetcher(WTF::move(scriptFetcher))
    , m_loader(loader, WriteBarrierEarlyInit)
#if USE(BUN_JSC_ADDITIONS)
    , m_requestedAt(vm.importOrderState().clock)
#endif
{
}

void ModuleRegistryEntry::destroy(JSCell* cell)
{
    SUPPRESS_MEMORY_UNSAFE_CAST auto* thisObject = static_cast<ModuleRegistryEntry*>(cell);
    thisObject->~ModuleRegistryEntry();
}

void ModuleRegistryEntry::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

template<typename Visitor>
void ModuleRegistryEntry::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<ModuleRegistryEntry>(cell);
    ASSERT_GC_OBJECT_INHERITS(thisObject, info());
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_loader);
    visitor.append(thisObject->m_record);
    visitor.append(thisObject->m_fetchPromise);
    visitor.append(thisObject->m_modulePromise);
    visitor.append(thisObject->m_loadPromise);
    visitor.append(thisObject->m_error);
}

DEFINE_VISIT_CHILDREN(ModuleRegistryEntry);

ModuleRegistryEntry* ModuleRegistryEntry::create(VM& vm, Structure* structure, JSModuleLoader* loader, Identifier key, ScriptFetchParameters::Type type, RefPtr<ScriptFetcher> scriptFetcher)
{
    ModuleRegistryEntry* instance = new (NotNull, allocateCell<ModuleRegistryEntry>(vm)) ModuleRegistryEntry(vm, structure, loader, WTF::move(key), type, WTF::move(scriptFetcher));
    instance->finishCreation(vm);
    return instance;
}

ModuleRegistryEntry* ModuleRegistryEntry::create(VM& vm, JSModuleLoader* loader, Identifier key, ScriptFetchParameters::Type type, RefPtr<ScriptFetcher> scriptFetcher)
{
    return create(vm, vm.moduleRegistryEntryStructure.get(), loader, WTF::move(key), type, WTF::move(scriptFetcher));
}

const Identifier& ModuleRegistryEntry::key() const
{
    return m_key;
}

ScriptFetchParameters::Type ModuleRegistryEntry::moduleType() const
{
    return m_type;
}

AbstractModuleRecord* ModuleRegistryEntry::record() const
{
    return m_record.get();
}

JSPromise* ModuleRegistryEntry::ensureFetchPromise(JSGlobalObject* globalObject)
{
    if (m_fetchPromise)
        return m_fetchPromise.get();

    VM& vm = globalObject->vm();

    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    promise->markAsHandled();

    if (m_status == Status::FetchFailed && m_error)
        promise->reject(vm, m_error.get());
#if USE(BUN_JSC_ADDITIONS)
    else if (m_record)
        promise->fulfill(vm, m_record.get()); // provideModule(): already fetched and instantiated
#endif

    m_fetchPromise.set(vm, this, promise);
    return promise;
}

JSPromise* ModuleRegistryEntry::ensureModulePromise(JSGlobalObject* globalObject)
{
    if (m_modulePromise)
        return m_modulePromise.get();

    VM& vm = globalObject->vm();

    // Pre-create the module promise. It will be resolved/rejected by the
    // ModuleRegistryFetchSettled and ModuleRegistryModuleSettled microtask handlers.
    JSPromise* modulePromise = JSPromise::create(vm, globalObject->promiseStructure());
    modulePromise->markAsHandled();
    m_modulePromise.set(vm, this, modulePromise);
#if USE(BUN_JSC_ADDITIONS)
    if (m_record) {
        modulePromise->fulfill(vm, m_record.get());
        return modulePromise;
    }
#endif

    JSPromise* fetchPromise = ensureFetchPromise(globalObject);
    fetchPromise->performPromiseThenWithInternalMicrotask(vm, InternalMicrotask::ModuleRegistryFetchSettled, modulePromise, this);

    return modulePromise;
}

JSPromise* ModuleRegistryEntry::loadPromise() const
{
    return m_loadPromise.get();
}

JSValue ModuleRegistryEntry::error(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue error = m_error.get()) {
        if (m_status == Status::FetchFailed) {
            if (auto* errorInstance = dynamicDowncast<ErrorInstance>(error))
                RELEASE_AND_RETURN(scope, JSModuleLoader::duplicateError(globalObject, errorInstance));
        }
        RELEASE_AND_RETURN(scope, error);
    }
    if (m_record) {
        if (auto* cyclic = dynamicDowncast<CyclicModuleRecord>(m_record.get()))
            RELEASE_AND_RETURN(scope, cyclic->evaluationError());
    }
    return { };
}

JSValue ModuleRegistryEntry::fetchError() const
{
    if (m_status == Status::FetchFailed)
        return m_error.get();
    return { };
}

auto ModuleRegistryEntry::status() const -> Status
{
    return m_status;
}

void ModuleRegistryEntry::setRecord(VM& vm, AbstractModuleRecord* record)
{
    m_record.set(vm, this, record);
}

void ModuleRegistryEntry::setLoadPromise(VM& vm, JSPromise* promise)
{
    m_loadPromise.set(vm, this, promise);
#if USE(BUN_JSC_ADDITIONS)
    m_wasLoadStartedSynchronously = !!vm.synchronousModuleQueue();
#endif
}

void ModuleRegistryEntry::setFetchError(JSGlobalObject* globalObject, JSValue error)
{
    ASSERT(error);
    VM& vm = globalObject->vm();
    m_error.set(vm, this, error);
    if (m_status == Status::New && m_fetchPromise)
        m_fetchPromise->reject(vm, error);
    setStatus(Status::FetchFailed);
}

void ModuleRegistryEntry::setInstantiationError(JSGlobalObject* globalObject, JSValue error)
{
    ASSERT(error);
    if (m_status == Status::FetchFailed)
        return;
    VM& vm = globalObject->vm();
    m_error.set(vm, this, error);
    setStatus(Status::InstantiationFailed);
}

void ModuleRegistryEntry::setEvaluationError(JSGlobalObject* globalObject, JSValue error)
{
    ASSERT(error);
    if (m_status == Status::FetchFailed)
        return;
    VM& vm = globalObject->vm();
    m_error.set(vm, this, error);
    setStatus(Status::EvaluationFailed);
}

void ModuleRegistryEntry::setStatus(Status status)
{
    m_status = status;
}

void ModuleRegistryEntry::provideFetch(JSGlobalObject* globalObject, SourceCode&& sourceCode)
{
    provideFetch(globalObject, JSSourceCode::create(globalObject->vm(), WTF::move(sourceCode)));
}

void ModuleRegistryEntry::provideFetch(JSGlobalObject* globalObject, JSValue moduleData)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    ASSERT(m_status == Status::New);

    ensureModulePromise(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    scope.release();
    m_status = Status::Fetching;
    m_fetchPromise->fulfill(vm, moduleData);
}

#if USE(BUN_JSC_ADDITIONS)
void ModuleRegistryEntry::provideModule(VM& vm, AbstractModuleRecord* record)
{
    ASSERT(!m_record);
    ASSERT(m_status == Status::New || m_status == Status::Fetching);
    m_record.set(vm, this, record);
    m_status = Status::Fetched;
    // Promises some in-flight load already created for this entry are settled now (with the record standing in for
    // the source: moduleLoadTopSettled has nothing to provide for it); their pending reactions see the settled module
    // promise and bail.
    if (m_fetchPromise && m_fetchPromise->status() == JSPromise::Status::Pending)
        m_fetchPromise->fulfillPromise(vm, record);
    if (m_modulePromise && m_modulePromise->status() == JSPromise::Status::Pending)
        m_modulePromise->fulfill(vm, record);
}

bool ModuleRegistryEntry::isLoaded() const
{
    return m_isLoaded || (m_loadPromise && m_loadPromise->status() == JSPromise::Status::Fulfilled);
}

JSPromise* ModuleRegistryEntry::loadedPromise(JSGlobalObject* globalObject)
{
    if (!m_loadPromise && m_isLoaded) {
        VM& vm = globalObject->vm();
        JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
        promise->markAsHandled();
        promise->fulfill(vm, m_record.get());
        m_loadPromise.set(vm, this, promise);
    }
    return m_loadPromise.get();
}

void ModuleRegistryEntry::failToLoad(JSGlobalObject* globalObject, JSValue error, IsFinal isFinal, bool wasThrownByModule)
{
    VM& vm = globalObject->vm();
    m_isFailureFinal |= isFinal == IsFinal::Yes;
    m_wasErrorThrownByModule = wasThrownByModule;
    m_error.set(vm, this, error);
    m_status = Status::EvaluationFailed;
    if (m_loadPromise && m_loadPromise->status() == JSPromise::Status::Pending)
        m_loadPromise->reject(vm, error);
    if (m_record && m_isFailureFinal)
        m_record->setWasReached(false);
    auto* synthetic = dynamicDowncast<SyntheticModuleRecord>(m_record.get());
    if (!synthetic)
        return;
    m_isFailureOfDeferredModule = synthetic->isOfDeferredModule();
    // It has no exports to link against.
    m_record.clear();
    m_isLoaded = false;
    JSPromise* promise = JSPromise::create(vm, globalObject->promiseStructure());
    promise->rejectAsHandled(vm, error);
    m_modulePromise.set(vm, this, promise);
    m_loadPromise.set(vm, this, promise);
}

void ModuleRegistryEntry::forgetFailureToLoad(JSGlobalObject* globalObject)
{
    ASSERT(m_record && !m_isFailureFinal);
    if (!m_error && (!m_loadPromise || m_loadPromise->status() == JSPromise::Status::Fulfilled))
        return;
    m_record->setDidLoadAfterFailure();
    m_error.clear();
    m_didFailToRecoverLoad = false;
    m_status = Status::Fetched;
    // If it is pending, the steps that settle it may yet report the failure.
    m_loadPromise.clear();
    m_isLoaded = true;
    loadedPromise(globalObject);
}

void ModuleRegistryEntry::forgetFailureToLink()
{
    auto* module = m_error && !m_isFailureFinal ? dynamicDowncast<CyclicModuleRecord>(m_record.get()) : nullptr;
    if (!module || module->status() != CyclicModuleRecord::Status::Unlinked)
        return;
    m_error.clear();
    m_status = Status::Fetched;
}

bool ModuleRegistryEntry::hasRunWithoutError() const
{
    auto* module = dynamicDowncast<CyclicModuleRecord>(m_record.get());
    if (!module || module->status() < CyclicModuleRecord::Status::Evaluating || module->evaluationError())
        return false;
    return !module->cycleRoot() || !module->cycleRoot()->evaluationError();
}

bool ModuleRegistryEntry::ignoresFailureToLoad(JSGlobalObject* globalObject)
{
    if (m_isFailureFinal)
        return !!m_error;
    auto* module = dynamicDowncast<CyclicModuleRecord>(m_record.get());
    if (!module || module->status() == CyclicModuleRecord::Status::New)
        return false;
#if ENABLE(AOT)
    if (AOT::compiledModule(globalObject->vm(), module))
        return module->didLoadAfterFailure();
#endif
    module->setDidLoadAfterFailure();
    if (!m_error)
        forgetFailureToLoad(globalObject);
    return true;
}

void ModuleRegistryEntry::supersede(VM& vm, AbstractModuleRecord::SupersededFor imports)
{
    m_isSuperseded = true;
    if (m_record)
        m_record->supersede(ImportOrderWalk::noteChange(vm), imports);
}
#endif

void ModuleRegistryEntry::fetchComplete(JSGlobalObject* globalObject, AbstractModuleRecord* record)
{
    if (m_status == Status::FetchFailed) {
        // This is possible if resolution fails during HostLoadImportedModule.
        return;
    }
    VM& vm = globalObject->vm();
#if USE(BUN_JSC_ADDITIONS)
    // Two loads of one key can be in flight (a mock laid over a mock whose factory is pending), and the other one may have failed first.
    ASSERT(m_status == Status::Fetching || m_status == Status::EvaluationFailed);
#else
    ASSERT(m_status == Status::Fetching);
#endif
    m_record.set(vm, this, record);
    m_status = Status::Fetched;
#if ENABLE(AOT)
    if (auto* fromSource = vm.hasCompiledCode() && loader() == globalObject->moduleLoader() ? dynamicDowncast<JSModuleRecord>(record) : nullptr)
        AOT::didMakeRecordFromSource(globalObject, fromSource);
#endif
}

} // namespace JSC
