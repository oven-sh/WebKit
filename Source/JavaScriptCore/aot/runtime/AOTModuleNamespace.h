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
    static ModuleNamespace* create(VM&, Structure*, ModuleID, ModulePhase, const DynamicExports&);

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;

    ModuleID module() const { return m_module; }
    bool isDeferred() const { return m_isDeferred; }
    bool isAnsweredByMock() const;
    JSObject* activeMock() const;
    void setKeepsToModule(bool keepsToModule) { m_keepsToModule = keepsToModule; }
    bool keepsToModule() const { return m_keepsToModule; }
    void setMockOfView(VM& vm, JSObject* mock) { m_mockOfView.set(vm, this, mock); }
    void clearMockOfView() { m_mockOfView.clear(); }
    AbstractModuleRecord* recordForResolution();
    bool hasNamespaceLayoutOf(ModuleID module) const { return (m_module == module || m_facade == module) && !m_isDeferred && !m_mockOfView; }
    void setFacade(ModuleID facade) { m_facade = facade; }
    std::optional<unsigned> find(UniquedStringImpl*) const;
    std::optional<unsigned> findWhetherShownOrNot(UniquedStringImpl*) const;
    bool hasExport(UniquedStringImpl* name) const { return findWhetherShownOrNot(name) || isDynamicExport(name); }
    bool tryReadExports(JSGlobalObject*, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values);
    void refuseToShowExport(PropertyName);

    void replaceCapturedValue(JSGlobalObject*, const Identifier& name, JSValue);
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
    ModuleID m_module;
    ModuleID m_facade;
    bool m_isDeferred;
    bool m_mayNeedEvaluation;
    bool m_hasESModuleMarker { false };
    bool m_isMarkerDeferred { false };
    bool m_hasOwnStructure { false };
    bool m_keepsToModule { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
