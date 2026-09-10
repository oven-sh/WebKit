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
#include "ErrorType.h"
#include "JSObject.h"
#include "ModuleGraphLoadingState.h"
#include "ModuleLoaderPayload.h"
#include "JSScope.h"
#include "ModuleMap.h"
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>

namespace JSC {

class ErrorInstance;
class JSPromise;
class JSModuleNamespaceObject;
class JSModuleRecord;
class JSSourceCode;
class ModuleRegistryEntry;
class SourceOrigin;

enum class ModuleLoadFlag : uint8_t {
    Evaluate = 1 << 0,
    Dynamic = 1 << 1,
    UseImportMap = 1 << 2,
    Deferred = 1 << 3,
};

class JSModuleLoader final : public JSCell {
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

    static JSModuleLoader* create(JSGlobalObject* globalObject, VM& vm, Structure* structure)
    {
        JSModuleLoader* object = new (NotNull, allocateCell<JSModuleLoader>(vm)) JSModuleLoader(vm, structure);
        object->finishCreation(globalObject, vm);
        return object;
    }

    static JSModuleLoader* create(JSGlobalObject* globalObject, VM& vm)
    {
        return create(globalObject, vm, vm.moduleLoaderStructure.get());
    }
    // An additional loader for globalObject (see moduleEnvironmentParentScope()).
    JS_EXPORT_PRIVATE static JSModuleLoader* createAdditional(JSGlobalObject*, VM&);

    DECLARE_INFO;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    // A global object has one module loader (JSGlobalObject::moduleLoader()); an
    // embedder may create more with create() to load and evaluate module graphs
    // again, separately, in the same global object: each loader has its own
    // registry, so the same specifiers give fresh module records with their own
    // environments and state. Records remember their loader; import() from module
    // code loads through the calling module's loader.
    //
    // Optional scope to place this loader's module environments under instead of
    // the global lexical environment (e.g. a JSLexicalEnvironment whose bindings
    // shadow some global names for this loader's modules).
    JSScope* moduleEnvironmentParentScope() const { return m_moduleEnvironmentParentScope.get(); }
    void setModuleEnvironmentParentScope(VM& vm, JSScope* scope) { m_moduleEnvironmentParentScope.setMayBeNull(vm, this, scope); }
    // For the embedder: whatever it associates with this loader.
    JSValue embedderData() const { return m_embedderData.get(); }
    void setEmbedderData(VM& vm, JSValue value) { m_embedderData.set(vm, this, value); }

    // APIs to control the module loader.
    void provideFetch(JSGlobalObject*, const Identifier& key, ScriptFetchParameters::Type, SourceCode&&);
    void provideFetch(JSGlobalObject*, const Identifier& key, ScriptFetchParameters::Type, JSSourceCode*);
    JSPromise* loadModule(JSGlobalObject*, const Identifier& moduleName, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>, OptionSet<ModuleLoadFlag>, int64_t referrerAsyncOrder = -1, const String& referrer = { });
    JSPromise* linkAndEvaluateModule(JSGlobalObject*, const Identifier& moduleKey, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>);
    JSPromise* requestImportModule(JSGlobalObject*, const Identifier& moduleName, const Identifier& referrer, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>, bool deferred = false, int64_t referrerAsyncOrder = -1);
#if USE(BUN_JSC_ADDITIONS)
    JS_EXPORT_PRIVATE int64_t asyncEvaluationOrderForKey(const Identifier& key);
#endif

    // Platform dependent hooked APIs.
    JSPromise* importModule(JSGlobalObject*, JSString* moduleName, JSValue parameters, const SourceOrigin& referrer, bool deferred = false);
    Identifier resolve(JSGlobalObject*, JSValue name, JSValue referrer, RefPtr<ScriptFetcher>, bool useImportMap);
    Identifier resolve(JSGlobalObject*, const Identifier& name, const Identifier& referrer, RefPtr<ScriptFetcher>, bool useImportMap);
    JSPromise* fetch(JSGlobalObject*, JSValue key, const String& referrer, RefPtr<ScriptFetchParameters>, RefPtr<ScriptFetcher>);
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

    JSPromise* hostLoadImportedModule(JSGlobalObject*, const ModuleReferrer&, const ModuleRequest&, JSCell* payload, RefPtr<ScriptFetcher>, bool useImportMap);
    JSPromise* loadModule(JSGlobalObject*, const ModuleReferrer&, const ModuleRequest&, JSCell* payload, RefPtr<ScriptFetcher>, OptionSet<ModuleLoadFlag>);
    void continueModuleLoading(JSGlobalObject*, ModuleGraphLoadingState*, ModuleCompletion result);
    void continueDynamicImport(JSGlobalObject*, ModuleLoaderPayload*, ModuleCompletion, RefPtr<ScriptFetcher>);
    JSPromise* loadRequestedModules(JSGlobalObject*, AbstractModuleRecord*, RefPtr<ScriptFetcher>);

