/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "AbstractModuleRecord.h"
#include "ArgList.h"
#include "EvaluationAheadOfLink.h"
#include "JSModuleLoader.h"
#include "VM.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Vector.h>

namespace JSC {

class CyclicModuleRecord;
class ModuleRegistryEntry;
class ScriptFetcher;
class SyntheticModuleRecord;
#if ENABLE(AOT)
namespace AOT {
class ModuleRecord;
}
#endif

// Runs the deferred modules (SyntheticSourceProvider::createDeferred()) of a graph, depth first in the order of the import
// declarations, which is the order InnerModuleEvaluation would reach them in.
//
// This is its own pass ahead of InnerModuleLinking because such a module can require() an ES module that imports back into
// the graph. That load links and evaluates; started from inside InnerModuleLinking it would find the records on the outer
// linking stack in the LINKING state and treat them as linked.
//
// The other modules that InnerModuleEvaluation would reach on the way are evaluated in between. See EvaluationAheadOfLink.
//
// The loader fetches a graph all at once, but a deferred module can change what the imports that follow it are answered
// with: it can remove a module from the registry, teach the host to resolve or fetch another way, or supply what could not
// be loaded. So an answer only stands once the walk has reached the import. Until then the walk asks again, and waits, if a
// module that it ran has made the answer obsolete.
class ImportOrderWalk {
    WTF_MAKE_NONCOPYABLE(ImportOrderWalk);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    using VisitedRecords = UncheckedKeyHashSet<AbstractModuleRecord*>;

    // See the functions of JSModuleLoader that call these two.
    static JSPromise* run(JSGlobalObject*, AbstractModuleRecord* root, RefPtr<ScriptFetcher>&&, bool mayHaveImportsInFlight, JSValue errorOfLoad = { }, std::optional<JSModuleLoader::EvaluationOfLoad> = { });
    static JSModuleLoader::RunAheadOfLoad runAheadOfLoad(JSGlobalObject*, CyclicModuleRecord* root);
    // For a load that is held up by a fetch that does not only wait for the host. Returns what to wait for before calling it again.
    static JSPromise* runAheadOfFetches(JSGlobalObject*, CyclicModuleRecord* root, JSModuleLoader::PassedImports&);
    // For a graph that is linked on the spot. It keeps the answers it has.
    static void runWithoutWaiting(JSGlobalObject*, AbstractModuleRecord* root, VisitedRecords&, AbstractModuleRecord::IsBehindWalk, EvaluationAheadOfLink* ofWalkThatCameToRoot = nullptr);

    // Whether the record is a deferred module that is running, or an importer that a walk came to one through.
    static bool isBeingReached(VM&, AbstractModuleRecord*);
    // If so, that walk settles whether the module loads, whatever becomes of a load that has started since.
    static bool isOnPathOfWalk(VM&, AbstractModuleRecord*, const ImportOrderWalk* otherThan = nullptr);
    // What a walk that comes through the record has to wait for.
    static JSPromise* pendingLoadBelow(CyclicModuleRecord*);
    static VisitedRecords modulesYetToBeLinkedBelow(CyclicModuleRecord* root);
    static Vector<CyclicModuleRecord*> modulesAboutToBeLinked(VM&);
    static bool wouldComeToRunningModule(JSGlobalObject*, AbstractModuleRecord* root);
#if ENABLE(AOT)
    using ImportsOfModules = Vector<std::pair<CyclicModuleRecord*, ModuleMapKey>, 1>;
    // By modules that are not compiled and have yet to be linked.
    static ImportsOfModules importsThatWalksHaveReached(AbstractModuleRecord* imported, std::span<AbstractModuleRecord* const> importsOfCompiledModules);
#endif
    static void forEachLoaderOfWalkInProgress(VM&, NOESCAPE const Function<void(JSModuleLoader*)>&);
    static bool hasPostponedImportOf(JSModuleLoader*, const Identifier& key);
    static void keepImportsOfWhatHasBegun(VM&);
    static uint64_t noteChange(VM&);

