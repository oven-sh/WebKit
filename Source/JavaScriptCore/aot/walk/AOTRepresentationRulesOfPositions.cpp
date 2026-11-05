/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRepresentationRules.h"

#if ENABLE(AOT)

#include "AOTArgumentLocations.h"
#include "AOTArrayStorageForms.h"
#include "AOTFunctionKinds.h"
#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSlotRepresentation.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(elementRepresentationOf(ArrayStorage::Int32) == Representation::Int32 && elementRepresentationOf(ArrayStorage::Double) == Representation::Double);
static_assert(elementRepresentationOf(ArrayStorage::Contiguous) == Representation::JSValue && elementRepresentationOf(ArrayStorage::Other) == Representation::JSValue);
static_assert(traitsOf(SlotRepresentation::DeferredClosure).isNarrowReference && widthOf(SlotRepresentation::DeferredClosure) == widthOf(SlotRepresentation::Cell32));

// Nothing: the operation touches no slot, so Plan gives the position no form.
// A store writes without asking whether the object has the slot, so it finds one only where every receiver has it.
static std::optional<PlacedSlot> findSlotAtPlannedPosition(const RepresentationContext& context, PropertyKeyID key, LayoutSetID receivers, bool isStore)
{
    if (!key || !receivers)
        return std::nullopt;
    FieldAccess access = context.plan.fieldAccess(context.program, key, receivers, membersOf(context.types, receivers), isStore);
    if (!access.hasLocation || !access.slot.hasStorage())
        return std::nullopt;
    switch (access.coverage) {
    case FieldAccess::Coverage::NoMember:
        return std::nullopt;
    case FieldAccess::Coverage::SomeMembers:
    case FieldAccess::Coverage::EveryMemberHasSlot:
        if (isStore)
            return std::nullopt;
        return access.slot;
    case FieldAccess::Coverage::EveryMember:
        return access.slot;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whoever enters such a unit hands over one GPR or one stack word a value: a caller by value knows no types.
static constexpr bool takesBoxedValuesAtEveryPosition(CodeRole role)
{
    return isGenericEntry(role);
}

Representation representationStoredInSlot(const RepresentationContext& context, PropertyKeyID key, LayoutSetID receivers)
{
    std::optional<PlacedSlot> slot = findSlotAtPlannedPosition(context, key, receivers, true);
    if (!slot)
        return Representation::Unchosen;
    // The closure that the slot leaves unmade has no value. Whatever else such a slot takes is a reference.
    if (slot->representation == SlotRepresentation::DeferredClosure)
        return Representation::Cell32;
    return representationOf(slot->representation);
}

Representation representationLoadedFromSlot(const RepresentationContext& context, PropertyKeyID key, LayoutSetID receivers, TypeID loaded)
{
    if (!loaded)
        return Representation::Unchosen;

    std::optional<PlacedSlot> slot = findSlotAtPlannedPosition(context, key, receivers, false);
    // The word of a slot that defers its closure is no value: the read makes the closure of it.
    if (!slot || slot->representation == SlotRepresentation::DeferredClosure)
        return representationOf(context.types, loaded);

    Type type = context.types.at(loaded);
    // A load from an Extension that is missing or too short answers absent, whatever is proven of the field.
    Representation held = slot->isOutOfLine() ? representationOf(slot->representation) : loadedRepresentationOf(slot->representation, type);
    // Where some receiver has no such field, or the read answers undefined for an empty slot, the result holds more than the slot does.
    return isLegalRepresentation(type, held) ? held : representationOf(context.types, loaded);
}

// closureCaptureSlotOf() sorts the captures into references and other words by this same answer, which Plan gave once for whoever makes, reads or visits a closure of the function.
Representation representationOfCapture(const RepresentationContext& context, FunctionID id, uint32_t capture)
{
    return closureCaptureRepresentationOf(context.program, context.plan.conventions(), id, capture);
}

Representation representationOfVariable(const RepresentationContext& context, VariableID id)
{
    PlannedVariable variable = context.plan.variable(id);
    switch (variable.storage) {
    case VariableStorage::Statics:
    // A read gives the closure in the form in which the word would have held it.
    case VariableStorage::ClosureTable:
        return representationOf(variable.representation);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Representation representationOfArrayElement(ArrayStorage storage)
{
    return elementRepresentationOf(storage);
}

Representation representationOfArgument(const RepresentationContext& context, CodeUnit callee, uint32_t entryParameter)
{
    Representation representation = context.plan.conventions().parameterRepresentation(context.program, callee, entryParameter);
    // Void: no caller hands the parameter over, and what stands in its place is taken in any form.
    RELEASE_ASSERT(representation != Representation::Unchosen);
    RELEASE_ASSERT(!hasRegister(representation) || !takesBoxedValuesAtEveryPosition(callee.role) || isCellOrBoxedValue(representation));
    return representation;
}

Representation representationOfResult(const RepresentationContext& context, CodeUnit callee, uint32_t resultIndex)
{
    Representation representation = context.plan.conventions().resultRepresentation(callee, resultIndex);
    RELEASE_ASSERT(hasRegister(representation));
    RELEASE_ASSERT(!isGenericEntry(callee.role) || representation == Representation::JSValue);
    return representation;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
