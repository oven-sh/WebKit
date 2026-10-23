/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCheckLines.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <compare>
#include <expected>
#include <wtf/ASCIICType.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct Position {
    unsigned line;
    unsigned column;

    friend constexpr auto operator<=>(const Position&, const Position&) = default;
};

struct Match {
    unsigned line;
    unsigned start;
    unsigned end;
};

struct Binding {
    StringView name;
    StringView token;
};

struct CheckBlock {
    std::span<const CheckLine> checkLines;
    Position start;
    Position end;
    std::optional<unsigned> labelLine;
};

class Matcher {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(Matcher);
public:
    explicit Matcher(StringView output);

    std::optional<CheckFailure> match(std::span<const CheckLine>);

private:
    using Elements = std::span<const PatternElement>;

    Position outputEnd() const { return { static_cast<unsigned>(m_lines.size()), 0 }; }
    StringView visibleText(unsigned line, Position limit) const;

    bool tryBind(StringView name, StringView token);
    std::optional<unsigned> matchElements(Elements, StringView text, unsigned column);
    std::optional<Match> find(Elements, Position from, Position limit);
    std::optional<Match> findWithoutBinding(Elements, Position from, Position limit);

    std::expected<Vector<CheckBlock>, CheckFailure> findBlocks(std::span<const CheckLine>);
    std::optional<CheckFailure> matchBlock(const CheckBlock&);
    std::expected<Match, CheckFailure> findFromCursor(const CheckLine&, Position cursor, Position blockEnd);
    std::expected<Match, CheckFailure> findOnNextLine(const CheckLine&, unsigned previousLine, Position blockEnd);
    std::optional<CheckFailure> checkExclusions(std::span<const CheckLine* const>, Position from, Position limit);

    String nearestLineNote(const CheckLine&, Position from, Position limit) const;
    String bindingNotes(const CheckLine&) const;

    Vector<StringView> m_lines;
    Vector<Binding, 4> m_bindings;
};

} // anonymous namespace

static bool isTokenCharacter(char16_t character)
{
    return isASCIIAlphanumeric(character) || character == '_' || character == '.' || character == '$';
}

static bool isVariableNameCharacter(char16_t character)
{
    return isASCIIAlphanumeric(character) || character == '_';
}

static unsigned findRunEnd(StringView text, unsigned start, bool (*isInRun)(char16_t))
{
    unsigned end = start;
    while (end < text.length() && isInRun(text[end]))
        ++end;
    return end;
}

static unsigned findVariableNameEnd(StringView pattern, unsigned start)
{
    if (start < pattern.length() && isASCIIDigit(pattern[start]))
        return start;
    return findRunEnd(pattern, start, isVariableNameCharacter);
}

static bool consume(StringView pattern, unsigned& index, StringView expected)
{
    if (!pattern.hasInfixStartingAt(expected, index))
        return false;
    index += expected.length();
    return true;
}

static std::optional<ASCIILiteral> parseElements(StringView pattern, Vector<PatternElement>& elements)
{
    using Kind = PatternElement::Kind;

    if (pattern.isEmpty())
        return "the pattern is empty"_s;

    unsigned textStart = 0;
    unsigned index = 0;
    auto appendPendingText = [&] {
        if (textStart < index)
            elements.append(PatternElement { Kind::Text, pattern.substring(textStart, index - textStart).toString() });
    };
    auto appendElement = [&](Kind kind, String&& text, unsigned end) {
        appendPendingText();
        elements.append(PatternElement { kind, WTF::move(text) });
        textStart = end;
        index = end;
    };

    while (index < pattern.length()) {
        unsigned end = index;
        if (isTabOrSpace(pattern[index]))
            appendElement(Kind::Blanks, { }, findRunEnd(pattern, index, isTabOrSpace<char16_t>));
        else if (consume(pattern, end, "{{"_s)) {
            if (!consume(pattern, end, "*}}"_s))
                return "only {{*}} can be written with {{: there are no regular expressions"_s;
            appendElement(Kind::AnyText, { }, end);
        } else if (consume(pattern, end, "[["_s)) {
            unsigned nameStart = end;
            end = findVariableNameEnd(pattern, nameStart);
            String name = pattern.substring(nameStart, end - nameStart).toString();
            if (name.isEmpty() || !consume(pattern, end, "]]"_s))
                return "a variable is written [[name]]"_s;
            appendElement(Kind::Variable, WTF::move(name), end);
        } else
            ++index;
    }
    appendPendingText();

    bool consumesCharacters = elements.containsIf([](const PatternElement& element) {
        return element.kind == Kind::Text || element.kind == Kind::Variable;
    });
    if (!consumesCharacters)
        return "the pattern matches every line"_s;
    return std::nullopt;
}

std::optional<String> parseCheckPattern(StringView pattern, Vector<PatternElement>& elements)
{
    elements.clear();
    auto error = parseElements(pattern.trim(isTabOrSpace<char16_t>), elements);
    if (!error)
        return std::nullopt;
    elements.clear();
    return String { *error };
}

