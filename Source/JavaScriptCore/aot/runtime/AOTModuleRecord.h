/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVector.h"
#include "CyclicModuleRecord.h"
#include <wtf/BitVector.h>
#include <wtf/HashSet.h>
#include <wtf/OrderedHashSet.h>

namespace JSC {

class ModuleGraphLoadingState;
class ModuleLoadingContext;

namespace AOT {

class Modules;
struct Instance;

using ModuleEvaluationStack = WTF::Vector<AbstractModuleRecord*, 8>;
using ModuleLinkingStack = WTF::Vector<CyclicModuleRecord*, 8>;
using StarExportTargets = WTF::Vector<AbstractModuleRecord*, 8>;
using VisitedModuleRecords = UncheckedKeyHashSet<AbstractModuleRecord*>;

class ModuleRecord final : public CyclicModuleRecord {
public:
    using Base = CyclicModuleRecord;

    static constexpr unsigned StructureFlags = Base::StructureFlags | StructureIsImmortal;
    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.aotModuleRecordSpace<mode>();
    }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    static ModuleRecord* create(Instance&, Structure*, JSModuleLoader*, const Identifier& moduleKey, ModuleID);

    DECLARE_EXPORT_INFO;

    ModuleID module() const { return m_module; }
    bool isMocked() const;
    JSGlobalObject* instanceRealm() const;
    bool isOf(const Instance& instance) const { return &m_instance == &instance; }

    bool isOnStack(const ModuleEvaluationStack& stack) const { return m_evaluationStack == &stack; }
    void setEvaluationStack(const ModuleEvaluationStack* stack) { m_evaluationStack = stack; }

    bool holdsStatus() const { return m_holdsStatus; }
    void setHoldsStatus() { m_holdsStatus = true; }
    void clearHoldsStatus() { m_holdsStatus = false; }

    bool isRetained() const { return m_isRetained || m_isHeldByLoader; }
    void retain() { m_isRetained = true; }
    using AbstractModuleRecord::forgetCachedResolutions;
    void setIsHeldByLoader(bool isHeldByLoader) { m_isHeldByLoader = isHeldByLoader; }

    unsigned innerModuleEvaluation(JSGlobalObject*, ModuleEvaluationStack&, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise);
    void recordTerminationAfterEvaluation(VM&, Exception*);
    void generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords&);
    void willLoadRequests(ModuleGraphLoadingState&);
    void countLoaderStep();
    void didGoOnWithLoadWhileRequestsWereListed(ModuleGraphLoadingState& load) { m_loadGoneOnWithWhileRequestsWereListed = &load; }
    bool takeWentOnWhileRequestsWereListed(ModuleGraphLoadingState& load)
    {
        if (m_loadGoneOnWithWhileRequestsWereListed != &load)
            return false;
        m_loadGoneOnWithWhileRequestsWereListed = nullptr;
        return true;
    }
    void didLoad();
    static void didFailToLoad(JSGlobalObject*, const ModuleGraphLoadingState&);
    void didReplaceRecordInRegistry();
    static std::optional<ModuleID> fileToWaitFor(JSGlobalObject*, const Identifier& key);
    static JSPromise* deferRequest(JSGlobalObject*, ModuleID file, ModuleLoadingContext*, bool useImportMap);
    static void didRequestModule(JSGlobalObject*, const Identifier& key, JSCell* payload);
    static void didSettleLoad(JSGlobalObject*, const ModuleGraphLoadingState&);
    static void didFailToMakeModule(JSGlobalObject*, const Identifier& key);
    static void didLoseErrorOfModule(JSGlobalObject*, const Identifier& key);
    void willBeLoadedByImport();
    bool takeStepOfMainModule();
    bool takesEveryStepOfLoader();
    bool takeLoadPromiseBelowMainModule();
    JSPromise* evaluateMainModule(JSPromise* evaluation);
    static void didEvaluateModulesInJob(JSGlobalObject*);
    static void didFailModulesInJob(JSGlobalObject*);
    void setStepsBehindLoad(uint32_t steps) { m_stepsBehindLoad = steps; }
    bool takeStepBehindLoad() { return m_stepsBehindLoad && m_stepsBehindLoad--; }
    bool holdGraphBehindEarlierCalls(JSCell* payload, bool isDeferred);
    void takeStatus();
    bool isRunningCommonJSFile();
    bool keepsToModule() const { return m_keepsToModule; }
    bool takesNamesFromModuleItself();
    void setKeepsToModule(bool keepsToModule = true) { m_keepsToModule = keepsToModule; }
    void setRunsWhenLoaded() { m_runsWhenLoaded = true; }
    bool runsWhenLoaded() const { return m_runsWhenLoaded; }
    void setRunsInItsTurn() { m_runsInItsTurn = true; }
    bool takeRunsInItsTurn() { return std::exchange(m_runsInItsTurn, false); }
    bool isRequestedByWalkedModule() const { return m_isRequestedByWalkedModule; }
    void setIsRequestedByWalkedModule() { m_isRequestedByWalkedModule = true; }
    void setHasRequestThatFindsNothing() { m_hasRequestThatFindsNothing = true; }
    uint8_t willAskHostForRequest(const Identifier& key);
    static BitVector letAllImagesAskHost(JSGlobalObject*);
    static void restoreImagesThatAskHost(JSGlobalObject*, const BitVector& wereAsking);
    void didAskHostForRequest(uint8_t flagsBefore);
    static uint8_t willAskHostForUncompiledModule(JSGlobalObject*);
    static void didAskHostForUncompiledModule(JSGlobalObject*, uint8_t flagsBefore);
    static void willAnswerRequestOfUncompiledModule(JSGlobalObject*, const Identifier& key, const Identifier& keyOfAsker);
    static void willAnswerFromRegistry(JSGlobalObject*, const Identifier& key, const String& pathOfAsker);
    AbstractModuleRecord* recordOfRequest(JSGlobalObject*, const Identifier& key);
    bool holdTextOfRequest(JSPromise* fetched, AbstractModuleRecord* requested);
    void didRequestCompiledModule(const Identifier& key, ModuleID module) { m_compiledRequests.append({ key.impl(), module }); }
    unsigned innerModuleLinking(JSGlobalObject*, ModuleLinkingStack&, unsigned index, RefPtr<ScriptFetcher>);
    void execute(JSGlobalObject*, JSPromise* capability = nullptr);
    void executeAsync(JSGlobalObject*);

