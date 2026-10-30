/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"

namespace JSC {

class JSCellButterfly;
class VM;

namespace AOT {

class ClassToken {
public:
    static constexpr unsigned classIndexSlot = 0;
    static constexpr unsigned brandFieldSlot = 1;
    static constexpr unsigned knownStructuresSlot = 2;
    static constexpr unsigned structuresWithoutNamesSlot = 3;
    static constexpr unsigned firstNameSlot = 4;

    // Where a private name of this evaluation of the class lies in an object that is no compiled object: an instance of a class with a built-in parent, an object that a constructor returned, or the extra properties of an instance of a class whose layout lacks the name.
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

    // How such an object takes a private name of this evaluation of the class: its Structure leads on to another by adding the name.
    enum DefinitionSlot : unsigned {
        // The number is written last. Compiled code compares it with the number in the header of the object.
        NumberOfStructureBefore,
        StructureAfter,
        // Two halves of 16 bits. The lower one says where the value goes: a location as above, or, with its lowest bit set, twice the capacity of the new storage, whose first free place takes the value.
        // The upper one is a location of the brand as above.
        Places,
        KeptStructureBefore,
    };
    static constexpr unsigned slotsPerDefinition = 4;
    static constexpr unsigned definitionsPerField = 4;

    // The records of a field lie together, the definitions behind the known Structures.
    static constexpr unsigned slotsPerField = recordsPerField * slotsPerRecord + definitionsPerField * slotsPerDefinition;
    static constexpr unsigned slotOfRecord(uint32_t field, unsigned record) { return field * slotsPerField + record * slotsPerRecord; }
    static constexpr unsigned slotOfDefinition(uint32_t field, unsigned definition) { return slotOfRecord(field, recordsPerField) + definition * slotsPerDefinition; }

    static JSCellButterfly* create(VM&, const LayoutData& classLayout);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
