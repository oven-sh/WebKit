/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDiagnostics.h"

#if ENABLE(AOT)

#include "AOTIDPrefix.h"
#include "AOTIR.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/EnumSet.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

static const Typed::Function* functionIfPresent(const Typed::Program* program, Typed::FunctionID id)
{
    if (!program || id.index() >= program->functions.size())
        return nullptr;
    return &program->functions[id.index()];
}

static const Typed::Layout* layoutIfPresent(const Typed::Program* program, Typed::LayoutID id)
{
    if (!program || id.index() >= program->layouts.size())
        return nullptr;
    return &program->layouts[id.index()];
}

Typed::FunctionID findFunctionAsRead(const Typed::Program* program, Typed::FunctionID id)
{
    // The chain is cyclic only in a program that the verifier rejected, whose diagnostics come through here.
    Vector<Typed::FunctionID, 4> visited;
    while (auto* function = functionIfPresent(program, id)) {
        if (id.index() < program->numberOfFunctionsAsRead())
            return id;
        if (visited.contains(id))
            break;
        visited.append(id);
        id = function->derivation.source;
    }
    return { };
}

Typed::LayoutID findLayoutAsRead(const Typed::Program* program, Typed::LayoutID id)
{
    if (!program || id.index() >= program->numberOfLayoutsAsRead())
        return { };
    return id;
}

std::optional<SourceLocation> findLocation(const Typed::Program* program, const Subject& subject)
{
    if (hasModule(subject.location))
        return subject.location;
    if (auto* function = functionIfPresent(program, subject.function)) {
        // Whoever reports gives the place of the value then and there, where it has one. A record that is asked here may be older than a renumbering of its function.
        bool namesValue = subject.value.index() < function->values.size();
#if AOT_STAMPS_VALUE_IDS
        namesValue = namesValue && function->isFromThisNumbering(subject.value);
#endif
        if (namesValue) {
            SourceLocation location = function->locationOf(subject.value);
            if (hasModule(location))
                return location;
        }
        SourceLocation declaration = function->declarationLocation();
        if (hasModule(declaration))
            return declaration;
    }
    auto* layout = layoutIfPresent(program, subject.layout);
    if (!layout || !layout->module)
        return std::nullopt;
    return SourceLocation { layout->module.index(), layout->position };
}

template<typename Record>
static void resolveLocations(Vector<Record>& records, const Typed::Program* program)
{
    if (!program)
        return;
    for (Record& record : records) {
        if (hasModule(record.location))
            continue;
        Subject subject { .function = record.function, .block = record.block, .value = record.value, .layout = record.layout };
        if (auto location = findLocation(program, subject))
            record.location = *location;
    }
}

static auto orderKey(const UserDiagnostic& diagnostic)
{
    const auto& [kind, function, block, value, layout, limit, count, text, location, origin, siteKind, minimum, maximum] = diagnostic;
    return std::tuple { location.module, location.position, function, block, value, layout, kind, limit, count, text, origin.module, origin.position, siteKind, minimum, maximum };
}

namespace {

struct OrderedText {
    StringView text;

    friend bool operator==(const OrderedText& a, const OrderedText& b) { return a.text == b.text; }
    friend std::weak_ordering operator<=>(const OrderedText& a, const OrderedText& b) { return codePointCompare(a.text, b.text); }
};

} // anonymous namespace

static auto orderKey(const InternalError& error)
{
    const auto& [kind, function, block, value, layout, limit, count, text, location, textPosition, pass, detail] = error;
    LineColumn lineColumn = textPosition.value_or(LineColumn { });
    return std::tuple { location.module, location.position, function, block, value, layout, kind, limit, count, text, pass, lineColumn.line, lineColumn.column, OrderedText { detail } };
}

template<typename Record>
static void convertToIDsAsRead(Vector<Record>& records, const Typed::Program* program)
{
    if (!program)
        return;
    for (Record& record : records) {
        Typed::FunctionID function = findFunctionAsRead(program, record.function);
        if (function != record.function) {
            record.function = function;
            record.block = { };
            record.value = { };
        }
        record.layout = findLayoutAsRead(program, record.layout);
    }
}

