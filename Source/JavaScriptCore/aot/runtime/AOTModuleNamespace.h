/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTVector.h"
#include "AbstractModuleRecord.h"
#include "ArgList.h"
#include "JSCellButterfly.h"
#include "JSObject.h"

namespace JSC { namespace AOT {

class Environment;
class Modules;
struct ModuleExportData;

class ModuleNamespace final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    using ModulePhase = AbstractModuleRecord::ModulePhase;

    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | GetOwnPropertySlotIsImpure | GetOwnPropertySlotIsImpureForPropertyAbsence | IsImmutablePrototypeExoticObject;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    struct DynamicExports {
        JSCellButterfly* names { nullptr };
        JSObject* capturedValues { nullptr };
        JSObject* liveSources { nullptr };
    };

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype, bool shouldPreventExtensions);
    static ModuleNamespace* create(VM&, Structure*, ModuleID, ModulePhase, const DynamicExports&, Environment* moduleInstance = nullptr);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    ModuleID module() const { return m_module; }
    bool isDeferred() const { return m_isDeferred; }
    bool isAnsweredByMock() const;
    JSObject* activeMock() const;
    void setKeepsToModule(bool);
    bool keepsToModule() const;
    void setMockOfView(VM&, JSObject*);
    void clearMockOfView();
    JSObject* mockOfView() const { return m_mockOfView.get(); }
    void setBelongsToInstance();
    void setMockWord() { m_isMockWord = true; }
    bool isLoadWord() const { return m_isLoadWord; }
    bool isReleasedLoad() const { return m_isReleasedLoad; }
    void setLoadWord() { m_isLoadWord = m_isMockWord = true; }
    void releaseLoad(VM&, JSObject* emptyTable);
    bool isFilled() const { return m_isFilled; }
    void setFilled() { m_isFilled = true; }
    JSObject* linkedExports() const
    {
        if (!m_isLoadWord)
            return activeMock();
        return m_isReleasedLoad ? m_capturedValues.get() : nullptr;
    }
    void setUninitializedMock(bool isUninitialized) { m_isUninitializedMock = isUninitialized; }
    bool isUninitializedMock() const { return m_isUninitializedMock && !linkedExports(); }
    bool belongsToInstance() const { return m_belongsToInstance; }
    bool isRestoredAfterPatch() const { return m_isRestoredAfterPatch; }
    void setRestoredAfterPatch(bool isRestored) { m_isRestoredAfterPatch = isRestored; }
    void setKeepsOriginalNames(bool keepsOriginalNames) { m_keepsOriginalNames = keepsOriginalNames; }
    bool keepsOriginalNames() const { return m_keepsOriginalNames; }
    void setHiddenExports(VM&, JSCellButterfly*);
    JSObject* namesOfMock() const { return m_mockNames.get(); }
    void setMockNames(VM& vm, JSObject* names) { m_mockNames.set(vm, this, names); }
    AbstractModuleRecord* recordForResolution();
    bool hasNamespaceLayoutOf(ModuleID module) const { return (m_module == module || m_facade == module) && !m_isDeferred && !m_mockOfView; }
    void setFacade(ModuleID facade) { m_facade = facade; }
    Environment* bindModuleInstance();
    bool needsModuleInstance() const { return m_hasInstances && !m_moduleInstance; }
    Environment* boundModuleInstance() const;
    static constexpr ptrdiff_t offsetOfModuleInstance() { return OBJECT_OFFSETOF(ModuleNamespace, m_moduleInstance); }
    std::optional<unsigned> find(UniquedStringImpl*) const;
    std::optional<unsigned> findWhetherShownOrNot(UniquedStringImpl*) const;
    bool hasExport(UniquedStringImpl* name) const { return findWhetherShownOrNot(name) || isDynamicExport(name); }
    bool tryReadExports(JSGlobalObject*, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values);
    void refuseToShowExport(PropertyName);

    void replaceCapturedValue(JSGlobalObject*, const Identifier& name, JSValue);
    DynamicExports dynamicExports() const { return { m_dynamicExportNames.get(), m_capturedValues.get(), m_liveSources.get() }; }
    void replaceDynamicExports(VM&, const DynamicExports&);
    JSValue capturedValueWithoutAllocating(UniquedStringImpl*) const;

    bool showsESModuleMarker() const { return m_hasESModuleMarker || (m_isMarkerDeferred && isDeferredMarkerReady()); }
    void setESModuleMarker(bool hasMarker) { m_hasESModuleMarker = hasMarker; }
    void setMarkerDeferred(bool isDeferred) { m_isMarkerDeferred = isDeferred; }
    void storeESModuleMarker(bool hasMarker);

    bool hasOwnStructure() const { return m_hasOwnStructure; }
    void ensureOwnStructure(VM&);
    void leaveStructureThatCachesKnow(VM&);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

private:
    ModuleNamespace(VM&, Structure*, ModuleID, ModulePhase, const DynamicExports&);

    Modules& modules() const;
    std::span<const ModuleExportData> exports() const;
    bool isHiddenExport(unsigned index) const;
    void didChangeMock();
    bool isSymbolLikeKey(UniquedStringImpl*) const;
    bool isOrdinaryKey(PropertyName) const;
    bool isDynamicExport(UniquedStringImpl*) const;
    JSValue readDynamicExport(JSGlobalObject*, PropertyName);
    bool getOwnPropertySlotOfMock(JSGlobalObject*, JSObject* mock, PropertyName, PropertySlot&);
    bool evaluateIfNeeded(JSGlobalObject*);
    bool isDeferredMarkerReady() const;

    WriteBarrier<JSCellButterfly> m_dynamicExportNames;
    WriteBarrier<JSObject> m_capturedValues;
    WriteBarrier<JSObject> m_liveSources;
    WriteBarrier<JSObject> m_mockOfView;
    WriteBarrier<JSCell> m_moduleInstance;
    WriteBarrier<JSCellButterfly> m_hiddenExports;
    WriteBarrier<JSObject> m_mockNames;
    ModuleID m_module;
    ModuleID m_facade;
    bool m_isDeferred;
    bool m_mayNeedEvaluation;
    bool m_hasInstances;
    bool m_wasMadeUnderMock;
    bool m_hasESModuleMarker { false };
    bool m_isMarkerDeferred { false };
    bool m_hasOwnStructure { false };
    bool m_keepsToModule { false };
    bool m_belongsToInstance { false };
    bool m_isMockWord { false };
    bool m_isLoadWord { false };
    bool m_isUninitializedMock { false };
    bool m_isReleasedLoad { false };
    bool m_isFilled { false };
    bool m_isRestoredAfterPatch { false };
    bool m_keepsOriginalNames { false };
    bool m_hasHiddenExports { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
