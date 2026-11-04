/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRules.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTResultRulesInternal.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// Only a type site tells `x as int32` from a declaration of int32. Without one the promise is a declaration: the target is int32 alone, and the value is tested for it.
static bool convertAssertsInt32(const Program& program, const DeclaredType& declared)
{
    if (!declared.has(PositionFlags::IsInt32Promise) || !declared.site)
        return false;
    return program.at(declared.site).kind == TypeSiteKind::Assertion;
}

// OmitsNull and OmitsUndefined remove nothing from what enters a position: nothing keeps null and undefined out of a reference.
// Only the entrance of a function leaves them out of what it tests for, where no call in sight passes them: computePositionTypes().
TypeID targetOfConvert(const TypingContext& context, const DeclaredType& declared)
{
    TypeID written = declared.expected().predicted();
    if (!written || !convertAssertsInt32(context.program, declared))
        return written;
    return context.types.withPrimitives(written, Primitive::Number);
}

ConvertResolution resolveConvert(const TypingContext& context, TypeID proven, const DeclaredType& declared)
{
    TypeID target = targetOfConvert(context, declared);
    if (!target || declared.isExpected())
        return { ConvertStep::Nothing, target, { } };

    TypeArena& types = context.types;
    // Even where the value is already proven to fit: Facts' scan has no types of values and must get the same answer as the walk.
    if (declared.isEstablished() && declared.has(PositionFlags::IsClaim))
        return { ConvertStep::TakeAsFact, target, types.testablePartOf(target) };

    // Only never is a subset of never, so a value that may exist is tested against a target of never, and fails.
    if (proven && types.isSubset(proven, target))
        return { ConvertStep::Nothing, target, { } };
    return { ConvertStep::Test, target, types.testablePartOf(target) };
}

bool changesCodeWhereProven(const TypeArena& types, TypeID tested)
{
    return holdsOnlyPrimitives(tested) || !types.at(tested).anyObject;
}

static constexpr size_t fewestFieldsOfBagOfOptions = 24;

bool describesBagOfOptions(const TypeArena& types, TypeID written)
{
    if (!written || holdsOnlyPrimitives(written))
        return false;
    Type type = types.at(written);
    std::span<const LayoutID> layouts = membersOf(types, type);
    const Program* program = TypeReader(types).program();
    if (!program)
        return false;
    // `{ target, module }` that is handed over where CompilerOptions is written lies below the interface, and the written type names both. It says no more of what arrives than the interface alone does.
    bool namesBag = false;
    for (LayoutID id : layouts) {
        const Layout& layout = program->at(id);
        if (layout.kind != LayoutKind::Record)
            return false;
        namesBag |= layout.fields.size() >= fewestFieldsOfBagOfOptions && std::ranges::none_of(layout.fields, [](const Field& field) {
            return field.flags.containsAny({ FieldFlags::IsRequired, FieldFlags::IsRequiredUntilDeleted });
        });
    }
    return namesBag;
}

// Returns never where the declaration says nothing about the value.
static TypeID writtenTypeCutByPossibleValues(TypeArena& types, TypeID possible, TypeID written)
{
    TypeID cut = types.castTo(possible, written);
    if (isNeverType(cut) || holdsOnlyPrimitives(possible) || holdsOnlyPrimitives(written))
        return cut;
    // `list?: { length: number }` is a layout, and arrays are passed in. Keeping only the primitives that both sides share would guess that no object ever arrives.
    if (!types.at(cut).hasObjects() && types.at(possible).hasObjects() && types.at(written).hasObjects())
        return TypeTable::never;
    return cut;
}

// isSubset() ignores the written contents of a class, and the rules at TypeLevel::Expected know nothing else about contents.
bool writtenContentsSayMoreThan(TypeArena& types, TypeID proven, TypeID cut)
{
    if (holdsOnlyPrimitives(cut))
        return false;
    Type cutType = types.at(cut);
    Type provenType = types.at(proven);
    if (!cutType.unnamedClasses || cutType.unnamedClasses == provenType.unnamedClasses)
        return false;

    return std::ranges::any_of(types.at(cutType.unnamedClasses), [&](const ClassPart& part) {
        if (!part.hasWrittenContents())
            return false;
        const ClassPart* known = findClassPart(types, provenType, part.builtinClass);
        return !known || known->writtenContents != part.writtenContents;
    });
}

static bool writtenTypeSaysMoreThan(TypeArena& types, TypeID proven, TypeID cut)
{
    if (cut == proven)
        return false;
    return !types.isSubset(proven, cut) || writtenContentsSayMoreThan(types, proven, cut);
}

ExpectedType expectationFromWrittenType(TypeArena& types, TypeID proven, ExpectedType before, TypeID written)
{
    if (!proven || !written)
        return { };

    TypeID cut = TypeTable::never;
    if (before)
        cut = writtenTypeCutByPossibleValues(types, before.predicted(), written);
    if (isNeverType(cut))
        cut = writtenTypeCutByPossibleValues(types, proven, written);
    if (isNeverType(cut) || !writtenTypeSaysMoreThan(types, proven, cut))
        return { };
    return ExpectedType(cut);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
