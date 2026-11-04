/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTSelectionContext.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTStringRuleSupport.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Part {
    Opcode row { Opcode::StringBuilderAppendValue };
    ValueID value { };
    StringID constant { };
    std::optional<StringRangeSource> range { };
};

using StringBuilderPartList = Vector<Part, 8>;

struct StringBuilderPartMatch {
    SelectionGroup group;
    ProgramPoint point;
    ValueID builder;
    StringBuilderPartList parts;
};

} // anonymous namespace

static bool isConstantPart(SelectionContext& context, ValueID part, const Type& type)
{
    if (std::optional<LiteralArgument> literal = literalArgumentOf(context, part))
        return literal->text.containsOnlyLatin1() && !context.pass().exceeds(LimitID::MaxLengthCopiedInline, literal->text.length(), part);
    return type.isSingleValue() && type.hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::Boolean);
}

static bool isRangeWithSingleUserInBlock(SelectionContext& context, ValueID part)
{
    if (context.isClaimed(part))
        return false;
    std::optional<StringRangeSource> source = stringRangeSourceOf(context.program(), context.function(), context.function().at(part));
    if (!source || source->mayBeUndefined)
        return false;
    std::optional<ValueID> user = context.singleUser(part);
    if (!user)
        return false;

    const UseLists& useLists = context.analyses().useLists();
    ValueLocation definition = useLists.locationOf(part);
    return definition.placement == ValuePlacement::InBlock && definition.block == useLists.blockOf(*user);
}

static Opcode rowOfPart(SelectionContext& context, ValueID part, CodeShape shapeAtAppend)
{
    const Type& type = context.typeOf(part);
    if (isConstantPart(context, part, type))
        return Opcode::StringBuilderAppendConstant;
    // In compact code a part is one call. The call for a string takes the string itself: it has no list of values to fill and does not ask what it was given.
    if (shapeAtAppend == CodeShape::Compact)
        return type.isAnyString() ? Opcode::StringBuilderAppendString : Opcode::StringBuilderAppendValue;
    if (type.isInteger())
        return Opcode::StringBuilderAppendInteger;
    if (type.isNumber())
        return Opcode::StringBuilderAppendNumber;
    if (type.isBoolean())
        return Opcode::StringBuilderAppendBoolean;
    if (type.isAnyString())
        return isRangeWithSingleUserInBlock(context, part) ? Opcode::StringBuilderAppendStringRange : Opcode::StringBuilderAppendString;
    if (type.isAnyStringOrNullish())
        return Opcode::StringBuilderAppendStringOrNullish;
    return Opcode::StringBuilderAppendValue;
}

static LiteralArgument constantTextOf(SelectionContext& context, ValueID part)
{
    if (std::optional<LiteralArgument> literal = literalArgumentOf(context, part))
        return WTF::move(*literal);

    switch (context.typeOf(part).primitives) {
    case Primitive::Undefined:
        return { { }, "undefined"_s };
    case Primitive::Null:
        return { { }, "null"_s };
    case Primitive::True:
        return { { }, "true"_s };
    case Primitive::False:
        return { { }, "false"_s };
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StringBuilderPartList collectParts(SelectionContext& context, ValueID append)
{
    StringBuilderPartList parts;
    std::optional<LiteralArgument> pendingConstant;
    auto flushConstant = [&] {
        if (!pendingConstant)
            return;
        LiteralArgument constant = *std::exchange(pendingConstant, std::nullopt);
        StringID string = constant.literal ? constant.literal : context.pass().strings.intern(constant.text);
        parts.append(Part { .row = Opcode::StringBuilderAppendConstant, .constant = string });
    };

    CodeShape shapeAtAppend = context.shapeAt(append);
    for (ValueID id : context.function().at(append).partsOperands()) {
        Part part { .row = rowOfPart(context, id, shapeAtAppend), .value = id };
        if (part.row != Opcode::StringBuilderAppendConstant) {
            flushConstant();
            if (part.row == Opcode::StringBuilderAppendStringRange) {
                part.range = stringRangeSourceOf(context.program(), context.function(), context.function().at(id));
                RELEASE_ASSERT(part.range);
            }
            parts.append(WTF::move(part));
            continue;
        }

        LiteralArgument constant = constantTextOf(context, id);
        if (constant.text.isEmpty())
            continue;
        if (pendingConstant && pendingConstant->text.length() + constant.text.length() <= context.pass().limit(LimitID::MaxLengthCopiedInline)) {
            pendingConstant = LiteralArgument { { }, makeString(pendingConstant->text, constant.text) };
            continue;
        }
        flushConstant();
        pendingConstant = WTF::move(constant);
    }
    flushConstant();
    return parts;
}

static SelectionGroup groupWithRanges(SelectionContext& context, ValueID append, const StringBuilderPartList& parts)
{
    const UseLists& uses = context.analyses().useLists();
    Vector<ValueID, 4> definitions;
    for (const Part& part : parts) {
        if (part.range)
            definitions.append(part.value);
    }
    AOT::sort(definitions, { }, [&](ValueID definition) {
        return uses.locationOf(definition).index;
    });

    SelectionGroup group;
    for (ValueID definition : definitions)
        group.members.append(GroupMember { definition });
    group.members.append(GroupMember { append });
    group.root = append;
    return group;
}

static void claim(SelectionContext& context, StringBuilderPartMatch& match)
{
    // What the nearest range may not cross, the ranges in front of it may not cross either.
    while (context.tryClaim(SelectionRuleID::StringBuilderParts, match.group) != ClaimResult::Claimed) {
        RELEASE_ASSERT(match.group.members.size() > 1);
        Part* part = std::ranges::find(match.parts, match.group.members.first().value, &Part::value);
        RELEASE_ASSERT(part != match.parts.end() && part->range);
        part->row = Opcode::StringBuilderAppendString;
        part->range = std::nullopt;
        match.group.members.removeAt(0);
    }
}

static Value makeRow(GroupRewriter& rewriter, const StringBuilderPartMatch& match, const Part& part)
{
    Value row = IRBuilder::make(part.row, { match.builder });
    if (part.row == Opcode::StringBuilderAppendConstant)
        row.setString(part.constant);
    else if (part.range) {
        StringRangeBounds bounds = insertStringRange(rewriter.builder(), match.point, *part.range, std::nullopt);
        row.setStringOperand(part.range->string);
        row.setStartOperand(bounds.start);
        row.setEndOperand(bounds.end);
    } else
        row.setValueOperand(part.value);
    return row;
}

void selectStringBuilderParts(SelectionContext& context)
{
    Vector<StringBuilderPartMatch> matches;
    for (ValueID append : context.valuesWith(Opcode::StringBuilderAppend)) {
        if (context.isClaimed(append))
            continue;
        StringBuilderPartList parts = collectParts(context, append);
        SelectionGroup group = groupWithRanges(context, append, parts);
        matches.append(StringBuilderPartMatch { WTF::move(group), context.analyses().useLists().pointOf(append), context.function().at(append).builderOperand(), WTF::move(parts) });
        claim(context, matches.last());
    }

    context.rewriteGroups(SelectionRuleID::StringBuilderParts, matches, [](GroupRewriter& rewriter, const StringBuilderPartMatch& match) {
        for (const Part& part : match.parts)
            rewriter.insert(makeRow(rewriter, match, part));
        rewriter.eraseRoot();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
