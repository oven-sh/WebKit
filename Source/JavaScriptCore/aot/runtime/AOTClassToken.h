/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"
#include "PropertyOffset.h"
#include "StructureID.h"

namespace JSC {

class JSCell;
class JSCellButterfly;
class JSObject;
class Structure;
class Symbol;
class VM;

namespace AOT {

struct State;

class ClassToken {
public:
    static constexpr unsigned classIndexSlot = 0;
    static constexpr unsigned brandFieldSlot = 1;
    static constexpr unsigned knownStructuresSlot = 2;
    static constexpr unsigned firstNameSlot = 3;

    // Where a private name of this evaluation of the class lies in an object that is no compiled object: an instance of a class with a built-in parent, or an object that a constructor returned.
    enum KnownStructureSlot : unsigned {
        // The number is written last. Compiled code compares it with the number in the header of the object.
        StructureNumber,
        // In bytes: above zero from the object, below zero from its butterfly.
        LocationOfValue,
        // Zero where the name alone says which evaluation it belongs to. Elsewhere the object belongs to this evaluation if this token lies there.
        LocationOfBrand,
        KeptStructure,
    };
    static constexpr unsigned slotsPerRecord = 4;
    static constexpr unsigned recordsPerField = 4;
    static constexpr unsigned recordsProbedByCompiledCode = 2;
    static constexpr unsigned slotOfRecord(uint32_t field, unsigned record) { return (field * recordsPerField + record) * slotsPerRecord; }

    struct KnownLocation {
        PropertyOffset value { invalidOffset };
        std::optional<PropertyOffset> brand;
    };
    // The offset of the value, if a record for the object's Structure says that the object has the name and belongs to this evaluation.
    static std::optional<PropertyOffset> findInKnownStructure(JSCell* token, uint32_t field, JSObject*);
    static void makeRoomForKnownStructures(VM&, State&, JSCell* token);
    static void addKnownStructure(VM&, JSCell* token, uint32_t field, Structure*, const KnownLocation&);

    static JSCellButterfly* create(VM&, const LayoutData& classLayout);
    static uint32_t classIndexOf(JSCell* token);
    static const LayoutData& classLayoutOf(State&, JSCell* token);
    // Null for a class that declares no brand of its own. A brand that the layout inherits from a base class does not count.
    static const FieldData* brandOf(State&, const LayoutData& classLayout, JSCell* token);
    static Symbol* privateNameOf(VM&, State&, JSCell* token, uint32_t field);
    static Symbol* privateNameIfCreated(JSCell* token, uint32_t field);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
