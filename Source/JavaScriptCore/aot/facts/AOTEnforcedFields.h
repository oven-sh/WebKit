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
#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>

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

class WrittenTypesOfFamilies {
    WTF_MAKE_NONCOPYABLE(WrittenTypesOfFamilies);
public:
    JS_EXPORT_PRIVATE WrittenTypesOfFamilies(const Program&, const Families&, TypeArena&);

    TypeID find(LayoutID, PropertyKeyID);

private:
    struct WrittenTypes {
        Vector<TypeID, 1> types;
        TypeID united;
    };

    void listMembersOf(FamilyID);

    const Program& m_program;
    const Families& m_families;
    TypeArena& m_types;
    BitVector m_isListed;
    UncheckedKeyHashMap<uint64_t, WrittenTypes, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_writtenTypes;
};

JS_EXPORT_PRIVATE EnforcedField enforcedTypeOfField(const Program&, const LinkTables& linked, const LinkTables& held, const Families&, const FamiliesOfInstances&, WrittenTypesOfFamilies&, TypeArena&, FieldReference);

bool slotsOfLayoutMayBeHeldToTypes(const Program&, const FamiliesOfInstances&, LayoutID);

JS_EXPORT_PRIVATE TypeID contentsOfEnforcedSlot(TypeArena&, TypeID slotType, bool holdsUndefinedAndNull);

JS_EXPORT_PRIVATE bool enforcedTypeSparesTestOfStoresByName(const TypeArena&, TypeID slotType);

bool isLibraryLayout(const Program&, LayoutID);

struct EnforcedFieldCounts {
    uint64_t seen { 0 };
    uint64_t ofFamily { 0 };
};
EnforcedFieldCounts fillEnforcedFieldsForFacts(Program&, const LinkTables& linked, const LinkTables& held, const Families&, Mechanisms, FactTables&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