    static JSPromise* makeModule(JSGlobalObject*, const Identifier& moduleKey, JSSourceCode*);

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
        // before falling back to a full scan for json/HostDefined variants.
        auto* impl = key.impl();
        if (auto entry = m_moduleMap.get({ impl, ScriptFetchParameters::Type::JavaScript }))
            return entry.get();
        if (!m_nonJavaScriptEntryCount) [[likely]]
            return nullptr; // a miss is common (require(esm), "is it registered yet?")
        using Type = ScriptFetchParameters::Type;
        static_assert(static_cast<unsigned>(Type::HostDefined) == 5, "every Type but JavaScript is listed below");
        for (Type type : { Type::HostDefined, Type::JSON, Type::Text, Type::WebAssembly, Type::None }) {
            if (auto entry = m_moduleMap.get({ impl, type }))
                return entry.get();
        }
        return nullptr;
    }
    const ModuleMap<WriteBarrier<ModuleRegistryEntry>>& moduleMap() const { return m_moduleMap; }
    bool removeEntry(const Identifier& key)
    {
        // Bun's registry is conceptually flat (one entry per specifier), so
        // delete every (specifier, type) variant — text/json/HostDefined etc.
        auto* impl = key.impl();
        Locker locker { cellLock() }; // visitChildren iterates these
        forgetPrelinkedRecordsWithKey(impl);
        m_loadedModules.removeIf([&](auto& entry) { return entry.key.first == impl; });
        m_resolutionFailures.removeIf([&](auto& entry) { return entry.key.first == impl || entry.key.second == impl; });
        return m_moduleMap.removeIf([&](auto& entry) {
            if (entry.key.first != impl)
                return false;
            didRemoveModuleMapEntry(entry.key.second);
            return true;
        });
    }
    void clearAll()
    {
        Locker locker { cellLock() };
        forgetPrelinkedRecordsWithKey(nullptr);
        m_loadedModules.clear();
        m_moduleMap.clear();
        m_nonJavaScriptEntryCount = 0;
        m_resolutionFailures.clear();
    }
    JS_EXPORT_PRIVATE JSPromise* loadModuleSync(JSGlobalObject*, const Identifier& moduleName, RefPtr<ScriptFetchParameters>&&, RefPtr<ScriptFetcher>&&);
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

    ModuleRegistryEntry* getRegisteredMayBeNull(const Identifier& key, ScriptFetchParameters::Type);

private:
    JSModuleLoader(VM&, Structure*);
    void finishCreation(JSGlobalObject*, VM&);

    void addResolutionFailure(VM&, const ResolutionMapKey&, JSValue error);
#if USE(BUN_JSC_ADDITIONS)
    void forgetPrelinkedRecordsWithKey(UniquedStringImpl* keyOrNullForAll);

    RefPtr<PrelinkedModuleGraph> m_prelinkedGraph;
    Vector<WriteBarrier<AbstractModuleRecord>> m_prelinkedRecords; // visited under cellLock()
    BitVector m_prelinkedRecordRemoved; // empty until the first removal
    unsigned m_nonJavaScriptEntryCount { 0 }; // m_moduleMap entries whose type is not JavaScript, for registryEntry()'s by-specifier fallback
#endif
    void didAddModuleMapEntry(ScriptFetchParameters::Type type)
    {
#if USE(BUN_JSC_ADDITIONS)
        m_nonJavaScriptEntryCount += type != ScriptFetchParameters::Type::JavaScript;
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

    WriteBarrier<JSScope> m_moduleEnvironmentParentScope;
    WriteBarrier<Unknown> m_embedderData;

    // Corresponds to RealmRecord.[[LoadedModules]].
    ModuleMap<AbstractModuleRecord::LoadedModuleRequest> m_loadedModules;

    ModuleMap<WriteBarrier<ModuleRegistryEntry>> m_moduleMap;

    ResolutionMap<WriteBarrier<Unknown>> m_resolutionFailures;
};

// Validates the host-defined payload threaded through HostLoadImportedModule / FinishLoadingImportedModule.
// Spec's `payload ∈ { GraphLoadingState Record, PromiseCapability Record }` is encoded in JSC as
// either ModuleGraphLoadingState* (graph load) or ModuleLoaderPayload* (top-level dynamic import).
inline bool isModuleLoaderHostDefinedPayload(JSCell* cell)
{
    return cell->inherits<ModuleGraphLoadingState>() || cell->inherits<ModuleLoaderPayload>();
}

} // namespace JSC
