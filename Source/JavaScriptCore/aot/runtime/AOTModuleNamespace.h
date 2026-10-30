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
    // A read of a deferred namespace object may evaluate the module, so that object does not have the module's namespace layout.
    // Image::position() of the image that has the module.
    unsigned imagePosition() const { return m_imagePosition; }
    bool hasNamespaceLayoutOf(ModuleID module) const { return (m_module == module || m_face == module) && !m_isDeferred; }
    // The object of a file is also what code gets that asks for the namespace object of the file's face, where that face captures nothing.
    void setFace(ModuleID face) { m_face = face; }
    std::optional<unsigned> find(UniquedStringImpl*) const;
    bool hasExport(UniquedStringImpl* name) const { return find(name) || isDynamicExport(name); }
    // Reads every export once, in the order of the keys, as [[OwnPropertyKeys]] and [[Get]] of each key would, without a lookup by name.
    bool tryReadExports(JSGlobalObject*, Vector<UniquedStringImpl*, 8>& names, MarkedArgumentBuffer& values);

    void replaceCapturedValue(JSGlobalObject*, const Identifier& name, JSValue);

    bool hasESModuleMarker() const { return m_hasESModuleMarker; }
    void setESModuleMarker(bool hasMarker) { m_hasESModuleMarker = hasMarker; }

    bool hasOwnStructure() const { return m_hasOwnStructure; }
    void ensureOwnStructure(VM&);

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
    bool isAmbiguousExport(unsigned index) const;
    bool isSymbolLikeKey(UniquedStringImpl*) const;
    bool isOrdinaryKey(PropertyName) const;
    bool isDynamicExport(UniquedStringImpl*) const;
    JSValue readDynamicExport(JSGlobalObject*, PropertyName);
    bool evaluateIfNeeded(JSGlobalObject*);

    WriteBarrier<JSCellButterfly> m_dynamicExportNames;
    WriteBarrier<JSObject> m_capturedValues;
    WriteBarrier<JSObject> m_liveSources;
    ModuleID m_module;
    ModuleID m_face;
    uint8_t m_imagePosition;
    bool m_isDeferred;
    bool m_mayNeedEvaluation;
    bool m_hasESModuleMarker { false };
    bool m_hasOwnStructure { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
