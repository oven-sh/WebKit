/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
