/*
 * Copyright (C) 2015-2022, 2026 Apple Inc. All rights reserved.
 * Copyright (C) 2016 Yusuke Suzuki <utatane.tea@gmail.com>.
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
#include "DeferredWorkTimer.h"
#include "ErrorType.h"
#include "JSObject.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleLoaderPayload.h"
#include "ModuleMap.h"
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC {

class ErrorInstance;
class JSPromise;
class JSModuleNamespaceObject;
class JSModuleRecord;
class JSSourceCode;
class ModuleLoadingContext;
class ModuleRegistryEntry;
class SourceOrigin;
class SyntheticModuleRecord;

enum class ModuleLoadFlag : uint8_t {
    Evaluate = 1 << 0,
    Dynamic = 1 << 1,
    UseImportMap = 1 << 2,
    Deferred = 1 << 3,
#if USE(BUN_JSC_ADDITIONS)
    IsSecondAttempt = 1 << 4, // See JSModuleLoader::tryToRecoverLoad().
    IsSynchronous = 1 << 5, // See ModuleLoadingContext::isAbandoned().
#endif
};

class JSModuleLoader final : public JSCell {
#if USE(BUN_JSC_ADDITIONS)
    friend class ImportOrderWalk;
#endif
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleLoaderSpace<mode>();
    }

    enum Status {
        Fetch = 1,
        Instantiate,
        Satisfy,
        Link,
        Ready,
    };

    // moduleScope: the scope the environments of this loader's modules are created
    // in; the global lexical environment, or lexical environments ending in it.
    static JSModuleLoader* create(JSGlobalObject* globalObject, VM& vm, Structure* structure, JSScope* moduleScope)
    {
        JSModuleLoader* object = new (NotNull, allocateCell<JSModuleLoader>(vm)) JSModuleLoader(vm, structure, moduleScope);
        object->finishCreation(globalObject, vm);
        return object;
    }

    JS_EXPORT_PRIVATE static JSModuleLoader* create(JSGlobalObject*, VM&, JSScope* moduleScope);

    JSScope* moduleScope() const { return m_moduleScope.get(); }

#if USE(BUN_JSC_ADDITIONS)
    // The async context (JSGlobalObject::m_asyncContextData field 0) the top-level code of
    // this loader's modules runs in, its top-level await continuations included. Empty
    // (the default): whatever is current when a module is executed.
    JSValue asyncContext() const { return m_asyncContext.get(); }
    void setAsyncContext(VM& vm, JSValue asyncContext) { m_asyncContext.set(vm, this, asyncContext); }
    // The one for the modules that an import() that is called now runs. Empty if the loader has one of its own.
    JSValue asyncContextOfImport(JSGlobalObject*) const;
#endif

    DECLARE_INFO;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    // APIs to control the module loader.
    void provideFetch(JSGlobalObject*, const Identifier& key, ScriptFetchParameters::Type, SourceCode&&);
    void provideFetch(JSGlobalObject*, const Identifier& key, ScriptFetchParameters::Type, JSValue);
    JSPromise* loadModule(JSGlobalObject*, const Identifier& moduleName, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>, OptionSet<ModuleLoadFlag>, int64_t referrerAsyncOrder = -1, const String& referrer = { }, AbstractModuleRecord* providedRecord = nullptr);
    JSPromise* linkAndEvaluateModule(JSGlobalObject*, const Identifier& moduleKey, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>);
    JSPromise* requestImportModule(JSGlobalObject*, const Identifier& moduleName, const Identifier& referrer, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>, bool deferred = false, int64_t referrerAsyncOrder = -1);
    JS_EXPORT_PRIVATE JSPromise* requestImportModuleForKey(JSGlobalObject*, const Identifier& resolvedKey, const Identifier& referrer, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>, bool deferred = false, int64_t referrerAsyncOrder = -1, AbstractModuleRecord* providedRecord = nullptr, JSPromise* resultPromise = nullptr);
#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE int64_t asyncEvaluationOrderForKey(const Identifier& key);
#endif

    // Platform dependent hooked APIs.
    JSPromise* importModule(JSGlobalObject*, JSString* moduleName, JSValue parameters, const SourceOrigin& referrer, bool deferred = false);
    static std::expected<RefPtr<ScriptFetchParameters>, JSPromise*> fetchParametersFromImportOptions(JSGlobalObject*, JSValue options);
    Identifier resolve(JSGlobalObject*, JSValue name, JSValue referrer, RefPtr<ScriptFetcher>, bool useImportMap);
    Identifier resolve(JSGlobalObject*, const Identifier& name, const Identifier& referrer, RefPtr<ScriptFetcher>, bool useImportMap);
    JSPromise* fetch(JSGlobalObject*, JSValue key, const String& referrer, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>, bool mayBePostponed = false);
    JSObject* createImportMetaProperties(JSGlobalObject*, JSValue key, JSModuleRecord*, RefPtr<ScriptFetcher>);

    // Additional platform dependent hooked APIs.
    JSValue evaluate(JSGlobalObject*, JSValue key, JSValue moduleRecord, RefPtr<ScriptFetcher>, JSValue sentValue, JSValue resumeMode);
    JSValue evaluateNonVirtual(JSGlobalObject*, JSValue key, JSValue moduleRecord, RefPtr<ScriptFetcher>, JSValue sentValue, JSValue resumeMode);

    // Utility functions.
    JSModuleNamespaceObject* getModuleNamespaceObject(JSGlobalObject*, JSValue moduleRecord);
    JSArray* dependencyKeysIfEvaluated(JSGlobalObject*, const String& key);

    DECLARE_VISIT_CHILDREN;

    static AbstractModuleRecord* getImportedModule(AbstractModuleRecord* referrer, const AbstractModuleRecord::ModuleRequest&);

    // Options correspond to Script Records, Cyclic Module Records and Realm Records, in that order.
    struct ModuleReferrer : Variant<ProgramExecutable*, CyclicModuleRecord*, JSGlobalObject*> {
        using Variant<ProgramExecutable*, CyclicModuleRecord*, JSGlobalObject*>::Variant;
        ProgramExecutable* getScript() const;
        CyclicModuleRecord* getModule() const;
        JSGlobalObject* getRealm() const;
        bool isScript() const;
        bool isModule() const;
        bool isRealm() const;
        JSValue toJSValue() const;
    };

    struct ModuleFailure {
        enum class Kind {
            Unknown,
            Instantiation,
            Evaluation,
        };

        ModuleFailure() = default;
        ModuleFailure(AbstractModuleRecord*, ScriptFetchParameters::Type, Kind);
        ModuleFailure(Identifier, ScriptFetchParameters::Type, Kind);

        bool isEvaluationError(const Identifier& expectedSpecifier, ScriptFetchParameters::Type expectedType) const;

        operator bool() const;

        AbstractModuleRecord* m_source { nullptr };
        Identifier m_key;
        ScriptFetchParameters::Type m_type { ScriptFetchParameters::Type::None };
        Kind m_kind { Kind::Unknown };
    };

    using ModuleRequest = AbstractModuleRecord::ModuleRequest;
    using ModuleCompletion = Variant<AbstractModuleRecord*, Exception*>;

    void innerModuleLoading(JSGlobalObject*, ModuleGraphLoadingState*, AbstractModuleRecord*);
    // payload is opaque to callers and is either a ModuleGraphLoadingState* (graph load) or a ModuleLoaderPayload* (top-level dynamic import).
    void finishLoadingImportedModule(JSGlobalObject*, const ModuleReferrer&, const ModuleRequest&, JSCell* payload, ModuleCompletion result, RefPtr<ScriptFetcher>);

    enum class RequestFlag : uint8_t {
        ForgetsObsoleteFailure = 1 << 0,
        CannotBePostponed = 1 << 1,
    };
    // Returns null without an exception if the host postponed the fetch.
    JSPromise* hostLoadImportedModule(JSGlobalObject*, const ModuleReferrer&, const ModuleRequest&, JSCell* payload, RefPtr<ScriptFetcher>, bool useImportMap, OptionSet<RequestFlag> = { });
    JSPromise* loadModule(JSGlobalObject*, const ModuleReferrer&, const ModuleRequest&, JSCell* payload, RefPtr<ScriptFetcher>, OptionSet<ModuleLoadFlag>, uint32_t queueOfLoad = 0);
    void continueModuleLoading(JSGlobalObject*, ModuleGraphLoadingState*, ModuleCompletion result);
    void continueDynamicImport(JSGlobalObject*, ModuleLoaderPayload*, ModuleCompletion, RefPtr<ScriptFetcher>);
    JSPromise* loadRequestedModules(JSGlobalObject*, AbstractModuleRecord*, RefPtr<ScriptFetcher>, ModuleGraphLoadingState* parentState = nullptr);

    // If moduleData is a JSSourceCode, then it's a JavaScript/WebAssembly/JSON/text source code,
    // and JSC is responsible for turning it into a usable module. Otherwise, the host has already
    // turned it into a module, and JSC should take that as the module's default export value.
    // An AbstractModuleRecord is the module itself.
    JSPromise* makeModule(JSGlobalObject*, const Identifier& moduleKey, JSValue moduleData);
#if USE(BUN_JSC_ADDITIONS)
    // Returns false if the host postponed the fetch.
    bool fetchSynchronously(JSGlobalObject*, JSPromise* fetchPromise, const Identifier& key, const String& referrer, RefPtr<ScriptFetchParameters>&&, RefPtr<ScriptFetcher>&&, bool mayBePostponed = false);
    void finishSynchronousLoad(JSGlobalObject*, const Identifier& root, ScriptFetchParameters::Type);
    void makePostponedRequest(JSGlobalObject*, const VM::PostponedModuleRequest&);
#endif

    static ErrorInstance* duplicateTypeError(JSGlobalObject*, ErrorInstance*);
    static ErrorInstance* duplicateError(JSGlobalObject*, ErrorInstance*);
    static ErrorInstance* maybeDuplicateFetchError(JSGlobalObject*, ErrorInstance*);
    static ModuleFailure getErrorInfo(JSGlobalObject*, ErrorInstance*);
    static bool isFetchError(JSGlobalObject*, ErrorInstance*);
    static bool attachErrorInfo(JSGlobalObject*, Exception*, AbstractModuleRecord* source, const Identifier& key, ScriptFetchParameters::Type, ModuleFailure::Kind);
    static bool attachErrorInfo(JSGlobalObject*, ThrowScope&, AbstractModuleRecord* source, const Identifier& key, ScriptFetchParameters::Type, ModuleFailure::Kind);
    static void attachErrorInfo(JSGlobalObject*, ErrorInstance*, AbstractModuleRecord* source, const Identifier& key, ScriptFetchParameters::Type, ModuleFailure::Kind);

    ModuleRegistryEntry* ensureRegistered(JSGlobalObject*, const Identifier& key, ScriptFetchParameters::Type);

#if USE(BUN_JSC_ADDITIONS)
    ModuleRegistryEntry* registryEntry(const Identifier& key)
    {
        // Bun's old JS loader keyed the registry by specifier alone. Almost
        // every entry uses the JavaScript type, so try that O(1) bucket first
        // before falling back to the other types.
        auto* impl = key.impl();
        if (auto entry = m_moduleMap.get({ impl, ScriptFetchParameters::Type::JavaScript }))
            return entry.get();
        if (!m_nonJavaScriptEntryCount) [[likely]]
            return nullptr; // a miss is common (require(esm), "is it registered yet?")
        for (ScriptFetchParameters::Type type : m_nonJavaScriptTypes) {
            if (auto entry = m_moduleMap.get({ impl, type }))
                return entry.get();
        }
        return nullptr;
    }
    // One for each type.
    JS_EXPORT_PRIVATE Vector<ModuleRegistryEntry*, 2> entriesWithKey(const Identifier&) const;
    // They have the type of the entry that answers. Null if they ask for what an import without attributes gets.
    JS_EXPORT_PRIVATE RefPtr<ScriptFetchParameters> parametersOfEntry(const Identifier& key, RefPtr<ScriptFetchParameters>&& parametersOfRequest) const;
    // What an import has loaded that is also what require() of the key would load.
    JS_EXPORT_PRIVATE ModuleRegistryEntry* registryEntryForRequire(const Identifier& key);
    JS_EXPORT_PRIVATE AbstractModuleRecord* recordOfOtherFormOfFile(const Identifier& key);
    // Null unless the record is registered under its own key.
    ModuleRegistryEntry* entryOf(AbstractModuleRecord*);
    const ModuleMap<WriteBarrier<ModuleRegistryEntry>>& moduleMap() const { return m_moduleMap; }
    // If a deferred module is what removes it, the imports that follow that module are answered anew.
    JS_EXPORT_PRIVATE bool removeEntry(const Identifier& key);
    // For a host that is about to answer for the key with another module. If nothing has made use of this one, it is removed,
    // and every importer asks again.
    JS_EXPORT_PRIVATE void removeEntryIfUnused(const Identifier& key);
    JS_EXPORT_PRIVATE void clearAll();

    // The steps that link call this first. If it returns a promise, an import has to be waited for: call it again once that has
    // settled, either way. See ImportOrderWalk.
    struct EvaluationOfLoad {
        int64_t referrerAsyncOrder { -1 };
        JSPromise* dynamicImportPromise { nullptr };
    };
    JSPromise* runDeferredModules(JSGlobalObject*, AbstractModuleRecord* root, RefPtr<ScriptFetcher>, bool hasWaited, JSValue errorOfImport, std::optional<EvaluationOfLoad>);
    // They call this once they have linked. If it returns a promise, an import of the module is being evaluated: start over once it has settled.
    JSPromise* promiseForTurnToBeEvaluated(JSGlobalObject*, AbstractModuleRecord*);
    void didEndImportOrderWalk(VM&);
    // It waits for an import, or it was a part of another walk, which goes on.
    void didStopImportOrderWalk(VM&);
    enum class FetchWaitsFor : uint8_t {
        Host,
        // It may be waiting for a deferred module, so those that precede it run while it is in flight.
        Script,
        // The host has a module without imports to give once those of the load have run. See below.
        DeferredModules,
    };
    JS_EXPORT_PRIVATE void setFetchWaitsFor(JSGlobalObject*, const Identifier& key, FetchWaitsFor);
    void didMakeModule(JSGlobalObject*, ModuleRegistryEntry*, AbstractModuleRecord*);
    void releaseAsyncContextOfLoad(ModuleRegistryEntry* root);
    void didSettleFetch(JSGlobalObject* globalObject)
    {
        if (!m_fetchesThatWaitForProgram.isEmpty()) [[unlikely]]
            runDeferredModulesAheadOfFetchesSoon(globalObject);
    }
    // How far the walks from one root have come, so that the next one goes on from there.
    struct PassedImports {
        HashMap<AbstractModuleRecord*, unsigned> countByImporter;
        bool includeModuleToEvaluate { false };
        bool areAllThatCanBePassed { false };
        uint64_t lastChange { 0 }; // See VM::ImportOrderState.
    };
    struct RunAheadOfLoad {
        bool didSomething { false };
        JSPromise* importInFlight { nullptr }; // The modules have yet to run.
    };
    // For when the load of `root` has done all it can without those fetches. If a module throws, the load fails.
    JS_EXPORT_PRIVATE RunAheadOfLoad runDeferredModulesAheadOfLoad(JSGlobalObject*, const Identifier& root);
    // A synchronous load gives the host no chance to call that. If this is true when moduleLoaderFetch is called, it can return null
    // without an exception, and is called again once those modules have run.
    bool mayPostponeFetch() const { return m_mayPostponeFetch; }
    // A deferred module has exported names that it lacked when an importer was linked against it.
    void forgetFailuresToLink();
    // A deferred module that precedes the import that failed may supply it. If this returns true, it settles resultPromise.
    JS_EXPORT_PRIVATE static bool mayRecoverLoad(ModuleRegistryEntry*);
    bool tryToRecoverLoad(JSGlobalObject*, ModuleLoadingContext*, JSPromise* resultPromise, JSValue& error);

    // See SyntheticSourceProvider::DeferredModule::generate(). If the module threw, the exception is pending.
    JS_EXPORT_PRIVATE static void didRunDeferredModule(JSGlobalObject*, SyntheticModuleRecord*);

    // To the deferred module that is running, it has not been requested yet.
    JS_EXPORT_PRIVATE static bool isAheadOfRunningModule(ModuleRegistryEntry*);
    JS_EXPORT_PRIVATE static bool isAheadOfRunningModule(AbstractModuleRecord* importer, const AbstractModuleRecord::LoadedModuleRequest&);
    // A load has come past the import, which stands for a fetch in flight.
    JS_EXPORT_PRIVATE bool hasPostponedImportOf(const Identifier& key);
    // To a module that is evaluated ahead of a link, the rest of its load is as good as linked to what has begun.
    JS_EXPORT_PRIVATE static bool someImporterAboutToBeLinked(AbstractModuleRecord* imported, NOESCAPE const Function<bool(CyclicModuleRecord*)>& counts);
    // It is a deferred module that has run, and its load has come to another one.
    JS_EXPORT_PRIVATE static bool hasRunAheadOfLink(SyntheticModuleRecord*);
    // Non-null while the module loads, is a deferred one that runs, or waits for one or for an import that is requested again.
    // Ask again once it has settled.
    JS_EXPORT_PRIVATE JSPromise* promiseForEndOfLoad(JSGlobalObject*, ModuleRegistryEntry*);
    JS_EXPORT_PRIVATE JSPromise* promiseForEndOfImportOrderWalk(JSGlobalObject*);
    JSPromise* promiseForEndOfImportOrderWalkIfAskedFor() const { return m_endOfImportOrderWalk.get(); }
    // For a host that a deferred module has taught to resolve or to fetch another way. They do nothing if none is running.
    JS_EXPORT_PRIVATE static void didChangeResolution(JSGlobalObject*);
    JS_EXPORT_PRIVATE static void didChangeFetch(VM&, NOESCAPE const Function<bool(const String& key)>& isAffected);
    // No module runs between the changes, so what loads in progress hold on to is settled once.
    class BatchOfChanges {
        WTF_MAKE_NONCOPYABLE(BatchOfChanges);
        WTF_FORBID_HEAP_ALLOCATION;
    public:
        JS_EXPORT_PRIVATE explicit BatchOfChanges(JSGlobalObject*);
        JS_EXPORT_PRIVATE ~BatchOfChanges();
        // It leaves because of an import that it holds on to, so its importers hold on to it.
        JS_EXPORT_PRIVATE void willEvictImporterAheadOfRunningModule(AbstractModuleRecord*);
    private:
        VM& m_vm;
        bool m_isInOuterBatch;
    };

    JS_EXPORT_PRIVATE JSPromise* loadModuleSync(JSGlobalObject*, const Identifier& moduleName, RefPtr<ScriptFetchParameters>&&, RefPtr<ScriptFetcher>&&);
#if ENABLE(AOT)
    bool hasResolutionFailure(const ResolutionMapKey& key) const { return m_resolutionFailures.contains(key); }
#endif
    JS_EXPORT_PRIVATE static void drainSynchronousModuleQueue(JSGlobalObject*);

    // Options::usePrelinkedModuleInfo(): the embedder's pre-resolved graph for this realm and the record it registered
    // for each of its modules (null until that module is fetched). Prelinked records resolve their pre-resolved
    // import bindings' module indices through this table.
    PrelinkedModuleGraph* prelinkedModuleGraph() const { return m_prelinkedGraph.get(); }
    JS_EXPORT_PRIVATE void setPrelinkedModuleGraph(Ref<PrelinkedModuleGraph>&&);
    AbstractModuleRecord* prelinkedRecord(uint32_t moduleIndex) const
    {
        return moduleIndex < m_prelinkedRecords.size() ? m_prelinkedRecords[moduleIndex].get() : nullptr;
    }
    JS_EXPORT_PRIVATE void setPrelinkedRecord(VM&, uint32_t moduleIndex, AbstractModuleRecord*);
    // A second record now exists for that module's key: clear the slot and resolve bindings into it by name from now on.
    JS_EXPORT_PRIVATE void forgetPrelinkedRecord(uint32_t moduleIndex);
    void pinPrelinkedEdges(uint32_t moduleIndex);
    // prelinkedRecord(), or null once that module's registry entry has ever been deleted: from then on the index may name
    // a record other than the one an importer's own (retained) graph edges lead to, so bindings into it resolve by name.
    AbstractModuleRecord* prelinkedRecordForResolution(uint32_t moduleIndex) const
    {
        if (moduleIndex < m_prelinkedRecordRemoved.size() && m_prelinkedRecordRemoved.quickGet(moduleIndex)) [[unlikely]]
            return nullptr;
        return prelinkedRecord(moduleIndex);
    }
#endif

    // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script step 13.1.2.
    void removeFailedFetchEntry(ModuleRegistryEntry*);
#if ENABLE(AOT)
    void removeEntriesForRecord(AbstractModuleRecord*, std::optional<AbstractModuleRecord::SupersededFor> = std::nullopt);
#endif

    ModuleRegistryEntry* getRegisteredMayBeNull(const Identifier& key, ScriptFetchParameters::Type);
    ModuleMapKey keyInModuleMap(UniquedStringImpl* key, ScriptFetchParameters::Type typeOfRequest) const
    {
#if USE(BUN_JSC_ADDITIONS)
        if (typeOfRequest != ScriptFetchParameters::Type::JavaScript) [[unlikely]]
            return { key, askHostForTypeInModuleMap(key, typeOfRequest) };
#endif
        return { key, typeOfRequest };
    }

    void addResolutionFailure(VM&, const ResolutionMapKey&, JSValue error);

private:
    JSModuleLoader(VM&, Structure*, JSScope* moduleScope);
    void finishCreation(JSGlobalObject*, VM&);

#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE ScriptFetchParameters::Type askHostForTypeInModuleMap(UniquedStringImpl* key, ScriptFetchParameters::Type) const;
    bool removeEntry(const Identifier& key, std::optional<AbstractModuleRecord::SupersededFor>);
    void runDeferredModulesAheadOfFetchesSoon(JSGlobalObject*);
    void runDeferredModulesAheadOfFetches(JSGlobalObject*);
    void didStartLoad(JSGlobalObject*, ModuleRegistryEntry* root, JSValue asyncContext);
    void removeFromLoadedModules(AbstractModuleRecord*);
    void forgetLoadsThatHaveEnded();
    void forgetPrelinkedRecordsWithKey(UniquedStringImpl* keyOrNullForAll);
    void pinPrelinkedEdgesOf(AbstractModuleRecord* importer, uint32_t onlyTarget);

    RefPtr<PrelinkedModuleGraph> m_prelinkedGraph;
    Vector<WriteBarrier<AbstractModuleRecord>> m_prelinkedRecords; // visited under cellLock()
    BitVector m_prelinkedRecordRemoved; // empty until the first removal
    unsigned m_nonJavaScriptEntryCount { 0 }; // m_moduleMap entries whose type is not JavaScript, for registryEntry()'s by-specifier fallback
    Vector<ScriptFetchParameters::Type> m_nonJavaScriptTypes; // that such entries have had
#endif
    void didAddModuleMapEntry(ScriptFetchParameters::Type type)
    {
#if USE(BUN_JSC_ADDITIONS)
        if (type == ScriptFetchParameters::Type::JavaScript) [[likely]]
            return;
        ++m_nonJavaScriptEntryCount;
        m_nonJavaScriptTypes.appendIfNotContains(type);
#else
        UNUSED_PARAM(type);
#endif
    }
    void didRemoveModuleMapEntry(ScriptFetchParameters::Type type)
    {
#if USE(BUN_JSC_ADDITIONS)
        ASSERT(type == ScriptFetchParameters::Type::JavaScript || m_nonJavaScriptEntryCount);
        m_nonJavaScriptEntryCount -= type != ScriptFetchParameters::Type::JavaScript;
#else
        UNUSED_PARAM(type);
#endif
    }

    // Corresponds to RealmRecord.[[LoadedModules]].
    ModuleMap<AbstractModuleRecord::LoadedModuleRequest> m_loadedModules;
#if USE(BUN_JSC_ADDITIONS)
    UniquedStringImpl* m_rootBeingReloaded { nullptr };
    bool m_mayPostponeFetch { false };
    bool m_isRunningAheadOfFetches { false };
    DeferredWorkTimer::WeakTicket m_runAheadOfFetches;
    // These name entries, so that they do not keep alive what has left the registry, and may name what has settled.
    struct NameOfEntry {
        Identifier key;
        ScriptFetchParameters::Type type;
    };
    struct LoadInProgress {
        NameOfEntry root;
        WriteBarrier<JSPromise> importInFlight;
        PassedImports passed;
        WriteBarrier<Unknown> asyncContext;
    };
    Vector<NameOfEntry> m_fetchesThatWaitForProgram;
    Vector<LoadInProgress> m_loadsInProgress; // visited under cellLock()
    bool m_hasAliasedLoadedModules { false };
#endif

    WriteBarrier<JSScope> m_moduleScope;
#if USE(BUN_JSC_ADDITIONS)
    WriteBarrier<Unknown> m_asyncContext;
    WriteBarrier<JSPromise> m_endOfImportOrderWalk;
    WriteBarrier<JSPromise> m_turnToBeEvaluated;
#endif
    ModuleMap<WriteBarrier<ModuleRegistryEntry>> m_moduleMap;

    struct ResolutionFailure {
        WriteBarrier<Unknown> error;
        RefPtr<UniquedStringImpl> referrer;
        RefPtr<UniquedStringImpl> specifier;
#if USE(BUN_JSC_ADDITIONS)
        uint64_t time { 0 }; // VM::ImportOrderState::clock
#endif
    };
    ResolutionMap<ResolutionFailure> m_resolutionFailures;
};

// Validates the host-defined payload threaded through HostLoadImportedModule / FinishLoadingImportedModule.
// Spec's `payload ∈ { GraphLoadingState Record, PromiseCapability Record }` is encoded in JSC as
// either ModuleGraphLoadingState* (graph load) or ModuleLoaderPayload* (top-level dynamic import).
inline bool isModuleLoaderHostDefinedPayload(JSCell* cell)
{
    return cell->inherits<ModuleGraphLoadingState>() || cell->inherits<ModuleLoaderPayload>();
}

} // namespace JSC