using Typed::IDPrefix;

namespace {

enum class Placeholder : uint8_t { Function, Block, Value, Layout, Limit, Count, Text, Pass, TextPosition };

struct PlaceholderText {
    Placeholder placeholder;
    ASCIILiteral name;
    ASCIILiteral label;
};

} // anonymous namespace

static constexpr PlaceholderText placeholderTexts[] = {
    { Placeholder::Function, "function"_s, "function "_s },
    { Placeholder::Block, "block"_s, "block "_s },
    { Placeholder::Value, "value"_s, "value "_s },
    { Placeholder::Layout, "layout"_s, "layout "_s },
    { Placeholder::Limit, "limit"_s, "limit "_s },
    { Placeholder::Count, "count"_s, { } },
    { Placeholder::Text, "text"_s, { } },
    { Placeholder::Pass, "pass"_s, "after pass "_s },
    { Placeholder::TextPosition, { }, "line "_s },
};

static Placeholder findPlaceholder(StringView name)
{
    for (auto& entry : placeholderTexts) {
        if (!entry.name.isNull() && name == entry.name)
            return entry.placeholder;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isSet(Placeholder placeholder, const InternalError& fields)
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
    case Placeholder::Text:
        return !!fields.text;
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

static void appendPlaceholder(StringBuilder& builder, Placeholder placeholder, const InternalError& fields, const Typed::Program* program)
{
    if (!isSet(placeholder, fields))
        return;
    switch (placeholder) {
    case Placeholder::Function:
        appendNameAndID(builder, program, functionIfPresent(program, fields.function), IDPrefix::Function, fields.function.index());
        return;
    case Placeholder::Block: {
        builder.append(letterOf(IDPrefix::Block), fields.block.index());
        auto* function = functionIfPresent(program, fields.function);
        if (function && !fields.value && fields.block.index() < function->blocks.size())
            builder.append(" (ends in "_s, nameOf(function->at(fields.block).terminator.kind), ')');
        return;
    }
    case Placeholder::Value: {
        builder.append(letterOf(IDPrefix::Value), fields.value.index());
        auto* function = functionIfPresent(program, fields.function);
        // The text is made at the end of the build. A pass that reports and then compacts its function has given the values other numbers by then.
        bool namesValue = function && fields.value.index() < function->values.size();
#if AOT_STAMPS_VALUE_IDS
        namesValue = namesValue && function->isFromThisNumbering(fields.value);
#endif
        if (namesValue)
            builder.append(" ("_s, nameOf(function->at(fields.value).opcode), ')');
        return;
    }
    case Placeholder::Layout:
        appendNameAndID(builder, program, layoutIfPresent(program, fields.layout), IDPrefix::Layout, fields.layout.index());
        return;
    case Placeholder::Limit:
        builder.append(nameOf(fields.limit), " ("_s, purposeOf(fields.limit), ')');
        return;
    case Placeholder::Count:
        builder.append(fields.count);
        return;
    case Placeholder::Text:
        if (program && fields.text.index() < program->strings.size())
            builder.append(String::fromUTF8ReplacingInvalidSequences(program->string(fields.text)));
        return;
    case Placeholder::Pass:
        builder.append(nameOf(fields.pass));
        return;
    case Placeholder::TextPosition:
        builder.append(fields.textPosition->line, ", column "_s, fields.textPosition->column, " of the IR text"_s);
        return;
    }
}

static EnumSet<Placeholder> appendTemplate(StringBuilder& builder, ASCIILiteral messageTemplate, const InternalError& fields, const Typed::Program* program)
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

static void appendContext(StringBuilder& builder, EnumSet<Placeholder> namedPlaceholders, const InternalError& fields, const Typed::Program* program)
{
    ASCIILiteral separator = "; "_s;
    for (auto& clause : placeholderTexts) {
        if (clause.label.isNull() || namedPlaceholders.contains(clause.placeholder) || !isSet(clause.placeholder, fields))
            continue;
        builder.append(separator, clause.label);
        appendPlaceholder(builder, clause.placeholder, fields, program);
        separator = ", "_s;
    }
}

static String format(ASCIILiteral messageTemplate, const InternalError& fields, const Typed::Program* program, EnumSet<Placeholder> omittedContext)
{
    StringBuilder builder;
    EnumSet<Placeholder> namedPlaceholders = appendTemplate(builder, messageTemplate, fields, program);
    appendContext(builder, namedPlaceholders | omittedContext, fields, program);
    return builder.toString();
}

static String formatMessage(const UserDiagnostic& diagnostic, const Typed::Program* program)
{
    // The message is printed under the line of source it is about, for a reader who has never seen the IR.
    constexpr EnumSet<Placeholder> positionsInIR { Placeholder::Function, Placeholder::Block, Placeholder::Value, Placeholder::Layout };
    InternalError fields {
        .kind = { },
        .function = diagnostic.function,
        .block = diagnostic.block,
        .value = diagnostic.value,
        .layout = diagnostic.layout,
        .limit = diagnostic.limit,
        .count = diagnostic.count,
        .text = diagnostic.text,
    };
    return format(Wire::messageOf(diagnostic.kind), fields, program, positionsInIR);
}

static String formatMessage(const InternalError& error, const Typed::Program* program)
{
    String message = format(Wire::messageOf(error.kind), error, program, { });
    if (error.detail.isNull())
        return message;
    return makeString(message, "; "_s, error.detail);
}

template<typename Record>
static Vector<String> finishRecords(Vector<Record>& records, const Typed::Program* program)
{
    resolveLocations(records, program);
    AOT::sort(records, { }, [](const Record& record) {
        return orderKey(record);
    });
    removeRepeatedElements(records, [](const Record& a, const Record& b) {
        return orderKey(a) == orderKey(b);
    });
    Vector<String> messages = WTF::map(records, [&](const Record& record) {
        return formatMessage(record, program);
    });
    convertToIDsAsRead(records, program);
    return messages;
}

Diagnostics::Diagnostics(Diagnostics&& other)
{
    *this = WTF::move(other);
}

Diagnostics& Diagnostics::operator=(Diagnostics&& other) WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    if (this == &other)
        return *this;
    m_userDiagnostics = std::exchange(other.m_userDiagnostics, { });
    m_internalErrors = std::exchange(other.m_internalErrors, { });
    m_keptFailures = std::exchange(other.m_keptFailures, { });
    m_functionsWithKeptFailure = std::exchange(other.m_functionsWithKeptFailure, { });
    RELEASE_ASSERT(!m_interception && !other.m_interception);
    m_userDiagnosticMessages = std::exchange(other.m_userDiagnosticMessages, { });
    m_internalErrorMessages = std::exchange(other.m_internalErrorMessages, { });
    m_hasErrors.store(other.m_hasErrors.exchange(false, std::memory_order_relaxed), std::memory_order_relaxed);
    m_isFinished = std::exchange(other.m_isFinished, false);
    return *this;
}

static constexpr bool failsBuild(InternalErrorKind) { return true; }

template<typename Record>
void Diagnostics::appendTo(Vector<Record> Diagnostics::* list, Vector<Record>&& records)
{
    if (records.isEmpty())
        return;
    bool containsError = false;
    for (const Record& record : records) {
        RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(record.function));
        containsError |= failsBuild(record.kind);
    }

    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    (this->*list).appendVector(WTF::move(records));
    if (containsError)
        m_hasErrors.store(true, std::memory_order_relaxed);
}

