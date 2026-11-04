/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `return { scan, getText: () => text, getToken: () => token, .. }`: an object of closures over the scope that made it. Most of them are never read off most such objects, and a call through a field needs the scope and no cell.
// The slot of such a field holds a mark until somebody reads the property as a value (SlotRepresentation::DeferredClosure, PlacedSlot::deferredFunction), and the object keeps the scope once, in a field that no name finds
// (PlannedLayout::environmentOfDeferredClosures). The record of the layout says both, and the stubs and the run time go by it: whoever reads the name gets the one closure of the object, made at the first read.
// WHICH FUNCTION A MARK STANDS FOR is Link's proof of the field: closures of one function, every writer known, none behind the construction. What else might be laid there comes in made, or moves the field out of its slot.
// Withheld with Deferral: no slot holds a mark.

#include "config.h"
#include "AOTClosuresOfLiterals.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTNameClass.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTType.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// A typed access through a family goes to one location in one form. So a layout that stands in a family beside another layout with objects keeps the form that the family agrees on.
static BitVector findLayoutsThatShareSlotsForClosuresOfLiterals(const Program& program, const Families& families, const SlotPlanParts& slots)
{
    BitVector shares(program.layouts.size());
    TypeReader types(program);
    for (uint32_t index = 0; index < families.size(); ++index) {
        std::span<const LayoutID> members = membersOf(types, families.layoutsOf(FamilyID(index)));
        size_t numberInUse = 0;
        for (LayoutID member : members)
            numberInUse += !slots.layoutsNeverAllocated.get(member.index());
        if (numberInUse < 2)
            continue;
        for (LayoutID member : members)
            shares.set(member.index());
    }
    return shares;
}

// The operations of the program make every object of the layout, and each of them makes that layout and no other.
static bool isMadeByOperationsAloneForClosuresOfLiterals(const Program& program, const Facts& facts, const SlotPlanParts& slots, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record || layout.constructor || !isFoundByShapeTest(layout) || layout.isDerived() || layout.flags.containsAny({ LayoutFlags::IsOfCreatedObject, LayoutFlags::IsConstructorFunction, LayoutFlags::HasHeritage }))
        return false;
    if (facts.parsersMayMake(id) || facts.has(id, LayoutFact::ObjectsAreDictionaries))
        return false;
    if (slots.layoutsNeverAllocated.get(id.index()) || !slots.layouts[id.index()].holdsCompiledObjects())
        return false;
    const LinkTables& linked = facts.linked();
    return id.index() < linked.layouts.size() && linked.layout(id).isAllocated;
}

static bool isLookedUpAsNameForClosuresOfLiterals(const Program& program, PropertyKeyID key)
{
    if (!key || !program.types.hasStableNumber(key) || program.at(key).kind != PropertyKeyKind::Text)
        return false;
    NameClass nameClass = program.nameClassOf(key);
    return !mayBeElementKey(nameClass) && nameClass != NameClass::Proto;
}

static bool mayDeferForClosuresOfLiterals(const Program& program, const Facts& facts, FieldReference reference, const PlacedSlot& slot)
{
    if (slot.storage != SlotStorage::Inline || slot.representation != SlotRepresentation::JSValue || !slot.indexInLayoutRecord)
        return false;
    if (slot.holdsAccessor || slot.mayExistWithoutStorage || slot.isFilledAtBirth || slot.typeOfRawValue || slot.deferredFunction)
        return false;
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsPrivate, FieldFlags::IsImmutable, FieldFlags::IsBrand, FieldFlags::IsBrandOfMethods, FieldFlags::IsHidden, FieldFlags::IsRemovedAsUnread, FieldFlags::IsAlsoInPrototype };
    const Field& field = fieldAt(program, declarationOf(program, reference));
    if (field.flags.containsAny(excludedFlags) || !field.form.isEmpty() || field.computedKey)
        return false;
    constexpr OptionSet<FieldFact> excludedFacts { FieldFact::IsAccessor, FieldFact::HoldsRawValueFromBirth };
    FieldFacts fieldFacts = facts.field(reference);
    return !fieldFacts.facts.containsAny(excludedFacts) && fieldFacts.formOfValue.isEmpty();
}

