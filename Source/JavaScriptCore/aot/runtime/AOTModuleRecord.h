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

    bool ownsStatus() const { return m_ownsStatus; }
    void setOwnsStatus() { m_ownsStatus = true; }
    void clearOwnsStatus() { m_ownsStatus = false; }

    bool isRetained() const { return m_isRetained || m_isHeldByLoader; }
    void retain() { m_isRetained = true; }
    using AbstractModuleRecord::forgetCachedResolutions;
    void setIsHeldByLoader(bool isHeldByLoader) { m_isHeldByLoader = isHeldByLoader; }

    unsigned innerModuleEvaluation(JSGlobalObject*, ModuleEvaluationStack&, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise);
    void recordTerminationAfterEvaluation(VM&, Exception*);
    void generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords&);
    void willLoadRequests(ModuleGraphLoadingState&);
    void countLoaderStep();
    void didResumeLoadReentrantly(ModuleGraphLoadingState& load) { m_reentrantlyResumedLoad = &load; }
    bool takeReentrantlyResumedLoad(ModuleGraphLoadingState& load)
    {
        if (m_reentrantlyResumedLoad != &load)
            return false;
        m_reentrantlyResumedLoad = nullptr;
        return true;
    }
    void didLoad();
    static void didFailToLoad(JSGlobalObject*, const ModuleGraphLoadingState&);
    void didReplaceRecordInRegistry();
    static std::optional<ModuleID> findEvaluatingCommonJSModule(JSGlobalObject*, const Identifier& key);
    static bool isModuleBeingTransferred(JSGlobalObject*, const Identifier& key);
    static bool tryDeferLinkStep(JSGlobalObject*, const VM::SynchronousModuleTask&);
    static bool wasRequestedByEnclosingLoad(JSGlobalObject*, const Identifier& key);
    static bool isBlockedOnModuleTransfer(JSGlobalObject*, const Identifier& key, bool& shouldRequeueStep);
    static JSPromise* deferRequest(JSGlobalObject*, ModuleID file, ModuleLoadingContext*, bool useImportMap);
    static void didRequestModule(JSGlobalObject*, const Identifier& key, JSCell* payload);
    static void didSettleLoad(JSGlobalObject*, const ModuleGraphLoadingState&);
    static void didFailToMakeModule(JSGlobalObject*, const Identifier& key);
    static void didDiscardModuleError(JSGlobalObject*, const Identifier& key);
    void willBeLoadedByImport();
    bool takeMainModuleLoadStep();
    bool isLoadedByLoader();
    bool importsEvaluatingCommonJSModule();
    bool takeMainPromiseSplit();
    JSPromise* evaluateMainModule(JSPromise* evaluation);
    static void didEvaluateModulesInJob(JSGlobalObject*);
    static void didFailModulesInJob(JSGlobalObject*);
    void setExtraLoadSteps(uint32_t steps) { m_extraLoadSteps = steps; }
    bool takeExtraLoadStep() { return m_extraLoadSteps && m_extraLoadSteps--; }
    bool tryDeferForEarlierImports(JSCell* payload, bool isDeferred);
    void adoptStatusIfNeeded();
    bool isEvaluatingCommonJSModule();
    bool keepsToModule() const { return m_keepsToModule; }
    bool takesNamesFromModuleItself();
    void setKeepsToModule(bool keepsToModule = true) { m_keepsToModule = keepsToModule; }
    void setRunsWhenLoaded() { m_runsWhenLoaded = true; }
    bool runsWhenLoaded() const { return m_runsWhenLoaded; }
    void setRunsInItsTurn() { m_runsInItsTurn = true; }
    bool takeRunsInItsTurn() { return std::exchange(m_runsInItsTurn, false); }
    bool markStepQueued(uint32_t synchronousLoad) { return std::exchange(m_synchronousLoadWithQueuedStep, synchronousLoad) != synchronousLoad; }
    bool isRequestedByWalkedModule() const { return m_isRequestedByWalkedModule; }
    void setIsRequestedByWalkedModule() { m_isRequestedByWalkedModule = true; }
    void setHasUnresolvableRequest() { m_hasUnresolvableRequest = true; }
    uint8_t willLoadImportedModule(const Identifier& key);
    static BitVector willRefetchFromHost(JSGlobalObject*);
    static void didRefetchFromHost(JSGlobalObject*, const BitVector& savedHostRequests);
    void didLoadImportedModule(uint8_t flagsBefore);
    static uint8_t willLoadForUncompiledReferrer(JSGlobalObject*);
    static void didLoadForUncompiledReferrer(JSGlobalObject*, uint8_t flagsBefore);
    static void checkAccessFromUncompiledReferrer(JSGlobalObject*, const Identifier& key, const Identifier& requesterKey);
    static void checkAccessOnRegistryHit(JSGlobalObject*, const Identifier& key, const String& requesterPath);
    AbstractModuleRecord* nextRequestedRecord(JSGlobalObject*, const Identifier& key);
    AbstractModuleRecord* findRequestedRecord(const Identifier& key);
    bool tryDeferFetchFulfillment(JSPromise* fetched, AbstractModuleRecord* requested);
    void didRequestCompiledModule(const Identifier& key, ModuleID module) { m_compiledRequests.append({ key.impl(), module }); }
    unsigned innerModuleLinking(JSGlobalObject*, ModuleLinkingStack&, unsigned index, RefPtr<ScriptFetcher>);
    void execute(JSGlobalObject*, JSPromise* capability = nullptr);
    void executeAsync(JSGlobalObject*);

    struct Statement {
        AbstractModuleRecord* target { nullptr };
        Identifier importName;
        ModuleRecord* reexporter { nullptr };
        bool isStarExportEntry { false };
        bool claimsWhatStarsFind { false };
    };
    Resolution resolveOwnExport(JSGlobalObject*, const Identifier& exportName, StarExportTargets&, ModuleRecord* reexporter, Statement&);
    Resolution claimAsOwn(const Identifier& exportName);
    struct Asker {
        ModuleRecord* record;
        UniquedStringImpl* name;
    };
    bool originalShows(JSGlobalObject*, const Identifier& exportName, std::span<const Asker>);
    bool originalShows(JSGlobalObject*, const Identifier& exportName, std::span<ModuleRecord* const> askersOfSameName);
    bool isNamedByEndedMockInFrontOfItsStar(JSGlobalObject*, const Identifier& exportName, unsigned index);
    bool resolvesPerReexporter() const;
    bool hasRemovedMockInUse() const;
    bool hasAnyActiveMock() const;
    bool mayOutlive(AbstractModuleRecord*) const;
    const Resolution* storedResolution(UniquedStringImpl* exportName);
    void storeResolution(UniquedStringImpl* exportName, const Resolution&);
    bool isReplacedByMock() const;
    void collectExportedNames(JSGlobalObject*, WTF::Vector<Identifier>&, StarExportTargets&);
    JSValue readExport(JSGlobalObject*, const Identifier& exportName, std::optional<unsigned> exportIndex);
    JSObject* moduleNamespaceObject(JSGlobalObject*, ModulePhase);
    static bool isSameBinding(const Resolution&, const Resolution&);
    static bool throwIfExportWasOmitted(JSGlobalObject*, AbstractModuleRecord* importer, AbstractModuleRecord* exporter, const Identifier& name);

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
    Vector<std::pair<UniquedStringImpl*, ModuleID>, 2> m_compiledRequests;
    unsigned m_nextCompiledRequest { 0 };
    ModuleGraphLoadingState* m_reentrantlyResumedLoad { nullptr };
    uint32_t m_extraLoadSteps { 0 };
    bool m_ownsStatus { false };
    bool m_isRetained { false };
    bool m_isHeldByLoader { false };
    bool m_runsWhenLoaded { false };
    bool m_runsInItsTurn { false };
    uint32_t m_synchronousLoadWithQueuedStep { std::numeric_limits<uint32_t>::max() };
    bool m_isRequestedByWalkedModule { false };
    bool m_hasUnresolvableRequest { false };
    bool m_keepsToModule { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
