/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypeNarrowing.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

// The type of the slot decides, because what lies in a typed slot always fits it, and beside it what Facts proves to be all that arrives at the field of the layout: PositionTypes::readOfSlot().
// The cell of a name on one object holds whatever was stored under the name there, so it tells no layouts apart.
// Plan lays a declaration and its inherited copies out for one type. So a field tells apart layouts that each declare it, and never the layouts that inherit it:
// neither the classes below the base that declares it nor the members of a tagged family, which have the tag from their maker.
static bool memberCanHoldFieldValue(const TypingContext& context, LayoutID member, PropertyKeyID key, TypeID read, bool readMayBeUndefined)
{
    bool isGuess = context.level == TypeLevel::Expected;
    std::optional<FieldReference> reference = findField(context.program, member, key);
    // findField() goes up the bases as they are written, and only a field that the layout itself lists has a slot in its objects.
    // For a proof the value may have come from elsewhere. For a guess an object without the property answers undefined.
    if (!reference || (!isGuess && reference->layout != member))
        return !isGuess || readMayBeUndefined;

    const Field& field = fieldAt(context.program, *reference);
    if (isGuess && readMayBeUndefined && !field.flags.contains(FieldFlags::IsRequired))
        return true;

    // The slot of an accessor holds the getter and the setter. What is written of the field is what the getter answers.
    TypeID held;
    if (!field.form.contains(FieldForm::IsAccessor))
        held = context.positions.slot(context.level, reference->layout, reference->field);
    if (!held && isGuess)
        held = enforceableTypeOf(context.types, field.declared);
    if (held && !context.types.overlap(held, read))
        return false;

    // `{ kind: "a" }` beside `{ kind: "b" }` in code without types: both slots are laid out for any text. What has never arrived at the field of the member was not read out of it.
    TypeID arriving = context.positions.readOfSlot(context.level, reference->layout, reference->field);
    return !arriving || context.types.overlap(arriving, read);
}

// Null: none of them.
static LayoutSetID membersThatCanHoldFieldValue(const TypingContext& context, LayoutSetID family, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    bool readMayBeUndefined = context.at(read).primitives & Primitive::Undefined;
    if (readMayBeUndefined && emptySlot == ReadOfEmptySlot::GivesUndefined)
        return family;

    // Asking about a member may make types and sets, so no set is read or made meanwhile.
    Vector<LayoutID, 16> members;
    context.types.at(family).forEach([&](LayoutID member) {
        members.append(member);
    });
    size_t sizeOfFamily = members.size();
    members.removeAllMatching([&](LayoutID member) {
        return !memberCanHoldFieldValue(context, member, key, read, readMayBeUndefined);
    });
    if (members.size() == sizeOfFamily)
        return family;
    if (members.isEmpty())
        return { };
    return context.types.layoutSets().ofSorted(members.span());
}

// `holderID` hasOnlyLayouts(). Null: no layout of the holder can have held the value.
static TypeID tryNarrowLayoutsOfHolderByFieldValue(const TypingContext& context, TypeID holderID, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    LayoutSetID layouts = context.at(holderID).layouts;
    LayoutSetID kept = membersThatCanHoldFieldValue(context, layouts, key, read, emptySlot);
    if (!kept)
        return { };
    if (kept == layouts)
        return holderID;

    // The holder has one of the layouts that are kept, which is what a shape test for them proves. Which layouts a named object may have is written there and nowhere else.
    TypeID narrowed = narrowByShapeCase(context, holderID, kept);
    // Each of them is a layout of the holder, so something is left. Never would end a block that runs.
    RELEASE_ASSERT(!isNeverType(narrowed));
    return narrowed;
}

TypeID narrowHolderByFieldValue(const TypingContext& context, TypeID holder, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    if (!holder)
        return { };
    if (isNeverType(read))
        return TypeTable::never;

    // The read would have thrown.
    TypeID withoutNullish = context.types.withoutNullish(holder);
    if (!read || !context.at(withoutNullish).hasOnlyLayouts())
        return withoutNullish;

    TypeID narrowed = tryNarrowLayoutsOfHolderByFieldValue(context, withoutNullish, key, read, emptySlot);
    return narrowed ? narrowed : withoutNullish;
}

bool narrowsHolderToNothing(const TypingContext& context, TypeID holder, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    if (!holder || !read || isNeverType(read))
        return false;

    TypeID withoutNullish = context.types.withoutNullish(holder);
    return context.at(withoutNullish).hasOnlyLayouts() && !tryNarrowLayoutsOfHolderByFieldValue(context, withoutNullish, key, read, emptySlot);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
