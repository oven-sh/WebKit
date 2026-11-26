/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include "ArgList.h"
#include "JSModuleLoader.h"
#include "VM.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/HashMap.h>
#include <wtf/Vector.h>

namespace JSC {

class CyclicModuleRecord;
class ScriptFetcher;

// An importer cannot be linked before its deferred modules have run, but what precedes a deferred module in the order of the
// imports does not import it. So a walk in that order (see ImportOrderWalk) links and evaluates that first.
//
// The exception is a module that leads back to an importer that the walk has yet to leave: it and the modules that follow it keep
// their order, and wait for that importer. A walk that leaves a module for a later one to evaluate stops at the deferred module
// that follows it.
class EvaluationAheadOfLink {
    WTF_MAKE_NONCOPYABLE(EvaluationAheadOfLink);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // A CyclicModuleRecord, or the number that AOT::Modules gave a compiled module.
    using Module = JSValue;
    static Module moduleOf(VM&, CyclicModuleRecord*);

    enum class RunsAheadOfLoad : bool { No, Yes };
    EvaluationAheadOfLink(JSGlobalObject*, RefPtr<ScriptFetcher>, std::optional<JSModuleLoader::EvaluationOfLoad>, RunsAheadOfLoad = RunsAheadOfLoad::No);

    void beginPass(bool hasLeftModuleForLater);

    enum class MayBeOnCycle : bool { No, Yes };
    // Returns the index of the module, which is greater than those of the modules entered before it.
    unsigned enter(Module, MayBeOnCycle = MayBeOnCycle::Yes);
    void leave(bool hasPassedItsImports);
    void didComeBackTo(Module);

    bool isBelowImporter() const { return !m_path.isEmpty(); }
    size_t numberOfModulesAhead() const { return m_modulesAhead.size(); }
    void didPassImport(Module, size_t numberOfModulesAheadOfIt);
    bool isBelowDeferredImport() const { return m_isBelowDeferredImport; }
    void setIsBelowDeferredImport(bool isBelowDeferredImport) { m_isBelowDeferredImport = isBelowDeferredImport; }
    void holdUpUntilWalkEnds() { holdUpUntilEndOf(0); }
    bool cameThroughRecordBelowRoot() const;

    void didLeaveImportForLater() { m_hasLeftImportForLater = true; }
    bool hasLeftModuleForLater() const { return m_hasLeftModuleForLater; }
    void setHasFailureAhead(bool hasFailureAhead) { m_hasFailureAhead = hasFailureAhead; }
    bool stopsAtDeferredModule();
    bool didStopAtDeferredModule() const { return m_didStopAtDeferredModule; }

    bool runsAheadOfLoad() const { return m_runsAheadOfLoad == RunsAheadOfLoad::Yes; }
    bool leavesModulesForLater() const { return runsAheadOfLoad() || m_hasLeftImportForLater; }
    bool evaluatesWhatItPasses() const { return m_evaluation && !leavesModulesForLater() && !m_holdUp && !m_isBelowDeferredImport; }

    enum class Result : uint8_t { IsDone, ChangedAnswers, Threw };
    enum class KeepsAnswers : bool { No, Yes };
    Result evaluateModulesAhead(KeepsAnswers);
    // The steps of that, for a walk that has a list of the modules.
    bool isToBeEvaluated(Module) const;
    void link(Module);
    JSPromise* evaluate(Module);

    bool isEvaluatingModule() const { return !!m_moduleBeingEvaluated; }

    bool didFailToLink() const { return m_didFailToLink; }
    // As opposed to one that could not be loaded.
    bool didModuleThrow() const { return m_didModuleThrow; }
    void setDidModuleThrow() { m_didModuleThrow = true; }
    // After a throw. If a module was evaluated and threw, what the walk entered behind its importer does not import it.
    bool isFailureOfImportOf(unsigned index) const { return !m_didFailToLink && (!m_indexOfImporterThatFailed || index <= m_indexOfImporterThatFailed); }

    // Whether a walk came through the module to one that it is evaluating. InnerModuleEvaluation would have it on its stack.
    static bool isUnderWay(VM& vm) { return vm.importOrderState().innermostEvaluationAheadOfLink; }
    static bool waitsForEvaluationOfImport(JSGlobalObject*, Module);

private:
    bool isOnPath(Module) const;
    void holdUpUntilEndOf(unsigned index);
    void takeBackImportersLinkedMeanwhile();
    Result failImporter(unsigned index, JSValue error);

    JSGlobalObject* const m_globalObject;
    const RefPtr<ScriptFetcher> m_scriptFetcher;
    const std::optional<JSModuleLoader::EvaluationOfLoad> m_evaluation;
    EvaluationAheadOfLink* m_outer { nullptr };

    struct Importer {
        Module module;
        unsigned index;
        unsigned ancestorIndexOfImporter;
        bool isOnStack;
        bool wasLinkedMeanwhile { false };
    };
    Vector<Importer, 16> m_path;
    // As in InnerModuleEvaluation. The indices start at 1.
    Vector<Module, 16> m_stack;
    UncheckedKeyHashMap<EncodedJSValue, unsigned, EncodedJSValueHash, EncodedJSValueHashTraits> m_indicesOnStack;
    unsigned m_lastIndex { 0 };
    unsigned m_ancestorIndex { std::numeric_limits<unsigned>::max() };

    MarkedArgumentBuffer m_modulesAhead;
    Vector<unsigned, 8> m_indicesOfImporters;
    size_t m_numberOfModulesEvaluated { 0 };
    Module m_moduleBeingEvaluated;
    struct HoldUp {
        size_t numberOfModulesAhead;
        unsigned untilEndOf;
    };
    std::optional<HoldUp> m_holdUp;
    unsigned m_indexOfImporterThatFailed { 0 };

    const RunsAheadOfLoad m_runsAheadOfLoad;
    bool m_isBelowDeferredImport { false };
    bool m_hasLeftImportForLater { false };
    bool m_hasLeftModuleForLater { false };
    bool m_hasFailureAhead { false };
    bool m_didStopAtDeferredModule { false };
    bool m_didFailToLink { false };
    bool m_didModuleThrow { false };
};

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
