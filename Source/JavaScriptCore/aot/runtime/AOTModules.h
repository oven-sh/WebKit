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
#include "ArgList.h"
#include "Identifier.h"
#include "JSPromise.h"
#include "ScriptFetcher.h"
#include "SourceOrigin.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class JSModuleLoader;
class JSObject;
class JSString;
class ScriptFetchParameters;
class ThrowScope;

namespace AOT {

class ModuleLinkWalk;
class StarExportResolver;
struct EvaluationFrame;
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
    const Identifier& identifier(uint32_t number) const;
    ModuleStatus status(ModuleID) const;
    ModuleRecord* recordIfExists(ModuleID) const;
    ModuleRecord* ensureRecord(ModuleID);
    JSValue value(ModuleID) const;
    JSValue readExport(ModuleID, unsigned index);
    JSValue readExportIfInitialized(ModuleID, unsigned index) const;
    bool isExportAmbiguous(ModuleID, unsigned index) const;
    JSObject* capturedExports(ModuleID) const;
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
    bool refusesRequestFromHost(ModuleID module) const { return refusesLoadsByName() && !data(module).isEntryPoint(); }
    void admitCodeNamedBy(JSValue specifier);
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

    JSValue callBody(ModuleID, JSValue thisValue, const ArgList&);
    JSValue execute(ModuleID);

    JSValue requireFrom(ModuleID, ModuleID referrer);
    JSValue require(ModuleID, JSObject* parent, JSObject* adoptedModule);
    void evaluateCommonJSBody(ModuleID, JSObject* commonJSModule);
    JSObject* moduleObject(ModuleID);
    JSValue requireFunction(ModuleID);
    JSValue requireDynamic(JSValue specifier, ModuleID referrer);
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);
    JSValue resolveFrom(ModuleID, ModuleID referrer, StringID writtenSpecifier, ResolvedSpecifierForm);
    void evict(ModuleID);
    void willSetRequireCacheEntry(ModuleID, JSValue entry);
    void willChangeRequireOverride(RequireOverride, bool isOverridden);
    void didObserveModuleChildren();

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

    struct LoadedModule {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LoadedModule);

        JSObject*& namespaceObjectFor(ModulePhase);

        JSObject* namespaceObject { nullptr };
        JSObject* deferredNamespaceObject { nullptr };
        JSObject* importMeta { nullptr };
        JSObject* requirer { nullptr };
        JSValue requireFunction;
        JSValue initialExports;
        JSObject* capturedExports { nullptr };
        AbstractModuleRecord* uncompiledRecord { nullptr };
        Identifier resolvedKey;
        std::optional<uint32_t> firstAbsentExport;
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
        bool hasEnteredBody { false };
        bool wasEvictedWhileEvaluating { false };
        bool isOverriddenInRequireCache { false };
    };

    struct DynamicExport {
        Identifier name;
        JSValue capturedValue;
        JSObject* liveSource { nullptr };
    };

    StoredModuleStatus storedStatus(ModuleID) const;
    void setStoredStatus(ModuleID, StoredModuleStatus);
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
    JSObject* createNamespace(ModuleID, ModulePhase);
    Structure* namespaceStructure();
    JSModuleLoader* loader() const;

    bool loadRuntimeModules(const ModuleLinkWalk&);
    void link(const ModuleLinkWalk&);
    void captureSyntheticExports(const ModuleLinkWalk&, VisitedModuleRecords&);
    void finishLink(const ModuleLinkWalk&, LinkContext&);
    void captureExports(ModuleID);
    void fillCapturedExports(ModuleID, JSObject* captured);
    void fillStarExports(ModuleID, StarExportResolver&);
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

    JSValue executeValueBody(ModuleID);
    JSValue executeRuntimeValue(ModuleID);
    JSValue executeHostValue(ModuleID);

    JSValue requireWithLink(ModuleID, JSObject* parent, JSObject* adoptedModule, ChildLink&);
    JSValue executeCommonJS(ModuleID, JSObject* parent, JSObject* adoptedModule, ChildLink&);
    ChildLink addChild(JSObject* parent, JSObject* child);
    void adoptCommonJSModule(ModuleID, JSObject* commonJSModule, CommonJSModuleObjects&);
    void callCommonJSBody(ModuleID, const CommonJSModuleObjects&);
    void resetBeforeNextRun(ModuleID);
    JSValue commonJSExports(ModuleID);
    JSValue namespaceForRequire(ModuleID);
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    void armRequireEdge(uint32_t edge);
    void disarmRequireEdges();
    bool requireIsOverridden() const { return !m_requireOverrides.isEmpty(); }
    JSValue requireThroughHost(JSValue specifier, ModuleID referrer);
    void throwLoaderChangeRefused(const String& change);

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
    std::unique_ptr<ModuleLinkWalk> m_linkWalkOfQueuedImport;
    Structure* m_namespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    OptionSet<RequireOverride> m_requireOverrides;
    // Uncompiled code loads its own dependencies at any later time, so the first module the build named as external admits all.
    bool m_admitsCodeOutsideImage;
    bool m_loadsModulesByName;
    bool m_replacesLoaderAnswers;
    bool m_isAskingHost { false };
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

inline JSObject*& Modules::LoadedModule::namespaceObjectFor(ModulePhase phase)
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
    return static_cast<StoredModuleStatus>(m_storedStatuses[module.index()]);
}

ALWAYS_INLINE void Modules::setStoredStatus(ModuleID module, StoredModuleStatus status)
{
    RELEASE_ASSERT(module.index() < m_storedStatuses.size());
    m_storedStatuses[module.index()] = static_cast<uint8_t>(status);
}

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    switch (storedStatus(module)) {
    case StoredModuleStatus::Unlinked:
        return data(module).needsLink() ? ModuleStatus::Unlinked : ModuleStatus::Linked;
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
