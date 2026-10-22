/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFieldTable.h"
#include "AOTProgramTables.h"

namespace JSC { namespace AOT {

// What the prototype object of a class has while it is as the class declares it. A class has a row in the field table, as a shape has: entries[classMemberRows[rank of the class] + name].
// The row is complete: it has an entry for every declared member and for `constructor`, so a name without an entry is absent from the prototype object.
// Only who comes from a class gets to such an entry, so the bits of the form have a meaning of their own there.
// The number that the entry holds has its low bits in the operand and the others beside the entry, in the place of the index of a field's record.
struct ClassRowEntry {
    enum class Form : uint8_t {
        // A method, which is the value of a data property. The number is its cell in the closure table.
        Method,
        // The number is that of the accessor in ProgramTables::classAccessors().
        Accessor,
    };

    static constexpr uint32_t maxNumber = (1u << (FieldTableEntry::operandBits + 15)) - 1;
    // The row of a class whose record does not say all that its prototype object has.
    static constexpr uint32_t noRow = UINT32_MAX;

    static constexpr FieldTableEntry create(uint32_t name, Form form, uint32_t number)
    {
        RELEASE_ASSERT_UNDER_CONSTEXPR_CONTEXT(number <= maxNumber);
        return FieldTableEntry::create(name, static_cast<FieldTableEntry::Form>(form), number & FieldTableEntry::maxOperand);
    }
    static constexpr Form formOf(FieldTableEntry entry) { return static_cast<Form>(entry.form()); }
    static constexpr uint16_t upperBitsOf(uint32_t number) { return static_cast<uint16_t>(number >> FieldTableEntry::operandBits); }
    static constexpr uint32_t numberIn(FieldTableEntry entry, uint16_t upperBits) { return entry.operand() | static_cast<uint32_t>(upperBits) << FieldTableEntry::operandBits; }
};

// The cells of the closure table that have the halves of an accessor. Zero: the accessor has no such half.
struct ClassAccessorData {
    uint32_t getter;
    uint32_t setter;

    friend constexpr bool operator==(const ClassAccessorData&, const ClassAccessorData&) = default;
};
static_assert(sizeof(ClassAccessorData) == 8 && isImageRecord<ClassAccessorData>);
static_assert(!(ClassRowEntry::upperBitsOf(ClassRowEntry::maxNumber) & FieldRecordIndex::isStoredWithoutTypeTest));

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
