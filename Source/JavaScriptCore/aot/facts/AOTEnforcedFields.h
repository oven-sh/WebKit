/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTTypeArena.h"
#include "JSExportMacros.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct FactTables;
struct LinkTables;

// WHAT A SLOT OF AN OBJECT IS HELD TO. It is no proof and needs none: the record of the layout has the type (FieldData::type), and whoever stores goes by it, so a slot is empty or holds a value of it.
// ANY TYPE WOULD BE SAFE. A type that stores break costs speed, by the object: there the field leaves its slot. So it is what the program WRITES in checked code, which no flow decides.
// WHAT IS ONLY GUESSED IS NEVER HELD: its slot is boxed, takes every value, and its stores test nothing.
enum class SourceOfSlotType : uint8_t { Nothing, Compiler, Written, WrittenInFamily, Proven };

struct EnforcedField {
    // Null: anything. The slot is boxed and its stores test nothing.
    TypeID type;
    SourceOfSlotType source { SourceOfSlotType::Nothing };
    bool isInt32ByGuess { false };
    // Something guarded was written, and what was seen to arrive gave it up.
    bool wasTakenBack { false };
    // FieldFact::SlotHoldsUndefinedAndNull: the two lie in the slot as markers, whether or not `type` lists them.
    bool holdsUndefinedAndNull { false };
};

// WHICH LAYOUTS SHARE A FAMILY WITH AN OBJECT OF A CLASS OR OF A CONSTRUCTOR FUNCTION. One look over the layouts and two over the families, whatever the program, and then one bit a question.
// Asked member by member it was a square: n literals in one family without an instance (11-04 00:41 to 01:08).
class FamiliesOfInstances {
public:
    JS_EXPORT_PRIVATE FamiliesOfInstances(const Program&, const Families&);

    bool sharesFamilyWithInstance(LayoutID id) const
    {
        FamilyID nearest = m_families.nearestFamilyOf(id);
        return nearest && m_leadsToInstance.quickGet(nearest.index());
    }

private:
    const Families& m_families;
    // Of a family: it, or a family that holds it, has an instance among its members.
    BitVector m_leadsToInstance;
};

// Of one field as its layout lists it: declared, or appended as the plan numbers them (LinkTables::appendedKeysOfPlan() of `linked`). In the words of the run time's test. It reads records and both tables, and keeps nothing.
JS_EXPORT_PRIVATE EnforcedField enforcedTypeOfField(const Program&, const LinkTables& linked, const LinkTables& held, const Families&, const FamiliesOfInstances&, TypeArena&, FieldReference);

// False: no slot of an object of the layout is held to anything, whatever is written of its fields.
bool slotsOfLayoutMayBeHeldToTypes(const Program&, const LinkTables& linked, const LinkTables& held, const FamiliesOfInstances&, LayoutID);

// All that lies in the slot while the field is in it: the type with the two markers. PlacedSlot::type IS THIS, NEVER FieldFacts::slotType BY ITSELF: whoever reads a filled slot takes the plan's type for all that he may find there.
JS_EXPORT_PRIVATE TypeID contentsOfEnforcedSlot(TypeArena&, TypeID slotType, bool holdsUndefinedAndNull);

// The type takes all that some raw form holds (every int32, every number, both booleans), so in that form a store by name needs no test of the type: FieldRecordIndex::isStoredWithoutTypeTest. So does no type at all.
JS_EXPORT_PRIVATE bool enforcedTypeSparesTestOfStoresByName(const TypeArena&, TypeID slotType);

// AOTFactsEnforcedFields.cpp. Writes FieldFacts::slotType, FieldFact::IsEnforced and FieldFact::SlotHoldsUndefinedAndNull, one answer a declaration, and gives the table a row for each field that the plan numbers.
// Withheld with TypedSlots: nothing is written, and every slot takes every value.
void fillEnforcedFieldsForFacts(Program&, const LinkTables& linked, const LinkTables& held, const Families&, Mechanisms, FactTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
