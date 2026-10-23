/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTModuleHost.h"
#include "AOTModuleRecord.h"
#include "AOTModuleTable.h"
#include "AOTModuleWords.h"
#include "AOTVector.h"
#include "ArgList.h"
#include "Identifier.h"
#include "JSPromise.h"
#include "RememberedSlot.h"
#include "ScriptFetcher.h"
#include "SourceOrigin.h"
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSCellButterfly;
class JSModuleLoader;
class JSObject;
class JSString;
class ScriptFetchParameters;
class ThrowScope;

namespace AOT {

class ModuleLinkWalk;
enum class LoaderChange : uint8_t;
class StarExportResolver;
struct EvaluationFrame;
struct EvaluationInProgress;
struct Instance;
struct State;

enum class ModuleStatus : uint8_t {
    Unlinked,
    Linking,
    Linked,
    Evaluating,
    EvaluatingAsync,
    Evaluated,
    Failed,
};

enum class SynchronousEvaluationReason : uint8_t { Require, DeferredNamespace };

struct LinkContext {
    ModuleLinkingStack& stack;
    unsigned index { 0 };
    RefPtr<ScriptFetcher> scriptFetcher { };
};

struct EvaluationContext {
    ModuleEvaluationStack& stack;
    unsigned index { 0 };
    std::optional<int64_t> referrerAsyncOrder { };
    JSPromise* importPromise { nullptr };
};

class Modules {
    WTF_MAKE_NONCOPYABLE(Modules);
    WTF_MAKE_TZONE_ALLOCATED(Modules);
public:
    using ModulePhase = AbstractModuleRecord::ModulePhase;
    using EvaluationFrames = Vector<EvaluationFrame, 16>;

    explicit Modules(State&);
    ~Modules();

    Instance& instance() { return m_instance; }
    const ModuleTable& table() const { return m_table; }
    const ModuleData& data(ModuleID module) const { return m_table.data(module); }
    const ModuleHost& host() const;
    Identifier identifier(uint32_t number) const;
    ModuleStatus status(ModuleID) const;
    ModuleRecord* recordIfExists(ModuleID) const;
    ModuleRecord* ensureRecord(ModuleID);
    JSValue value(ModuleID) const;
    JSValue readExport(ModuleID, unsigned index);
    JSValue readExportIfInitialized(ModuleID, unsigned index) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    JSObject* capturedExports(ModuleID) const;
    // Empty if the module has no export of the name. Reads a deferred export now and keeps the value.
    JSValue capturedValue(ModuleID, const Identifier&);
    bool hasCapturedExport(ModuleID, const Identifier&) const;
    JSObject* moduleNamespace(ModuleID, ModulePhase);
    JSObject* importMeta(ModuleID);
    JSCell* builtinCell(BuiltinID, ModuleID);
    JSString* pathString(ModuleID);
    JSString* resolve(ModuleID, ResolvedSpecifierForm);
    const SourceOrigin& sourceOrigin(ModuleID);
    void throwFailure(ModuleFailureID);
    void throwExportNotFound(ModuleID exporter, uint32_t nameIdentifier);
    void throwExportAmbiguous(ModuleID exporter, uint32_t nameIdentifier);
    void didRequestFromHost(ModuleID);
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    bool refusesCodeOutsideImage() const { return !m_admitsCodeOutsideImage; }
    bool refusesLoadsByName() const { return !m_loadsModulesByName && !m_isAskingHost; }
    bool isAskingHost() const { return m_isAskingHost; }
    // A name that the build did not follow finds an entry point and no other file of the image, as in an executable that `bun build --compile` makes without --aot.
    bool refusesRequestFromHost(ModuleID module) const { return !m_admitsInspector && !m_isAskingHost && !data(module).isEntryPoint() && data(module).kind != ModuleKind::Data && m_table.hasFilePath(module) && !data(module).isLibraryCodeWithFilePath(); }
    void admitCodeNamedBy(JSValue specifier);
    void abandonEvaluation(ModuleID, JSValue error);
    template<typename Visitor> void visitRoots(Visitor&);

    bool loadRuntimeModules(ModuleID root);
    void link(ModuleID root);
    void captureSyntheticExports(ModuleID root, VisitedModuleRecords&);
    void finishLink(ModuleID root, LinkContext&);