    using ModuleRequest = AbstractModuleRecord::ModuleRequest;

    enum class Mode : uint8_t { WaitsForImports, RunsAheadOfLoad, KeepsAnswers };
    enum class Outcome : uint8_t {
        Passed,
        PassedInPart, // An import was left for later.
        NeedsImports,
        Threw,
    };
    enum class Turn : uint8_t { IsToCome, HasCome, IsLast };

    Outcome reachImport(AbstractModuleRecord*);
    bool requestImportsBelowImport(AbstractModuleRecord*);
    // Forgets one that is obsolete.
    JSValue failureToResolve(JSModuleLoader*, const Identifier& importer, const Identifier& specifier, Turn);
    void failToImportModuleThatThrew(CyclicModuleRecord* importer, JSValue error);

#if ENABLE(AOT)
    // A compiled module goes through its imports by itself. It comes back for those that the loader answers, which it names by key.
    Mode mode() const { return m_mode; }
    VisitedRecords& visited() const { return m_visited; }
    EvaluationAheadOfLink& evaluation() const { return m_evaluation; }
    bool leavesFactoriesForLater() const { return m_mode == Mode::RunsAheadOfLoad || (m_mode == Mode::WaitsForImports && !m_didCallFactories); }
    bool letsGiveWayToCopy(AbstractModuleRecord* module) const { return m_mode == Mode::WaitsForImports || (m_mode == Mode::RunsAheadOfLoad && module != m_root); }
    bool didRequestImports() const { return m_didRequestImports; }
    bool didFailToImport() const { return m_didFailToImport; }
    bool didFailToTakeOverImport() const { return m_didFailToTakeOverImport; }
    void willLinkBehindWalk(AOT::ModuleRecord*);
    void didLeaveFactoryForLater() { m_hasFactoryBehindImportHeldBack |= hasPassedImportHeldBack(); }
    void didLeaveFactoryToFetch() { m_didLeaveFactoryToFetch = true; }
    Outcome reachImportThatHasNotLoaded(AOT::ModuleRecord*, const ModuleRequest&);
    bool requestImportThatHasNotLoaded(AOT::ModuleRecord*, const ModuleRequest&);
    Outcome failToImport(AOT::ModuleRecord*, JSValue error);
    // A synchronous load gives them the chance to supply the import, as it does the fetches that the host has postponed.
    bool willCallFactoriesAheadOfFailure();
    Outcome didCallFactoriesAheadOfFailure(JSPromise* pendingFactories);
#endif

private:
    struct Answer {
        enum class Kind : uint8_t { None, InFlight, HeldBack, Postponed, Record, Failure };
        Kind kind { Kind::None };
        ModuleRegistryEntry* entry { nullptr }; // Null for a failure to resolve.
    };

    ImportOrderWalk(JSGlobalObject*, AbstractModuleRecord* root, Mode, RefPtr<ScriptFetcher>&&, VisitedRecords&, EvaluationAheadOfLink&);
    ~ImportOrderWalk();

    JSPromise* run(CyclicModuleRecord* root, bool mayHaveImportsInFlight);

    Outcome reach(CyclicModuleRecord*);
    Outcome reachImportsOf(CyclicModuleRecord*);
    Outcome reachImportThatHasNotLoaded(CyclicModuleRecord*, const ModuleRequest&, AbstractModuleRecord*& imported);
    void didEnd();
    static Vector<AbstractModuleRecord*, 16> rootsOfLoadsInProgress(JSModuleLoader*);
    void takeAnswer(CyclicModuleRecord*, const ModuleRequest&, AbstractModuleRecord* imported, Turn);
    void postpone(CyclicModuleRecord*, const ModuleRequest&);
    Outcome passImportLeftForLater(CyclicModuleRecord*, const ModuleRequest&);
    Outcome reachSynthetic(SyntheticModuleRecord*);
    void didLoad(CyclicModuleRecord*);

