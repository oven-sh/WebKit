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

    // record()'s requested modules are loaded, or it has none. What fails from then on is a link or an evaluation
    // error, and stays the answer for the key.
    bool isModuleLoaded() const;
    // The error of a load of this entry that failed before that, if there is one.
    JSValue failedLoadError() const
    {
        JSValue error = m_error.get();
        return error && !isModuleLoaded() ? error : JSValue();
    }
    // The load of this entry failed before its module was loaded, so the module was never linked and never ran. The
    // entry keeps the error for the loads that hold it, and leaves the registry (JSModuleLoader::unregisterFailedLoad()):
    // the next load of the key starts with a fetch. Two kinds of entry stay. One whose record the host made or
    // registered with others (hostOwnsRecord()) stays with the error, as before. One whose record another graph has
    // loaded in the meantime stays as the entry of a loaded module.
    void failLoad(JSGlobalObject*, JSValue error);
    // makeModule() rejected for what the fetch gave.
    void failMakeModule(JSGlobalObject*, JSValue error);
#endif

private:
    ModuleRegistryEntry(VM&, Structure*, JSModuleLoader*, Identifier key, ScriptFetchParameters::Type, RefPtr<ScriptFetcher>);

#if USE(BUN_JSC_ADDITIONS)
    JSSourceCode* fetchedSource() const;
    bool hostOwnsRecord() const;
#endif

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
    bool m_isLoaded { false };
    // The load of this entry failed after another graph had loaded record(): what a load reports from then on is not
    // stored as the module's error (failLoad()).
    bool m_isLoadedByAnotherGraph { false };
#endif
};

} // namespace JSC
