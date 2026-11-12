/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTLayoutData.h"

namespace JSC {

class JSCellButterfly;
class JSValue;
class Symbol;
class VM;

namespace AOT {

class Object;
struct State;

class ClassToken {
public:
    static constexpr unsigned classIndexSlot = 0;
    static constexpr unsigned brandFieldSlot = 1;
    static constexpr unsigned knownStructuresSlot = 2;
    static constexpr unsigned structuresWithoutNamesSlot = 3;
    static constexpr unsigned numberOfStructuresWithoutNames = 8;
    static constexpr unsigned shiftOfStructureWithoutName = 4;
    static constexpr unsigned firstNameSlot = 4;

    enum KnownStructureSlot : unsigned {
        StructureNumber,
        LocationOfValue,
        LocationOfBrand,
        KeptStructure,
    };
    static constexpr unsigned slotsPerRecord = 4;
    static constexpr unsigned recordsPerField = 4;
    static constexpr unsigned recordsProbedByCompiledCode = 2;

    enum DefinitionSlot : unsigned {
        NumberOfStructureBefore,
        StructureAfter,
        Places,
        KeptStructureBefore,
    };
    static constexpr unsigned slotsPerDefinition = 4;
    static constexpr unsigned definitionsPerField = 4;

    static constexpr unsigned slotsPerField = recordsPerField * slotsPerRecord + definitionsPerField * slotsPerDefinition;
    static constexpr unsigned slotOfRecord(uint32_t field, unsigned record) { return field * slotsPerField + record * slotsPerRecord; }
    static constexpr unsigned slotOfDefinition(uint32_t field, unsigned definition) { return slotOfRecord(field, recordsPerField) + definition * slotsPerDefinition; }

    static JSCellButterfly* create(VM&, const LayoutData& classLayout);
    static Symbol* tryNameForText(VM&, State&, JSValue token, UniquedStringImpl* declaredName);
    static UniquedStringImpl* declaredNameUnder(VM&, Object& holder, UniquedStringImpl* nameOfEvaluation);
    static UniquedStringImpl* nameBesideFields(VM&, const Object& allocated, const FieldData&, bool holderIsCompiled);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
