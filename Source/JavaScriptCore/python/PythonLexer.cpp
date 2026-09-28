/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonLexer.h"

#include "VM.h"
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <wtf/ASCIICType.h>
#include <wtf/HexNumber.h>
#include <wtf/dtoa.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Python {

ASCIILiteral tokenKindName(TokenKind kind)
{
    switch (kind) {
    case TokenKind::EndMarker:
        return "ENDMARKER"_s;
    case TokenKind::Error:
        return "ERRORTOKEN"_s;
    case TokenKind::Newline:
        return "NEWLINE"_s;
    case TokenKind::Indent:
        return "INDENT"_s;
    case TokenKind::Dedent:
        return "DEDENT"_s;
    case TokenKind::Name:
        return "NAME"_s;
    case TokenKind::Number:
        return "NUMBER"_s;
    case TokenKind::String:
        return "STRING"_s;
    case TokenKind::FStringStart:
        return "FSTRING_START"_s;
    case TokenKind::FStringMiddle:
        return "FSTRING_MIDDLE"_s;
    case TokenKind::FStringEnd:
        return "FSTRING_END"_s;
    case TokenKind::TStringStart:
        return "TSTRING_START"_s;
    case TokenKind::TStringMiddle:
        return "TSTRING_MIDDLE"_s;
    case TokenKind::TStringEnd:
        return "TSTRING_END"_s;
#define CASE(name, text) \
    case TokenKind::name: \
        return text ""_s;
    FOR_EACH_PYTHON_OPERATOR_TOKEN(CASE)
#undef CASE
#define CASE(name, text) \
    case TokenKind::Keyword##name: \
        return text ""_s;
    FOR_EACH_PYTHON_KEYWORD(CASE)
#undef CASE
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

constexpr unsigned tabSize = 8;
constexpr unsigned maximumIndentation = 100;
constexpr unsigned maximumBracketNesting = 200;
constexpr unsigned maximumStringNesting = 150;
constexpr int maximumExpressionNesting = 3;

template<typename CharacterType>
class Lexer {
public:
    Lexer(VM& vm, Arena& arena, std::span<const CharacterType> source, const ScanRange& range, Vector<Token>& tokens, Vector<SyntaxWarning>& warnings, SyntaxError& error)
        : m_vm(vm)
        , m_arena(arena)
        , m_source(source)
        , m_tokens(tokens)
        , m_warnings(warnings)
        , m_error(error)
        , m_position(range.start)
        , m_end(std::min<size_t>(range.end, source.size()))
        , m_line(range.line)
        , m_lineStart(range.lineStart)
        , m_columnCacheOffset(range.lineStart)
    {
        if (!range.start) {
            if constexpr (sizeof(CharacterType) == 2) {
                if (m_end && m_source[0] == 0xFEFF) {
                    m_position = 1;
                    m_lineStart = 1;
                    m_columnCacheOffset = 1;
                }
            }
            m_indentation.append({ 0, 0 });
            return;
        }

        // In the middle of a line. What it is indented by is what its own blocks are indented further than.
        Indentation indentation;
        for (unsigned i = range.lineStart; i < range.start; ++i) {
            if (!measureIndentation(m_source[i], indentation))
                break;
        }
        m_indentation.append(indentation);
        m_isAtBeginningOfLine = false;
        m_lineHasTokens = true;
        if (range.isInsideBrackets) {
            m_brackets.append({ '(', 0, 0 });
            m_hasEnclosingBracket = true;
        }
    }

    bool run()
    {
        while (!m_isDone) {
            bool ok = !m_strings.isEmpty() && m_strings.last().isScanningText ? scanStringText() : scanToken();
            if (!ok) {
                m_position = std::min(m_position, m_end);
                add(TokenKind::Error, m_position);
                return false;
            }
        }
        return true;
    }

private:
    struct Indentation {
        unsigned column { 0 };
        unsigned alternateColumn { 0 }; // With a tab counting for one, to find indentation that depends on what a tab counts for.
    };

    struct Bracket {
        char character;
        unsigned line;
        unsigned column;
    };

    // An f-string or a t-string that is being scanned. Between its braces are ordinary tokens, and other such strings.
    struct StringState {
        bool isScanningText { true };
        bool isRaw { false };
        bool isTemplate { false };
        bool isInFormatSpecification { false };
        bool isInDebugExpression { false }; // f"{x=}"
        uint8_t quoteSize { 1 };
        CharacterType quote { 0 };
        int braceDepth { 0 };
        int expressionStartDepth { -1 };
        unsigned expressionStart { 0 };
        std::optional<unsigned> expressionEnd;
        unsigned line { 0 };
        unsigned column { 0 };

        char prefix() const { return isTemplate ? 't' : 'f'; }
        bool isInExpression() const { return expressionStartDepth >= 0; }
        bool isAtTopOfExpression() const { return braceDepth - expressionStartDepth == 1; }
    };

    // ---- Reading

    unsigned at(unsigned position) const { return position < m_end ? m_source[position] : 0; }
    unsigned current() const { return at(m_position); }
    bool isAtEnd() const { return m_position >= m_end; }
    static bool isNewline(unsigned c) { return c == '\n' || c == '\r'; }

    // The line that this is on, or the last one there is if this is the end, after its newline.
    unsigned lastLine() const { return isAtEnd() && m_position == m_lineStart && m_line > 1 ? m_line - 1 : m_line; }

    // Takes a newline, however it is spelled.
    void consumeNewline()
    {
        ASSERT(isNewline(current()));
        if (current() == '\r' && at(m_position + 1) == '\n')
            ++m_position;
        ++m_position;
        ++m_line;
        m_lineStart = m_position;
        m_columnCacheOffset = m_position;
        m_columnCacheValue = 0;
    }

    // Takes a character of a string, which may be a newline. That is always '\n'.
    unsigned consumeInString()
    {
        unsigned c = current();
        if (isNewline(c)) {
            consumeNewline();
            return '\n';
        }
        ++m_position;
        return c;
    }

    static unsigned lengthInUTF8(CharacterType c)
    {
        if (c < 0x80)
            return 1;
        if constexpr (sizeof(CharacterType) == 1)
            return 2;
        if (c < 0x800)
            return 2;
        if (U16_IS_SURROGATE(c))
            return 2; // Half of the four that the pair takes.
        return 3;
    }

    // Of a place on the current line.
    unsigned columnOf(unsigned position)
    {
        if (position < m_columnCacheOffset) {
            m_columnCacheOffset = m_lineStart;
            m_columnCacheValue = 0;
        }
        for (; m_columnCacheOffset < position; ++m_columnCacheOffset)
            m_columnCacheValue += lengthInUTF8(m_source[m_columnCacheOffset]);
        return m_columnCacheValue;
    }

    // ---- Results

    // For an error that is of a line and of nowhere in it. What Python sees is one more than this, which is 0.
    static constexpr unsigned noColumn = std::numeric_limits<unsigned>::max();

    bool fail(String&& message, unsigned line, unsigned column, unsigned endLine, unsigned endColumn, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        m_error = { kind, false, WTF::move(message), line, column, endLine, endColumn };
        return false;
    }

    bool fail(String&& message, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        unsigned column = columnOf(std::min(m_position, m_end));
        return fail(WTF::move(message), m_line, column, m_line, column, kind);
    }

    void warn(String&& message, unsigned line)
    {
        m_warnings.append({ WTF::move(message), line });
    }

    // A token that began at `start`, on this line, and ends here.
    Token& add(TokenKind kind, unsigned start)
    {
        unsigned column = columnOf(start);
        return add(kind, start, m_line, column);
    }

    // One that may have begun on an earlier line.
    Token& add(TokenKind kind, unsigned start, unsigned line, unsigned column)
    {
        Token token;
        token.kind = kind;
        token.start = start;
        token.end = m_position;
        token.line = line;
        token.column = column;
        token.endLine = m_line;
        token.endColumn = columnOf(m_position);
        m_tokens.append(token);
        m_lineHasTokens = true;
        return m_tokens.last();
    }

    const Identifier* makeText(std::span<const CharacterType> characters)
    {
        return &m_arena.identifiers().makeIdentifier(m_vm, characters);
    }

    const Identifier* makeText(const Vector<char16_t, 64>& characters, char16_t allCharacters)
    {
        if (allCharacters < 0x100)
            return &m_arena.identifiers().makeLatin1Identifier(m_vm, characters.span());
        return &m_arena.identifiers().makeIdentifier(m_vm, characters.span());
    }

    // ---- Lines and indentation

    static bool measureIndentation(unsigned c, Indentation& indentation)
    {
        if (c == ' ') {
            ++indentation.column;
            ++indentation.alternateColumn;
            return true;
        }
        if (c == '\t') {
            indentation.column = (indentation.column / tabSize + 1) * tabSize;
            ++indentation.alternateColumn;
            return true;
        }
        if (c == '\f') {
            indentation = { };
            return true;
        }
        return false;
    }

    bool failForTabs()
    {
        return fail("inconsistent use of tabs and spaces in indentation"_s, SyntaxError::Kind::TabError);
    }

    // A backslash has been taken. What follows has to be the end of the line.
    bool consumeLineContinuation()
    {
        if (isAtEnd())
            return fail("unexpected EOF while parsing"_s);
        if (!isNewline(current()))
            return fail("unexpected character after line continuation character"_s);
        consumeNewline();
        if (isAtEnd())
            return fail("unexpected EOF while parsing"_s);
        return true;
    }

    // At the beginning of a line: Indent and Dedent tokens for what has changed. Sets m_isBlankLine.
    bool scanIndentation()
    {
        Indentation indentation;
        unsigned columnBeforeContinuation = 0;
        while (true) {
            unsigned c = current();
            if (measureIndentation(c, indentation)) {
                ++m_position;
                continue;
            }
            if (c != '\\')
                break;
            // Indentation cannot be spread over lines: what counts is what was there before the first backslash, if anything was.
            if (!columnBeforeContinuation)
                columnBeforeContinuation = indentation.column;
            ++m_position;
            if (!consumeLineContinuation())
                return false;
        }

        // What is still open at the end is closed by finish().
        if (isAtEnd())
            return true;
        unsigned c = current();
        if (c == '#' || isNewline(c)) {
            // Lines with only a comment or nothing at all have no say in it.
            m_isBlankLine = true;
            return true;
        }
        if (!m_brackets.isEmpty())
            return true;

        if (columnBeforeContinuation)
            indentation = { columnBeforeContinuation, columnBeforeContinuation };

        Indentation last = m_indentation.last();
        if (indentation.column == last.column) {
            if (indentation.alternateColumn != last.alternateColumn)
                return failForTabs();
            return true;
        }
        if (indentation.column > last.column) {
            if (m_indentation.size() >= maximumIndentation)
                return fail("too many levels of indentation"_s, SyntaxError::Kind::IndentationError);
            if (indentation.alternateColumn <= last.alternateColumn)
                return failForTabs();
            m_indentation.append(indentation);
            add(TokenKind::Indent, m_position);
            return true;
        }
        while (m_indentation.size() > 1 && indentation.column < m_indentation.last().column) {
            m_indentation.removeLast();
            add(TokenKind::Dedent, m_position);
        }
        if (indentation.column != m_indentation.last().column) {
            // A function that is scanned by itself ends where something is indented less than it is.
            if (m_indentation.size() == 1 && indentation.column < m_indentation.last().column)
                return true;
            return fail("unindent does not match any outer indentation level"_s, SyntaxError::Kind::IndentationError);
        }
        if (indentation.alternateColumn != m_indentation.last().alternateColumn)
            return failForTabs();
        return true;
    }

    bool finish()
    {
        if (m_brackets.size() > (m_hasEnclosingBracket ? 1 : 0)) {
            Bracket bracket = m_brackets.last();
            fail(makeString('\'', bracket.character, "' was never closed"_s), bracket.line, bracket.column, bracket.line, bracket.column + 1);
            m_error.isUnclosedBracket = true;
            return false;
        }
        if (m_lineHasTokens && !m_hasEnclosingBracket)
            add(TokenKind::Newline, m_position);
        while (m_indentation.size() > 1) {
            m_indentation.removeLast();
            add(TokenKind::Dedent, m_position);
        }
        add(TokenKind::EndMarker, m_position);
        m_isDone = true;
        return true;
    }

    // ---- Tokens

    static bool isIdentifierStart(unsigned c) { return isASCIIAlpha(c) || c == '_' || c >= 0x80; }
    static bool isIdentifierPart(unsigned c) { return isASCIIAlphanumeric(c) || c == '_' || c >= 0x80; }

    bool scanToken()
    {
        while (true) {
            if (m_isAtBeginningOfLine) {
                m_isAtBeginningOfLine = false;
                m_isBlankLine = false;
                m_lineHasTokens = false;
                if (!scanIndentation())
                    return false;
            }

            unsigned c = current();
            while (c == ' ' || c == '\t' || c == '\f')
                c = at(++m_position);

            if (c == '#') {
                while (!isAtEnd() && !isNewline(current()))
                    ++m_position;
                c = current();
            }

            if (isAtEnd())
                return finish();

            if (isNewline(c)) {
                unsigned start = m_position;
                bool isSignificant = !m_isBlankLine && m_brackets.isEmpty();
                if (isSignificant) {
                    // It ends where the line does, and the next line has not begun.
                    m_position += c == '\r' && at(m_position + 1) == '\n' ? 2 : 1;
                    add(TokenKind::Newline, start);
                    m_position = start;
                }
                consumeNewline();
                m_isAtBeginningOfLine = true;
                if (isSignificant)
                    return true;
                continue;
            }

            if (c == '\\') {
                ++m_position;
                if (!consumeLineContinuation())
                    return false;
                continue;
            }

            if (!c)
                return fail("source code cannot contain null bytes"_s);
            if (isIdentifierStart(c))
                return scanNameOrPrefixedString();
            if (isASCIIDigit(c) || (c == '.' && isASCIIDigit(at(m_position + 1))))
                return scanNumber();
            if (c == '"' || c == '\'')
                return scanString(m_position, false, false, false);
            return scanOperator();
        }
    }

    bool scanNameOrPrefixedString()
    {
        unsigned start = m_position;
        bool sawB = false, sawR = false, sawU = false, sawF = false, sawT = false;
        unsigned c = current();
        while (true) {
            if (!sawB && (c == 'b' || c == 'B'))
                sawB = true;
            else if (!sawU && (c == 'u' || c == 'U'))
                sawU = true;
            else if (!sawR && (c == 'r' || c == 'R'))
                sawR = true;
            else if (!sawF && (c == 'f' || c == 'F'))
                sawF = true;
            else if (!sawT && (c == 't' || c == 'T'))
                sawT = true;
            else
                break;
            c = at(++m_position);
            if (c != '"' && c != '\'')
                continue;

            auto incompatible = [&] (char first, char second) {
                unsigned column = columnOf(start);
                return fail(makeString('\'', first, "' and '"_s, second, "' prefixes are incompatible"_s), m_line, column, m_line, columnOf(m_position));
            };
            if (sawU && sawB)
                return incompatible('u', 'b');
            if (sawU && sawR)
                return incompatible('u', 'r');
            if (sawU && sawF)
                return incompatible('u', 'f');
            if (sawU && sawT)
                return incompatible('u', 't');
            if (sawB && sawF)
                return incompatible('b', 'f');
            if (sawB && sawT)
                return incompatible('b', 't');
            if (sawF && sawT)
                return incompatible('f', 't');
            if (sawF || sawT)
                return scanStringStart(start, sawR, sawT);
            return scanString(start, sawR, sawB, sawU);
        }

        bool isASCII = true;
        while (isIdentifierPart(c)) {
            if (c >= 0x80)
                isASCII = false;
            c = at(++m_position);
        }
        auto characters = m_source.subspan(start, m_position - start);
        if (!isASCII)
            return addNormalizedName(start, characters);

        TokenKind kind = keywordOrName(characters);
        Token& token = add(kind, start);
        if (kind != TokenKind::Name)
            return true;
        token.text = makeText(characters);
        token.softKeyword = softKeyword(characters);
        return true;
    }

    static bool equals(std::span<const CharacterType> characters, ASCIILiteral literal)
    {
        if (characters.size() != literal.length())
            return false;
        for (size_t i = 0; i < characters.size(); ++i) {
            if (characters[i] != static_cast<unsigned char>(literal[i]))
                return false;
        }
        return true;
    }

    static TokenKind keywordOrName(std::span<const CharacterType> characters)
    {
        // The shortest is "as" and the longest are "continue" and "nonlocal".
        if (characters.size() < 2 || characters.size() > 8)
            return TokenKind::Name;
#define CHECK(name, text) \
        if (equals(characters, text ""_s)) \
            return TokenKind::Keyword##name;
        FOR_EACH_PYTHON_KEYWORD(CHECK)
#undef CHECK
        return TokenKind::Name;
    }

    static SoftKeyword softKeyword(std::span<const CharacterType> characters)
    {
        switch (characters[0]) {
        case '_':
            return characters.size() == 1 ? SoftKeyword::Underscore : SoftKeyword::None;
        case 'm':
            return equals(characters, "match"_s) ? SoftKeyword::Match : SoftKeyword::None;
        case 'c':
            return equals(characters, "case"_s) ? SoftKeyword::Case : SoftKeyword::None;
        case 't':
            return equals(characters, "type"_s) ? SoftKeyword::Type : SoftKeyword::None;
        default:
            return SoftKeyword::None;
        }
    }

    // A name with something in it that is not ASCII: it has to be made of what Unicode allows in one, and it is its NFKC form that counts.
    bool addNormalizedName(unsigned start, std::span<const CharacterType> characters)
    {
        Vector<char16_t, 64> buffer;
        buffer.appendRange(characters.begin(), characters.end());

        unsigned length = buffer.size();
        for (unsigned i = 0; i < length;) {
            unsigned before = i;
            char32_t c;
            U16_NEXT(buffer.span().data(), i, length, c);
            bool isValid = !before ? c == '_' || u_hasBinaryProperty(c, UCHAR_XID_START) : u_hasBinaryProperty(c, UCHAR_XID_CONTINUE);
            if (isValid)
                continue;
            // Everything up to it is a name, and it is what is wrong.
            m_position = start + before;
            unsigned column = columnOf(m_position);
            unsigned endColumn = columnOf(start + i);
            if (u_isprint(c))
                return fail(makeString("invalid character '"_s, StringView(buffer.span().subspan(before, i - before)), "' (U+"_s, hex(static_cast<unsigned>(c), 4), ')'), m_line, column, m_line, endColumn);
            return fail(makeString("invalid non-printable character U+"_s, hex(static_cast<unsigned>(c), 4)), m_line, column, m_line, endColumn);
        }

        UErrorCode status = U_ZERO_ERROR;
        const UNormalizer2* normalizer = unorm2_getNFKCInstance(&status);
        RELEASE_ASSERT(U_SUCCESS(status));
        Vector<char16_t, 64> normalized;
        if (!unorm2_isNormalized(normalizer, buffer.span().data(), length, &status)) {
            status = U_ZERO_ERROR;
            normalized.grow(length * 2 + 16);
            int32_t normalizedLength = unorm2_normalize(normalizer, buffer.span().data(), length, normalized.mutableSpan().data(), normalized.size(), &status);
            if (status == U_BUFFER_OVERFLOW_ERROR) {
                status = U_ZERO_ERROR;
                normalized.grow(normalizedLength);
                normalizedLength = unorm2_normalize(normalizer, buffer.span().data(), length, normalized.mutableSpan().data(), normalized.size(), &status);
            }
            RELEASE_ASSERT(U_SUCCESS(status));
            normalized.shrink(normalizedLength);
            buffer = WTF::move(normalized);
        }

        char16_t allCharacters = 0;
        for (char16_t c : buffer)
            allCharacters |= c;
        add(TokenKind::Name, start).text = makeText(buffer, allCharacters);
        return true;
    }

    // ---- Numbers

    bool failInNumber(ASCIILiteral kind)
    {
        return fail(makeString("invalid "_s, kind, " literal"_s));
    }

    // Digits, with single underscores between them.
    bool consumeDecimalTail()
    {
        while (true) {
            while (isASCIIDigit(current()))
                ++m_position;
            if (current() != '_')
                return true;
            ++m_position;
            if (!isASCIIDigit(current()))
                return failInNumber("decimal"_s);
        }
    }

    bool isFollowedBy(unsigned position, ASCIILiteral rest) const
    {
        for (size_t i = 0; i < rest.length(); ++i) {
            if (at(position + i) != static_cast<unsigned char>(rest[i]))
                return false;
        }
        return !isIdentifierPart(at(position + rest.length()));
    }

    // A number may run into a keyword, as in 1if x else y, which is frowned upon. It may not run into anything else that could be part of a name.
    bool verifyEndOfNumber(ASCIILiteral kind)
    {
        unsigned c = current();
        bool isKeyword = false;
        switch (c) {
        case 'a':
            isKeyword = isFollowedBy(m_position + 1, "nd"_s);
            break;
        case 'e':
            isKeyword = isFollowedBy(m_position + 1, "lse"_s);
            break;
        case 'f':
            isKeyword = isFollowedBy(m_position + 1, "or"_s);
            break;
        case 'i': {
            unsigned next = at(m_position + 1);
            isKeyword = next == 'f' || next == 'n' || next == 's';
            break;
        }
        case 'o':
            isKeyword = isFollowedBy(m_position + 1, "r"_s);
            break;
        case 'n':
            isKeyword = isFollowedBy(m_position + 1, "ot"_s);
            break;
        default:
            break;
        }
        if (isKeyword) {
            warn(makeString("invalid "_s, kind, " literal"_s), m_line);
            return true;
        }
        if (isIdentifierPart(c)) {
            ++m_position;
            return failInNumber(kind);
        }
        return true;
    }

    template<typename IsDigit>
    bool consumeRadixDigits(ASCIILiteral kind, const IsDigit& isDigit)
    {
        do {
            if (current() == '_')
                ++m_position;
            if (!isDigit(current())) {
                if (isASCIIDigit(current())) {
                    char digit = current();
                    ++m_position;
                    return fail(makeString("invalid digit '"_s, digit, "' in "_s, kind, " literal"_s));
                }
                ++m_position;
                return failInNumber(kind);
            }
            while (isDigit(current()))
                ++m_position;
        } while (current() == '_');
        if (isASCIIDigit(current())) {
            char digit = current();
            ++m_position;
            return fail(makeString("invalid digit '"_s, digit, "' in "_s, kind, " literal"_s));
        }
        return verifyEndOfNumber(kind);
    }

    bool addInteger(unsigned start, unsigned digitsStart, uint8_t radix)
    {
        uint64_t value = 0;
        bool overflowed = false;
        Vector<Latin1Character, 32> digits;
        for (unsigned i = digitsStart; i < m_position; ++i) {
            unsigned c = m_source[i];
            if (c == '_')
                continue;
            digits.append(c);
            uint64_t next = value * radix + toASCIIHexValue(c);
            if (value > (std::numeric_limits<uint64_t>::max() - toASCIIHexValue(c)) / radix)
                overflowed = true;
            value = next;
        }
        Token& token = add(TokenKind::Number, start);
        if (!overflowed) {
            token.numberKind = NumberKind::Integer;
            token.integer = value;
            return true;
        }
        if (unsigned limit = m_arena.maximumDigitsOfIntLiteral; limit && radix == 10 && digits.size() > limit) {
            // Which line is enough. Nobody overlooks such a thing once they are told that.
            return fail(makeString("Exceeds the limit ("_s, limit, " digits) for integer string conversion: value has "_s, digits.size(), " digits; use sys.set_int_max_str_digits() to increase the limit - Consider hexadecimal for huge integer literals to avoid decimal conversion limits."_s), m_line, noColumn, m_line, noColumn);
        }
        token.numberKind = NumberKind::BigInteger;
        token.radix = radix;
        token.text = &m_arena.identifiers().makeIdentifier(m_vm, digits.span());
        return true;
    }

    bool addReal(unsigned start, NumberKind kind)
    {
        Vector<Latin1Character, 32> characters;
        unsigned end = kind == NumberKind::Imaginary ? m_position - 1 : m_position;
        for (unsigned i = start; i < end; ++i) {
            if (m_source[i] != '_')
                characters.append(m_source[i]);
        }
        size_t parsedLength;
        double value = parseDouble(characters.span(), parsedLength);
        ASSERT(parsedLength == characters.size());
        Token& token = add(TokenKind::Number, start);
        token.numberKind = kind;
        token.real = value;
        return true;
    }

    bool scanNumber()
    {
        unsigned start = m_position;
        bool isReal = false;

        if (current() == '0') {
            unsigned next = at(m_position + 1);
            if (next == 'x' || next == 'X') {
                m_position += 2;
                return consumeRadixDigits("hexadecimal"_s, isASCIIHexDigit<unsigned>) && addInteger(start, start + 2, 16);
            }
            if (next == 'o' || next == 'O') {
                m_position += 2;
                return consumeRadixDigits("octal"_s, isASCIIOctalDigit<unsigned>) && addInteger(start, start + 2, 8);
            }
            if (next == 'b' || next == 'B') {
                m_position += 2;
                return consumeRadixDigits("binary"_s, isASCIIBinaryDigit<unsigned>) && addInteger(start, start + 2, 2);
            }

            // Any number of zeros is zero. Other digits after them are only good for a float.
            ++m_position;
            while (true) {
                if (current() == '_') {
                    ++m_position;
                    if (!isASCIIDigit(current()))
                        return failInNumber("decimal"_s);
                }
                if (current() != '0')
                    break;
                ++m_position;
            }
            unsigned zerosEnd = m_position;
            bool hasOtherDigits = isASCIIDigit(current());
            if (hasOtherDigits && !consumeDecimalTail())
                return false;
            unsigned c = current();
            if (hasOtherDigits && c != '.' && c != 'e' && c != 'E' && c != 'j' && c != 'J') {
                unsigned column = columnOf(start);
                return fail("leading zeros in decimal integer literals are not permitted; use an 0o prefix for octal integers"_s, m_line, column, m_line, columnOf(zerosEnd));
            }
        } else if (current() != '.') {
            if (!consumeDecimalTail())
                return false;
        }

        if (current() == '.') {
            isReal = true;
            ++m_position;
            if (isASCIIDigit(current()) && !consumeDecimalTail())
                return false;
        }

        if (current() == 'e' || current() == 'E') {
            unsigned next = at(m_position + 1);
            if (next == '+' || next == '-') {
                m_position += 2;
                if (!isASCIIDigit(current()))
                    return failInNumber("decimal"_s);
                isReal = true;
                if (!consumeDecimalTail())
                    return false;
            } else if (isASCIIDigit(next)) {
                ++m_position;
                isReal = true;
                if (!consumeDecimalTail())
                    return false;
            } else {
                // Not an exponent. It had better be "else".
                if (!verifyEndOfNumber("decimal"_s))
                    return false;
                return isReal ? addReal(start, NumberKind::Float) : addInteger(start, start, 10);
            }
        }

        if (current() == 'j' || current() == 'J') {
            ++m_position;
            return verifyEndOfNumber("imaginary"_s) && addReal(start, NumberKind::Imaginary);
        }
        if (!verifyEndOfNumber("decimal"_s))
            return false;
        return isReal ? addReal(start, NumberKind::Float) : addInteger(start, start, 10);
    }

    // ---- Strings

    static void appendCodePoint(Vector<char16_t, 64>& buffer, char16_t& allCharacters, char32_t c)
    {
        if (U_IS_BMP(c)) {
            buffer.append(static_cast<char16_t>(c));
            allCharacters |= static_cast<char16_t>(c);
            return;
        }
        buffer.append(U16_LEAD(c));
        buffer.append(U16_TRAIL(c));
        allCharacters |= 0xFF00;
    }

    // The value of what is between the quotes, or of a piece of it. Null if it has none, and then there is an error.
    const Identifier* decodeString(unsigned start, unsigned end, bool isRaw, bool isBytes, unsigned line, unsigned column)
    {
        auto failHere = [&] (String&& message) -> const Identifier* {
            fail(WTF::move(message), line, column, m_line, columnOf(m_position));
            return nullptr;
        };

        bool isPlain = true;
        for (unsigned i = start; i < end; ++i) {
            unsigned c = m_source[i];
            if (isBytes && c >= 0x80)
                return failHere("bytes can only contain ASCII literal characters"_s);
            if (c == '\r' || (c == '\\' && !isRaw))
                isPlain = false;
        }
        if (isPlain)
            return makeText(m_source.subspan(start, end - start));

        Vector<char16_t, 64> buffer;
        char16_t allCharacters = 0;
        auto append = [&] (unsigned c) {
            buffer.append(static_cast<char16_t>(c));
            allCharacters |= static_cast<char16_t>(c);
        };

        unsigned currentLine = line;
        for (unsigned i = start; i < end;) {
            unsigned c = m_source[i++];
            if (c == '\r') {
                if (i < end && m_source[i] == '\n')
                    ++i;
                c = '\n';
            }
            if (c == '\n')
                ++currentLine;
            if (c != '\\' || isRaw) {
                append(c);
                continue;
            }
            // Only a piece of an f-string can end in a backslash, before a brace. It stands for itself.
            if (i >= end) {
                append('\\');
                break;
            }

            unsigned escapeStart = i - 1 - start;
            c = m_source[i++];
            switch (c) {
            case '\r':
                if (i < end && m_source[i] == '\n')
                    ++i;
                [[fallthrough]];
            case '\n':
                ++currentLine;
                break;
            case '\\':
            case '\'':
            case '"':
                append(c);
                break;
            case 'a':
                append('\a');
                break;
            case 'b':
                append('\b');
                break;
            case 'f':
                append('\f');
                break;
            case 'n':
                append('\n');
                break;
            case 'r':
                append('\r');
                break;
            case 't':
                append('\t');
                break;
            case 'v':
                append('\v');
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7': {
                unsigned digitsStart = i - 1;
                unsigned value = c - '0';
                for (unsigned count = 1; count < 3 && i < end && isASCIIOctalDigit(m_source[i]); ++count)
                    value = value * 8 + (m_source[i++] - '0');
                if (value > 0377) {
                    StringView digits { m_source.subspan(digitsStart, i - digitsStart) };
                    warn(makeString("\"\\"_s, digits, "\" is an invalid octal escape sequence. Such sequences will not work in the future. Did you mean \"\\\\"_s, digits, "\"? A raw string is also an option."_s), currentLine);
                }
                append(isBytes ? value & 0xFF : value);
                break;
            }
            case 'x':
            case 'u':
            case 'U': {
                if (isBytes && c != 'x') {
                    warnAboutEscape(c, currentLine);
                    append('\\');
                    append(c);
                    break;
                }
                unsigned count = c == 'x' ? 2 : c == 'u' ? 4 : 8;
                char32_t value = 0;
                unsigned digits = 0;
                for (; digits < count && i < end && isASCIIHexDigit(m_source[i]); ++digits)
                    value = value * 16 + toASCIIHexValue(m_source[i++]);
                if (digits < count) {
                    if (isBytes)
                        return failHere(makeString("(value error) invalid \\x escape at position "_s, escapeStart));
                    ASCIILiteral form = c == 'x' ? "\\xXX"_s : c == 'u' ? "\\uXXXX"_s : "\\UXXXXXXXX"_s;
                    return failHere(makeString("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + 1 + digits, ": truncated "_s, form, " escape"_s));
                }
                if (value > UCHAR_MAX_VALUE)
                    return failHere(makeString("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + 9, ": illegal Unicode character"_s));
                appendCodePoint(buffer, allCharacters, value);
                break;
            }
            case 'N': {
                if (isBytes) {
                    warnAboutEscape(c, currentLine);
                    append('\\');
                    append(c);
                    break;
                }
                unsigned nameEnd = i;
                if (i < end && m_source[i] == '{') {
                    nameEnd = i + 1;
                    while (nameEnd < end && m_source[nameEnd] != '}')
                        ++nameEnd;
                }
                if (nameEnd <= i + 1 || nameEnd >= end)
                    return failHere(makeString("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + (nameEnd > i ? nameEnd - i + 1 : 1), ": malformed \\N character escape"_s));
                Vector<char, 64> name;
                bool isASCII = true;
                for (unsigned k = i + 1; k < nameEnd; ++k) {
                    if (m_source[k] >= 0x80)
                        isASCII = false;
                    name.append(toASCIIUpper(static_cast<char>(m_source[k])));
                }
                name.append('\0');
                UErrorCode status = U_ZERO_ERROR;
                char32_t value = isASCII ? u_charFromName(U_UNICODE_CHAR_NAME, name.span().data(), &status) : 0;
                if (isASCII && U_FAILURE(status)) {
                    status = U_ZERO_ERROR;
                    value = u_charFromName(U_CHAR_NAME_ALIAS, name.span().data(), &status);
                }
                if (!isASCII || U_FAILURE(status))
                    return failHere(makeString("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + (nameEnd - i) + 2, ": unknown Unicode character name"_s));
                appendCodePoint(buffer, allCharacters, value);
                i = nameEnd + 1;
                break;
            }
            default:
                // Not an escape, so both stay.
                warnAboutEscape(c, currentLine);
                append('\\');
                append(c);
                break;
            }
        }
        return makeText(buffer, allCharacters);
    }

    void warnAboutEscape(unsigned c, unsigned line)
    {
        char16_t character = c;
        StringView view { std::span<const char16_t> { &character, 1 } };
        warn(makeString("\"\\"_s, view, "\" is an invalid escape sequence. Such sequences will not work in the future. Did you mean \"\\\\"_s, view, "\"? A raw string is also an option."_s), line);
    }

    // At the opening quote. `start` is where the prefix began.
    bool scanString(unsigned start, bool isRaw, bool isBytes, bool hasUnicodePrefix)
    {
        unsigned line = m_line;
        unsigned column = columnOf(start);
        unsigned quote = current();
        unsigned quoteSize = at(m_position + 1) == quote && at(m_position + 2) == quote ? 3 : 1;
        unsigned quoteColumn = columnOf(m_position);
        m_position += quoteSize;
        unsigned contentStart = m_position;

        bool hasEscapedQuote = false;
        unsigned endQuoteSize = 0;
        while (endQuoteSize != quoteSize) {
            if (isAtEnd() || (quoteSize == 1 && isNewline(current()))) {
                unsigned detectedAt = lastLine();
                // In f"{x" the second quote was meant to end the whole, and what is missing is the brace.
                if (!m_strings.isEmpty() && m_strings.last().quote == quote && m_strings.last().quoteSize == quoteSize)
                    return fail(makeString(m_strings.last().prefix(), "-string: expecting '}'"_s), line, quoteColumn, line, quoteColumn);
                if (quoteSize == 3)
                    return fail(makeString("unterminated triple-quoted string literal (detected at line "_s, detectedAt, ')'), line, quoteColumn, line, quoteColumn);
                if (hasEscapedQuote)
                    return fail(makeString("unterminated string literal (detected at line "_s, detectedAt, "); perhaps you escaped the end quote?"_s), line, quoteColumn, line, quoteColumn);
                return fail(makeString("unterminated string literal (detected at line "_s, detectedAt, ')'), line, quoteColumn, line, quoteColumn);
            }
            unsigned c = consumeInString();
            if (c == quote) {
                ++endQuoteSize;
                continue;
            }
            endQuoteSize = 0;
            if (c == '\\' && !isAtEnd()) {
                if (consumeInString() == quote)
                    hasEscapedQuote = true;
            }
        }

        const Identifier* value = decodeString(contentStart, m_position - quoteSize, isRaw, isBytes, line, column);
        if (!value)
            return false;
        Token& token = add(TokenKind::String, start, line, column);
        token.text = value;
        token.isBytes = isBytes;
        token.hasUnicodePrefix = hasUnicodePrefix;
        return true;
    }

    // ---- f-strings and t-strings

    // At the opening quote of one.
    bool scanStringStart(unsigned start, bool isRaw, bool isTemplate)
    {
        if (m_strings.size() >= maximumStringNesting)
            return fail("too many nested f-strings or t-strings"_s);

        StringState state;
        state.isRaw = isRaw;
        state.isTemplate = isTemplate;
        state.quote = current();
        state.quoteSize = at(m_position + 1) == state.quote && at(m_position + 2) == state.quote ? 3 : 1;
        state.line = m_line;
        state.column = columnOf(m_position);
        m_position += state.quoteSize;
        add(isTemplate ? TokenKind::TStringStart : TokenKind::FStringStart, start);
        m_strings.append(state);
        return true;
    }

    // The text is what is between `start` and `end`. Of two braces that stand for one, the second is part of the token and not of the text.
    bool addStringText(StringState& state, unsigned start, unsigned end, unsigned line, unsigned column)
    {
        const Identifier* value = decodeString(start, end, state.isRaw, false, line, column);
        if (!value)
            return false;
        Token& token = add(state.isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle, start, line, column);
        token.text = value;
        return true;
    }

    bool enterExpression(StringState& state)
    {
        if (++state.expressionStartDepth >= maximumExpressionNesting)
            return fail(makeString(state.prefix(), "-string: expressions nested too deeply"_s));
        state.isScanningText = false;
        return true;
    }

    // The text of an f-string or of a format specification, up to a brace or the closing quotes.
    bool scanStringText()
    {
        StringState& state = m_strings.last();
        unsigned start = m_position;
        unsigned line = m_line;
        unsigned column = columnOf(start);

        if (current() == '{' && at(m_position + 1) != '{')
            return enterExpression(state) && scanToken();

        bool isAtClosingQuotes = true;
        for (unsigned i = 0; i < state.quoteSize; ++i) {
            if (at(m_position + i) != state.quote)
                isAtClosingQuotes = false;
        }
        if (isAtClosingQuotes) {
            m_position += state.quoteSize;
            add(state.isTemplate ? TokenKind::TStringEnd : TokenKind::FStringEnd, start);
            m_strings.removeLast();
            return true;
        }

        bool isInNamedEscape = false;
        unsigned endQuoteSize = 0;
        while (endQuoteSize != state.quoteSize) {
            bool isInFormatSpecification = state.isInFormatSpecification && state.isInExpression();
            if (isAtEnd() || (state.quoteSize == 1 && isNewline(current()))) {
                if (isInFormatSpecification && !isAtEnd())
                    return fail(makeString(state.prefix(), "-string: newlines are not allowed in format specifiers for single quoted "_s, state.prefix(), "-strings"_s));
                unsigned detectedAt = lastLine();
                if (state.quoteSize == 3)
                    return fail(makeString("unterminated triple-quoted "_s, state.prefix(), "-string literal (detected at line "_s, detectedAt, ')'), state.line, state.column, state.line, state.column);
                return fail(makeString("unterminated "_s, state.prefix(), "-string literal (detected at line "_s, detectedAt, ')'), state.line, state.column, state.line, state.column);
            }

            unsigned c = consumeInString();
            if (c == state.quote) {
                ++endQuoteSize;
                continue;
            }
            endQuoteSize = 0;

            if (c == '{') {
                state.expressionStart = m_position;
                state.expressionEnd = std::nullopt;
                if (current() != '{' || isInFormatSpecification) {
                    --m_position;
                    if (!enterExpression(state))
                        return false;
                    state.isInFormatSpecification = false;
                    return addStringText(state, start, m_position, line, column);
                }
                ++m_position;
                return addStringText(state, start, m_position - 1, line, column);
            }

            if (c == '}') {
                if (isInNamedEscape)
                    return addStringText(state, start, m_position, line, column);
                if (current() == '}' && !isInFormatSpecification && !state.braceDepth) {
                    ++m_position;
                    return addStringText(state, start, m_position - 1, line, column);
                }
                --m_position;
                state.isScanningText = false;
                state.isInFormatSpecification = false;
                return addStringText(state, start, m_position, line, column);
            }

            if (c == '\\') {
                unsigned next = current();
                // Before a brace it is only a backslash, and the brace is still a brace.
                if (next == '{' || next == '}') {
                    if (!state.isRaw)
                        warnAboutEscape(next, m_line);
                    continue;
                }
                if (isAtEnd())
                    continue;
                consumeInString();
                if (!state.isRaw && next == 'N' && current() == '{') {
                    ++m_position;
                    isInNamedEscape = true;
                }
            }
        }

        // The closing quotes are the next token.
        m_position -= state.quoteSize;
        return addStringText(state, start, m_position, line, column);
    }

    // The source of the expression between braces, without its comments.
    const Identifier* makeExpressionSource(const StringState& state)
    {
        // Whether there are any. A quote counts as opening or closing a string, which comes out right for three of them too.
        bool hasComment = false;
        unsigned quote = 0;
        for (unsigned i = state.expressionStart; i < *state.expressionEnd && !hasComment; ++i) {
            unsigned c = m_source[i];
            if (c == '\\')
                ++i;
            else if (c == '"' || c == '\'') {
                if (!quote)
                    quote = c;
                else if (c == quote)
                    quote = 0;
            } else if (c == '#' && !quote)
                hasComment = true;
        }

        Vector<char16_t, 64> buffer;
        char16_t allCharacters = 0;
        quote = 0;
        for (unsigned i = state.expressionStart; i < *state.expressionEnd; ++i) {
            unsigned c = m_source[i];
            if (c == '\r') {
                if (at(i + 1) == '\n')
                    ++i;
                c = '\n';
            }
            if (!hasComment) {
                // As it is.
            } else if (c == '"' || c == '\'') {
                if (!quote)
                    quote = c;
                else if (c == quote)
                    quote = 0;
            } else if (c == '#' && !quote) {
                while (i < *state.expressionEnd && !isNewline(m_source[i]))
                    ++i;
                if (i >= *state.expressionEnd)
                    break;
                if (m_source[i] == '\r' && at(i + 1) == '\n')
                    ++i;
                c = '\n';
            }
            buffer.append(static_cast<char16_t>(c));
            allCharacters |= static_cast<char16_t>(c);
        }
        return makeText(buffer, allCharacters);
    }

    // ---- Operators

    static std::optional<TokenKind> oneCharacterToken(unsigned c)
    {
        switch (c) {
        case '(':
            return TokenKind::LeftParenthesis;
        case ')':
            return TokenKind::RightParenthesis;
        case '[':
            return TokenKind::LeftBracket;
        case ']':
            return TokenKind::RightBracket;
        case '{':
            return TokenKind::LeftBrace;
        case '}':
            return TokenKind::RightBrace;
        case ':':
            return TokenKind::Colon;
        case ',':
            return TokenKind::Comma;
        case ';':
            return TokenKind::Semicolon;
        case '+':
            return TokenKind::Plus;
        case '-':
            return TokenKind::Minus;
        case '*':
            return TokenKind::Star;
        case '/':
            return TokenKind::Slash;
        case '|':
            return TokenKind::VerticalBar;
        case '&':
            return TokenKind::Ampersand;
        case '<':
            return TokenKind::Less;
        case '>':
            return TokenKind::Greater;
        case '=':
            return TokenKind::Equal;
        case '.':
            return TokenKind::Dot;
        case '%':
            return TokenKind::Percent;
        case '~':
            return TokenKind::Tilde;
        case '^':
            return TokenKind::Circumflex;
        case '@':
            return TokenKind::At;
        case '!':
            return TokenKind::Exclamation;
        default:
            return std::nullopt;
        }
    }

    static std::optional<TokenKind> twoCharacterToken(unsigned first, unsigned second)
    {
        switch (first) {
        case '!':
            return second == '=' ? std::optional { TokenKind::NotEqual } : std::nullopt;
        case '%':
            return second == '=' ? std::optional { TokenKind::PercentEqual } : std::nullopt;
        case '&':
            return second == '=' ? std::optional { TokenKind::AmpersandEqual } : std::nullopt;
        case '*':
            return second == '*' ? std::optional { TokenKind::DoubleStar } : second == '=' ? std::optional { TokenKind::StarEqual } : std::nullopt;
        case '+':
            return second == '=' ? std::optional { TokenKind::PlusEqual } : std::nullopt;
        case '-':
            return second == '=' ? std::optional { TokenKind::MinusEqual } : second == '>' ? std::optional { TokenKind::Arrow } : std::nullopt;
        case '/':
            return second == '/' ? std::optional { TokenKind::DoubleSlash } : second == '=' ? std::optional { TokenKind::SlashEqual } : std::nullopt;
        case ':':
            return second == '=' ? std::optional { TokenKind::ColonEqual } : std::nullopt;
        case '<':
            return second == '<' ? std::optional { TokenKind::LeftShift } : second == '=' ? std::optional { TokenKind::LessEqual } : second == '>' ? std::optional { TokenKind::NotEqual } : std::nullopt;
        case '=':
            return second == '=' ? std::optional { TokenKind::EqualEqual } : std::nullopt;
        case '>':
            return second == '=' ? std::optional { TokenKind::GreaterEqual } : second == '>' ? std::optional { TokenKind::RightShift } : std::nullopt;
        case '@':
            return second == '=' ? std::optional { TokenKind::AtEqual } : std::nullopt;
        case '^':
            return second == '=' ? std::optional { TokenKind::CircumflexEqual } : std::nullopt;
        case '|':
            return second == '=' ? std::optional { TokenKind::VerticalBarEqual } : std::nullopt;
        default:
            return std::nullopt;
        }
    }

    static std::optional<TokenKind> threeCharacterToken(TokenKind firstTwo, unsigned third)
    {
        if (third != '=')
            return std::nullopt;
        switch (firstTwo) {
        case TokenKind::DoubleStar:
            return TokenKind::DoubleStarEqual;
        case TokenKind::DoubleSlash:
            return TokenKind::DoubleSlashEqual;
        case TokenKind::LeftShift:
            return TokenKind::LeftShiftEqual;
        case TokenKind::RightShift:
            return TokenKind::RightShiftEqual;
        default:
            return std::nullopt;
        }
    }

    bool scanOperator()
    {
        unsigned start = m_position;
        unsigned c = current();
        ++m_position;

        StringState* state = m_strings.isEmpty() ? nullptr : &m_strings.last();
        const Identifier* expressionSource = nullptr;

        if (state && state->isInExpression() && (c == ':' || c == '}' || c == '!' || c == '{')) {
            // The depth has not been changed for this character yet.
            int depth = state->braceDepth - (c != '{');
            bool isEndOfExpression = !depth || (depth == 1 && (state->isInDebugExpression || state->isInFormatSpecification));
            if (isEndOfExpression) {
                if (c == '{') {
                    state->expressionStart = m_position;
                    state->expressionEnd = std::nullopt;
                } else {
                    if (c != ':' || !state->expressionEnd)
                        state->expressionEnd = start;
                    if (state->isInDebugExpression || state->isTemplate)
                        expressionSource = makeExpressionSource(*state);
                }
            }
            if (c == ':' && depth == state->expressionStartDepth) {
                state->isScanningText = true;
                state->isInFormatSpecification = true;
                add(TokenKind::Colon, start).expressionSource = expressionSource;
                return true;
            }
        }

        if (c == '.' && current() == '.' && at(m_position + 1) == '.') {
            m_position += 2;
            add(TokenKind::Ellipsis, start);
            return true;
        }

        if (auto two = twoCharacterToken(c, current())) {
            bool isLessGreater = c == '<' && current() == '>';
            ++m_position;
            if (auto three = threeCharacterToken(*two, current())) {
                ++m_position;
                add(*three, start);
                return true;
            }
            add(*two, start).isLessGreater = isLessGreater;
            return true;
        }

        switch (c) {
        case '(':
        case '[':
        case '{':
            if (m_brackets.size() >= maximumBracketNesting)
                return fail("too many nested parentheses"_s);
            m_brackets.append({ static_cast<char>(c), m_line, columnOf(start) });
            if (state)
                ++state->braceDepth;
            break;
        case ')':
        case ']':
        case '}': {
            char character = c;
            if (state && !state->braceDepth && c == '}')
                return fail(makeString(state->prefix(), "-string: single '}' is not allowed"_s));
            if (m_brackets.size() <= (m_hasEnclosingBracket ? 1u : 0u)) {
                // What closes the bracket that a lambda is in is where the lambda ends.
                if (m_hasEnclosingBracket) {
                    m_position = start;
                    m_end = start;
                    return finish();
                }
                return fail(makeString("unmatched '"_s, character, '\''));
            }
            Bracket opening = m_brackets.takeLast();
            if (!((opening.character == '(' && c == ')') || (opening.character == '[' && c == ']') || (opening.character == '{' && c == '}'))) {
                if (state && opening.character == '{' && state->braceDepth - 1 == state->expressionStartDepth)
                    return fail(makeString(state->prefix(), "-string: unmatched '"_s, character, '\''));
                if (opening.line != m_line)
                    return fail(makeString("closing parenthesis '"_s, character, "' does not match opening parenthesis '"_s, opening.character, "' on line "_s, opening.line));
                return fail(makeString("closing parenthesis '"_s, character, "' does not match opening parenthesis '"_s, opening.character, '\''));
            }
            if (state) {
                if (--state->braceDepth < 0)
                    return fail(makeString(state->prefix(), "-string: unmatched '"_s, character, '\''));
                if (c == '}' && state->braceDepth == state->expressionStartDepth) {
                    --state->expressionStartDepth;
                    state->isScanningText = true;
                    state->isInFormatSpecification = false;
                    state->isInDebugExpression = false;
                }
            }
            break;
        }
        default:
            break;
        }

        auto kind = oneCharacterToken(c);
        if (!kind) {
            m_position = start;
            unsigned column = columnOf(start);
            if (c < 0x20 || c == 0x7F)
                return fail(makeString("invalid non-printable character U+"_s, hex(c, 4)), m_line, column, m_line, column + 1);
            return fail("invalid syntax"_s, m_line, column, m_line, column + 1);
        }

        if (c == '=' && state && state->isInExpression() && state->isAtTopOfExpression())
            state->isInDebugExpression = true;

        add(*kind, start).expressionSource = expressionSource;
        return true;
    }

    VM& m_vm;
    Arena& m_arena;
    std::span<const CharacterType> m_source;
    Vector<Token>& m_tokens;
    Vector<SyntaxWarning>& m_warnings;
    SyntaxError& m_error;

    unsigned m_position;
    unsigned m_end;
    unsigned m_line;
    unsigned m_lineStart;
    unsigned m_columnCacheOffset;
    unsigned m_columnCacheValue { 0 };

    bool m_isAtBeginningOfLine { true };
    bool m_isBlankLine { false };
    bool m_lineHasTokens { false };
    bool m_hasEnclosingBracket { false };
    bool m_isDone { false };

    Vector<Indentation, 16> m_indentation;
    Vector<Bracket, 16> m_brackets;
    Vector<StringState, 4> m_strings;
};

} // anonymous namespace

bool tokenize(VM& vm, Arena& arena, StringView source, const ScanRange& range, Vector<Token>& tokens, Vector<SyntaxWarning>& warnings, SyntaxError& error)
{
    if (source.is8Bit())
        return Lexer<Latin1Character>(vm, arena, source.span8(), range, tokens, warnings, error).run();
    return Lexer<char16_t>(vm, arena, source.span16(), range, tokens, warnings, error).run();
}

} } // namespace JSC::Python

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