    Resolution resolveOwnExport(JSGlobalObject*, const Identifier& exportName, StarExportTargets&, ModuleRecord* leader);
    bool originalShows(JSGlobalObject*, const Identifier& exportName, std::span<ModuleRecord* const> askers);
    bool isNamedByEndedMockInFrontOfItsStar(JSGlobalObject*, const Identifier& exportName, unsigned index);
    bool answersByWhoLedToIt() const;
    bool keepsEndedMock() const;
    bool someMockStandsInPlace() const;
    bool mayOutlive(AbstractModuleRecord*) const;
    const Resolution* resolutionKept(UniquedStringImpl* exportName);
    void keepResolution(UniquedStringImpl* exportName, const Resolution&);
    bool standsForMock() const;
    void collectExportedNames(JSGlobalObject*, WTF::Vector<Identifier>&, StarExportTargets&);
    JSValue readExport(JSGlobalObject*, const Identifier& exportName, std::optional<unsigned> exportIndex);
    JSObject* moduleNamespaceObject(JSGlobalObject*, ModulePhase);
    static bool isSameBinding(const Resolution&, const Resolution&);
    static bool throwIfExportWasLeftOut(JSGlobalObject*, AbstractModuleRecord* importer, AbstractModuleRecord* exporter, const Identifier& name);

    void gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen);
    bool readyForSyncExecution(VisitedModuleRecords& seen);

private:
    ModuleRecord(Instance&, Structure*, JSModuleLoader*, const Identifier& moduleKey, ModuleID);
    Modules& modules() const;
    void appendDynamicStarTargets(StarExportTargets&);
    bool appendStarsThatMayProvide(const Identifier& exportName, unsigned index, StarExportTargets&);
    AbstractModuleRecord* recordBehindStar(ModuleID);

    Instance& m_instance;
    ModuleID m_module;
    const ModuleEvaluationStack* m_evaluationStack { nullptr };
    Vector<UniquedStringImpl*, 2> m_resolveSet;
    ModuleRecord* m_leaderOfLastQuestion { nullptr };
    Vector<std::pair<UniquedStringImpl*, ModuleID>, 2> m_compiledRequests;
    unsigned m_nextCompiledRequest { 0 };
    ModuleGraphLoadingState* m_loadGoneOnWithWhileRequestsWereListed { nullptr };
    uint32_t m_stepsBehindLoad { 0 };
    bool m_holdsStatus { false };
    bool m_isRetained { false };
    bool m_isHeldByLoader { false };
    bool m_runsWhenLoaded { false };
    bool m_runsInItsTurn { false };
    bool m_isRequestedByWalkedModule { false };
    bool m_hasRequestThatFindsNothing { false };
    bool m_keepsToModule { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