void Diagnostics::add(const UserDiagnostic& diagnostic)
{
    append(Vector<UserDiagnostic> { diagnostic });
}

void Diagnostics::add(const InternalError& error)
{
    append(Vector<InternalError> { error });
}

void Diagnostics::append(Vector<UserDiagnostic>&& diagnostics)
{
    appendTo(&Diagnostics::m_userDiagnostics, WTF::move(diagnostics));
}

void Diagnostics::append(Vector<InternalError>&& errors)
{
    {
        Locker locker { m_lock };
        if (m_interception) {
            m_interception->m_errors.appendVector(WTF::move(errors));
            return;
        }
    }
    appendTo(&Diagnostics::m_internalErrors, WTF::move(errors));
}

Diagnostics::Interception::Interception(Diagnostics& diagnostics)
    : m_diagnostics(diagnostics)
{
    Locker locker { diagnostics.m_lock };
    RELEASE_ASSERT(!diagnostics.m_interception);
    diagnostics.m_interception = this;
}

Diagnostics::Interception::~Interception()
{
    Locker locker { m_diagnostics.m_lock };
    m_diagnostics.m_interception = nullptr;
}

void Diagnostics::keepFirstFailureOfFunction(const InternalError& failure, InternalErrorKind found)
{
    RELEASE_ASSERT(!Typed::ProvisionalFunctions::contains(failure.function));
    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    if (failure.function && m_functionsWithKeptFailure.set(failure.function.index()))
        return;
    m_keptFailures.append({ failure, found });
}

