/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDiagnosticMessages.h"

#if ENABLE(AOT)

#include "AOTIDPrefix.h"
#include "AOTIR.h"
#include <wtf/EnumSet.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

using Typed::IDPrefix;

namespace {

enum class Placeholder : uint8_t { Function, Block, Value, Layout, Limit, Count, Pass, TextPosition };

struct PlaceholderName {
    ASCIILiteral name;
    Placeholder placeholder;
};

struct ContextClause {
    Placeholder placeholder;
    ASCIILiteral label;
};

struct MessageFields {
    Typed::FunctionID function;
    Typed::BlockID block;
    Typed::ValueID value;
    Typed::LayoutID layout;
    LimitID limit { LimitID::None };
    uint32_t count { 0 };
    PassID pass { PassID::None };
    std::optional<LineColumn> textPosition;
};

} // anonymous namespace

static constexpr PlaceholderName placeholderNames[] = {
    { "function"_s, Placeholder::Function },
    { "block"_s, Placeholder::Block },
    { "value"_s, Placeholder::Value },
    { "layout"_s, Placeholder::Layout },
    { "limit"_s, Placeholder::Limit },
    { "count"_s, Placeholder::Count },
    { "pass"_s, Placeholder::Pass },
};

static constexpr ContextClause contextClauses[] = {
    { Placeholder::Function, "function "_s },
    { Placeholder::Block, "block "_s },
    { Placeholder::Value, "value "_s },
    { Placeholder::Layout, "layout "_s },
    { Placeholder::Limit, "limit "_s },
    { Placeholder::Pass, "after pass "_s },
    { Placeholder::TextPosition, "line "_s },
};

static const Typed::Function* tryGetFunction(const Typed::Program* program, Typed::FunctionID id)
{
    if (!program || id.index() >= program->functions.size())
        return nullptr;
    return &program->functions[id.index()];
}

static const Typed::Layout* tryGetLayout(const Typed::Program* program, Typed::LayoutID id)
{
    if (!program || id.index() >= program->layouts.size())
        return nullptr;
    return &program->layouts[id.index()];
}

