/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mechanism::TypedSlots: the constant that an allocation lists where a written value is not proven to fit its slot. It reads the plan's slot, once a misfit, and keeps nothing.
// At a suspension and at a throw: nothing stands between the allocation and the store behind it but the test of the value. Withheld: no slot has a type, so every value fits.

#include "config.h"
#include "AOTWalkEnforcedFillers.h"

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

std::optional<Draft> fillerThatFitsSlot(Walker& walker, FieldReference field)
{
    const PlacedSlot& slot = walker.plan().slot(field.layout, field.field);
    if (!slot.hasStorage() || !slot.type || slot.holdsAccessor || slot.representation == SlotRepresentation::DeferredClosure)
        return std::nullopt;
    // What does not fit after all leaves the slot, by the store behind the allocation.
    if (!canLeaveSlot(slot.representation))
        return std::nullopt;

    Type held = walker.types().at(slot.type);
    if ((held.primitives & Primitive::Integer) && !held.integers.isEmpty()) {
        Draft integer(Opcode::Integer);
        integer.setInteger(std::clamp<int64_t>(0, held.integers.minimum, held.integers.maximum));
        return integer;
    }
    if (held.primitives & Primitive::String) {
        Draft text(Opcode::String);
        text.setString(walker.program().strings.emptyString());
        return text;
    }
    if ((held.primitives & Primitive::LiteralString) && held.stringLiterals) {
        Draft text(Opcode::String);
        text.setString(walker.types().at(held.stringLiterals).first());
        return text;
    }
    SlotTraits traits = traitsOf(slot.representation);
    if ((held.primitives & Primitive::Undefined) && traits.hasUndefined)
        return Draft(Opcode::Undefined);
    if (held.primitives & Primitive::Null)
        return Draft(Opcode::Null);
    if (held.primitives & Primitive::Boolean) {
        Draft boolean(Opcode::Boolean);
        boolean.setBoolean(!(held.primitives & Primitive::False));
        return boolean;
    }
    return std::nullopt;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