// Null: none. The closures of the function are slim cells with one word, which names a scope.
static FunctionID functionWhoseClosuresFieldHoldsForClosuresOfLiterals(const Program& program, const LinkTables& linked, const SlotPlanParts& slots, FieldReference reference, LayoutID& scope)
{
    const LinkedCell* cell = linked.findCellOfField(reference);
    if (!cell || !cell->proven || !cell->everyWriterIsKnown || cell->isWrittenAfterConstruction)
        return { };
    Type held = program.at(cell->proven);
    if (!held.isJavaScriptValue() || !held.isObject() || !held.hasOnlyClosures())
        return { };
    FunctionID id = singleFunctionOf(TypeReader(program), held);
    if (!id || id.index() >= slots.closureForms.size() || slots.closureForms[id.index()] != ClosureForm::Slim)
        return { };
    const Function& function = program.at(id);
    if (function.captures.size() != 1 || !function.captures[0].isEstablished() || !function.captures[0].written())
        return { };
    scope = program.at(function.captures[0].written()).environment;
    return scope ? id : FunctionID();
}

void leaveClosuresOfLiteralsUnmade(const Program& program, const Facts& facts, bool useWideReferences, SlotPlanParts& slots)
{
    // A read as a value makes the closure in a stub (generateMakeClosureOfField()) or in the run time (Object::materializeDeferredClosure()), stores it into the slot, and runs the barrier of the object.
    // The mark and the reference beside it are narrow words.
    if (useWideReferences || !withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::Deferral))
        return;

    const LinkTables& linked = facts.linked();
    std::optional<BitVector> layoutsThatShareSlots;
    for (uint32_t index = 0; index < slots.layouts.size(); ++index) {
        LayoutID id(index);
        PlannedLayout& planned = slots.layouts[index];
        if (!planned.numberOfFields() || planned.environmentOfDeferredClosures || !isMadeByOperationsAloneForClosuresOfLiterals(program, facts, slots, id))
            continue;
        const Layout& layout = program.at(id);
        if (planned.numberOfFields() + 1 > maxFieldsInOrder)
            continue;

        // The scope of the first such field is the scope of the layout: an object keeps one.
        LayoutID scopeOfLayout;
        std::optional<FieldIndex> first;
        for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
            FieldReference reference { id, field };
            PlacedSlot& slot = planned.slots[field];
            if (!isLookedUpAsNameForClosuresOfLiterals(program, layout.fields[field].key) || !mayDeferForClosuresOfLiterals(program, facts, reference, slot))
                continue;
            LayoutID scope;
            FunctionID function = functionWhoseClosuresFieldHoldsForClosuresOfLiterals(program, linked, slots, reference, scope);
            if (!function || (scopeOfLayout && scope != scopeOfLayout))
                continue;
            if (!layoutsThatShareSlots)
                layoutsThatShareSlots = findLayoutsThatShareSlotsForClosuresOfLiterals(program, facts.families(), slots);
            if (layoutsThatShareSlots->get(index))
                break;

            scopeOfLayout = scope;
            if (!first)
                first = field;
            slot.representation = SlotRepresentation::DeferredClosure;
            slot.deferredFunction = function;
            slot.deferredClosureCapturesEnvironment = true;
            slot.type = linked.findCellOfField(reference)->proven;
            slots.keysOfSlotsThatHoldMarks.set(layout.fields[field].key.index());
        }
        if (!first)
            continue;

        // A mark is four bytes wide and its slot eight: the scope lies in the other half of the first.
        static_assert(widthOf(SlotRepresentation::DeferredClosure) + widthOf(SlotRepresentation::Cell32) <= sizeof(uint64_t));
        PlacedSlot scope;
        scope.representation = SlotRepresentation::Cell32;
        scope.storage = SlotStorage::Inline;
        scope.offset = planned.slots[*first].offset + widthOf(SlotRepresentation::DeferredClosure);
        scope.indexInLayoutRecord = planned.numberOfRecordFields++;
        FieldIndex fieldOfScope = planned.numberOfFields();
        planned.slots.insert(fieldOfScope, scope);
        planned.appendedKeys.append(PropertyKeyID());
        planned.indexAppendedKeys();
        planned.environmentOfDeferredClosures = fieldOfScope;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
