/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVector.h"
#include "CyclicModuleRecord.h"
#include <wtf/HashSet.h>
#include <wtf/OrderedHashSet.h>

namespace JSC {

class ModuleGraphLoadingState;

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
    JSGlobalObject* instanceRealm() const;

    bool isOnStack(const ModuleEvaluationStack& stack) const { return m_evaluationStack == &stack; }
    void setEvaluationStack(const ModuleEvaluationStack* stack) { m_evaluationStack = stack; }

    // Until it does, the table of the modules has the status, and the record only stands for the module where a name is resolved.
    bool holdsStatus() const { return m_holdsStatus; }
    void setHoldsStatus() { m_holdsStatus = true; }
    void clearHoldsStatus() { m_holdsStatus = false; }

    bool isRetained() const { return m_isRetained; }
    void retain() { m_isRetained = true; }

    unsigned innerModuleEvaluation(JSGlobalObject*, ModuleEvaluationStack&, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise);
    void generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords&);
    void willLoadRequests();
    void didLoad();
    static void didFailToLoad(const ModuleGraphLoadingState&);
    void didReplaceRecordInRegistry();
    bool takeStepOfMainModule();
    void takeStatus();
    bool isRunningCommonJSFile();
    void setRunsWhenLoaded() { m_runsWhenLoaded = true; }
    bool isRequestedByWalkedModule() const { return m_isRequestedByWalkedModule; }
    void setIsRequestedByWalkedModule() { m_isRequestedByWalkedModule = true; }
    // Around a request that the loader makes for the module. The first returns what the second takes.
    uint8_t willAskHostForRequest();
    void didAskHostForRequest(uint8_t flagsBefore);
    unsigned innerModuleLinking(JSGlobalObject*, ModuleLinkingStack&, unsigned index, RefPtr<ScriptFetcher>);
    void execute(JSGlobalObject*, JSPromise* capability = nullptr);
    void executeAsync(JSGlobalObject*);

    // What the module itself says of the name. The search goes on in the targets: they are those of the `export *` whose names the build did not see.
    Resolution resolveOwnExport(JSGlobalObject*, const Identifier& exportName, StarExportTargets&);
    void collectExportedNames(JSGlobalObject*, WTF::Vector<Identifier>&, StarExportTargets&);
    JSValue readExport(JSGlobalObject*, const Identifier& exportName, std::optional<unsigned> exportIndex);
    JSObject* moduleNamespaceObject(JSGlobalObject*, ModulePhase);
    static bool isSameBinding(const Resolution&, const Resolution&);

    void gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen);
    bool readyForSyncExecution(VisitedModuleRecords& seen);

private:
    ModuleRecord(Instance&, Structure*, JSModuleLoader*, const Identifier& moduleKey, ModuleID);
    Modules& modules() const;
    void appendDynamicStarTargets(StarExportTargets&);

    Instance& m_instance;
    ModuleID m_module;
    const ModuleEvaluationStack* m_evaluationStack { nullptr };
    Vector<UniquedStringImpl*, 2> m_resolveSet;
    bool m_holdsStatus { false };
    bool m_isRetained { false };
    bool m_runsWhenLoaded { false };
    bool m_isRequestedByWalkedModule { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
