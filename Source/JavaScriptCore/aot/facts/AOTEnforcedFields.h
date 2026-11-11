/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTFamilies.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTTypeArena.h"
#include "JSExportMacros.h"

namespace JSC { namespace AOT { namespace Typed {

class Program;
struct FactTables;
struct LinkTables;

enum class SourceOfSlotType : uint8_t { Nothing, Compiler, Written, WrittenInFamily, Proven, Seen };

struct EnforcedField {
    TypeID type;
    SourceOfSlotType source { SourceOfSlotType::Nothing };
    bool isInt32ByGuess { false };
    bool wasTakenBack { false };
    bool holdsUndefinedAndNull { false };
};

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
    BitVector m_leadsToInstance;
};

JS_EXPORT_PRIVATE EnforcedField enforcedTypeOfField(const Program&, const LinkTables& linked, const LinkTables& held, const Families&, const FamiliesOfInstances&, TypeArena&, FieldReference);

bool slotsOfLayoutMayBeHeldToTypes(const Program&, const FamiliesOfInstances&, LayoutID);

JS_EXPORT_PRIVATE TypeID contentsOfEnforcedSlot(TypeArena&, TypeID slotType, bool holdsUndefinedAndNull);

JS_EXPORT_PRIVATE bool enforcedTypeSparesTestOfStoresByName(const TypeArena&, TypeID slotType);

uint64_t fillEnforcedFieldsForFacts(Program&, const LinkTables& linked, const LinkTables& held, const Families&, Mechanisms, FactTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
