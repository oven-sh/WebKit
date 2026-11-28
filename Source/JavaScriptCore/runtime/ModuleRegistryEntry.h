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

#pragma once

#include "AbstractModuleRecord.h"
#include "ErrorInstance.h"
#include "JSModuleLoader.h"
#include "JSObject.h"
#include "JSSourceCode.h"
#include "ScriptFetchParameters.h"
#include <wtf/RefPtr.h>

namespace JSC {

class JSModuleLoader;
class ScriptFetcher;

class ModuleRegistryEntry final : public JSCell {
    friend class LLIntOffsetsExtractor;
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    enum class Status : uint8_t {
        New,
        Fetching,
        Fetched,
        FetchFailed,
        InstantiationFailed,
        EvaluationFailed,
    };

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleRegistryEntrySpace<mode>();
    }

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);
    static ModuleRegistryEntry* create(VM&, Structure*, JSModuleLoader*, Identifier key, ScriptFetchParameters::Type, RefPtr<ScriptFetcher>);
    static ModuleRegistryEntry* create(VM&, JSModuleLoader*, Identifier key, ScriptFetchParameters::Type, RefPtr<ScriptFetcher>);

    JSModuleLoader* loader() const { return m_loader.get(); }

    const Identifier& key() const;
    ScriptFetchParameters::Type moduleType() const;
    AbstractModuleRecord* record() const;
    JSPromise* ensureFetchPromise(JSGlobalObject*);
    JSPromise* ensureModulePromise(JSGlobalObject*);
    JSPromise* loadPromise() const;
    JSValue error(JSGlobalObject*) const;
    JSValue fetchError() const;
    Status status() const;

    void setRecord(VM&, AbstractModuleRecord*);
    void setLoadPromise(VM&, JSPromise*);
    void setFetchError(JSGlobalObject*, JSValue);
    void setInstantiationError(JSGlobalObject*, JSValue);
    void setEvaluationError(JSGlobalObject*, JSValue);
    void setStatus(Status);

    void provideFetch(JSGlobalObject*, SourceCode&&);
    void provideFetch(JSGlobalObject*, JSValue);
    void fetchComplete(JSGlobalObject*, AbstractModuleRecord*);
