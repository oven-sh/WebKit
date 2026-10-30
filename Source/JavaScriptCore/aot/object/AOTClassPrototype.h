/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreshold.h"
#include "JSObject.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT {

class ClassPrototype final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | GetOwnPropertySlotIsImpure | GetOwnPropertySlotIsImpureForPropertyAbsence;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;
    static constexpr unsigned inlineCapacity = 2;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    // memberRow: where the row of the class begins in the field table, or ClassRowEntry::noRow.
    static ClassPrototype* create(VM&, Structure*, uint32_t classIndex, uint32_t memberRow, unsigned imagePosition);

    DECLARE_INFO;

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool putByIndex(JSCell*, JSGlobalObject*, unsigned, JSValue, bool shouldThrow);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static bool deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned);
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow);
    static bool preventExtensions(JSObject*, JSGlobalObject*);
    static bool setPrototype(JSObject*, JSGlobalObject*, JSValue, bool shouldThrowIfCantSet);

    static void ensureMembers(JSObject* holder)
    {
        if (holder->classInfo() == info()) [[unlikely]]
            createMembers(holder);
    }

    void convertToOrdinaryObject(VM&, Structure* ordinary);

    static bool hasMember(JSObject* classPrototype, PropertyName);
    uint32_t classIndex() const { return m_classIndex & maxClassIndex; }
    // Image::position() of the image that has the class.
    unsigned imagePosition() const { return m_classIndex >> bitsOfClassIndex; }
    static constexpr ptrdiff_t offsetOfClassIndex() { return OBJECT_OFFSETOF(ClassPrototype, m_classIndex); }
    static constexpr unsigned bitsOfClassIndex = 24;
    JSObject* classObject() const;
    static constexpr ptrdiff_t offsetOfMemberRow() { return OBJECT_OFFSETOF(ClassPrototype, m_memberRow); }
    std::optional<bool> tryGetDeclaredMember(VM&, PropertyName, PropertySlot&);

private:
    enum class Flag : uint8_t {
        HasCheckedThen = 1,
        DeclaresThen = 2,
    };
    AOT_CLASS_THRESHOLD(unsigned, minEarlyAnswers, 4);
    static constexpr unsigned numberOfNameBits = 32;
    static constexpr uint32_t maxClassIndex = (1u << bitsOfClassIndex) - 1;

    ClassPrototype(VM& vm, Structure* structure, uint32_t classIndex, uint32_t memberRow, unsigned imagePosition)
        : Base(vm, structure)
        , m_classIndex(classIndex | imagePosition << bitsOfClassIndex)
        , m_memberRow(memberRow)
    {
    }

    static void createMembers(JSObject*);

    uint32_t m_classIndex;
    uint16_t m_numberOfLookups { 0 };
    bool m_hasRepeatedLookup { false };
    OptionSet<Flag> m_flags;
    uint32_t m_namesLookedUp { 0 };
    uint32_t m_memberRow;
    static_assert(numberOfNameBits == sizeof(uint32_t) * 8);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
