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
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Part {
    StringBuilderPartForm form { StringBuilderPartForm::Value };
    ValueID value { };
    StringID constant { };
    std::optional<StringRangeSource> range { };
};

using Parts = Vector<Part, 8>;

struct Match {
    SelectionGroup group;
    ProgramPoint point;
    ValueID builder;
    Parts parts;
};

struct ConstantText {
    StringID internedString;
    String text;
};

class PartCollector {
    WTF_MAKE_NONCOPYABLE(PartCollector);
public:
    PartCollector(SelectionContext& context, CodeShape shapeAtAppend)
        : m_context(context)
        , m_shapeAtAppend(shapeAtAppend)
    {
    }

    void add(ValueID part);
    Parts finish();

private:
    void addConstant(ConstantText&&);
    void flushConstant();

    SelectionContext& m_context;
    CodeShape m_shapeAtAppend;
    Parts m_parts;
    std::optional<ConstantText> m_pendingConstant;
};

} // anonymous namespace

static ConstantText constantTextOf(SelectionContext& context, ValueID part)
{
    if (std::optional<LiteralArgument> literal = literalArgumentOf(context, part))
        return { literal->literal, WTF::move(literal->text) };

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

void PartCollector::add(ValueID id)
{
    StringBuilderPartForm form = partFormOf(m_context, id, m_shapeAtAppend);
    if (form == StringBuilderPartForm::Constant) {
        addConstant(constantTextOf(m_context, id));
        return;
    }

    flushConstant();
    Part part { .form = form, .value = id };
    if (form == StringBuilderPartForm::StringRange) {
        part.range = stringRangeSourceOf(m_context.program(), m_context.function(), m_context.function().at(id));
        RELEASE_ASSERT(part.range);
    }
    m_parts.append(WTF::move(part));
}

Parts PartCollector::finish()
{
    flushConstant();
    return std::exchange(m_parts, { });
}

void PartCollector::addConstant(ConstantText&& constant)
{
    if (constant.text.isEmpty())
        return;

    if (m_pendingConstant && m_pendingConstant->text.length() + constant.text.length() <= m_context.pass().limit(LimitID::MaxLengthCopiedInline)) {
        m_pendingConstant = ConstantText { { }, makeString(m_pendingConstant->text, constant.text) };
        return;
    }
    flushConstant();
    m_pendingConstant = WTF::move(constant);
}

void PartCollector::flushConstant()
{
    if (!m_pendingConstant)
        return;

    ConstantText constant = *std::exchange(m_pendingConstant, std::nullopt);
    StringID string = constant.internedString ? constant.internedString : m_context.pass().strings.intern(constant.text);
    m_parts.append(Part { .form = StringBuilderPartForm::Constant, .constant = string });
}

static SelectionGroup groupWithRanges(SelectionContext& context, ValueID append, const Parts& parts)
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

static Match createMatch(SelectionContext& context, ValueID append)
{
    const Value& value = context.function().at(append);
    PartCollector collector(context, context.shapeAt(append));
    for (ValueID part : value.partsOperands())
        collector.add(part);

    Parts parts = collector.finish();
    SelectionGroup group = groupWithRanges(context, append, parts);
    return { WTF::move(group), context.analyses().useLists().pointOf(append), value.builderOperand(), WTF::move(parts) };
}

static void demoteFirstRange(Match& match)
{
    ValueID definition = match.group.members.first().value;
    Part* part = std::ranges::find(match.parts, definition, &Part::value);
    RELEASE_ASSERT(part != match.parts.end() && part->range);
    part->form = StringBuilderPartForm::String;
    part->range = std::nullopt;
    match.group.members.removeAt(0);
}

static void claim(SelectionContext& context, Match& match)
{
    // What the nearest range may not cross, the ranges in front of it may not cross either.
    while (context.tryClaim(SelectionRuleID::StringBuilderParts, match.group) != ClaimResult::Claimed) {
        RELEASE_ASSERT(match.group.members.size() > 1);
        demoteFirstRange(match);
    }
}

static Opcode rowOf(StringBuilderPartForm form)
{
    switch (form) {
    case StringBuilderPartForm::Value:
        return Opcode::StringBuilderAppendValue;
    case StringBuilderPartForm::Constant:
        return Opcode::StringBuilderAppendConstant;
    case StringBuilderPartForm::Integer:
        return Opcode::StringBuilderAppendInteger;
    case StringBuilderPartForm::Number:
        return Opcode::StringBuilderAppendNumber;
    case StringBuilderPartForm::Boolean:
        return Opcode::StringBuilderAppendBoolean;
    case StringBuilderPartForm::String:
        return Opcode::StringBuilderAppendString;
    case StringBuilderPartForm::StringRange:
        return Opcode::StringBuilderAppendStringRange;
    case StringBuilderPartForm::StringOrNullish:
        return Opcode::StringBuilderAppendStringOrNullish;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeRow(GroupRewriter& rewriter, const Match& match, const Part& part)
{
    Value row = IRBuilder::make(rowOf(part.form), { match.builder });
    switch (part.form) {
    case StringBuilderPartForm::Constant:
        row.setString(part.constant);
        return row;
    case StringBuilderPartForm::StringRange: {
        StringRangeBounds bounds = insertStringRange(rewriter.builder(), match.point, *part.range, std::nullopt);
        row.setStringOperand(part.range->string);
        row.setStartOperand(bounds.start);
        row.setEndOperand(bounds.end);
        return row;
    }
    case StringBuilderPartForm::Value:
    case StringBuilderPartForm::Integer:
    case StringBuilderPartForm::Number:
    case StringBuilderPartForm::Boolean:
    case StringBuilderPartForm::String:
    case StringBuilderPartForm::StringOrNullish:
        row.setValueOperand(part.value);
        return row;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void selectStringBuilderParts(SelectionContext& context)
{
    Vector<Match> matches;
    for (ValueID append : context.valuesWith(Opcode::StringBuilderAppend)) {
        if (context.isClaimed(append))
            continue;
        Match match = createMatch(context, append);
        claim(context, match);
        matches.append(WTF::move(match));
    }

    context.rewriteGroups(SelectionRuleID::StringBuilderParts, matches, [](GroupRewriter& rewriter, const Match& match) {
        for (const Part& part : match.parts)
            rewriter.insert(makeRow(rewriter, match, part));
        rewriter.eraseRoot();
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