static bool hasVariable(const CheckLine& checkLine)
{
    return checkLine.elements.containsIf([](const PatternElement& element) {
        return element.kind == PatternElement::Kind::Variable;
    });
}

std::optional<CheckLineError> validateCheckLines(std::span<const CheckLine> checkLines)
{
    const CheckLine* previous = nullptr;
    for (const CheckLine& checkLine : checkLines) {
        if (checkLine.kind == CheckKind::Next && (!previous || previous->kind == CheckKind::Not))
            return CheckLineError { checkLine.line, "CHECK-NEXT needs a match directly before it"_s };
        if (checkLine.kind == CheckKind::Label && hasVariable(checkLine))
            return CheckLineError { checkLine.line, "a label cannot use a variable"_s };
        previous = &checkLine;
    }
    return std::nullopt;
}

std::optional<CheckFailure> matchCheckLines(std::span<const CheckLine> checkLines, StringView output)
{
    return Matcher(output).match(checkLines);
}

Matcher::Matcher(StringView output)
{
    unsigned start = 0;
    while (start < output.length()) {
        size_t newline = output.find('\n', start);
        bool hasNewline = newline != notFound;
        unsigned end = hasNewline ? static_cast<unsigned>(newline) : output.length();
        bool hasCarriageReturn = hasNewline && end > start && output[end - 1] == '\r';
        unsigned textEnd = hasCarriageReturn ? end - 1 : end;
        m_lines.append(output.substring(start, textEnd - start));
        start = end + 1;
    }
}

std::optional<CheckFailure> Matcher::match(std::span<const CheckLine> checkLines)
{
    auto blocks = findBlocks(checkLines);
    if (!blocks)
        return WTF::move(blocks.error());

    for (const CheckBlock& block : *blocks) {
        if (auto failure = matchBlock(block))
            return failure;
    }
    return std::nullopt;
}

StringView Matcher::visibleText(unsigned line, Position limit) const
{
    StringView text = m_lines[line];
    return line == limit.line ? text.left(limit.column) : text;
}

bool Matcher::tryBind(StringView name, StringView token)
{
    for (const Binding& binding : m_bindings) {
        if (binding.name == name)
            return binding.token == token;
    }
    m_bindings.append(Binding { name, token });
    return true;
}

std::optional<unsigned> Matcher::matchElements(Elements elements, StringView text, unsigned column)
{
    for (size_t index = 0; index < elements.size(); ++index) {
        const PatternElement& element = elements[index];
        switch (element.kind) {
        case PatternElement::Kind::Text:
            if (!text.hasInfixStartingAt(element.text, column))
                return std::nullopt;
            column += element.text.length();
            break;

        case PatternElement::Kind::Blanks: {
            unsigned end = findRunEnd(text, column, isTabOrSpace<char16_t>);
            if (end == column)
                return std::nullopt;
            column = end;
            break;
        }

        case PatternElement::Kind::Variable: {
            if (column && isTokenCharacter(text[column - 1]))
                return std::nullopt;
            unsigned end = findRunEnd(text, column, isTokenCharacter);
            if (end == column || !tryBind(element.text, text.substring(column, end - column)))
                return std::nullopt;
            column = end;
            break;
        }

        case PatternElement::Kind::AnyText: {
            Elements rest = elements.subspan(index + 1);
            size_t savedBindingCount = m_bindings.size();
            for (unsigned end = column; end <= text.length(); ++end) {
                if (auto result = matchElements(rest, text, end))
                    return result;
                m_bindings.shrink(savedBindingCount);
            }
            return std::nullopt;
        }
        }
    }
    return column;
}

std::optional<Match> Matcher::find(Elements elements, Position from, Position limit)
{
    size_t savedBindingCount = m_bindings.size();
    for (unsigned line = from.line; line <= limit.line && line < m_lines.size(); ++line) {
        StringView text = visibleText(line, limit);
        for (unsigned start = line == from.line ? from.column : 0; start < text.length(); ++start) {
            if (auto end = matchElements(elements, text, start))
                return Match { line, start, *end };
            m_bindings.shrink(savedBindingCount);
        }
    }
    return std::nullopt;
}

std::optional<Match> Matcher::findWithoutBinding(Elements elements, Position from, Position limit)
{
    size_t savedBindingCount = m_bindings.size();
    auto match = find(elements, from, limit);
    m_bindings.shrink(savedBindingCount);
    return match;
}

static String makeNoMatchMessage(const CheckLine& checkLine, Position from)
{
    return makeString("nothing from output line "_s, from.line + 1, " on matches: "_s, checkLine.pattern);
}

