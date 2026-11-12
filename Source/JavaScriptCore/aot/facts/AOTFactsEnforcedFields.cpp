/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnforcedFields.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTIRProgram.h"

namespace JSC { namespace AOT { namespace Typed {

static void giveRowsToFieldsThatPlanNumbersForFactsOfEnforcedFields(const LinkTables& linked, FactTables& tables)
{
    if (linked.keysAppendedByHeldAlone.isEmpty())
        return;
    size_t numberOfLayouts = tables.firstFieldOfLayout.size() - 1;
    Vector<uint32_t> firstFieldOfLayout;
    Vector<FieldFacts> fields;
    firstFieldOfLayout.reserveInitialCapacity(numberOfLayouts + 1);
    fields.reserveInitialCapacity(tables.fields.size() + linked.keysAppendedByHeldAlone.size());
    for (size_t index = 0; index < numberOfLayouts; ++index) {
        firstFieldOfLayout.append(static_cast<uint32_t>(fields.size()));
        fields.append(tables.fields.span().subspan(tables.firstFieldOfLayout[index], tables.firstFieldOfLayout[index + 1] - tables.firstFieldOfLayout[index]));
        for (size_t left = linked.keysAppendedByHeldAloneOf(LayoutID(index)).size(); left--;)
            fields.append(FieldFacts { });
    }
    firstFieldOfLayout.append(static_cast<uint32_t>(fields.size()));
    tables.firstFieldOfLayout = WTF::move(firstFieldOfLayout);
    tables.fields = WTF::move(fields);
}

static bool hasOrSharesSlotsForFactsOfEnforcedFields(const Program& program, const LinkTables& linked, LayoutID id)
{
    const Layout& layout = program.at(id);
    bool mayBeAllocated = !layout.has(LayoutFlags::IsNeverAllocated) && linked.layouts[id.index()].isAllocated;
    return mayBeAllocated || writtenBaseOf(layout) || !program.layoutsExpectedBelow(id).empty();
}

static void writeForFactsOfEnforcedFields(FieldFacts& facts, TypeID slotType, bool holdsUndefinedAndNull)
{
    facts.slotType = slotType;
    facts.facts.set(FieldFact::IsEnforced, !!slotType);
    facts.facts.set(FieldFact::SlotHoldsUndefinedAndNull, holdsUndefinedAndNull);
}

EnforcedFieldCounts fillEnforcedFieldsForFacts(Program& program, const LinkTables& linked, const LinkTables& held, const Families& families, Mechanisms mechanisms, FactTables& tables)
{
    if (!withoutMechanismsMissingDependencies(mechanisms).contains(Mechanism::TypedSlots))
        return { };
    uint32_t numberOfLayouts = static_cast<uint32_t>(program.layouts.size());
    RELEASE_ASSERT(linked.layouts.size() == numberOfLayouts && tables.firstFieldOfLayout.size() == numberOfLayouts + 1);
    giveRowsToFieldsThatPlanNumbersForFactsOfEnforcedFields(linked, tables);
    FamiliesOfInstances familiesOfInstances(program, families);

    auto rowOf = [&](FieldReference field) -> FieldFacts& {
        return tables.fields[tables.firstFieldOfLayout[field.layout.index()] + field.field];
    };
    auto isTakenFromAbove = [&](FieldReference field) {
        const Vector<Field>& declared = program.at(field.layout).fields;
        return field.field < declared.size() && declared[field.field].flags.contains(FieldFlags::IsInherited);
    };
    auto hasRowsToWrite = [&](LayoutID id) {
        return hasOrSharesSlotsForFactsOfEnforcedFields(program, linked, id) && slotsOfLayoutMayBeHeldToTypes(program, familiesOfInstances, id);
    };
    for (size_t index : tables.layoutsThatTakeRowsOfBase) {
        LayoutID id(static_cast<uint32_t>(index));
        if (hasRowsToWrite(id) != hasRowsToWrite(writtenBaseOf(program.at(id))))
            tables.layoutsThatTakeRowsOfBase.clear(index);
    }
    auto forEachFieldWithSlot = [&](const Invocable<bool(LayoutID)> auto& takeRowsOfBase, const Invocable<void(FieldReference)> auto& functor) {
        for (uint32_t index = 0; index < numberOfLayouts; ++index) {
            LayoutID id(index);
            if (!hasRowsToWrite(id))
                continue;
            uint32_t numberOfFields = tables.firstFieldOfLayout[index + 1] - tables.firstFieldOfLayout[index];
            for (uint32_t field = 0; field < numberOfFields; ++field) {
                if (field == program.layouts[index].ownFieldCount && takeRowsOfBase(id)) {
                    field = static_cast<uint32_t>(program.layouts[index].fields.size()) - 1;
                    continue;
                }
                functor(FieldReference { id, field });
            }
        }
    };

    TypeArena types(program.types, ArenaKeeps::InProgram);
    EnforcedFieldCounts counts;
    auto isMadeByLibrary = [&](LayoutID id) {
        FunctionID constructor = program.at(id).constructor;
        return constructor && program.at(constructor).role == FunctionRole::Library;
    };
    forEachFieldWithSlot([&](LayoutID id) {
        return tables.layoutsThatTakeRowsOfBase.get(id.index());
    }, [&](FieldReference field) {
        if (isTakenFromAbove(field))
            return;
        EnforcedField answer = enforcedTypeOfField(program, linked, held, families, familiesOfInstances, types, field);
        counts.seen += answer.type && answer.source == SourceOfSlotType::Seen && !isMadeByLibrary(field.layout);
        counts.ofFamily += answer.type && answer.source == SourceOfSlotType::WrittenInFamily && !isLibraryLayout(program, field.layout);
        writeForFactsOfEnforcedFields(rowOf(field), answer.type, answer.holdsUndefinedAndNull);
    });
    forEachFieldWithSlot([&](LayoutID id) {
        if (!tables.layoutsThatTakeRowsOfBase.get(id.index()))
            return false;
        LayoutID base = writtenBaseOf(program.at(id));
        std::ranges::copy(tables.fields.span().subspan(tables.firstFieldOfLayout[base.index()], program.at(base).fields.size()), tables.fields.mutableSpan().subspan(tables.firstFieldOfLayout[id.index()] + program.at(id).ownFieldCount).begin());
        return true;
    }, [&](FieldReference field) {
        if (!isTakenFromAbove(field) || rowOf(field).facts.contains(FieldFact::IsAccessor))
            return;
        const FieldFacts& declaration = rowOf(declarationOf(program, field));
        writeForFactsOfEnforcedFields(rowOf(field), declaration.slotType, declaration.facts.contains(FieldFact::SlotHoldsUndefinedAndNull));
    });
    return counts;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
