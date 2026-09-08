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
#include <wtf/OrderedHashMap.h>
#include <wtf/OrderedHashSet.h>
#include <wtf/RefPtr.h>

namespace JSC {

class CyclicModuleRecord;
class JSLexicalEnvironment;
class JSModuleEnvironment;
class JSModuleNamespaceObject;
class JSMap;
class JSPromise;

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

        static ExportEntry NODELETE createLocal(const Identifier& exportName, const Identifier& localName);
        static ExportEntry NODELETE createIndirect(const Identifier& exportName, const Identifier& importName, const Identifier& moduleName, ScriptFetchParameters::Type moduleRequestType);
        static ExportEntry NODELETE createNamespace(const Identifier& exportName, const Identifier& moduleName, ScriptFetchParameters::Type moduleRequestType);

        Type type;
        ScriptFetchParameters::Type moduleRequestType { ScriptFetchParameters::Type::JavaScript };
        Identifier exportName;
        Identifier moduleName;
        Identifier importName;
        Identifier localName;
    };

    enum class ModulePhase : uint8_t { Evaluation, Defer };

    enum class ImportEntryType {
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
    ScriptFetchParameters::Type moduleType() const;
    const Vector<ModuleRequest>& requestedModules() const LIFETIME_BOUND { return m_requestedModules; }
    ModuleMap<LoadedModuleRequest>& loadedModules() LIFETIME_BOUND { return m_loadedModules; }
    const ModuleMap<LoadedModuleRequest>& loadedModules() const LIFETIME_BOUND { return m_loadedModules; }
#if USE(BUN_JSC_ADDITIONS)
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
    void setHasTLA(bool);

    static size_t estimatedSize(JSCell*, VM&);

    void dump();

    struct Resolution {
        enum class Type { Resolved, NotFound, Ambiguous, Error };

        static Resolution NODELETE notFound();
        static Resolution NODELETE error();
        static Resolution NODELETE ambiguous();

        bool isSameBinding(const Resolution& other) const { return moduleRecord == other.moduleRecord && localName == other.localName; }
        bool isEquivalentTo(const Resolution& other) const { return type == other.type && (type != Type::Resolved || isSameBinding(other)); }

        Type type;
        AbstractModuleRecord* moduleRecord;
        Identifier localName;
    };

    Resolution resolveExport(JSGlobalObject*, const Identifier& exportName);
    Resolution resolveImport(JSGlobalObject*, const Identifier& localName);
    // The same over the by-name entry maps only, never answered from a PrelinkedModuleGraph (a prelinked record builds its maps first).
    Resolution resolveExportByName(JSGlobalObject*, const Identifier& exportName);
    Resolution resolveImportByName(JSGlobalObject*, const Identifier& localName);

    // Options::useModuleEnvironmentResolveCache(). Mutator only, not visited: environments in it are this record's or ones m_loadedModules / m_prelinkedRequested retain.
    struct ScopeResolution {
        enum class Kind : uint8_t { Local, Import, Above };
        JSLexicalEnvironment* lexicalEnvironment { nullptr };
        RefPtr<UniquedStringImpl> importedName;
        uint32_t operand { 0 };
        Kind kind { Kind::Above };
        uint8_t skip { 0 };
    };
    using ScopeResolutions = UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, ScopeResolution, IdentifierRepHash>;
    ScopeResolutions& scopeResolutionsForGlobalLexicalBindingCount(size_t globalLexicalBindingCount) LIFETIME_BOUND
    {
        if (globalLexicalBindingCount != m_scopeResolutionsBindingCount) [[unlikely]] {
            m_scopeResolutions.clear();
            m_scopeResolutionsBindingCount = globalLexicalBindingCount;
        }
        return m_scopeResolutions;
    }

    AbstractModuleRecord* hostResolveImportedModule(JSGlobalObject*, const Identifier& moduleName, ScriptFetchParameters::Type moduleRequestType);
    void setImportedModule(JSGlobalObject*, const ModuleRequest&, AbstractModuleRecord*);

    JSModuleNamespaceObject* getModuleNamespace(JSGlobalObject*, ModulePhase = ModulePhase::Evaluation, bool shouldPreventExtensions = true);
#if USE(BUN_JSC_ADDITIONS)
    JSModuleNamespaceObject* getModuleNamespace(JSGlobalObject* globalObject, bool shouldPreventExtensions)
    {
        return getModuleNamespace(globalObject, ModulePhase::Evaluation, shouldPreventExtensions);
    }
#endif

    void gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, UncheckedKeyHashSet<AbstractModuleRecord*>& seen);
    bool readyForSyncExecution();
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

    // Options::usePrelinkedModuleInfo(): this record is module `prelinkedIndex()` of an embedder-resolved graph. Its
    // requests, import and export entries live in the graph; requested modules are wired by index
    // (setPrelinkedRequestedModule) instead of through [[LoadedModules]] by specifier; import bindings come from the
    // graph's resolutions. The by-name maps above are built from the graph the first time something needs them.
    bool isPrelinked() const { return !!m_prelinked; }
    PrelinkedModuleGraph* prelinkedGraph() const { return m_prelinked.get(); }
    uint32_t prelinkedIndex() const { return m_prelinkedIndex; }
    const PrelinkedModuleGraph::Module& prelinkedModule() const { return m_prelinked->module(m_prelinkedIndex); }
    // The record loaded for requestedModules()[i], if it was wired by index; null otherwise (then [[LoadedModules]] has it, or nothing does yet).
    AbstractModuleRecord* prelinkedRequestedModule(unsigned requestIndex) const;
    // Same, for a `request` of this record (an element of requestedModules(), or a copy of one).
    AbstractModuleRecord* prelinkedRequestedModule(const ModuleRequest&) const;
    // GetImportedModule: the wired record, else [[LoadedModules]], else null.
    AbstractModuleRecord* importedModuleForPrelinkedRequest(unsigned requestIndex) const;
    JS_EXPORT_PRIVATE void setPrelinkedRequestedModule(VM&, unsigned requestIndex, AbstractModuleRecord*);
    bool hasAllPrelinkedRequestedModules() const;
    bool hasAnyPrelinkedRequestedModule() const;
    bool prelinkedEntriesMaterialized() const { return m_prelinkedEntriesMaterialized; }
    JS_EXPORT_PRIVATE void materializePrelinkedEntries();
    // Right after createPrelinked, before any request is wired: turn this into an ordinary record (entry maps built, graph dropped).
    JS_EXPORT_PRIVATE void convertPrelinkedToEager();
    // The record a pre-resolved binding's module index names, or null if index-based resolution must not be used for it.
    AbstractModuleRecord* prelinkedRecordForResolution(JSGlobalObject*, uint32_t moduleIndex) const;
