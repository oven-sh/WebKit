/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSObject.h"

namespace JSC { namespace AOT {

class PoisonedCell final : public JSObject {
public:
    using Base = JSObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | OverridesGetPrototype | OverridesIsExtensible | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | ProhibitsPropertyCaching | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;
    DECLARE_VISIT_CHILDREN;
    static size_t estimatedSize(JSCell*, VM&);

    static Structure* createStructure(VM&);

    static bool tryPoison(VM&, JSCell*, uint32_t function, uint32_t position);
    static constexpr uint32_t isLentForCallAhead = 1u << 31;
    static constexpr uint32_t isFrameLaidForCalleeOfEngine = 1u << 30;
    static void noteThatCellIsLent(JSCell*, const void* frame, uint32_t function, uint32_t position);
    static bool someCellIsLent() { return s_numberOfCellsLent; }
    static uint32_t poisonCellsLentBelow(VM&, const void* frame, bool andByThatFrame);
    [[noreturn]] static void stop(const JSCell*);

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool isExtensible(JSObject*, JSGlobalObject*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);
    static JSValue getPrototype(JSObject*, JSGlobalObject*);
    static CallData getCallData(JSCell*);
    static CallData getConstructData(JSCell*);

private:
    static inline thread_local uint32_t s_numberOfCellsLent { 0 };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
