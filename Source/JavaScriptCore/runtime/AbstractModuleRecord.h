/*
 * Copyright (C) 2015-2021 Apple Inc. All rights reserved.
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

#include "Identifier.h"
#include "JSGenerator.h"
#include "JSInternalFieldObjectImpl.h"
#include "ModuleMap.h"
#if USE(BUN_JSC_ADDITIONS)
#include "PrelinkedModuleGraph.h"
#endif
#include "ScriptFetchParameters.h"
#include "ScriptFetcher.h"
#include <wtf/FixedVector.h>
#include <wtf/Markable.h>
#include <wtf/OrderedHashMap.h>
#include <wtf/OrderedHashSet.h>
#include <wtf/RefPtr.h>

namespace JSC {

class CyclicModuleRecord;
class JSModuleEnvironment;
class JSModuleLoader;
class JSModuleNamespaceObject;
class JSMap;
class JSPromise;
class SyntheticModuleRecord;
#if ENABLE(AOT)
namespace AOT {
class ModuleRecord;
}
#endif

enum class SourceProviderSourceType : uint8_t;

// Based on the Source Text Module Record
// http://www.ecma-international.org/ecma-262/6.0/#sec-source-text-module-records
class AbstractModuleRecord : public JSInternalFieldObjectImpl<2> {
    friend class LLIntOffsetsExtractor;
public:
    using Base = JSInternalFieldObjectImpl<2>;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    template<typename CellType, SubspaceAccess>
    static void subspaceFor(VM&)
    {
        RELEASE_ASSERT_NOT_REACHED();
    }

    using Argument = JSGenerator::Argument;
    using State = JSGenerator::State;
    using ResumeMode = JSGenerator::ResumeMode;

    enum class Field : uint32_t {
        State,
        Frame,
    };

    static_assert(numberOfInternalFields == 2);
    static std::array<JSValue, numberOfInternalFields> initialValues()
    {
        return { {
            jsNumber(static_cast<int32_t>(State::Init)),
            jsUndefined(),
        } };
    }

    // https://tc39.github.io/ecma262/#sec-source-text-module-records
    struct ExportEntry {
        enum class Type {
            Local,
            Indirect,
            Namespace,
        };

        NODELETE static ExportEntry createLocal(const Identifier& exportName, const Identifier& localName);
        NODELETE static ExportEntry createIndirect(const Identifier& exportName, const Identifier& importName, const Identifier& moduleName, ScriptFetchParameters::Type moduleRequestType);
        NODELETE static ExportEntry createNamespace(const Identifier& exportName, const Identifier& moduleName, ScriptFetchParameters::Type moduleRequestType);

        Type type;
        ScriptFetchParameters::Type moduleRequestType { ScriptFetchParameters::Type::JavaScript };
        Identifier exportName;
        Identifier moduleName;
        Identifier importName;
        Identifier localName;
    };

    enum class ModulePhase : uint8_t { Evaluation, Defer };

    enum class ImportEntryType : uint8_t {
        Single,
#if USE(BUN_JSC_ADDITIONS)
        // If the corresponding export is not found, do not emit an error.
        SingleTypeScript,
#endif
        Namespace,
    };
    struct ImportEntry {
        ImportEntryType type;
        ModulePhase phase { ModulePhase::Evaluation };
        ScriptFetchParameters::Type moduleRequestType { ScriptFetchParameters::Type::JavaScript };
        Identifier moduleRequest;
        Identifier importName;
        Identifier localName;
    };

    using StarExportEntry = std::pair<RefPtr<UniquedStringImpl>, ScriptFetchParameters::Type>;

    struct StarExportEntryHash {
        static unsigned hash(const StarExportEntry& entry)
        {
            unsigned identifierHash = entry.first ? entry.first->existingSymbolAwareHash() : 0;
            unsigned enumHash(entry.second);
            return WTF::pairIntHash(identifierHash, enumHash);
        }

        static bool equal(const StarExportEntry& a, const StarExportEntry& b)
        {
            return a == b;
        }

        static constexpr bool safeToCompareToEmptyOrDeleted = false;
    };
    using StarExportEntries = OrderedHashSet<StarExportEntry, StarExportEntryHash>;

    using ImportEntries = OrderedHashMap<RefPtr<UniquedStringImpl>, ImportEntry, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>>;
    using ExportEntries = OrderedHashMap<RefPtr<UniquedStringImpl>, ExportEntry, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>>;

    struct ModuleRequest {
        Identifier m_specifier;
        RefPtr<ScriptFetchParameters> m_attributes;
        ModulePhase m_phase { ModulePhase::Evaluation };

        ScriptFetchParameters::Type type(ScriptFetchParameters::Type fallback = ScriptFetchParameters::Type::JavaScript) const;
        bool operator==(const ModuleRequest&) const;
    };

    struct LoadedModuleRequest : ModuleRequest {
        LoadedModuleRequest() = default;
        LoadedModuleRequest(VM&, ModuleRequest, AbstractModuleRecord* loadedModule, JSCell* owner);
#if USE(BUN_JSC_ADDITIONS)
        // An ImportOrderWalk has come to this request.
        bool m_wasReached { false };
#endif
        WriteBarrier<AbstractModuleRecord> m_module;
    };

    DECLARE_EXPORT_INFO;

    void appendRequestedModule(const Identifier&, RefPtr<ScriptFetchParameters>&&, ModulePhase = ModulePhase::Evaluation);
#if USE(BUN_JSC_ADDITIONS)
    // For records built from a serialized description whose sizes are known up front.
    void reserveCapacity(unsigned requestedModules, unsigned importEntries, unsigned exportEntries)
    {
        m_requestedModules.reserveInitialCapacity(requestedModules);
        m_importEntries.reserveInitialCapacity(importEntries);
        m_exportEntries.reserveInitialCapacity(exportEntries);
    }
#endif
    void addStarExportEntry(const Identifier&, ScriptFetchParameters::Type);
    void addImportEntry(const ImportEntry&);
    void addExportEntry(const ExportEntry&);

    std::optional<ImportEntry> tryGetImportEntry(UniquedStringImpl* localName);
    std::optional<ExportEntry> tryGetExportEntry(UniquedStringImpl* exportName);

    class AsyncEvaluationOrder {
    public:
        AsyncEvaluationOrder() = default;
        AsyncEvaluationOrder(int64_t order);

        bool isDone() const { return m_order == Done; }
        bool isUnset() const { return m_order == Unset; }
        bool hasOrder() const { return m_order >= 0; }
        void setDone() { m_order = Done; }

        int64_t order() const;
        AsyncEvaluationOrder& order(int64_t);

        static AsyncEvaluationOrder done() { return { Done }; }

    private:
        static constexpr int64_t Unset = -2;
        static constexpr int64_t Done = -1;
        int64_t m_order { Unset };
    };

    const Identifier& moduleKey() const { return m_moduleKey; }
    JSModuleLoader* moduleLoader() const { return m_moduleLoader.get(); }
    ScriptFetchParameters::Type moduleType() const;
    const Vector<ModuleRequest>& requestedModules() const LIFETIME_BOUND { return m_requestedModules; }
    ModuleMap<LoadedModuleRequest>& loadedModules() LIFETIME_BOUND { return m_loadedModules; }
    const ModuleMap<LoadedModuleRequest>& loadedModules() const LIFETIME_BOUND { return m_loadedModules; }
#if USE(BUN_JSC_ADDITIONS)
    // The key the host resolved each static import to, until that import has loaded. See
    // JSModuleLoader::hostLoadImportedModule(). The map's keys point into requestedModules(), which keeps them alive.
    struct ResolvedRequest {
        enum class State : uint8_t {
            Loading,
            Postponed, // See JSModuleLoader::mayPostponeFetch().
            Failed,
            Obsolete, // See ImportOrderWalk.
        };
        RefPtr<UniquedStringImpl> key;
        State state { State::Loading };
        bool wasLeftForLater { false }; // By an ImportOrderWalk, which has gone on.
        uint32_t number { 0 }; // Tells the loads that were started for it from those of an earlier request for the same key.
        // The value of VM::ImportOrderState::clock when the host last failed to supply it, which the registry need not remember.
        uint64_t failedAt { 0 };
    };
    ModuleMap<ResolvedRequest>& resolvedRequests() LIFETIME_BOUND { return m_resolvedRequests; }
    void removeResolvedRequest(const ModuleRequest& request) { m_resolvedRequests.remove(ModuleMapKey { request.m_specifier.impl(), request.type() }); }
    void setStateOfResolvedRequest(const ModuleRequest&, ResolvedRequest::State);
    void didFailForRequest(const ModuleRequest&);
    bool hasRequestBeingLoaded() const;
    void forEachKeyBeingLoaded(NOESCAPE const auto& function) const
    {
        for (const ResolvedRequest& resolved : m_resolvedRequests.values()) {
            if (resolved.state != ResolvedRequest::State::Failed)
                function(resolved.key.get());
        }
    }
    // A prelinked record keeps these in its PrelinkedModuleGraph and only builds the maps when asked for them here.
    const ExportEntries& exportEntries() const LIFETIME_BOUND { ensurePrelinkedEntriesMaterialized(); return m_exportEntries; }
    const ImportEntries& importEntries() const LIFETIME_BOUND { ensurePrelinkedEntriesMaterialized(); return m_importEntries; }
    const StarExportEntries& starExportEntries() const LIFETIME_BOUND { ensurePrelinkedEntriesMaterialized(); return m_starExportEntries; }
#else
    const ExportEntries& exportEntries() const LIFETIME_BOUND { return m_exportEntries; }
    const ImportEntries& importEntries() const LIFETIME_BOUND { return m_importEntries; }
    const StarExportEntries& starExportEntries() const LIFETIME_BOUND { return m_starExportEntries; }
#endif
    const Vector<WriteBarrier<AbstractModuleRecord>>& asyncParentModules() const LIFETIME_BOUND { return m_asyncParentModules; }
    CyclicModuleRecord* cycleRoot() const { return m_cycleRoot.get(); }
    AsyncEvaluationOrder asyncEvaluationOrder() const { return m_asyncEvaluationOrder; }
    std::optional<int> pendingAsyncDependencies() const { return m_pendingAsyncDependencies; }
    bool hasTLA() const { return m_hasTLA; }

    JSPromise* topLevelCapability() const { return m_topLevelCapability.get(); }
    void setCycleRoot(VM&, CyclicModuleRecord*);
    void setAsyncEvaluationOrder(AsyncEvaluationOrder newOrder) { m_asyncEvaluationOrder = newOrder; }
    void setPendingAsyncDependencies(std::optional<int> newDependencies) { m_pendingAsyncDependencies = newDependencies; }

    void appendAsyncParentModule(VM&, AbstractModuleRecord*);
    void setTopLevelCapability(VM&, JSPromise*);
    void clearTopLevelCapability() { m_topLevelCapability.clear(); }
    void setHasTLA(bool);

    static size_t estimatedSize(JSCell*, VM&);

    void dump();

    struct Resolution {
        enum class Type : uint8_t { Resolved, NotFound, Ambiguous, Error };

#if ENABLE(AOT)
        Resolution() = default;
        Resolution(Type type, AbstractModuleRecord* moduleRecord, Identifier localName, Markable<unsigned> compiledExportIndex = std::nullopt)
            : type(type)
            , compiledExportIndex(compiledExportIndex)
            , moduleRecord(moduleRecord)
            , localName(WTF::move(localName))
        {
        }
#endif

        NODELETE static Resolution notFound();
        NODELETE static Resolution error();
        NODELETE static Resolution ambiguous();

        JS_EXPORT_PRIVATE bool isSameBinding(const Resolution& other) const;
        bool isEquivalentTo(const Resolution& other) const { return type == other.type && (type != Type::Resolved || isSameBinding(other)); }
#if USE(BUN_JSC_ADDITIONS)
        // True if this binding has to be read with moduleRecord->getLiveExport(localName) instead of from the module environment.
        // `export * as ns` from a live record doesn't count: it resolves to the namespace object, which is an ordinary binding.
        bool isLiveExport(VM&) const;
        // `export * as ns` from a compiled original, which has no module environment. It is read with moduleRecord->getModuleNamespace().
        bool isNamespaceOfCompiledOriginal(VM&) const;
#endif

        Type type;
#if ENABLE(AOT)
        // In the padding before the pointer.
        bool isFromRemovedMock { false };
        bool cameFromStarExportEntry { false };
        Markable<unsigned> compiledExportIndex;
#endif
        AbstractModuleRecord* moduleRecord;
        Identifier localName;
    };

    Resolution resolveExport(JSGlobalObject*, const Identifier& exportName);
#if ENABLE(AOT)
    static_assert(sizeof(Resolution) == 3 * sizeof(void*));
    Resolution resolveExportViaReexporter(JSGlobalObject*, const Identifier& exportName, AOT::ModuleRecord* reexporter);
#endif
    // requestedModule is set to the module the import names. For a re-export, that isn't the resolution's moduleRecord.
    Resolution resolveImport(JSGlobalObject*, const Identifier& localName, AbstractModuleRecord** requestedModule = nullptr);
    // The same over the by-name entry maps only, never answered from a PrelinkedModuleGraph (a prelinked record builds its maps first).
    Resolution resolveExportByName(JSGlobalObject*, const Identifier& exportName);
    Resolution resolveImportByName(JSGlobalObject*, const Identifier& localName, AbstractModuleRecord** requestedModule = nullptr);

    AbstractModuleRecord* hostResolveImportedModule(JSGlobalObject*, const Identifier& moduleName, ScriptFetchParameters::Type moduleRequestType);
    void setImportedModule(JSGlobalObject*, const ModuleRequest&, AbstractModuleRecord*);

    JSModuleNamespaceObject* getModuleNamespace(JSGlobalObject*, ModulePhase = ModulePhase::Evaluation, bool shouldPreventExtensions = true);
    JSModuleNamespaceObject* createModuleNamespace(JSGlobalObject*, ModulePhase, bool shouldPreventExtensions, Vector<SyntheticModuleRecord*>& modulesYetToFinish);
#if USE(BUN_JSC_ADDITIONS)
    JSModuleNamespaceObject* getModuleNamespace(JSGlobalObject* globalObject, bool shouldPreventExtensions)
    {
        return getModuleNamespace(globalObject, ModulePhase::Evaluation, shouldPreventExtensions);
    }
#endif
    JS_EXPORT_PRIVATE JSObject* moduleNamespaceObject(JSGlobalObject*, ModulePhase = ModulePhase::Evaluation);

    void gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, HashSet<AbstractModuleRecord*>& seen);
    bool readyForSyncExecution();
    bool readyForSyncExecution(HashSet<AbstractModuleRecord*>& seen);
    void evaluateSync(JSGlobalObject*);

    JSPromise* asyncCapability() const;
    void asyncCapability(VM&, JSPromise*);
    
    JSModuleEnvironment* moduleEnvironment()
    {
        ASSERT(m_moduleEnvironment);
        return m_moduleEnvironment.get();
    }

    JSModuleEnvironment* moduleEnvironmentMayBeNull()
    {
        return m_moduleEnvironment.get();
    }

    void link(JSGlobalObject*, RefPtr<ScriptFetcher> = nullptr);
#if USE(BUN_JSC_ADDITIONS)
    // link() for a caller that has JSModuleLoader::runDeferredModules() behind it.
    void linkAfterDeferredModules(JSGlobalObject*, RefPtr<ScriptFetcher>);
    enum class IsBehindWalk : bool { No, Yes };
    void generateDeferredSyntheticModules(JSGlobalObject*, HashSet<AbstractModuleRecord*>& visited, IsBehindWalk = IsBehindWalk::No);
    static bool importPromiseGatesAsyncDependency(JSPromise* importPromise, CyclicModuleRecord* dependency);
#endif
    JS_EXPORT_PRIVATE JSValue evaluate(JSGlobalObject*, JSValue sentValue, JSValue resumeMode);
    WriteBarrier<Unknown>& internalField(Field field) { return Base::internalField(static_cast<uint32_t>(field)); }
    WriteBarrier<Unknown> internalField(Field field) const { return Base::internalField(static_cast<uint32_t>(field)); }

    void evaluateModuleSync(JSGlobalObject*);
#if USE(BUN_JSC_ADDITIONS)
    unsigned innerModuleEvaluation(JSGlobalObject*, Vector<AbstractModuleRecord*, 8>& stack, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise);
#else
    unsigned innerModuleEvaluation(JSGlobalObject*, Vector<AbstractModuleRecord*, 8>& stack, unsigned index);
#endif
    unsigned innerModuleLinking(JSGlobalObject*, Vector<CyclicModuleRecord*, 8>& stack, unsigned index, RefPtr<ScriptFetcher>);

    DECLARE_VISIT_CHILDREN;

#if USE(BUN_JSC_ADDITIONS)
    JSPromise* evaluate(JSGlobalObject*, int64_t referrerAsyncOrder = -1, JSPromise* dynamicImportPromise = nullptr);
#else
    JSPromise* evaluate(JSGlobalObject*);
#endif

#if USE(BUN_JSC_ADDITIONS)
    bool m_isTypeScript = false;

private:
    bool m_wasReached : 1 { false };
    bool m_hasStartedLinking : 1 { false };
    bool m_didLoadAfterFailure : 1 { false };
    bool m_isSupersededForEarlierImports : 1 { false };
    bool m_isValue : 1 { false };
    bool m_isOtherFormOfFile : 1 { false };
    uint64_t m_supersededAt { 0 };

public:
    // What an ImportOrderWalk keeps on the record. The times are values of VM::ImportOrderState::clock.
    bool wasReached() const { return m_wasReached; }
    void setWasReached(bool wasReached = true) { m_wasReached = wasReached; }
    // From then on it keeps what it imports: resolutions and environments may point there.
    bool hasStartedLinking() const { return m_hasStartedLinking; }
    void setHasStartedLinking() { m_hasStartedLinking = true; }
    bool didLoadAfterFailure() const { return m_didLoadAfterFailure; }
    void setDidLoadAfterFailure() { m_didLoadAfterFailure = true; }
    // When the record was created or last asked for what it imports.
    uint64_t loadedAt() const { return m_loadedAt; }
    void setLoadedAt(uint64_t time) { m_loadedAt = time; }
    // The modules that have yet to be linked ask for this one's key again.
    enum class SupersededFor : bool { LaterImports, AllImports };
    void supersede(uint64_t time, SupersededFor imports)
    {
        m_supersededAt = time;
        m_isSupersededForEarlierImports |= imports == SupersededFor::AllImports;
    }
    uint64_t supersededAt() const { return m_supersededAt; }
    bool isSupersededForEarlierImports() const { return m_isSupersededForEarlierImports; }

    // A file of data. require() returns its default export.
    bool isValue() const { return m_isValue; }
    void setIsValue(bool isValue) { m_isValue = isValue; }
    // An import attribute made something else of the file than its name does.
    bool isOtherFormOfFile() const { return m_isOtherFormOfFile; }
    void setIsOtherFormOfFile(bool isOtherFormOfFile) { m_isOtherFormOfFile = isOtherFormOfFile; }

    // Live exports, used for module mocks. Exports of a live record are never cached: every read, through an import binding or a
    // namespace object, does liveExportsSource()->get(exportName).
    // - Any export name resolves to (record, exportName), declared or not. A query that arrives through `export *` only matches
    //   names the record declares or gets from its own `export *`, or that no other module exports. See resolveExportImpl().
    // - A live export never makes a name ambiguous. The first `export *` that provides the name wins.
    // - Imports of live exports get ResolveType Dynamic and never read the exporter's slot. See ModuleProgramExecutable::liveImports().
    // Must be set before anything links against the record. It can't be unset.
    bool hasLiveExports() const { return m_hasLiveExports; }
    void setHasLiveExports();
    // The original of a module mock whose body is AOT-compiled code. Its exports resolve as those of any module, and reads of them go to liveExportsSource(), which has their values.
    bool isCompiledOriginal() const { return m_isCompiledOriginal; }
    void setIsCompiledOriginal(VM&, JSObject* source);
    bool hasExportsSource() const { return m_hasLiveExports || m_isCompiledOriginal; }
    const Identifier& bindingName(const ExportEntry& entry) const { return m_isCompiledOriginal ? entry.exportName : entry.localName; }
    // Null until the embedder sets it. Reading an export before that throws a TDZ error.
    JSObject* liveExportsSource() const { return m_liveExportsSource.get(); }
    JS_EXPORT_PRIVATE void setLiveExportsSource(VM&, JSObject*);
    // All three can run arbitrary JS and throw.
    JSValue getLiveExport(JSGlobalObject*, PropertyName exportName);
    bool hasLiveExport(JSGlobalObject*, PropertyName exportName);
    // For a read through another module's `export { x } from` or `export *`. A name the source doesn't have is undefined, as in
    // Vitest. The source's [[Get]] can then throw for such a name, and only an import from the live record itself sees that.
    JSValue getReexportedLiveExport(JSGlobalObject*, PropertyName exportName);

    // Options::usePrelinkedModuleInfo(): this record is module `prelinkedIndex()` of an embedder-resolved graph. Its
    // requests, import and export entries live in the graph; a request that names a graph module is answered by the
    // loader's index table, any other request by [[LoadedModules]] as usual (the embedder calls setImportedModule);
    // import bindings come from the graph's resolutions. The by-name maps above are built from the graph the first time
    // something needs them.
    bool isPrelinked() const { return !!m_prelinked; }
    bool importEntriesArePrelinked() const { return m_prelinked && !m_prelinkedEntriesMaterialized; }
    PrelinkedModuleGraph* prelinkedGraph() const { return m_prelinked.get(); }
    uint32_t prelinkedIndex() const { return m_prelinkedIndex; }
    const PrelinkedModuleGraph::Module& prelinkedModule() const { return m_prelinked->module(m_prelinkedIndex); }
    // GetImportedModule for requestedModules()[i]: the loader's record for the graph module the request names, else
    // [[LoadedModules]], else null (not loaded yet).
    JS_EXPORT_PRIVATE AbstractModuleRecord* prelinkedRequestedModule(unsigned requestIndex) const;
    bool requestsPrelinkedModule(unsigned requestIndex) const { return m_prelinked && m_prelinked->requests(prelinkedModule())[requestIndex].moduleIndex != PrelinkedModuleGraph::noModule; }
    // Same, for a `request` of this record (an element of requestedModules(), or a copy of one).
    AbstractModuleRecord* prelinkedRequestedModule(const ModuleRequest&) const;
    JS_EXPORT_PRIVATE bool hasAllPrelinkedRequestedModules() const;
    bool prelinkedEntriesMaterialized() const { return m_prelinkedEntriesMaterialized; }
    JS_EXPORT_PRIVATE void materializePrelinkedEntries();
    // Right after createPrelinked, before any request is wired: turn this into an ordinary record (entry maps built, graph dropped).
    JS_EXPORT_PRIVATE void convertPrelinkedToEager();
    // The record a pre-resolved binding's module index names, or null if index-based resolution must not be used for it.
    AbstractModuleRecord* prelinkedRecordForResolution(JSGlobalObject*, uint32_t moduleIndex) const;
#else
    const Identifier& bindingName(const ExportEntry& entry) const { return entry.localName; }
#endif

    void setModuleEnvironment(JSGlobalObject*, JSModuleEnvironment*);

protected:
    AbstractModuleRecord(VM&, Structure*, JSModuleLoader*, Identifier, SourceProviderSourceType);
    void finishCreation(JSGlobalObject*, VM&);
#if USE(BUN_JSC_ADDITIONS)
    // Before the record is visible to anyone: adopts the graph and fills requestedModules() from it.
    void initializePrelinked(VM&, Ref<PrelinkedModuleGraph>&&, uint32_t moduleIndex);
    // Before anything has resolved an export of the record.
    void clearExports();
#endif
#if ENABLE(AOT)
    void forgetCachedResolutions() { m_resolutionCache.clear(); }
    void recordLinkedImport(UniquedStringImpl* localName, const Resolution&);
#endif

private:
    struct ResolveQuery;
#if ENABLE(AOT)
    static Resolution resolveExportImpl(JSGlobalObject*, const ResolveQuery&, AOT::ModuleRecord* reexporter = nullptr);
#else
    static Resolution resolveExportImpl(JSGlobalObject*, const ResolveQuery&);
#endif
    NODELETE std::optional<Resolution> tryGetCachedResolution(UniquedStringImpl* exportName);
    void cacheResolution(UniquedStringImpl* exportName, const Resolution&);
#if USE(BUN_JSC_ADDITIONS)
    void ensurePrelinkedEntriesMaterialized() const
    {
        if (m_prelinked && !m_prelinkedEntriesMaterialized) [[unlikely]]
            const_cast<AbstractModuleRecord*>(this)->materializePrelinkedEntries();
    }
protected:
    // nullopt: not answerable from the graph (take the by-name path).
    std::optional<Resolution> tryResolveImportPrelinked(JSGlobalObject*, const Identifier& localName);
    std::optional<Resolution> tryResolveImportPrelinked(JSGlobalObject*, const PrelinkedModuleGraph::Import&);
    std::optional<Resolution> tryResolveExportPrelinked(JSGlobalObject*, const Identifier& exportName);
    std::optional<Resolution> tryResolveExportPrelinked(JSGlobalObject*, const PrelinkedModuleGraph::Export&);
    std::optional<Resolution> prelinkedResolution(JSGlobalObject*, PrelinkedModuleGraph::ResolutionKind, uint32_t resolvedModule, uint32_t resolvedLocalSid, uint32_t requestIndex, uint32_t importNameSid);
    bool collectPrelinkedNamespaceResolutions(JSGlobalObject*, Vector<std::pair<Identifier, Resolution>>&);
private:
#endif

    // The loader resolves the given module name to the module key. The module key is the unique value to represent this module.
    Identifier m_moduleKey;
    WriteBarrier<JSModuleLoader> m_moduleLoader;

    // Map localName -> ImportEntry.
    ImportEntries m_importEntries;

    // Map exportName -> ExportEntry.
    ExportEntries m_exportEntries;

    // Save the occurrence order since resolveExport requires it.
    StarExportEntries m_starExportEntries;

    // Save the occurrence order since the module loader loads and runs the modules in this order.
    // http://www.ecma-international.org/ecma-262/6.0/#sec-moduleevaluation
    Vector<ModuleRequest> m_requestedModules;

    WriteBarrier<JSModuleNamespaceObject> m_moduleNamespaceObject;
    WriteBarrier<JSModuleNamespaceObject> m_deferredNamespaceObject;

    WriteBarrier<JSPromise> m_asyncCapability;

    // We assume that all the AbstractModuleRecord are retained by JSModuleLoader's registry.
    // So here, we don't visit each object for GC. The resolution cache map caches the once
    // looked up correctly resolved resolution, since (1) we rarely looked up the non-resolved one,
    // and (2) if we cache all the attempts the size of the map becomes infinitely large.
    typedef HashMap<RefPtr<UniquedStringImpl>, Resolution, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>> Resolutions;
    Resolutions m_resolutionCache;
#if ENABLE(AOT)
    Resolutions m_linkedImports;
#endif

protected:
    WriteBarrier<JSModuleEnvironment> m_moduleEnvironment;

    ModuleMap<LoadedModuleRequest> m_loadedModules;
#if USE(BUN_JSC_ADDITIONS)
    ModuleMap<ResolvedRequest> m_resolvedRequests;
#endif

    Vector<WriteBarrier<AbstractModuleRecord>> m_asyncParentModules;

    WriteBarrier<CyclicModuleRecord> m_cycleRoot;

    AsyncEvaluationOrder m_asyncEvaluationOrder { };

    WriteBarrier<JSPromise> m_topLevelCapability;

    std::optional<int> m_pendingAsyncDependencies;

    bool m_hasTLA { false };
    SourceProviderSourceType m_sourceType;
#if USE(BUN_JSC_ADDITIONS)
    bool m_prelinkedEntriesMaterialized { false };
    bool m_hasLiveExports { false };
    bool m_isCompiledOriginal { false };
    WriteBarrier<JSObject> m_liveExportsSource;
    uint32_t m_prelinkedIndex { 0 };
    uint64_t m_loadedAt;
    RefPtr<PrelinkedModuleGraph> m_prelinked;
    // By import index, filled per import on its first resolveImport (at/after Link, so a memoized target is already
    // reachable through the loader's table or, once that slot is forgotten, [[LoadedModules]] -- JSModuleLoader::pinPrelinkedEdges);
    // { Resolved, null } = unfilled.
    FixedVector<Resolution> m_prelinkedImportResolutions;
#endif
};

#if ENABLE(AOT)
static_assert(sizeof(AbstractModuleRecord::Resolution) == 3 * sizeof(void*));
#endif

} // namespace JSC