    void forgetPassedImports();
#if ENABLE(AOT)
    AOT::ModuleRecord* compiledModuleBehindFetch(CyclicModuleRecord* importer, const ModuleRequest&) const;
    bool hasPassedImportHeldBack() const { return m_importHeldBack || m_didLeaveFactoryToFetch; }
    Outcome reachCompiled(AOT::ModuleRecord*);
    Outcome callFactoriesOfCompiledModules(JSPromise*& pendingFactory);
    void prepareCompiledModulesForLink();
#endif
    void askForPostponedImports();
    void askForPostponedImportsAfterThrow();

    unsigned numberOfImportsPassed(CyclicModuleRecord* module) const { return m_passed ? m_passed->countByImporter.get(module) : 0; }
    bool requestImportsBelow(CyclicModuleRecord*);
    void dropObsoleteAnswers(CyclicModuleRecord*);

    bool isObsolete(CyclicModuleRecord*, const AbstractModuleRecord::LoadedModuleRequest&) const;
    bool didHostJustFail(CyclicModuleRecord*, const ModuleRequest&) const;
    Answer answerTo(CyclicModuleRecord*, const ModuleRequest&) const;
    Answer answerWithoutRecord(ModuleRegistryEntry*) const;
    bool takesOver(const Answer&) const;
    bool noteImportInFlight(const Answer&);
    Outcome reachImportInFlight(const Answer&);
    JSValue ask(CyclicModuleRecord*, const ModuleRequest&, Turn);
    Outcome failToImport(CyclicModuleRecord*, const ModuleRequest&, JSValue error);
    Outcome failAgain(CyclicModuleRecord*);
    void failToLoad(ModuleRegistryEntry*, JSValue error);
    Outcome fail(AbstractModuleRecord*, JSValue error);
    Outcome evaluateModulesAheadOfDeferredModule();

    JSGlobalObject* const m_globalObject;
    VM& m_vm;
    VM::ImportOrderState& m_state;
    AbstractModuleRecord* const m_root;
    JSModuleLoader* const m_loaderOfRoot;
    ImportOrderWalk* const m_outer;
    const RefPtr<ScriptFetcher> m_scriptFetcher;
    VisitedRecords& m_visited;
    EvaluationAheadOfLink& m_evaluation;
    JSModuleLoader::PassedImports* m_passed { nullptr };
    Vector<CyclicModuleRecord*, 16> m_path;
    Vector<std::pair<CyclicModuleRecord*, ModuleRequest>> m_postponed;
    MarkedArgumentBuffer m_importersThatPostponed; // What runs meanwhile can take them out of the graph.
    const ModuleRequest* m_importOfDeferredModule { nullptr }; // It is reached once the modules ahead of it have been evaluated.
    JSPromise* m_importInFlight { nullptr };
    JSPromise* m_importInFlightInScript { nullptr };
    JSPromise* m_importHeldBack { nullptr };
    JSPromise* m_promiseOfLastRequest { nullptr };
    JSValue m_errorOfHost;
    const Mode m_mode;
    bool m_didRequestImportInFlight { false };
    bool m_hasFailureAhead { false };
    bool m_hasImportHeldBackAhead { false };
    bool m_didStopAtImportInFlight { false };
    bool m_isFailureBehindPostponedImports { false };
    bool m_didFailToTakeOverImport { false };
    bool m_isBehindWalk { false };
#if ENABLE(AOT)
    bool m_didRequestImports { false };
    bool m_didFailToImport { false };
    bool m_didCallFactories { false };
    bool m_didCallFactoriesAheadOfFailure { false };
    bool m_hasFactoryBehindImportHeldBack { false };
    bool m_didLeaveFactoryToFetch { false };
    JSPromise* m_factoriesAheadOfFailure { nullptr };
    MarkedArgumentBuffer m_compiledModulesToLink;
    Vector<Identifier> m_keysPostponedForCompiledModules;
#endif
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
