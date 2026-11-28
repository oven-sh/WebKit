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

static bool memberCanHoldFieldValue(const TypingContext& context, LayoutID member, PropertyKeyID key, TypeID read, bool readMayBeUndefined)
{
    bool isGuess = context.level == TypeLevel::Expected;
    std::optional<FieldReference> reference = findField(context.program, member, key);
    if (!reference || (!isGuess && reference->layout != member))
        return !isGuess || readMayBeUndefined;

    const Field& field = fieldAt(context.program, *reference);
    if (isGuess && readMayBeUndefined && !field.flags.contains(FieldFlags::IsRequired))
        return true;

    TypeID held;
    if (!field.form.contains(FieldForm::IsAccessor))
        held = context.positions.slot(context.level, reference->layout, reference->field);
    if (!held && isGuess)
        held = enforceableTypeOf(context.types, field.declared);
    if (held && !context.types.overlap(held, read))
        return false;

    TypeID arriving = context.positions.readOfSlot(context.level, reference->layout, reference->field);
    return !arriving || context.types.overlap(arriving, read);
}

static LayoutSetID membersThatCanHoldFieldValue(const TypingContext& context, LayoutSetID family, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    bool readMayBeUndefined = context.at(read).primitives & Primitive::Undefined;
    if (readMayBeUndefined && emptySlot == ReadOfEmptySlot::GivesUndefined)
        return family;

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

static TypeID tryNarrowLayoutsOfHolderByFieldValue(const TypingContext& context, TypeID holderID, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    LayoutSetID layouts = context.at(holderID).layouts;
    LayoutSetID kept = membersThatCanHoldFieldValue(context, layouts, key, read, emptySlot);
    if (!kept)
        return { };
    if (kept == layouts)
        return holderID;

    TypeID narrowed = narrowByShapeCase(context, holderID, kept);
    RELEASE_ASSERT(!isNeverType(narrowed));
    return narrowed;
}

TypeID narrowHolderByFieldValue(const TypingContext& context, TypeID holder, PropertyKeyID key, TypeID read, ReadOfEmptySlot emptySlot)
{
    if (!holder)
        return { };
    if (isNeverType(read))
        return TypeTable::never;

    TypeID withoutNullish = context.types.withoutNullish(holder);
    if (!read || !context.at(withoutNullish).hasOnlyLayouts())
        return withoutNullish;

    TypeID narrowed = tryNarrowLayoutsOfHolderByFieldValue(context, withoutNullish, key, read, emptySlot);
    return narrowed ? narrowed : withoutNullish;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
