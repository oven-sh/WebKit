/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "CyclicModuleRecord.h"
#include <optional>
#include <wtf/HashSet.h>
#include <wtf/OrderedHashSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class Modules;
struct Instance;

using ModuleEvaluationStack = Vector<AbstractModuleRecord*, 8>;
using ModuleLinkingStack = Vector<CyclicModuleRecord*, 8>;
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
    DECLARE_VISIT_CHILDREN;

    ModuleID module() const { return m_module; }
    Instance& instance() const { return m_instance; }
    JSGlobalObject* instanceRealm() const;

    unsigned dfsIndex() const { return m_dfsIndex; }
    void setDFSIndex(unsigned index) { m_dfsIndex = index; }

    bool isOnStack(const ModuleEvaluationStack& stack) const { return m_evaluationStack == &stack; }
    void setEvaluationStack(const ModuleEvaluationStack* stack) { m_evaluationStack = stack; }

    bool isRetained() const { return m_isRetained; }
    void retain() { m_isRetained = true; }

    unsigned innerModuleEvaluation(JSGlobalObject*, ModuleEvaluationStack&, unsigned index, int64_t referrerAsyncOrder, JSPromise* dynamicImportPromise);
    void generateDeferredSyntheticModules(JSGlobalObject*, VisitedModuleRecords&);
    unsigned innerModuleLinking(JSGlobalObject*, ModuleLinkingStack&, unsigned index, RefPtr<ScriptFetcher>);
    void execute(JSGlobalObject*, JSPromise* capability = nullptr);
    void executeAsync(JSGlobalObject*);

    Resolution resolveExport(JSGlobalObject*, const Identifier& exportName);
    void collectExportedNames(JSGlobalObject*, Vector<Identifier>&);
    JSValue readExport(JSGlobalObject*, const Identifier& exportName, std::optional<unsigned> exportIndex);
    JSObject* moduleNamespaceObject(JSGlobalObject*, ModulePhase);
    static bool isSameBinding(const Resolution&, const Resolution&);

    void gatherAsynchronousTransitiveDependencies(OrderedHashSet<AbstractModuleRecord*>& result, VisitedModuleRecords& seen);
    bool readyForSyncExecution(VisitedModuleRecords& seen);

private:
    ModuleRecord(Instance&, Structure*, JSModuleLoader*, const Identifier& moduleKey, ModuleID);
    void finishCreation(JSGlobalObject*, VM&);
    Modules& modules() const;
    Resolution resolveCapturedExport(JSGlobalObject*, const Identifier& exportName);
    Resolution resolveExportEntry(JSGlobalObject*, const Identifier& exportName, unsigned index);

    Instance& m_instance;
    ModuleID m_module;
    unsigned m_dfsIndex { 0 };
    const ModuleEvaluationStack* m_evaluationStack { nullptr };
    Vector<UniquedStringImpl*, 2> m_resolveSet;
    bool m_isInExportStarSet { false };
    bool m_isRetained { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
