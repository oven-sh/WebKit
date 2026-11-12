/*
 * Copyright (C) 2015-2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "AbstractModuleRecord.h"
#include "JSDestructibleObject.h"
#include <wtf/OrderedHashMap.h>
#include <wtf/ScopedLambda.h>

namespace JSC {

#if USE(BUN_JSC_ADDITIONS)
class JSArray;

enum class ExportOverrideEnd : uint8_t {
    NotOverridden, // The owner shows no value in this binding.
    HandedOver, // An owner that stored later keeps showing its value. It has now found what this owner had found.
    Restored, // The binding has what the owner had found.
    HadNoValue, // The owner had found no value. The binding keeps the owner's value, because it cannot go back to having none.
};

// The owners that override one binding of a module, for overrides that their makers end (a module mock, a spy on a namespace). An owner is any cell
// that its maker keeps for as long as the override.
//
// The owners end in any order. The one whose value the binding shows gives back what it found. Any other hands that to the owner that stored next, which
// keeps showing its value. An owner that stores again shows its value again: it comes to stand above the owners that have stored meanwhile.
// An owner that is set aside ends in the same way but keeps its place: its next value shows from there, under the owners that stored after it.
//
// `table` is an object that its holder keeps for all of its bindings. It has two elements for the binding that `binding` numbers:
//   - none, where no owner overrides the binding;
//   - the owner and what it found (no element where the binding had no value yet), for one owner that is not set aside. No cell is made for it;
//   - an array and the table itself, for the rest. The array has three elements for each owner, in the order in which the owners stored: the owner,
//     what it found (the array itself where the binding had no value yet), and whether it is set aside.
class ExportOverrideLayers {
public:
    ExportOverrideLayers(JSGlobalObject*, JSObject* table, unsigned binding, const ScopedLambda<JSValue()>& read, const ScopedLambda<void(JSValue)>& store);

    void lay(JSCell* owner, JSValue);
    ExportOverrideEnd end(JSCell* owner, bool staysAside, JSCell** ownerAbove = nullptr);
    // False if the owner shows no value in the binding. `found` is empty where the owner found no value.
    bool found(JSCell* owner, JSValue& found);
    // The binding has got a value from elsewhere after the owner found none: that value is what the owner found.
    void didInitialize(JSCell* owner);

private:
    static constexpr unsigned fieldsOfLayer = 3;
    enum Field : unsigned { Owner, Found, IsAside };

    JSValue element(unsigned);
    void setElement(unsigned, JSValue);
    void clearElements();
    // The one owner of the binding, where the table has no array for it.
    JSCell* onlyOwner();
    // Makes the array for a binding that had one owner.
    void makeLayers();

    unsigned numberOfLayers();
    JSValue field(unsigned layer, Field);
    void setField(unsigned layer, Field, JSValue);
    JSValue foundBy(unsigned layer);
    void setFoundBy(unsigned layer, JSValue);
    std::optional<unsigned> layerOf(JSCell* owner);
    // The next owner above `layer` that is not set aside.
    std::optional<unsigned> standingAbove(unsigned layer);
    void putLayer(unsigned layer, JSValue owner, JSValue found);
    void moveToTop(unsigned layer, JSValue found);

    JSGlobalObject* m_globalObject;
    JSObject* m_table;
    unsigned m_index;
    JSArray* m_layers { nullptr };
    const ScopedLambda<JSValue()>& m_read;
    const ScopedLambda<void(JSValue)>& m_store;
};
#endif

class JSModuleNamespaceObject final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | GetOwnPropertySlotIsImpureForPropertyAbsence | IsImmutablePrototypeExoticObject;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;
    static void destroy(JSCell*);

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.moduleNamespaceObjectSpace<mode>();
    }

    static JSModuleNamespaceObject* create(JSGlobalObject* globalObject, Structure* structure, AbstractModuleRecord* moduleRecord, Vector<std::pair<Identifier, AbstractModuleRecord::Resolution>>&& resolutions, bool shouldPreventExtensions = true, bool isDeferred = false)
    {
        VM& vm = getVM(globalObject);
        JSModuleNamespaceObject* object = new (NotNull, allocateCell<JSModuleNamespaceObject>(vm)) JSModuleNamespaceObject(vm, structure, moduleRecord, WTF::move(resolutions), isDeferred);
        object->finishCreation(globalObject, shouldPreventExtensions);
        return object;
    }

    JS_EXPORT_PRIVATE static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    JS_EXPORT_PRIVATE static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned propertyName, PropertySlot&);
    JS_EXPORT_PRIVATE static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    JS_EXPORT_PRIVATE static bool putByIndex(JSCell*, JSGlobalObject*, unsigned propertyName, JSValue, bool shouldThrow);
    JS_EXPORT_PRIVATE static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    JS_EXPORT_PRIVATE static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned propertyName);
    JS_EXPORT_PRIVATE static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    JS_EXPORT_PRIVATE static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);

    DECLARE_EXPORT_INFO;

#if USE(BUN_JSC_ADDITIONS)
    bool overrideExportValue(JSGlobalObject*, PropertyName, JSValue);
    void overrideExports(JSGlobalObject* globalObject, const WTF::Function<bool(JSGlobalObject* globalObject, const Identifier& exportName, JSC::JSValue &result)>& iter);

    // An override that its maker ends has an owner: any cell that the maker keeps for as long as the override. The record that declares the binding
    // keeps the owners of each of its bindings: see ExportOverrideLayers.
    using ExportOverrideEnd = JSC::ExportOverrideEnd;
    // False, and nothing is stored, if the export is not a binding of a module environment. overrideExportValue() stores such an export, with no owner.
    bool layExportOverride(JSGlobalObject*, PropertyName, JSValue, JSCell* owner);
    ExportOverrideEnd removeExportOverride(JSGlobalObject* globalObject, PropertyName name, JSCell* owner, JSCell** ownerAbove = nullptr) { return endExportOverride(globalObject, name, owner, false, ownerAbove); }
    ExportOverrideEnd setExportOverrideAside(JSGlobalObject* globalObject, PropertyName name, JSCell* owner, JSCell** ownerAbove = nullptr) { return endExportOverride(globalObject, name, owner, true, ownerAbove); }
    // False if the owner shows no value in this binding. `found` is empty where the owner found no value.
    bool exportFoundByOverride(JSGlobalObject*, PropertyName, JSCell* owner, JSValue& found);
    // The module has given the binding a value after an owner found none: that value is what the owner found.
    void didInitializeExportUnderOverride(JSGlobalObject*, PropertyName, JSCell* owner);
#endif
    DECLARE_VISIT_CHILDREN;

    inline static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    AbstractModuleRecord* moduleRecord() LIFETIME_BOUND { return m_moduleRecord.get(); }

#if USE(BUN_JSC_ADDITIONS)
    WTF::TriState m_hasESModuleMarker = WTF::TriState::Indeterminate;
#endif

private:
    JS_EXPORT_PRIVATE JSModuleNamespaceObject(VM&, Structure*, AbstractModuleRecord*, Vector<std::pair<Identifier, AbstractModuleRecord::Resolution>>&&, bool isDeferred);
    JS_EXPORT_PRIVATE void finishCreation(JSGlobalObject*, bool shouldPreventExtensions);
    bool getOwnPropertySlotCommon(JSGlobalObject*, PropertyName, PropertySlot&);
#if USE(BUN_JSC_ADDITIONS)
    ExportOverrideEnd endExportOverride(JSGlobalObject*, PropertyName, JSCell* owner, bool staysAside, JSCell** ownerAbove);
    bool getOwnPropertySlotForLiveExports(JSGlobalObject*, PropertyName, PropertySlot&);
    std::optional<bool> getOwnPropertySlotIfDeferredOrLive(JSGlobalObject*, PropertyName, PropertySlot&);
    JSObject* liveExportsSource() const { return m_isDeferredOrLive && m_moduleRecord->hasLiveExports() ? m_moduleRecord->liveExportsSource() : nullptr; }
#endif
    ALWAYS_INLINE bool isSymbolLikeNamespaceKey(VM&, PropertyName);
    void ensureDeferredNamespaceEvaluation(JSGlobalObject*);

    struct ExportEntry {
        Identifier localName;
        WriteBarrier<AbstractModuleRecord> moduleRecord;
#if ENABLE(AOT)
        std::optional<unsigned> compiledExportIndex { };
#endif
    };

    using ExportMap = OrderedHashMap<RefPtr<UniquedStringImpl>, ExportEntry, IdentifierRepHash, HashTraits<RefPtr<UniquedStringImpl>>>;

    ExportMap m_exports;
    WriteBarrier<AbstractModuleRecord> m_moduleRecord;
    const bool m_isDeferred;
#if USE(BUN_JSC_ADDITIONS)
    bool m_isOverridingValue = false;
    // m_isDeferred, or the record or one it re-exports from has live exports. A record has them from the time it is created.
    // Where upstream tests m_isDeferred, this is tested instead. So any other namespace object does no more work than upstream's.
    bool m_isDeferredOrLive;
#endif

    friend size_t cellSize(JSCell*);
};

} // namespace JSC
