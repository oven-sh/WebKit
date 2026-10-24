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

    static JSCellButterfly* create(VM&, const LayoutData& classLayout);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