    JSPromise* evaluate(ModuleID root, std::optional<int64_t> referrerAsyncOrder, JSPromise* importPromise, const ModuleLinkWalk* = nullptr);
    void evaluateSynchronously(ModuleID, SynchronousEvaluationReason);
    void innerModuleEvaluation(ModuleID root, EvaluationContext&);
    void gatherAsynchronousTransitiveDependencies(ModuleID, OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen);
    bool readyForSyncExecution(ModuleID, VisitedModuleRecords& seen);

    JSValue callBody(ModuleID);
    // For a body that stores no status of its own. The modules that compiled code begins to evaluate below it fail with it.
    JSValue callBodyOfModuleEnteredByLoader(ModuleID);
    bool beginEvaluationOfModuleInRecord(ModuleID);
    JSValue execute(ModuleID);

    JSValue require(ModuleID);
    void evaluateCommonJS(ModuleID);
    // What `module.exports` holds. Undefined for a module whose exports are variables and nothing else: no code asks for their object.
    JSValue commonJSExports(ModuleID) const;
    // What the loader takes for it. Whoever holds the module object may have defined another property in the place of `exports`, and compiled code reads the exports of such a module off that object.
    JSValue exportsSeenByLoader(ModuleID);
    void setCommonJSExports(ModuleID, JSValue);
    void recordRequire(ModuleID, ModuleID referrer);
    JSObject* moduleObject(ModuleID);
    JSObject* mainModuleObject();
    JSObject* parentModuleObject(ModuleID);
    bool forEachNewChild(ModuleID, const ScopedLambda<void(JSObject*)>&);
    JSValue requireFunction(ModuleID);
    JSValue requireDynamic(JSValue specifier, ModuleID referrer);
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);
    void willSetRequireCacheEntry(ModuleID, JSValue entry);
    void willChangeRequireOverride(RequireOverride, bool isOverridden);

    JSPromise* import(ModuleID, std::optional<ModuleID> referrer);
    JSPromise* importFromHost(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void didSettleImport(JSPromise* importPromise, ModuleID, JSValue resolution, JSPromise::Status);

    ModuleRecord* exposeRecord(ModuleID);
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    JSObject* uncompiledNamespace(ModuleID, ModulePhase);

private:
    friend class RequestToHostScope;

    // What the collector finds through a loaded module. A store is recorded: the roots are visited at every stop of the mutator, and the loaded modules only after a store.
    template<typename T>
    class Reference {
        WTF_MAKE_NONCOPYABLE(Reference);
    public:
        Reference() = default;

        operator T() const { return m_value; }
        explicit operator bool() const { return !!m_value; }
        T operator->() const { return m_value; }
        T get() const { return m_value; }

        void set(Modules& modules, T value)
        {
            m_value = value;
            modules.m_storedReferences.recordAll();
        }

    private:
        T m_value { };
    };

    struct LoadedModule {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadedModule);

        Reference<JSObject*>& namespaceObjectFor(ModulePhase);

        // Of a module without a word for it.
        Reference<JSObject*> namespaceObject;
        Reference<JSObject*> deferredNamespaceObject;
        Reference<JSObject*> importMeta;
        // The module object, which the require function is bound to.
        Reference<JSObject*> requirer;
        Reference<JSValue> requireFunction;
        // What the loader's require() has answered for a module with an export table: the namespace object, or the export "module.exports".
        Reference<JSValue> requiredValue;
        // The loader's require() is evaluating the module. In plain Bun the cache holds a module object with empty exports for that time, which a require() of the module inside that evaluation answers.
        bool isBeingRequired { false };
        Reference<JSObject*> exportsWhileRequired;
        Reference<JSObject*> capturedExports;
        Reference<JSObject*> deferredExportsSource;
        // In place of capturedExports, until somebody asks for all the names: the values that the fields of the exports object held when the exports were captured.
        Reference<JSCellButterfly*> capturedFields;
        // Beside them, of an exports object with pending stars: what the getters of their bindings answered then.
        Reference<JSCellButterfly*> capturedStars;
        Reference<JSObject*> sourceOfCapturedFields;
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Reference<AbstractModuleRecord*> uncompiledRecord;
        Identifier resolvedKey;
        std::optional<uint32_t> firstAbsentExport;
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        // What the program has recorded: the CommonJS modules that a require() of this module came to, each once, in the order of time, and the module whose require() came to this one first.
        Vector<ModuleID> requiredModules;
        unsigned numberOfChildrenShown { 0 };
        std::optional<ModuleID> firstRequirer;
    };