bool Diagnostics::hasKeptFailureOf(Typed::FunctionID function) const
{
    Locker locker { m_lock };
    return function && m_functionsWithKeptFailure.get(function.index());
}

bool Diagnostics::hasKeptFailures() const
{
    Locker locker { m_lock };
    return !m_keptFailures.isEmpty();
}

void Diagnostics::resolveLocations(const Typed::Program& program)
{
    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    AOT::resolveLocations(m_userDiagnostics, &program);
    AOT::resolveLocations(m_internalErrors, &program);
}

void Diagnostics::finish(const Typed::Program* program)
{
    Locker locker { m_lock };
    RELEASE_ASSERT(!m_isFinished);
    if (!m_keptFailures.isEmpty()) {
        struct Row {
            PassID pass;
            InternalErrorKind found;
            uint32_t count;
        };
        Vector<Row> rows;
        for (KeptFailure& kept : m_keptFailures) {
            size_t index = rows.findIf([&](const Row& row) { return row.pass == kept.failure.pass && row.found == kept.found; });
            if (index == notFound)
                rows.append({ kept.failure.pass, kept.found, 1 });
            else
                ++rows[index].count;
            m_internalErrors.append(WTF::move(kept.failure));
        }
        AOT::sort(rows, std::ranges::greater { }, [](const Row& row) { return std::tuple(row.count, static_cast<unsigned>(row.pass), static_cast<unsigned>(row.found)); });
        StringBuilder table;
        for (const Row& row : rows)
            table.append(table.isEmpty() ? ""_s : " . "_s, row.pass == PassID::None ? "typing"_s : nameOf(row.pass), ' ', nameOf(row.found), ' ', row.count);
        m_internalErrors.append(InternalError { .kind = InternalErrorKind::BuildWentOnBehindFailedVerifications, .count = static_cast<uint32_t>(m_keptFailures.size()), .detail = table.toString() });
        m_keptFailures.clear();
        m_hasErrors.store(true, std::memory_order_relaxed);
    }
    m_userDiagnosticMessages = finishRecords(m_userDiagnostics, program);
    m_internalErrorMessages = finishRecords(m_internalErrors, program);
    m_isFinished = true;
}

const Vector<UserDiagnostic>& Diagnostics::userDiagnostics() const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_userDiagnostics;
}

const Vector<InternalError>& Diagnostics::internalErrors() const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_internalErrors;
}

const String& Diagnostics::messageOfUserDiagnostic(size_t index) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_userDiagnosticMessages[index];
}

const String& Diagnostics::messageOfInternalError(size_t index) const WTF_IGNORES_THREAD_SAFETY_ANALYSIS
{
    RELEASE_ASSERT(m_isFinished);
    return m_internalErrorMessages[index];
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