#if USE(BUN_JSC_ADDITIONS)
    // For an embedder that already built the module record: leaves the entry Fetched without allocating the
    // fetch / module / load promises or scheduling the fetch -> makeModule microtask chain. The promises are
    // materialized on demand, already settled with the record, if a later top-level load or import edge asks for them.
    // The entry must not have a record yet.
    JS_EXPORT_PRIVATE void provideModule(VM&, AbstractModuleRecord*);
    // The embedder additionally guarantees record()'s [[LoadedModules]] is complete and every module reachable from
    // it is registered the same way, i.e. what a settled loadPromise means.
    void markLoaded() { ASSERT(m_record); m_isLoaded = true; }
    bool isLoaded() const;
    // loadPromise(), materializing it first for an entry that was markLoaded().
    JSPromise* loadedPromise(JSGlobalObject*);
    // loadPromise() is of a load that JSModuleLoader::loadModuleSync() ran.
    bool wasLoadStartedSynchronously() const { return m_wasLoadStartedSynchronously; }
    // The VM::SynchronousModuleQueue that has the steps that make the record of what the host has supplied.
    uint32_t queueThatMakesRecord() const { return m_queueThatMakesRecord; }
    void setQueueThatMakesRecord(uint32_t queue) { m_queueThatMakesRecord = queue; }
    // loadModuleSync() has returned without linking it, and no load has asked for it since.
    bool isAbandoned() const { return m_isAbandoned; }
    void setIsAbandoned(bool isAbandoned) { m_isAbandoned = isAbandoned; }

    using FetchWaitsFor = JSModuleLoader::FetchWaitsFor;

    // See ImportOrderWalk. A failure to load is final once a walk has come to it.
    bool isFailureFinal() const { return m_isFailureFinal; }
    void setIsFailureFinal() { m_isFailureFinal = true; }
    bool isFailureOfDeferredModule() const { return m_isFailureOfDeferredModule; }
    bool isFailureObsolete(VM& vm) const { return !m_isFailureFinal && !m_wasFetchPostponed && m_fetchWaitsFor != FetchWaitsFor::DeferredModules && m_requestedAt != vm.importOrderState().clock; }
    void setWasFetchPostponed() { m_wasFetchPostponed = true; }
    FetchWaitsFor fetchWaitsFor() const { return m_fetchWaitsFor; }
    void setFetchWaitsFor(FetchWaitsFor what) { m_fetchWaitsFor = what; }
    // The module that the host starts the program with. No import precedes it, so if it is deferred it runs when it is fetched.
    bool isEntryPoint() const { return m_isEntryPoint; }
    void setIsEntryPoint() { m_isEntryPoint = true; }
    bool isRootOfLoad() const { return m_isRootOfLoad; }
    void setIsRootOfLoad() { m_isRootOfLoad = true; }
    bool isRootOfLoadThatLinks() const { return m_isRootOfLoadThatLinks; }
    void setIsRootOfLoadThatLinks() { m_isRootOfLoadThatLinks = true; }
    // The error stands for the loads that start at the module, whatever becomes of those that come through it.
    bool didFailToRecoverLoad() const { return m_didFailToRecoverLoad; }
    void setDidFailToRecoverLoad() { m_didFailToRecoverLoad = true; }
    bool isRecoveringLoad() const { return m_isRecoveringLoad; }
    void setIsRecoveringLoad(bool isRecoveringLoad) { m_isRecoveringLoad = isRecoveringLoad; }
    enum class IsFinal : bool { No, Yes };
    void failToLoad(JSGlobalObject*, JSValue error, IsFinal, bool wasThrownByModule);
    // If so, a walk evaluates what precedes the module before it fails with the error.
    bool wasErrorThrownByModule() const { return m_wasErrorThrownByModule; }
    void forgetFailureToLink();
    // A deferred module can throw after a load that it started has run its importer. That fails the load, not the importer.
    bool hasRunWithoutError() const;
    // From then on the record stands in for it, as after provideModule().
    void releaseSource() { m_fetchPromise.clear(); }
    void forgetFailureToLoad(JSGlobalObject*);
    // For a step of a load that failed. Another load may have settled what becomes of the module in the meantime.
    bool ignoresFailureToLoad(JSGlobalObject*);
    bool isSuperseded() const { return m_isSuperseded; }
    void supersede(VM&, AbstractModuleRecord::SupersededFor);
#endif

private:
    ModuleRegistryEntry(VM&, Structure*, JSModuleLoader*, Identifier key, ScriptFetchParameters::Type, RefPtr<ScriptFetcher>);

    void finishCreation(VM&);

    const Identifier m_key;
    const ScriptFetchParameters::Type m_type;
    const RefPtr<ScriptFetcher> m_scriptFetcher;
    WriteBarrier<JSModuleLoader> m_loader;
    WriteBarrier<AbstractModuleRecord> m_record;
    WriteBarrier<JSPromise> m_fetchPromise;
    WriteBarrier<JSPromise> m_modulePromise;
    WriteBarrier<JSPromise> m_loadPromise;
    // The fetch / instantiation / evaluation errors are mutually exclusive: m_status
    // disambiguates which kind m_error holds (FetchFailed / InstantiationFailed / EvaluationFailed).
    WriteBarrier<Unknown> m_error;
    Status m_status { Status::New };
#if USE(BUN_JSC_ADDITIONS)
    bool m_isLoaded : 1 { false };
    bool m_wasLoadStartedSynchronously : 1 { false };
    bool m_isFailureFinal : 1 { false };
    bool m_isFailureOfDeferredModule : 1 { false };
    bool m_wasErrorThrownByModule : 1 { false };
    bool m_didFailToRecoverLoad : 1 { false };
    bool m_isRecoveringLoad : 1 { false };
    bool m_isSuperseded : 1 { false };
    bool m_isRootOfLoad : 1 { false };
    bool m_isRootOfLoadThatLinks : 1 { false };
    bool m_wasFetchPostponed : 1 { false };
    bool m_isEntryPoint : 1 { false };
    bool m_isAbandoned : 1 { false };
    FetchWaitsFor m_fetchWaitsFor { FetchWaitsFor::Host };
    uint32_t m_queueThatMakesRecord { 0 };
    uint64_t m_requestedAt;
#endif
};

} // namespace JSC