    struct DynamicExport {
        Identifier name;
        JSValue capturedValue;
        JSObject* liveSource { nullptr };
    };

    StoredModuleStatus storedStatus(ModuleID) const;
    void setStoredStatus(ModuleID, StoredModuleStatus);
    bool isLinkedWithoutStoredStatus(const ModuleData&) const;
    ModuleStatus statusInRecord(ModuleID) const;
    void setStatus(ModuleID, ModuleStatus);
    void setFailed(ModuleID, JSValue error);
    void retireRecord(ModuleRecord*);
    void setValue(ModuleID, JSValue);
    LoadedModule& ensureLoaded(ModuleID);
    LoadedModule* loadedIfExists(ModuleID) const;
    void throwEvaluationError(ModuleID);
    static JSPromise* rejectedPromiseWithPendingException(JSGlobalObject*, ThrowScope&);
    Identifier moduleKey(ModuleID);
    JSObject* namespaceIfCreated(ModuleID, ModulePhase) const;
    void setNamespace(ModuleID, ModulePhase, JSObject*);
    JSObject* createNamespace(ModuleID, ModulePhase);
    JSObject* createTableOfExports();
    Structure* namespaceStructure();
    JSModuleLoader* loader() const;

    bool loadRuntimeModules(const ModuleLinkWalk&);
    void link(const ModuleLinkWalk&);
    void captureSyntheticExports(const ModuleLinkWalk&, VisitedModuleRecords&);
    void finishLink(const ModuleLinkWalk&, LinkContext&);
    void didLinkEntryPoint();
    bool didCaptureExports(ModuleID) const;
    void captureExports(ModuleID);
    void captureExportsBehindEvaluation(ModuleID);
    JSValue evaluateForCapture(ModuleID);
    bool tryCaptureFields(ModuleID, JSValue exports);
    JSValue capturedField(const LoadedModule&, UniquedStringImpl* name) const;
    void makeTableOfCapturedFields(LoadedModule&);
    void fillCapturedExports(ModuleID);
    void fillDeferredExports(ModuleID importer);
    void fillStarExports(ModuleID, StarExportResolver&);
    bool isDeferredExport(const LoadedModule&, const Identifier&) const;
    bool findsItsNamesInLibraryAlone(ModuleID) const;
    Vector<DynamicExport> collectDynamicExports(ModuleID);

