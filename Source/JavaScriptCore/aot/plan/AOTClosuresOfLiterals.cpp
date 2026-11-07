/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosuresOfLiterals.h"

#if ENABLE(AOT)

#include "AOTFacts.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTNameClass.h"
#include "AOTPromotedEnvironments.h"
#include "AOTSlotPlan.h"
#include "AOTSlotRepresentation.h"
#include "AOTType.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

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

static FunctionID functionWhoseClosuresFieldHoldsForClosuresOfLiterals(const Program& program, const LinkTables& linked, const PromotedEnvironments& promoted, const SlotPlanParts& slots, FieldReference reference, LayoutID& scope)
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
    if (function.captures.size() != 1 || !function.captures[0].isEstablished() || !function.captures[0].written() || promoted.numberOfRemovedCapturesOf(id))
        return { };
    scope = program.at(function.captures[0].written()).environment;
    return scope ? id : FunctionID();
}

void leaveClosuresOfLiteralsUnmade(const Program& program, const Facts& facts, const PromotedEnvironments& promoted, bool useWideReferences, SlotPlanParts& slots)
{
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

        LayoutID scopeOfLayout;
        for (FieldIndex field = 0; field < layout.fields.size(); ++field) {
            FieldReference reference { id, field };
            PlacedSlot& slot = planned.slots[field];
            LayoutID scope;
            FunctionID function = functionWhoseClosuresFieldHoldsForClosuresOfLiterals(program, linked, promoted, slots, reference, scope);
            if (!function || (scopeOfLayout && scope != scopeOfLayout))
                continue;
            if (!isLookedUpAsNameForClosuresOfLiterals(program, layout.fields[field].key) || !mayDeferForClosuresOfLiterals(program, facts, reference, slot))
                continue;
            if (!layoutsThatShareSlots)
                layoutsThatShareSlots = findLayoutsThatShareSlotsForClosuresOfLiterals(program, facts.families(), slots);
            if (layoutsThatShareSlots->get(index))
                break;

            scopeOfLayout = scope;
            slot.representation = SlotRepresentation::DeferredClosure;
            slot.deferredFunction = function;
            slot.deferredClosureCapturesEnvironment = true;
            slot.type = linked.findCellOfField(reference)->proven;
            slots.keysOfSlotsThatHoldMarks.set(layout.fields[field].key.index());
        }
        if (!scopeOfLayout)
            continue;

        PlacedSlot scope;
        scope.representation = SlotRepresentation::Cell32;
        scope.storage = SlotStorage::Inline;
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