#endif

    void setModuleEnvironment(JSGlobalObject*, JSModuleEnvironment*);

protected:
    AbstractModuleRecord(VM&, Structure*, Identifier, SourceProviderSourceType);
    void finishCreation(JSGlobalObject*, VM&);
#if USE(BUN_JSC_ADDITIONS)
    // Before the record is visible to anyone: adopts the graph and fills requestedModules() from it.
    void initializePrelinked(VM&, Ref<PrelinkedModuleGraph>&&, uint32_t moduleIndex);
#endif

private:
    struct ResolveQuery;
    static Resolution resolveExportImpl(JSGlobalObject*, const ResolveQuery&);
    std::optional<Resolution> NODELETE tryGetCachedResolution(UniquedStringImpl* exportName);
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
    std::optional<Resolution> tryResolveExportPrelinked(JSGlobalObject*, const Identifier& exportName);
    std::optional<Resolution> tryResolveExportPrelinked(JSGlobalObject*, const PrelinkedModuleGraph::Export&);
    std::optional<Resolution> prelinkedResolution(JSGlobalObject*, PrelinkedModuleGraph::ResolutionKind, uint32_t resolvedModule, uint32_t resolvedLocalSid, uint32_t requestIndex, uint32_t importNameSid);
    bool collectPrelinkedNamespaceResolutions(JSGlobalObject*, Vector<std::pair<Identifier, Resolution>>&);
private:
#endif

    // The loader resolves the given module name to the module key. The module key is the unique value to represent this module.
    Identifier m_moduleKey;

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
    typedef UncheckedKeyHashMap<RefPtr<UniquedStringImpl>, Resolution, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>> Resolutions;
    Resolutions m_resolutionCache;

    ScopeResolutions m_scopeResolutions;
    size_t m_scopeResolutionsBindingCount { 0 };

protected:
    WriteBarrier<JSModuleEnvironment> m_moduleEnvironment;

    ModuleMap<LoadedModuleRequest> m_loadedModules;

    Vector<WriteBarrier<AbstractModuleRecord>> m_asyncParentModules;

    WriteBarrier<CyclicModuleRecord> m_cycleRoot;

    AsyncEvaluationOrder m_asyncEvaluationOrder { };

    WriteBarrier<JSPromise> m_topLevelCapability;

    std::optional<int> m_pendingAsyncDependencies;

    bool m_hasTLA { false };
    SourceProviderSourceType m_sourceType;
#if USE(BUN_JSC_ADDITIONS)
    bool m_prelinkedEntriesMaterialized { false };
    uint32_t m_prelinkedIndex { 0 };
    RefPtr<PrelinkedModuleGraph> m_prelinked;
    Vector<WriteBarrier<AbstractModuleRecord>> m_prelinkedRequested; // by request index; visited under cellLock()
    // By import index, filled per import on its first resolveImport (at/after Link, so a memoized target is already
    // reachable through m_prelinkedRequested / the loader's table, like m_resolutionCache); { Resolved, null } = unfilled.
    FixedVector<Resolution> m_prelinkedImportResolutions;
#endif
};

} // namespace JSC