    bool enterModule(ModuleID, EvaluationContext&, EvaluationFrames&);
    void evaluateUncompiledRecord(AbstractModuleRecord*, EvaluationContext&, EvaluationFrames&);
    void didEvaluateRequiredModule(EvaluationFrame& importer, CyclicModuleRecord* required, EvaluationContext&);
    void leaveModule(EvaluationFrame&, EvaluationContext&);
    void pushRecord(EvaluationFrame&, ModuleRecord*, EvaluationContext&);
    ModuleRecord* ensureFrameRecord(EvaluationFrame&, EvaluationContext&);
    void materializeFrames(EvaluationFrames&, EvaluationContext&);
    void failFrames(JSValue error, const EvaluationFrames&);
    void failStack(JSValue error, ModuleEvaluationStack&);
    void evaluateWithOwnStack(ModuleID root);
    void evaluateByCallingBody(ModuleID);
    void didThrowInBody(ModuleID, JSValue error);
    void failModulesLeftEvaluating(uint8_t mark, JSValue error);
    void failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID> failedModules, JSValue error);
    void recordEvaluationIfNothingIsLeftToRun(ModuleID);
    bool isPassedOverByEvaluationInProgress(ModuleID) const;

    JSValue executeValueBody(ModuleID);
    JSValue executeRuntimeValue(ModuleID);
    JSValue executeHostValue(ModuleID);

    std::optional<ModuleID> mainCommonJSModule() const;
    bool mayHandOutModuleObject(ModuleID) const;
    JSValue namespaceForRequire(ModuleID);
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    void markNamespaceAsRequired(ModuleID);
    JSValue requireThroughHost(JSValue specifier, ModuleID referrer);
    void throwLoaderChangeRefused(LoaderChange, StringView changed);

    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);
    std::optional<int64_t> asyncEvaluationOrder(ModuleID) const;

    void appendUncompiledRequests(ModuleRecord*);
    ModuleRecord* registerWithLoader(ModuleID);
    JSPromise* importThroughLoader(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    JSPromise* evaluateThroughLoaderSynchronously(ModuleID);
    const Identifier& resolvedKey(ModuleID);

    State& m_state;
    Instance& m_instance;
    ModuleTable m_table;
    std::span<uint8_t> m_storedStatuses;
    UncheckedKeyHashMap<ModuleID, ModuleRecord*> m_records;
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LoadedModule>> m_loaded;
    RememberedSlotSet m_storedReferences;
    std::unique_ptr<ModuleLinkWalk> m_linkWalkOfQueuedImport;
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    Structure* m_namespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    // The pairs of a requiring and a required module that recordRequire() has seen, and whether a require() of the pair has returned.
    UncheckedKeyHashMap<uint64_t, bool> m_recordedRequires;
    // A require() has answered the namespace object of the module, which reads `true` under `__esModule` from then on. The object is made when somebody wants it.
    BitVector m_modulesWithMarkOfRequire;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    // Uncompiled code loads its own dependencies at any later time, so the first module the build named as external admits all.
    bool m_admitsCodeOutsideImage;
    const bool m_admitsInspector;
    bool m_loadsModulesByName;
    bool m_isAskingHost { false };
    bool m_isStartingRuntimeModule { false };
    // The link of the entry point that the image has a plan for stores no status: this says Linked for every module with ModuleData::isLinkedWithEntryPoint().
    bool m_entryPointIsLinked { false };
};

// The engine asks the host for a module that the code of the image names. What the host then asks the engine for is no load by a name that the build did not follow.
class RequestToHostScope {
    WTF_MAKE_NONCOPYABLE(RequestToHostScope);
public:
    explicit RequestToHostScope(Modules& modules)
        : m_isAskingHost(modules.m_isAskingHost, true)
    {
    }

private:
    SetForScope<bool> m_isAskingHost;
};

Modules& modulesOf(Instance&);
Modules* modulesOf(JSGlobalObject*);

inline auto Modules::LoadedModule::namespaceObjectFor(ModulePhase phase) -> Reference<JSObject*>&
{
    switch (phase) {
    case ModulePhase::Evaluation:
        return namespaceObject;
    case ModulePhase::Defer:
        return deferredNamespaceObject;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE StoredModuleStatus Modules::storedStatus(ModuleID module) const
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    return static_cast<StoredModuleStatus>(std::min(m_storedStatuses[module.index()], static_cast<uint8_t>(StoredModuleStatus::Evaluating)));
}

ALWAYS_INLINE void Modules::setStoredStatus(ModuleID module, StoredModuleStatus status)
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    m_storedStatuses[module.index()] = static_cast<uint8_t>(status);
}

ALWAYS_INLINE bool Modules::isLinkedWithoutStoredStatus(const ModuleData& moduleData) const
{
    return !moduleData.needsLink() || (m_entryPointIsLinked && moduleData.isLinkedWithEntryPoint());
}

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    switch (storedStatus(module)) {
    case StoredModuleStatus::Unlinked:
        return isLinkedWithoutStoredStatus(data(module)) ? ModuleStatus::Linked : ModuleStatus::Unlinked;
    case StoredModuleStatus::Linked:
        return ModuleStatus::Linked;
    case StoredModuleStatus::Evaluating:
        return ModuleStatus::Evaluating;
    case StoredModuleStatus::Evaluated:
        return ModuleStatus::Evaluated;
    case StoredModuleStatus::InRecord:
        return statusInRecord(module);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ALWAYS_INLINE ModuleRecord* Modules::recordIfExists(ModuleID module) const
{
    return storedStatus(module) == StoredModuleStatus::InRecord ? m_records.get(module) : nullptr;
}

inline void Modules::didRequestFromHost(ModuleID module)
{
    if (!m_firstModuleRequestedByHost)
        m_firstModuleRequestedByHost = module;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
