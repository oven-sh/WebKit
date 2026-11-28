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
#include "AOTInliningAcrossFiles.h"
#include "AOTPlan.h"
#include "AOTSlotRepresentation.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static_assert(elementRepresentationOf(ArrayStorage::Int32) == Representation::Int32 && elementRepresentationOf(ArrayStorage::Double) == Representation::Double);
static_assert(elementRepresentationOf(ArrayStorage::Contiguous) == Representation::JSValue && elementRepresentationOf(ArrayStorage::Other) == Representation::JSValue);
static_assert(traitsOf(SlotRepresentation::DeferredClosure).isNarrowReference && widthOf(SlotRepresentation::DeferredClosure) == widthOf(SlotRepresentation::Cell32));

static ALWAYS_INLINE std::optional<PlacedSlot> findSlotAtPlannedPosition(const RepresentationContext& context, PropertyKeyID key, LayoutSetID receivers, bool isStore)
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
        if (isStore)
            return std::nullopt;
        return access.slot;
    case FieldAccess::Coverage::EveryMember:
        return access.slot;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr bool takesBoxedValuesAtEveryPosition(CodeRole role)
{
    return isGenericEntry(role);
}

Representation representationStoredInSlot(const RepresentationContext& context, PropertyKeyID key, LayoutSetID receivers)
{
    std::optional<PlacedSlot> slot = findSlotAtPlannedPosition(context, key, receivers, true);
    if (!slot)
        return Representation::Unchosen;
    if (slot->representation == SlotRepresentation::DeferredClosure)
        return Representation::Cell32;
    return representationOf(slot->representation);
}

Representation representationLoadedFromSlot(const RepresentationContext& context, PropertyKeyID key, LayoutSetID receivers, TypeID loaded)
{
    if (!loaded)
        return Representation::Unchosen;

    std::optional<PlacedSlot> slot = findSlotAtPlannedPosition(context, key, receivers, false);
    if (!slot || slot->representation == SlotRepresentation::DeferredClosure)
        return representationOf(context.types, loaded);

    Type type = context.types.at(loaded);
    Representation held = slot->isOutOfLine() ? representationOf(slot->representation) : loadedRepresentationOf(slot->representation, type);
    return isLegalRepresentation(type, held) ? held : representationOf(context.types, loaded);
}

Representation representationOfCapture(const RepresentationContext& context, FunctionID id, uint32_t capture)
{
    return closureCaptureRepresentationOf(context.program, context.plan.conventions(), id, capture);
}

Representation representationOfVariable(const RepresentationContext& context, VariableID id)
{
    PlannedVariable variable = context.plan.variable(id);
    switch (variable.storage) {
    case VariableStorage::Statics:
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
    RELEASE_ASSERT(representation != Representation::Unchosen);
    RELEASE_ASSERT(!hasRegister(representation) || !takesBoxedValuesAtEveryPosition(callee.role) || isCellOrBoxedValue(representation));
    return representation;
}

ALWAYS_INLINE_INTO_OTHER_FILES Representation representationOfResult(const RepresentationContext& context, CodeUnit callee, uint32_t resultIndex)
{
    Representation representation = context.plan.conventions().resultRepresentation(callee, resultIndex);
    RELEASE_ASSERT(hasRegister(representation));
    RELEASE_ASSERT(!isGenericEntry(callee.role) || representation == Representation::JSValue);
    return representation;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