std::expected<Vector<CheckBlock>, CheckFailure> Matcher::findBlocks(std::span<const CheckLine> checkLines)
{
    Vector<CheckBlock> blocks;
    size_t firstIndex = 0;
    Position start { 0, 0 };
    std::optional<unsigned> labelLine;

    for (size_t index = 0; index < checkLines.size(); ++index) {
        const CheckLine& label = checkLines[index];
        if (label.kind != CheckKind::Label)
            continue;

        auto match = find(label.elements.span(), start, outputEnd());
        if (!match)
            return makeUnexpected(CheckFailure { CheckFailureKind::LabelNotFound, label.line, makeNoMatchMessage(label, start) });

        blocks.append(CheckBlock { checkLines.subspan(firstIndex, index - firstIndex), start, { match->line, 0 }, labelLine });
        firstIndex = index + 1;
        start = { match->line, match->end };
        labelLine = match->line;
    }

    blocks.append(CheckBlock { checkLines.subspan(firstIndex), start, outputEnd(), labelLine });
    return blocks;
}

std::optional<CheckFailure> Matcher::matchBlock(const CheckBlock& block)
{
    Position cursor = block.start;
    std::optional<unsigned> previousLine = block.labelLine;
    Vector<const CheckLine*, 4> pendingExclusions;

    for (const CheckLine& checkLine : block.checkLines) {
        switch (checkLine.kind) {
        case CheckKind::Not:
            pendingExclusions.append(&checkLine);
            continue;
        case CheckKind::Next:
            RELEASE_ASSERT(previousLine);
            break;
        case CheckKind::Plain:
        case CheckKind::Count:
            break;
        case CheckKind::Label:
            RELEASE_ASSERT_NOT_REACHED();
        }

        for (unsigned repetition = 0; repetition < checkLine.count; ++repetition) {
            auto match = checkLine.kind == CheckKind::Next
                ? findOnNextLine(checkLine, *previousLine, block.end)
                : findFromCursor(checkLine, cursor, block.end);
            if (!match)
                return WTF::move(match.error());

            if (auto failure = checkExclusions(pendingExclusions.span(), cursor, { match->line, match->start }))
                return failure;

            pendingExclusions.clear();
            cursor = { match->line, match->end };
            previousLine = match->line;
        }
    }
    return checkExclusions(pendingExclusions.span(), cursor, block.end);
}

std::expected<Match, CheckFailure> Matcher::findFromCursor(const CheckLine& checkLine, Position cursor, Position blockEnd)
{
    if (auto match = find(checkLine.elements.span(), cursor, blockEnd))
        return *match;

    return makeUnexpected(CheckFailure { CheckFailureKind::NoMatch, checkLine.line, makeString(
        makeNoMatchMessage(checkLine, cursor), nearestLineNote(checkLine, cursor, blockEnd), bindingNotes(checkLine)) });
}

std::expected<Match, CheckFailure> Matcher::findOnNextLine(const CheckLine& checkLine, unsigned previousLine, Position blockEnd)
{
    Elements elements = checkLine.elements.span();
    Position nextLineStart { previousLine + 1, 0 };
    Position nextLineEnd { previousLine + 2, 0 };
    if (nextLineStart < blockEnd) {
        if (auto match = find(elements, nextLineStart, nextLineEnd))
            return *match;
    }

    auto laterMatch = findWithoutBinding(elements, nextLineEnd, blockEnd);
    return makeUnexpected(CheckFailure { CheckFailureKind::NotOnNextLine, checkLine.line, makeString(
        "output line "_s, nextLineStart.line + 1, " does not match: "_s, checkLine.pattern,
        laterMatch ? makeString(" (line "_s, laterMatch->line + 1, " does)"_s) : String(),
        nearestLineNote(checkLine, nextLineStart, blockEnd), bindingNotes(checkLine)) });
}

std::optional<CheckFailure> Matcher::checkExclusions(std::span<const CheckLine* const> exclusions, Position from, Position limit)
{
    for (const CheckLine* exclusion : exclusions) {
        auto match = findWithoutBinding(exclusion->elements.span(), from, limit);
        if (!match)
            continue;
        return CheckFailure { CheckFailureKind::ExcludedTextFound, exclusion->line, makeString(
            "output line "_s, match->line + 1, " matches the excluded: "_s, exclusion->pattern, bindingNotes(*exclusion)) };
    }
    return std::nullopt;
}

String Matcher::nearestLineNote(const CheckLine& checkLine, Position from, Position limit) const
{
    StringView longestText;
    for (const PatternElement& element : checkLine.elements) {
        if (element.kind == PatternElement::Kind::Text && element.text.length() > longestText.length())
            longestText = element.text;
    }
    if (longestText.isEmpty())
        return { };

    for (unsigned line = from.line; line <= limit.line && line < m_lines.size(); ++line) {
        StringView text = visibleText(line, limit);
        if (text.contains(longestText))
            return makeString("; nearest: "_s, text.trim(isTabOrSpace<char16_t>));
    }
    return { };
}

String Matcher::bindingNotes(const CheckLine& checkLine) const
{
    StringBuilder notes;
    for (const Binding& binding : m_bindings) {
        bool isNamed = checkLine.elements.containsIf([&](const PatternElement& element) {
            return element.kind == PatternElement::Kind::Variable && binding.name == StringView { element.text };
        });
        if (isNamed)
            notes.append("; [["_s, binding.name, "]] is "_s, binding.token);
    }
    return notes.toString();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
