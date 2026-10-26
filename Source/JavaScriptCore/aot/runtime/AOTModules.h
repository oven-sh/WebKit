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
#include "JSPromise.h"
#include "MicrotaskQueue.h"
#include "RememberedSlot.h"
#include "ScriptFetcher.h"
#include "SourceOrigin.h"
#include "Strong.h"
#include <wtf/HashMap.h>
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {

class JSCellButterfly;
class JSModuleLoader;
class ScriptFetchParameters;
class ThrowScope;

namespace AOT {

class ModuleLinkWalk;
struct EvaluationInProgress;
struct Instance;
struct State;

enum class ModuleStatus : uint8_t {
    Unlinked = static_cast<uint8_t>(CyclicModuleRecord::Status::Unlinked),
    Linking,
    Linked,
    Evaluating,
    EvaluatingAsync,
    Evaluated,
    Failed,
};
static_assert(static_cast<uint8_t>(ModuleStatus::Evaluated) == static_cast<uint8_t>(CyclicModuleRecord::Status::Evaluated));

enum class SynchronousEvaluationReason : uint8_t { Require, DeferredNamespace };
enum class JobsOfImportedFiles : uint8_t { StayQueued, WaitForLoader };
enum class ErrorOfImportedFile : uint8_t { IsThrown, IsLeftToLoader };

struct LinkContext {
    ModuleLinkingStack& stack;
    unsigned index { 0 };
    RefPtr<ScriptFetcher> scriptFetcher { };
};

// capturedFields: in place of capturedExports, until somebody asks for all the names.
#define FOR_EACH_REFERENCE_OF_LOADED_MODULE(macro) \
    macro(JSObject*, namespaceObject) \
    macro(JSObject*, deferredNamespaceObject) \
    macro(JSObject*, importMeta) \
    macro(JSObject*, requirer) \
    macro(JSValue, requireFunction) \
    macro(JSValue, requiredValue) \
    macro(JSObject*, exportsWhileRequired) \
    macro(JSObject*, capturedExports) \
    macro(JSObject*, deferredExportsSource) \
    macro(JSCellButterfly*, capturedFields) \
    macro(JSCellButterfly*, capturedStars) \
    macro(JSObject*, sourceOfCapturedFields) \
    macro(AbstractModuleRecord*, uncompiledRecord)

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
    JSObject* capturedExports(ModuleID);
    // Empty if the module has no export of the name.
    JSValue capturedValue(ModuleID, const Identifier&);
    bool hasCapturedExport(ModuleID, const Identifier&);
    // Hands over no value for a deferred export.
    void forEachCapturedExport(ModuleID, const ScopedLambda<void(const Identifier&, JSValue)>&);
    // What a table of exports holds under the name. In the captured exports of a module the table itself stands for a deferred export: capturedValue() reads that one.
    static JSValue storedValue(JSGlobalObject*, JSObject* table, const Identifier& name);
    JSObject* moduleNamespace(ModuleID, ModulePhase);
    JSObject* importMeta(ModuleID);
    JSCell* builtinCell(BuiltinID, ModuleID);
    JSString* pathString(ModuleID);
    JSString* resolve(ModuleID, ResolvedSpecifierForm);
    const SourceOrigin& sourceOrigin(ModuleID);
    void throwFailure(ModuleFailureID, bool isForRequire);
    void throwExportNotFound(ModuleID exporter, uint32_t nameIdentifier);
    void throwExportAmbiguous(ModuleID exporter, uint32_t nameIdentifier);
    void didRequestFromHost(ModuleID module) { m_firstModuleRequestedByHost = m_firstModuleRequestedByHost.value_or(module); }
    std::optional<ModuleID> firstModuleRequestedByHost() const { return m_firstModuleRequestedByHost; }
    bool refusesCodeOutsideImage() const { return !m_admitsCodeOutsideImage; }
    bool refusesLoadsByName() const { return !m_loadsModulesByName && !m_isAskingHost; }
    bool isAskingHost() const { return m_isAskingHost; }
    // A name that the build did not follow finds an entry point and no other file of the image.
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
    // The modules that compiled code begins to evaluate below the body fail with it.
    JSValue callBodyOfModuleEnteredByLoader(ModuleID module) { return callBodyUnderOwnMark(module, false); }
    void evaluateModuleInRecord(ModuleID);
    // Says whether the start that asks goes on to run the module.
    bool beginModuleInRecord(ModuleID);
    // Says whether a file ran.
    bool evaluateImportedCommonJSFiles(ModuleID root, JobsOfImportedFiles = JobsOfImportedFiles::StayQueued, ErrorOfImportedFile = ErrorOfImportedFile::IsThrown);
    void queueJobsThatWaitForLoader();
    void didLoadGraph(ModuleID root);
    void evaluateFilesOfLevel(ModuleID moduleOnWay);
    // The start of the module is on the stack only to run the files that the module imports.
    bool isRunningFilesOf(ModuleID root) const { return m_rootWhoseFilesRun == root; }
    bool stopLoadingBehindFile(ModuleID file);
    void evaluateLoadedCommonJSFile(ModuleID);
    bool isCommonJSFileOrFace(ModuleID module) const { return data(module).kind == ModuleKind::CommonJs || m_table.fileBehindFace(module); }
    // The loader can load a file of the program as it loads code that is not compiled.
    bool isFileOfProgram(ModuleID module) const { return !data(module).isOfLibrary() && (data(module).kind == ModuleKind::SourceText || data(module).kind == ModuleKind::CommonJs); }
    bool isLoadedWithRequests(ModuleID module) const { return isFileOfProgram(module) && data(module).kind == ModuleKind::SourceText; }
    bool isImportedByLoader(ModuleID module) const { return isFileOfProgram(m_table.fileBehindFace(module).value_or(module)); }
    bool needsLoaderForRequire(ModuleID);
    bool canTellHowGraphIsLoaded(ModuleID root);
    void appendRequestsForLoader(ModuleID, ModuleRecord*);
    ModuleRecord* appendRequestsOfUncompiledCode(ModuleID, ModuleRecord*);
    bool setIsWalkingRequests(bool isWalkingRequests) { return std::exchange(m_isWalkingRequests, isWalkingRequests); }
    // A graph is being loaded until its root is linked.
    unsigned numberOfGraphsBeingLoaded();
    bool loaderGoesThroughGraph();
    void didStartLoad(JSPromise*, ModuleID root);
    ModuleRecord* recordOfCommonJSFile(ModuleID);
    bool isNotLoaded(ModuleID) const;
    bool wasLoadedWithEarlierGraph(ModuleID);
    bool setIsAskingHost(bool isAskingHost) { return std::exchange(m_isAskingHost, isAskingHost); }
    bool wasImportedWhileItRan(ModuleID file) const;
    void evaluateCommonJSForLoader(ModuleID file);
    JSValue execute(ModuleID);

    JSValue require(ModuleID);
    void evaluateCommonJS(ModuleID);
    JSValue commonJSExports(ModuleID) const;
    JSValue exportsSeenByLoader(ModuleID);
    void setCommonJSExports(ModuleID module, JSValue exports) { setValue(module, exports); }
    void recordRequire(ModuleID, ModuleID referrer);
    JSObject* moduleObject(ModuleID);
    JSObject* mainModuleObject();
    JSObject* parentModuleObject(ModuleID);
    bool forEachNewChild(ModuleID, const ScopedLambda<void(JSObject*)>&);
    JSValue requireSpecifier(JSValue specifier, ModuleID referrer, bool isNamedByBuild);
    JSValue resolveDynamic(JSValue specifier, ModuleID referrer, ResolvedSpecifierForm);

    JSPromise* import(ModuleID, ModuleID referrer);
    JSPromise* startImport(ModuleID, std::optional<int64_t> referrerAsyncOrder);
    JSPromise* importDynamic(JSValue specifier, JSValue options, ModuleID referrer);
    void continueImport(JSPromise* importPromise, ModuleID, std::optional<int64_t> referrerAsyncOrder);
    void resolveWithNamespace(JSPromise* importPromise, ModuleID);

    ModuleRecord* exposeRecord(ModuleID);
    AbstractModuleRecord* uncompiledRecord(ModuleID);
    // The record that stands for the module in ResolveExport() and GetExportedNames().
    AbstractModuleRecord* recordForResolution(ModuleID);
    JSObject* uncompiledNamespace(ModuleID, ModulePhase);

private:
    friend class RequestToHostScope;

    // The roots are visited at every stop of the mutator, and the loaded modules only after a store.
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

        Reference<JSObject*>& namespaceObjectFor(ModulePhase phase) { return phase == ModulePhase::Defer ? deferredNamespaceObject : namespaceObject; }

#define AOT_DECLARE_REFERENCE(type, name) Reference<type> name;
        FOR_EACH_REFERENCE_OF_LOADED_MODULE(AOT_DECLARE_REFERENCE)
#undef AOT_DECLARE_REFERENCE
        bool isBeingRequired { false };
        bool capturedFieldsAreOfESModule { false };
        bool capturedFieldsAreOfRuntimeModule { false };
        Identifier resolvedKey;
        std::optional<uint32_t> firstAbsentExport;
        bool wasLoadedWhileItRan { false };
        BitVector ambiguousExports;
        std::unique_ptr<SourceOrigin> sourceOrigin;
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
    ModuleStatus statusInRecord(ModuleID) const;
    void setStatus(ModuleID, ModuleStatus);
    void setFailed(ModuleID, JSValue error);
    ModuleRecord* ensureRecordWithoutStatus(ModuleID);
    void retireRecord(ModuleRecord*);
    void setValue(ModuleID, JSValue);
    LoadedModule& ensureLoaded(ModuleID);
    LoadedModule* loadedIfExists(ModuleID module) const { return m_loaded.get(module); }
    void throwEvaluationError(ModuleID);
    static JSPromise* rejectedPromiseWithPendingException(JSGlobalObject*, ThrowScope&);
    Identifier moduleKey(ModuleID);
    JSObject* namespaceIfCreated(ModuleID, ModulePhase) const;
    void setNamespace(ModuleID, ModulePhase, JSObject*);
    JSObject* createNamespace(ModuleID, ModulePhase);
    JSObject* createTableOfExports();
    JSModuleLoader* loader() const;

    bool loadRuntimeModules(const ModuleLinkWalk&);
    void link(const ModuleLinkWalk&);
    void captureSyntheticExports(const ModuleLinkWalk&, VisitedModuleRecords&);
    void finishLink(const ModuleLinkWalk&, LinkContext&);
    void didLinkEntryPoint();
    bool didCaptureExports(ModuleID) const;
    void captureExports(ModuleID);
    void captureExportsBehindEvaluation(ModuleID);
    void captureExportsOfFilesBeingEvaluated(ModuleID root);
    JSValue evaluateForCapture(ModuleID);
    bool tryCaptureFields(ModuleID, JSValue exports);
    JSValue capturedField(const LoadedModule&, UniquedStringImpl* name) const;
    void makeTableOfCapturedFields(LoadedModule&);
    void fillCapturedExports(ModuleID);
    void fillDeferredExports(ModuleID importer);
    AbstractModuleRecord::Resolution::Type resolveRequiredName(ModuleID exporter, const Identifier&);
    // Throws for the first name that an unlinked module of the list requires and that its exporter lacks or has from two bindings.
    void checkRequiredNames(std::span<const ModuleID> modules);
    bool isDeferredExport(const LoadedModule&, const Identifier&) const;
    bool findsItsNamesInLibraryAlone(ModuleID) const;
    Vector<DynamicExport> collectDynamicExports(ModuleID);

    void evaluateWithOwnStack(ModuleID root);
    void holdJobsBehind(size_t numberOfJobs, ModuleID file);
    bool planLevelsOfLoad(ModuleID root);
    JSValue callBody(ModuleID);
    JSValue callBodyUnderOwnMark(ModuleID, bool bodyStoresStatus, bool runsImportedFilesAlone = false);
    Vector<ModuleID, 8> modulesThatEvaluationWillBegin(ModuleID root);
    void evaluateByCallingBody(ModuleID module) { callBodyUnderOwnMark(module, true); }
    void didThrowInBody(ModuleID, JSValue error);
    void failEvaluatedModulesOfUnfinishedCycles(std::span<const ModuleID> failedModules, JSValue error);
    void recordEvaluationIfNothingIsLeftToRun(ModuleID);
    bool isPassedOverByEvaluationInProgress(ModuleID) const;

    std::optional<ModuleID> mainCommonJSModule() const;
    bool mayHandOutModuleObject(ModuleID module) const { return data(module).kind != ModuleKind::CommonJs || data(module).hasValue(); }
    JSValue readExportForRequire(JSObject* namespaceObject, const Identifier& name);
    void markNamespaceAsRequired(ModuleID);

    JSPromise* importUncompiled(const Identifier& key, ModuleID referrer, RefPtr<ScriptFetchParameters>&&);
    std::optional<int64_t> asyncEvaluationOrder(ModuleID) const;
    ModuleRecord* registerWithLoader(ModuleID);
    const Identifier& resolvedKey(ModuleID);

    State& m_state;
    Instance& m_instance;
    ModuleTable m_table;
    std::span<uint8_t> m_storedStatuses;
    UncheckedKeyHashMap<ModuleID, ModuleRecord*> m_records;
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LoadedModule>> m_loaded;
    RememberedSlotSet m_storedReferences;
    std::unique_ptr<ModuleLinkWalk> m_linkWalkOfQueuedImport;
    // Compared, never followed: its job holds the promise.
    JSPromise* m_importThatWaitsForQueuedJobs { nullptr };
    const EvaluationInProgress* m_innermostEvaluation { nullptr };
    Structure* m_namespaceStructure { nullptr };
    Identifier m_moduleExportsName;
    // The pairs of a requiring and a required module that recordRequire() has seen, and whether a require() of the pair has returned.
    UncheckedKeyHashMap<uint64_t, bool> m_recordedRequires;
    BitVector m_modulesWithMarkOfRequire;
    BitVector m_modulesEvaluatedUnderAnotherEvaluation;
    std::optional<ModuleID> m_firstModuleRequestedByHost;
    bool m_admitsCodeOutsideImage;
    const bool m_admitsInspector;
    bool m_loadsModulesByName;
    bool m_isAskingHost { false };
    bool m_isWalkingRequests { false };
    struct LoadInFlight {
        Strong<JSPromise> promise;
        ModuleID root;
        bool isWalked { false };
    };
    Vector<LoadInFlight, 2> m_loadsInFlight;
    // `bun run` loads a graph level by level, and the steps of one level follow each other with no other job between them. So the loader need only go down one way to the lowest level:
    // when it comes to the module of a level on that way, the files of the level that stand in front of the module run. It comes to the first file behind the module too, and those behind that one run with it.
    struct LevelOfLoad {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(LevelOfLoad);

        std::optional<ModuleID> next;
        std::optional<ModuleID> firstFileBehindNext;
        Vector<ModuleID, 4> files;
        Strong<Unknown> firstError;
    };
    UncheckedKeyHashMap<ModuleID, std::unique_ptr<LevelOfLoad>> m_levelsOfLoads;
    // Their imported files ran without the loader. The set is filled from the list when it is asked.
    Vector<ModuleID, 4> m_rootsLoadedWithoutLoader;
    BitVector m_filesLoadedWithoutLoader;
    // A file ran before the loader came to it and queued jobs. They are queued where the loader comes to the file, as under `bun run`.
    std::optional<ModuleID> m_fileWhoseJobsWaitForLoader;
    std::optional<ModuleID> m_rootWhoseFilesRun;
    // The loader goes through its graph, and comes to files that have run.
    std::optional<ModuleID> m_rootWhoseFilesRanAhead;
    RefPtr<MicrotaskQueue> m_jobsThatWaitForLoader;
    bool m_isStartingRuntimeModule { false };
    // The link of the entry point that the image has a plan for stores no status.
    bool m_entryPointIsLinked { false };
};

// What the host asks the engine for while the engine asks it for a module that the image names is no load by a name that the build did not follow.
class RequestToHostScope {
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

ALWAYS_INLINE ModuleStatus Modules::status(ModuleID module) const
{
    switch (storedStatus(module)) {
    case StoredModuleStatus::Unlinked:
        return !data(module).needsLink() || (m_entryPointIsLinked && data(module).isLinkedWithEntryPoint()) ? ModuleStatus::Linked : ModuleStatus::Unlinked;
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