static Placeholder findPlaceholder(StringView name)
{
    for (auto& entry : placeholderNames) {
        if (name == entry.name)
            return entry.placeholder;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isSet(Placeholder placeholder, const MessageFields& fields)
{
    switch (placeholder) {
    case Placeholder::Function:
        return !!fields.function;
    case Placeholder::Block:
        return !!fields.block;
    case Placeholder::Value:
        return !!fields.value;
    case Placeholder::Layout:
        return !!fields.layout;
    case Placeholder::Limit:
        return fields.limit != LimitID::None;
    case Placeholder::Count:
        return true;
    case Placeholder::Pass:
        return fields.pass != PassID::None;
    case Placeholder::TextPosition:
        return fields.textPosition.has_value();
    }
    return false;
}

template<typename Declaration>
static void appendNameAndID(StringBuilder& builder, const Typed::Program* program, const Declaration* declaration, IDPrefix prefix, uint32_t index)
{
    std::span<const uint8_t> name;
    if (declaration && declaration->name.index() < program->strings.size())
        name = program->string(declaration->name);
    if (name.empty()) {
        builder.append(letterOf(prefix), index);
        return;
    }
    builder.append('\'', String::fromUTF8ReplacingInvalidSequences(name), "' ("_s, letterOf(prefix), index, ')');
}

static void appendPlaceholder(StringBuilder& builder, Placeholder placeholder, const MessageFields& fields, const Typed::Program* program)
{
    if (!isSet(placeholder, fields))
        return;
    switch (placeholder) {
    case Placeholder::Function:
        appendNameAndID(builder, program, tryGetFunction(program, fields.function), IDPrefix::Function, fields.function.index());
        return;
    case Placeholder::Block: {
        builder.append(letterOf(IDPrefix::Block), fields.block.index());
        auto* function = tryGetFunction(program, fields.function);
        if (function && !fields.value && fields.block.index() < function->blocks.size())
            builder.append(" (ends in "_s, nameOf(function->at(fields.block).terminator.kind), ')');
        return;
    }
    case Placeholder::Value: {
        builder.append(letterOf(IDPrefix::Value), fields.value.index());
        auto* function = tryGetFunction(program, fields.function);
        if (function && fields.value.index() < function->values.size())
            builder.append(" ("_s, nameOf(function->at(fields.value).opcode), ')');
        return;
    }
    case Placeholder::Layout:
        appendNameAndID(builder, program, tryGetLayout(program, fields.layout), IDPrefix::Layout, fields.layout.index());
        return;
    case Placeholder::Limit:
        builder.append(nameOf(fields.limit), " ("_s, purposeOf(fields.limit), ')');
        return;
    case Placeholder::Count:
        builder.append(fields.count);
        return;
    case Placeholder::Pass:
        builder.append(nameOf(fields.pass));
        return;
    case Placeholder::TextPosition:
        builder.append(fields.textPosition->line, ", column "_s, fields.textPosition->column, " of the IR text"_s);
        return;
    }
}

static EnumSet<Placeholder> appendTemplate(StringBuilder& builder, ASCIILiteral messageTemplate, const MessageFields& fields, const Typed::Program* program)
{
    EnumSet<Placeholder> namedPlaceholders;
    StringView remainder = messageTemplate;
    while (true) {
        size_t open = remainder.find('{');
        size_t close = open == notFound ? notFound : remainder.find('}', open);
        if (close == notFound)
            break;
        Placeholder placeholder = findPlaceholder(remainder.substring(open + 1, close - open - 1));
        builder.append(remainder.left(open));
        appendPlaceholder(builder, placeholder, fields, program);
        namedPlaceholders.add(placeholder);
        remainder = remainder.substring(close + 1);
    }
    builder.append(remainder);
    return namedPlaceholders;
}

static void appendContext(StringBuilder& builder, EnumSet<Placeholder> namedPlaceholders, const MessageFields& fields, const Typed::Program* program)
{
    ASCIILiteral separator = "; "_s;
    for (auto& clause : contextClauses) {
        if (namedPlaceholders.contains(clause.placeholder) || !isSet(clause.placeholder, fields))
            continue;
        builder.append(separator, clause.label);
        appendPlaceholder(builder, clause.placeholder, fields, program);
        separator = ", "_s;
    }
}

static String format(ASCIILiteral messageTemplate, const MessageFields& fields, const Typed::Program* program, EnumSet<Placeholder> omittedContext)
{
    StringBuilder builder;
    EnumSet<Placeholder> namedPlaceholders = appendTemplate(builder, messageTemplate, fields, program);
    appendContext(builder, namedPlaceholders | omittedContext, fields, program);
    return builder.toString();
}

template<typename Record>
static MessageFields commonFieldsOf(const Record& record)
{
    return {
        .function = record.function,
        .block = record.block,
        .value = record.value,
        .layout = record.layout,
        .limit = record.limit,
        .count = record.count,
    };
}

String formatMessage(const UserDiagnostic& diagnostic, const Typed::Program* program)
{
    // The message is printed under the line of source it is about, for a reader who has never seen the IR.
    constexpr EnumSet<Placeholder> positionsInIR { Placeholder::Function, Placeholder::Block, Placeholder::Value, Placeholder::Layout };
    return format(Wire::messageOf(diagnostic.kind), commonFieldsOf(diagnostic), program, positionsInIR);
}

String formatMessage(const InternalError& error, const Typed::Program* program)
{
    MessageFields fields = commonFieldsOf(error);
    fields.pass = error.pass;
    fields.textPosition = error.textPosition;
    String message = format(Wire::messageOf(error.kind), fields, program, { });
    if (error.detail.isNull())
        return message;
    return makeString(message, "; "_s, error.detail);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
