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
#include "PythonText.h"

#include "PythonUnicodeType.h"
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
    case TokenKind::Invalid:
        return "OP"_s;
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
    case TokenKind::TypeComment:
        return "TYPE_COMMENT"_s;
    case TokenKind::TypeIgnore:
        return "TYPE_IGNORE"_s;
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
        , m_lastLine(range.lastLine)
    {
        if (!range.start) {
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

    // What a TokenStream has besides.
    struct Stream {
        LineSource& lines;
        Vector<char16_t>& buffer;
        Vector<StreamedToken>& tokens;
        bool hasExtraTokens;
        Vector<unsigned> lineStarts { }; // Of each line, from the first.
        bool hasFailedToRead { false };
        bool isAtEndOfFile { false }; // tok->done == E_EOF
        bool hasImplicitNewline { false };
        bool isBetweenTokens { true }; // tok->start == NULL
        bool isAfterCommentOnBlankLine { false }; // tok->comment_newline
        std::optional<unsigned> inputEndAtEndOfFile;
        unsigned firstLine { 0 };
        unsigned multiLineStart { 0 };
    };

    // For a TokenStream.
    Lexer(VM& vm, Arena& arena, Stream& stream, Vector<Token>& tokens, Vector<SyntaxWarning>& warnings, SyntaxError& error)
        : Lexer(vm, arena, std::span<const CharacterType> { }, ScanRange { }, tokens, warnings, error)
    {
        m_stream = &stream;
        m_line = 0;
    }

    bool isDone() const { return m_isDone; }
    unsigned line() const { return m_line; }
    unsigned bufferStart() const { return m_bufferStart; }

    // tok_get(): as far as the next token, or the next few if they come together. False if it can go no further.
    bool step()
    {
        return !m_strings.isEmpty() && m_strings.last().isScanningText ? scanStringText() : scanToken();
    }

    bool run()
    {
        m_bufferStart = m_lineStart;
        while (!m_isDone) {
            bool ok = step();
            if (!ok) {
                m_position = std::min(m_position, m_end);
                // It is nowhere. What is kept for it is where the tokenizer is, which is where something is said to be wrong if the tokenizer says so, or if that is where it is said to be.
                bool isWhereTokenizerIs = m_error.isFromTokenizer || m_error.endColumn == -2;
                auto [line, column] = isWhereTokenizerIs ? std::pair { m_error.line, static_cast<unsigned>(m_error.column + 1) } : whereTokenizerEnds();
                Token& token = add(TokenKind::Error, m_position);
                token.line = token.endLine = line;
                token.column = token.endColumn = column;
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

    unsigned at(unsigned position)
    {
        if (position < m_end) [[likely]]
            return m_source[position];
        while (readMore()) {
            if (position < m_end)
                return m_source[position];
        }
        return 0;
    }
    unsigned current() { return at(m_position); }
    bool isAtEnd() { return m_position >= m_end && !readMore(); }
    static bool isNewline(unsigned c) { return c == '\n' || c == '\r'; }

    // tok_nextc(), when it has come to the end of what has been read, and tok_underflow_readline(). False if there is no more, which is always so of a source that is all there from the start.
    NEVER_INLINE bool readMore()
    {
        if constexpr (sizeof(CharacterType) == sizeof(char16_t)) {
            if (!m_stream || m_stream->isAtEndOfFile || m_stream->hasFailedToRead || m_error)
                return false;
            // What came before is let go of, unless a token is under way.
            unsigned bufferStartBefore = m_bufferStart;
            bool letsGo = m_stream->isBetweenTokens && m_strings.isEmpty();
            if (letsGo)
                m_bufferStart = m_end;
            auto& buffer = m_stream->buffer;
            unsigned start = buffer.size();
            if (m_stream->lines.readLine(buffer) == LineSource::Result::Failed) {
                m_stream->hasFailedToRead = true;
                return false;
            }
            if (buffer.size() == start) {
                m_stream->isAtEndOfFile = true;
                // CPython's tokenizer is then holding nothing, at the beginning of the room that it has, and still has it that the line begins where the last one did in that room.
                if (letsGo)
                    m_stream->inputEndAtEndOfFile = bufferStartBefore;
                return false;
            }
            m_stream->hasImplicitNewline = buffer.last() != '\n';
            if (m_stream->hasImplicitNewline)
                buffer.append('\n');
            m_source = buffer.span();
            m_end = buffer.size();
            ++m_line;
            m_lineStart = start;
            m_columnCacheOffset = start;
            m_columnCacheValue = 0;
            m_stream->lineStarts.append(start);
            if (std::ranges::find(buffer.span().subspan(start), 0) != buffer.span().end()) {
                fail("source code cannot contain null bytes"_s, m_line, -1, m_line, -1);
                return false;
            }
            return true;
        }
        return false;
    }

    // The line that this is on, or the last one there is if this is the end, after its newline.
    unsigned lastLine()
    {
        if (m_stream)
            return m_line;
        return isAtEnd() && m_position == m_lineStart && m_line > 1 ? m_line - 1 : m_line;
    }

    // Takes a newline, however it is spelled.
    void consumeNewline()
    {
        ASSERT(isNewline(current()));
        if (current() == '\r' && at(m_position + 1) == '\n')
            ++m_position;
        ++m_position;
        // A line is what was read for one.
        if (m_stream)
            return;
        ++m_line;
        m_lineStart = m_position;
        m_columnCacheOffset = m_position;
        m_columnCacheValue = 0;
    }

    // What ends a line in a string. What is read a line at a time is taken as it comes, and there a carriage return is a character like any other.
    bool isNewlineInString(unsigned c) const { return c == '\n' || (c == '\r' && !m_stream); }

    // Takes a character of a string, which may be a newline. That is always '\n'.
    unsigned consumeInString()
    {
        unsigned c = current();
        if (isNewlineInString(c)) {
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
    static constexpr int noColumn = -1;

    // Who says it in CPython. See SyntaxError::isFromTokenizer.
    enum class SaidBy : bool { Parser, Tokenizer };

    bool fail(String&& message, unsigned line, int column, unsigned endLine, int endColumn, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError, SaidBy saidBy = SaidBy::Tokenizer)
    {
        // What went wrong in reading comes to light as the end of the source, and is not that.
        if (m_stream && (m_error || m_stream->hasFailedToRead))
            return false;
        m_error = { kind, false, WTF::move(message), line, column, endLine, endColumn };
        m_error.isFromTokenizer = saidBy == SaidBy::Tokenizer;
        m_error.isInsideFString = !m_strings.isEmpty();
        m_error.tokenizerLine = lastLine();
        m_error.lastLineIsEnded = m_lastLine == ScanRange::LastLine::IsEnded;
        if (m_brackets.size() > (m_hasEnclosingBracket ? 1 : 0)) {
            m_error.openBracket = m_brackets.last().character;
            m_error.openBracketLine = m_brackets.last().line;
            m_error.openBracketColumn = m_brackets.last().column;
        }
        return false;
    }

    // _PyTokenizer_syntaxerror(): it is said to be at the last character that has been taken.
    bool fail(String&& message, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        int column = static_cast<int>(columnOf(std::min(m_position, m_end))) - 1;
        return fail(WTF::move(message), m_line, column, m_line, column, kind);
    }

    // _Pypegen_tokenizer_error(), for what has no more to say about where than the line
    bool failOnLine(String&& message, SyntaxError::Kind kind)
    {
        return fail(WTF::move(message), m_line, 0, m_line, noColumn, kind, SaidBy::Parser);
    }

    // _PyPegen_raise_error(), of the token that says that the tokenizer can go no further, which is nowhere. It is said to be as far along the line as the tokenizer is: `position`.
    bool failWhereTokenizerIs(String&& message, unsigned position, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        // What ends the last line may not be there to be counted.
        unsigned within = std::min(position, m_end);
        return fail(WTF::move(message), m_line, static_cast<int>(columnOf(within) + (position - within)) - 1, m_line, -2, kind, SaidBy::Parser);
    }

    // Where the line that is being scanned ends, before what ends it.
    unsigned endOfLine()
    {
        unsigned position = std::min(m_position, m_end);
        while (position < m_end && !isNewline(m_source[position]))
            ++position;
        return position;
    }

    // `start` and `end` are where in the source it would be if it were an error, and are on the line.
    void warn(String&& message, String&& errorMessage, unsigned line, unsigned start, unsigned end, bool isFromTokenizer = false)
    {
        unsigned lineStart = start;
        while (lineStart && m_source[lineStart - 1] != '\n')
            --lineStart;
        unsigned column = 0;
        for (unsigned i = lineStart; i < start; ++i)
            column += lengthInUTF8(m_source[i]);
        unsigned endColumn = column;
        for (unsigned i = start; i < end; ++i)
            endColumn += lengthInUTF8(m_source[i]);
        m_warnings.append({ WTF::move(message), WTF::move(errorMessage), line, column, line, endColumn, true, isFromTokenizer });
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
        token.isInsideBrackets = !m_brackets.isEmpty();
        m_tokens.append(token);
        m_lineHasTokens = true;
        if (m_stream) [[unlikely]]
            m_stream->tokens.append({ kind, true, start, m_position, m_line, m_stream->firstLine, m_lineStart, m_stream->multiLineStart, m_stream->inputEndAtEndOfFile.value_or(m_end), m_stream->hasImplicitNewline, m_stream->isAtEndOfFile });
        return m_tokens.last();
    }

    bool hasExtraTokens() const { return m_stream && m_stream->hasExtraTokens; }

    // What the token that has just been added is said to be made of, if not of all that was taken for it.
    void setStreamedText(unsigned start, unsigned end)
    {
        if (!m_stream)
            return;
        m_stream->tokens.last().start = start;
        m_stream->tokens.last().end = end;
    }

    void setStreamedTokenIsNowhere()
    {
        if (m_stream)
            m_stream->tokens.last().hasText = false;
    }

    // A string is beginning, or a piece of one.
    void noteStartOfString()
    {
        if (!m_stream)
            return;
        m_stream->firstLine = m_line;
        m_stream->multiLineStart = m_lineStart;
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

    bool stop(SyntaxError::Stop why)
    {
        if (m_error.stop == SyntaxError::Stop::None)
            m_error.stop = why;
        return false;
    }

    bool failForTabs()
    {
        failOnLine("inconsistent use of tabs and spaces in indentation"_s, SyntaxError::Kind::TabError);
        return stop(SyntaxError::Stop::TabSpace);
    }

    bool failForUnclosedBracket()
    {
        Bracket bracket = m_brackets.last();
        fail(concatenate('\'', bracket.character, "' was never closed"_s), bracket.line, bracket.column, bracket.line, noColumn, SyntaxError::Kind::SyntaxError, SaidBy::Parser);
        m_error.isAtEndOfSource = true;
        return stop(SyntaxError::Stop::EndOfFile);
    }

    // The source has ended where it may not.
    bool failAtEnd(unsigned position)
    {
        if (m_brackets.size() > (m_hasEnclosingBracket ? 1 : 0)) {
            // The tokenizer has taken what ends the line.
            if (!isAtEnd())
                consumeNewline();
            return failForUnclosedBracket();
        }
        failWhereTokenizerIs("unexpected EOF while parsing"_s, position);
        m_error.isAtEndOfSource = true;
        return stop(SyntaxError::Stop::EndOfFile);
    }

    // A backslash has been taken. What follows has to be the end of the line.
    bool consumeLineContinuation()
    {
        // The end of the source is the end of a line, so what comes after the backslash is that, and then there is nothing.
        if (isAtEnd() && m_lastLine == ScanRange::LastLine::IsEnded)
            return failAtEnd(m_position + 1);
        if (isAtEnd() || !isNewline(current())) {
            // It is said to be as far as the character is from where CPython's tokenizer has kept the source from, which is not always the beginning of the line. See m_bufferStart. What is
            // beyond the end of the line is brought back to it when a program is told. If there is no character it is the backslash.
            int fromBeginning = isAtEnd() ? -1 : 0;
            for (unsigned i = m_bufferStart; i < m_position; ++i)
                fromBeginning += lengthInUTF8(m_source[i]);
            fail("unexpected character after line continuation character"_s, m_line, fromBeginning, m_line, noColumn, SyntaxError::Kind::SyntaxError, SaidBy::Parser);
            return stop(SyntaxError::Stop::LineContinuation);
        }
        // There being no other line to go on to, the tokenizer is still on this one.
        unsigned afterNewline = m_position + (current() == '\r' && at(m_position + 1) == '\n' ? 2 : 1);
        if (afterNewline >= m_end && !readMore())
            return failAtEnd(m_position + 1);
        consumeNewline();
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

        // What is still open at the end is closed by finish(). But a last line of nothing but white space, that nothing ends, is a line like any other: it is what ends a line that shows it
        // to be blank.
        if (isAtEnd() && (m_lastLine == ScanRange::LastLine::IsEnded || m_position == m_lineStart))
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
            if (m_indentation.size() >= maximumIndentation) {
                failOnLine("too many levels of indentation"_s, SyntaxError::Kind::IndentationError);
                return stop(SyntaxError::Stop::TooDeep);
            }
            if (indentation.alternateColumn <= last.alternateColumn)
                return failForTabs();
            m_indentation.append(indentation);
            add(TokenKind::Indent, m_position);
            // It is the white space, or it is nowhere.
            if (hasExtraTokens())
                setStreamedText(m_bufferStart, m_position);
            else
                setStreamedTokenIsNowhere();
            return true;
        }
        // Whether it comes back to where something began is found out before anything is said to have ended.
        size_t remaining = m_indentation.size();
        while (remaining > 1 && indentation.column < m_indentation[remaining - 1].column)
            --remaining;
        // A function that is scanned by itself ends where something is indented less than it is.
        bool endsWhatIsScanned = remaining == 1 && indentation.column < m_indentation[0].column;
        if (indentation.column != m_indentation[remaining - 1].column && !endsWhatIsScanned)
{
            failWhereTokenizerIs("unindent does not match any outer indentation level"_s, endOfLine() + (endOfLine() < m_end || m_lastLine == ScanRange::LastLine::IsEnded), SyntaxError::Kind::IndentationError);
            return stop(SyntaxError::Stop::Dedent);
        }
        while (m_indentation.size() > remaining) {
            m_indentation.removeLast();
            add(TokenKind::Dedent, m_position);
            if (!hasExtraTokens())
                setStreamedTokenIsNowhere();
        }
        if (endsWhatIsScanned)
            return true;
        if (indentation.alternateColumn != m_indentation.last().alternateColumn)
            return failForTabs();
        return true;
    }

    // The line that the tokenizer is on when it has come to the end of the source, and how far along it: the end of the last line that there was to read, with what ends it.
    std::pair<unsigned, unsigned> whereTokenizerEnds()
    {
        unsigned line = m_end ? m_line : 0;
        unsigned column = columnOf(m_position) + (isAtEnd() && m_lastLine == ScanRange::LastLine::IsEnded);
        if (m_position == m_lineStart && m_lineStart) {
            --line;
            unsigned lineEnd = m_lineStart - 1;
            if (m_source[lineEnd] == '\n' && lineEnd && m_source[lineEnd - 1] == '\r')
                --lineEnd;
            column = 1;
            for (unsigned i = lineEnd; i && !isNewline(m_source[i - 1]); --i)
                column += lengthInUTF8(m_source[i - 1]);
        }
        return { line, column };
    }

    bool finish(unsigned endOfLineStart)
    {
        // It is not the end if there is no telling what comes next.
        if (m_stream && (m_error || m_stream->hasFailedToRead))
            return false;
        if (m_brackets.size() > (m_hasEnclosingBracket ? 1 : 0))
            return failForUnclosedBracket();
        // What ends the last line is added if it is not there, and takes up room.
        bool lastLineIsEnded = m_lastLine == ScanRange::LastLine::IsEnded;
        bool isInLine = m_lineHasTokens && !m_hasEnclosingBracket;
        if (isInLine && lastLineIsEnded)
            ++add(TokenKind::Newline, endOfLineStart).endColumn;
        // These are nowhere. What is kept for them is where the tokenizer is, which is at the end of the last line that there was to read, with what ends it.
        auto [line, column] = whereTokenizerEnds();
        auto addAtEnd = [&] (TokenKind kind) {
            Token& token = add(kind, m_position);
            token.line = token.endLine = line;
            token.column = token.endColumn = column;
            if (kind != TokenKind::Dedent || !hasExtraTokens())
                setStreamedTokenIsNowhere();
        };
        auto addDedents = [&] {
            while (m_indentation.size() > 1) {
                m_indentation.removeLast();
                addAtEnd(TokenKind::Dedent);
            }
        };
        // What has been begun is only found to have ended at the beginning of a line, and the end of the source is one only if there is nothing at all on the last line.
        if (lastLineIsEnded || m_position == m_lineStart || m_endIsBeginningOfLine)
            addDedents();
        // The parser has not begun if all that there has been is what it is not shown.
        bool hasBegun = std::ranges::any_of(m_tokens, [] (const Token& token) { return token.kind != TokenKind::TypeIgnore; });
        if (m_lastLine == ScanRange::LastLine::IsEndedByTheEnd && hasBegun) {
            auto addNewline = [&] {
                addAtEnd(TokenKind::Newline);
                m_tokens.last().isMadeOfTheEnd = true;
            };
            addNewline();
            // The end is taken for the end of a line again whenever some other token has come since it last was.
            if (m_indentation.size() > 1 && m_arena.impliesDedent) {
                addDedents();
                addNewline();
            }
        }
        addAtEnd(TokenKind::EndMarker);
        m_isDone = true;
        return true;
    }

    // ---- Tokens

    static bool isIdentifierStart(unsigned c) { return isASCIIAlpha(c) || c == '_' || c >= 0x80; }
    static bool isIdentifierPart(unsigned c) { return isASCIIAlphanumeric(c) || c == '_' || c >= 0x80; }

    // A comment, which has been gone past, if it says what type something is. Where there is a space in `# type: ` there can be any number of spaces and tabs, or none.
    bool scanTypeComment(unsigned start)
    {
        unsigned position = start;
        // CPython goes on for as long as there is something to look at, which what ends the line is. So `# type:` will not do if the source ends there.
        unsigned limit = m_position + (isAtEnd() && m_lastLine != ScanRange::LastLine::IsEnded ? 0 : 1);
        for (char expected : "# type: "_span) {
            if (position >= limit)
                return false;
            if (expected == ' ') {
                while (at(position) == ' ' || at(position) == '\t')
                    ++position;
            } else if (at(position) == static_cast<unsigned>(expected))
                ++position;
            else
                return false;
        }
        // `ignore`, and then the end or anything that could not go on a word.
        unsigned afterIgnore = position + 6;
        bool isIgnore = m_position >= afterIgnore;
        for (unsigned i = 0; isIgnore && i < 6; ++i)
            isIgnore = at(position + i) == static_cast<unsigned>("ignore"[i]);
        if (isIgnore && m_position > afterIgnore && (at(afterIgnore) >= 128 || isASCIIAlphanumeric(at(afterIgnore))))
            isIgnore = false;
        unsigned textStart = isIgnore ? afterIgnore : position;
        bool lineHadTokens = m_lineHasTokens;
        Token& token = add(isIgnore ? TokenKind::TypeIgnore : TokenKind::TypeComment, textStart);
        auto text = m_source.subspan(textStart, m_position - textStart);
        token.text = makeText(text);
        if (!isIgnore) {
            // A line with nothing else on it has no say in the indentation, as with any comment, but it does end.
            m_isBlankLine = false;
            return true;
        }
        if (!m_isBlankLine)
            return true;
        // That is all that there is on the line, so the line is no line, and the end of it goes with this. CPython works out how long this is once it has gone past that.
        m_lineHasTokens = lineHadTokens;
        if (isAtEnd() && m_lastLine != ScanRange::LastLine::IsEnded) {
            m_endIsBeginningOfLine = true;
            return true;
        }
        Vector<char16_t, 64> withEndOfLine;
        char16_t allCharacters = 0;
        for (auto character : text) {
            withEndOfLine.append(character);
            allCharacters |= character;
        }
        withEndOfLine.append('\n');
        token.text = makeText(withEndOfLine, allCharacters);
        if (!isAtEnd()) {
            consumeNewline();
            m_bufferStart = m_position;
            m_isAtBeginningOfLine = true;
        }
        return true;
    }

    bool scanToken()
    {
        // That a line is blank is forgotten once anything on it has been given.
        if (m_stream && !m_isAtBeginningOfLine)
            m_isBlankLine = false;
        while (true) {
            if (m_stream)
                m_stream->isBetweenTokens = true;
            if (m_isAtBeginningOfLine) {
                m_isAtBeginningOfLine = false;
                m_isBlankLine = false;
                m_lineHasTokens = false;
                size_t tokensBefore = m_tokens.size();
                if (!scanIndentation())
                    return false;
                // No more is looked at than has to be before these are given.
                if (m_stream && m_tokens.size() != tokensBefore)
                    return true;
            }

            unsigned c = current();
            while (c == ' ' || c == '\t' || c == '\f')
                c = at(++m_position);
            if (m_stream)
                m_stream->isBetweenTokens = false;

            // What ends the line begins where the comment does, if there is one.
            unsigned endOfLineStart = m_position;
            if (c == '#') {
                while (!isAtEnd() && !isNewline(current()))
                    ++m_position;
                if (m_arena.hasTypeComments && scanTypeComment(endOfLineStart))
                    return true;
                if (hasExtraTokens()) {
                    bool lineHadTokens = m_lineHasTokens;
                    add(TokenKind::Comment, endOfLineStart);
                    m_lineHasTokens = lineHadTokens;
                    m_stream->isAfterCommentOnBlankLine = m_isBlankLine;
                    return true;
                }
                c = current();
            }

            if (isAtEnd())
                return finish(endOfLineStart);

            // What is read a line at a time is taken as it comes, and there a carriage return by itself ends nothing. It is passed over, and is part of whatever follows, which is not looked at to see if it begins a name.
            bool isAfterCarriageReturn = m_stream && c == '\r' && at(m_position + 1) != '\n';
            if (isAfterCarriageReturn)
                c = at(++m_position);

            if (isNewline(c) && !isAfterCarriageReturn) {
                unsigned start = m_position;
                bool isSignificant = !m_isBlankLine && m_brackets.isEmpty();
                if (m_stream && std::exchange(m_stream->isAfterCommentOnBlankLine, false))
                    isSignificant = false;
                unsigned length = c == '\r' && at(m_position + 1) == '\n' ? 2 : 1;
                if (isSignificant) {
                    // It ends where the line does, and the next line has not begun.
                    m_position += length;
                    add(TokenKind::Newline, endOfLineStart);
                    setStreamedText(endOfLineStart, m_position - 1);
                    m_position = start;
                } else if (hasExtraTokens()) {
                    m_position += length;
                    add(TokenKind::NonLogicalNewline, start);
                    m_position = start;
                }
                consumeNewline();
                // A stream sees to this when it reads.
                if (!m_stream)
                    m_bufferStart = m_position;
                m_isAtBeginningOfLine = true;
                if (isSignificant || hasExtraTokens())
                    return true;
                continue;
            }

            if (isAfterCarriageReturn)
                return scanAfterCarriageReturn(endOfLineStart, c);

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

    // Only in a stream. `start` is where the carriage return is, and `c` is what follows it, which is where this is.
    bool scanAfterCarriageReturn(unsigned start, unsigned c)
    {
        size_t tokensBefore = m_stream->tokens.size();
        bool succeeded;
        if (c == '\\') {
            ++m_position;
            return consumeLineContinuation() && scanToken();
        }
        if (isASCIIDigit(c) || (c == '.' && isASCIIDigit(at(m_position + 1))))
            succeeded = scanNumber();
        else if (c == '"' || c == '\'')
            succeeded = scanString(m_position, false, false, false);
        else
            succeeded = scanOperator();
        if (succeeded && m_stream->tokens.size() > tokensBefore) {
            m_stream->tokens[tokensBefore].start = start;
            m_stream->tokens[tokensBefore].endsInMiddleOfCharacter = c >= 0x80;
        }
        return succeeded;
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
                fail(concatenate('\'', first, "' and '"_s, second, "' prefixes are incompatible"_s), m_line, column, m_line, columnOf(m_position));
                m_error.hasColumnsInBytes = true;
                return false;
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
        if (m_stream && (isASCII || m_stream->hasExtraTokens)) {
            add(TokenKind::Name, start);
            return true;
        }
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
            bool isValid = !before ? c == '_' || Unicode::isXIDStart(c) : Unicode::isXIDContinue(c);
            if (isValid)
                continue;
            // Everything up to it is a name, and it is what is wrong.
            m_position = start + before;
            unsigned column = columnOf(m_position);
            if (Unicode::isPrintable(c))
                return fail(concatenate("invalid character '"_s, StringView(buffer.span().subspan(before, i - before)), "' (U+"_s, hex(static_cast<unsigned>(c), 4), ')'), m_line, column, m_line, column);
            return fail(concatenate("invalid non-printable character U+"_s, hex(static_cast<unsigned>(c), 4)), m_line, column, m_line, column);
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
        return fail(concatenate("invalid "_s, kind, " literal"_s));
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

    bool isFollowedBy(unsigned position, ASCIILiteral rest)
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
        if (hasExtraTokens())
            return true;
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
            warn(concatenate("invalid "_s, kind, " literal"_s), { }, m_line, m_position - 1, m_position - 1, true);
            return true;
        }
        if (c < 0x80 && isIdentifierPart(c))
            return failInNumber(kind);
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
                    return fail(concatenate("invalid digit '"_s, digit, "' in "_s, kind, " literal"_s));
                }
                return failInNumber(kind);
            }
            while (isDigit(current()))
                ++m_position;
        } while (current() == '_');
        if (isASCIIDigit(current())) {
            char digit = current();
            ++m_position;
            return fail(concatenate("invalid digit '"_s, digit, "' in "_s, kind, " literal"_s));
        }
        return verifyEndOfNumber(kind);
    }

    bool addInteger(unsigned start, unsigned digitsStart, uint8_t radix)
    {
        if (m_stream) {
            add(TokenKind::Number, start);
            return true;
        }
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
            return fail(concatenate("Exceeds the limit ("_s, limit, " digits) for integer string conversion: value has "_s, digits.size(), " digits; use sys.set_int_max_str_digits() to increase the limit - Consider hexadecimal for huge integer literals to avoid decimal conversion limits."_s), m_line, noColumn, m_line, noColumn, SyntaxError::Kind::SyntaxError, SaidBy::Parser);
        }
        token.numberKind = NumberKind::BigInteger;
        token.radix = radix;
        token.text = &m_arena.identifiers().makeIdentifier(m_vm, digits.span());
        return true;
    }

    bool addReal(unsigned start, NumberKind kind)
    {
        if (m_stream) {
            add(TokenKind::Number, start);
            return true;
        }
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
            if (hasOtherDigits && c != '.' && c != 'e' && c != 'E' && c != 'j' && c != 'J' && !hasExtraTokens()) {
                unsigned column = columnOf(start);
                fail("leading zeros in decimal integer literals are not permitted; use an 0o prefix for octal integers"_s, m_line, column, m_line, columnOf(zerosEnd));
                m_error.hasColumnsInBytes = true;
                return false;
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
            fail(WTF::move(message), line, column, m_line, columnOf(m_position), SyntaxError::Kind::SyntaxError, SaidBy::Parser);
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
        // Only the first in each is warned of.
        bool hasWarned = false;
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
                    if (!std::exchange(hasWarned, true)) {
                        warn(concatenate("\"\\"_s, digits, "\" is an invalid octal escape sequence. Such sequences will not work in the future. Did you mean \"\\\\"_s, digits, "\"? A raw string is also an option."_s),
                            concatenate("\"\\"_s, digits, "\" is an invalid octal escape sequence. Did you mean \"\\\\"_s, digits, "\"? A raw string is also an option."_s), currentLine, digitsStart - 1, digitsStart + 1);
                    }
                }
                append(isBytes ? value & 0xFF : value);
                break;
            }
            case 'x':
            case 'u':
            case 'U': {
                if (isBytes && c != 'x') {
                    if (!std::exchange(hasWarned, true))
                        warnAboutEscape(c, currentLine, i - 2);
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
                        return failHere(concatenate("(value error) invalid \\x escape at position "_s, escapeStart));
                    ASCIILiteral form = c == 'x' ? "\\xXX"_s : c == 'u' ? "\\uXXXX"_s : "\\UXXXXXXXX"_s;
                    return failHere(concatenate("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + 1 + digits, ": truncated "_s, form, " escape"_s));
                }
                if (value > UCHAR_MAX_VALUE)
                    return failHere(concatenate("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + 9, ": illegal Unicode character"_s));
                appendCodePoint(buffer, allCharacters, value);
                break;
            }
            case 'N': {
                if (isBytes) {
                    if (!std::exchange(hasWarned, true))
                        warnAboutEscape(c, currentLine, i - 2);
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
                    return failHere(concatenate("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + (nameEnd > i ? nameEnd - i + 1 : 1), ": malformed \\N character escape"_s));
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
                    return failHere(concatenate("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, escapeStart, '-', escapeStart + (nameEnd - i) + 2, ": unknown Unicode character name"_s));
                appendCodePoint(buffer, allCharacters, value);
                i = nameEnd + 1;
                break;
            }
            default:
                // Not an escape, so both stay.
                if (!std::exchange(hasWarned, true))
                        warnAboutEscape(c, currentLine, i - 2);
                append('\\');
                append(c);
                break;
            }
        }
        return makeText(buffer, allCharacters);
    }

    // `position` is where the backslash is.
    void warnAboutEscape(unsigned c, unsigned line, unsigned position)
    {
        char16_t character = c;
        StringView view { std::span<const char16_t> { &character, 1 } };
        warn(concatenate("\"\\"_s, view, "\" is an invalid escape sequence. Such sequences will not work in the future. Did you mean \"\\\\"_s, view, "\"? A raw string is also an option."_s),
            concatenate("\"\\"_s, view, "\" is an invalid escape sequence. Did you mean \"\\\\"_s, view, "\"? A raw string is also an option."_s), line, position, position + 2);
    }

    // At the opening quote. `start` is where the prefix began.
    bool scanString(unsigned start, bool isRaw, bool isBytes, bool hasUnicodePrefix)
    {
        unsigned line = m_line;
        unsigned column = columnOf(start);
        noteStartOfString();
        unsigned quote = current();
        unsigned quoteSize = at(m_position + 1) == quote && at(m_position + 2) == quote ? 3 : 1;
        m_position += quoteSize;
        unsigned contentStart = m_position;

        bool hasEscapedQuote = false;
        unsigned endQuoteSize = 0;
        while (endQuoteSize != quoteSize) {
            if (isAtEnd() || (quoteSize == 1 && isNewlineInString(current()))) {
                unsigned detectedAt = lastLine();
                // In f"{x" the second quote was meant to end the whole, and what is missing is the brace.
                if (!m_strings.isEmpty() && m_strings.last().quote == quote && m_strings.last().quoteSize == quoteSize)
                    return fail(concatenate(m_strings.last().prefix(), "-string: expecting '}'"_s), line, column, line, column);
                // A line that nothing ends may be taken to be ended, and then it is the end of the line that has been come to.
                bool isAtEndOfSource = isAtEnd() && (quoteSize == 3 || m_lastLine != ScanRange::LastLine::IsEnded);
                if (quoteSize == 3)
                    fail(concatenate("unterminated triple-quoted string literal (detected at line "_s, detectedAt, ')'), line, column, line, column);
                else if (hasEscapedQuote)
                    fail(concatenate("unterminated string literal (detected at line "_s, detectedAt, "); perhaps you escaped the end quote?"_s), line, column, line, column);
                else
                    fail(concatenate("unterminated string literal (detected at line "_s, detectedAt, ')'), line, column, line, column);
                m_error.isAtEndOfSource = isAtEndOfSource;
                return false;
            }
            unsigned c = consumeInString();
            if (c == quote) {
                ++endQuoteSize;
                continue;
            }
            endQuoteSize = 0;
            if (c == '\\' && !isAtEnd()) {
                unsigned escaped = consumeInString();
                if (escaped == quote)
                    hasEscapedQuote = true;
                // Whatever follows a carriage return goes with it.
                if (escaped == '\r' && !isAtEnd())
                    consumeInString();
            }
        }

        // What it comes to is for the parser to find out.
        const Identifier* value = m_stream ? nullptr : decodeString(contentStart, m_position - quoteSize, isRaw, isBytes, line, column);
        if (!value && !m_stream)
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
        StringState state;
        state.isRaw = isRaw;
        state.isTemplate = isTemplate;
        state.quote = current();
        state.quoteSize = at(m_position + 1) == state.quote && at(m_position + 2) == state.quote ? 3 : 1;
        state.line = m_line;
        state.column = columnOf(start);
        noteStartOfString();
        m_position += state.quoteSize;
        // What is not in any such string counts for one.
        if (m_strings.size() + 2 > maximumStringNesting)
            return fail("too many nested f-strings or t-strings"_s);
        add(isTemplate ? TokenKind::TStringStart : TokenKind::FStringStart, start);
        m_strings.append(state);
        return true;
    }

    // The text is what is between `start` and `end`. Of two braces that stand for one, the second is part of the token and not of the text.
    bool addStringText(StringState& state, unsigned start, unsigned end, unsigned line, unsigned column)
    {
        if (m_stream) {
            add(state.isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle, start, line, column);
            setStreamedText(start, end);
            return true;
        }
        const Identifier* value = decodeString(start, end, state.isRaw, false, line, column);
        Token& token = add(state.isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle, start, line, column);
        token.text = value;
        if (!value) {
            token.hasDecodingError = true;
            String message = std::exchange(m_error, { }).message;
            token.text = message.is8Bit() ? &m_arena.identifiers().makeIdentifier(m_vm, message.span8()) : &m_arena.identifiers().makeIdentifier(m_vm, message.span16());
        }
        return true;
    }

    bool enterExpression(StringState& state)
    {
        if (++state.expressionStartDepth >= maximumExpressionNesting)
            return fail(concatenate(state.prefix(), "-string: expressions nested too deeply"_s));
        state.isScanningText = false;
        return true;
    }

    // The text of an f-string or of a format specification, up to a brace or the closing quotes.
    bool scanStringText()
    {
        StringState& state = m_strings.last();
        // If it is at the end of what has been read, the line that it begins on is the next.
        isAtEnd();
        unsigned start = m_position;
        unsigned line = m_line;
        unsigned column = columnOf(start);
        if (m_stream) {
            m_stream->isBetweenTokens = false;
            m_stream->firstLine = m_line;
        }

        if (current() == '{' && at(m_position + 1) != '{')
            return enterExpression(state) && scanToken();

        bool isAtClosingQuotes = true;
        for (unsigned i = 0; i < state.quoteSize && isAtClosingQuotes; ++i) {
            if (at(m_position + i) != state.quote)
                isAtClosingQuotes = false;
        }
        if (isAtClosingQuotes) {
            m_position += state.quoteSize;
            add(state.isTemplate ? TokenKind::TStringEnd : TokenKind::FStringEnd, start);
            m_strings.removeLast();
            return true;
        }

        if (m_stream)
            m_stream->multiLineStart = m_lineStart;
        bool isInNamedEscape = false;
        unsigned endQuoteSize = 0;
        while (endQuoteSize != state.quoteSize) {
            bool isInFormatSpecification = state.isInFormatSpecification && state.isInExpression();
            if (isAtEnd() || (state.quoteSize == 1 && isNewlineInString(current()))) {
                if (isInFormatSpecification && !isAtEnd())
                    return fail(concatenate(state.prefix(), "-string: newlines are not allowed in format specifiers for single quoted "_s, state.prefix(), "-strings"_s), m_line, columnOf(m_position), m_line, columnOf(m_position));
                unsigned detectedAt = lastLine();
                if (state.quoteSize == 3) {
                    fail(concatenate("unterminated triple-quoted "_s, state.prefix(), "-string literal (detected at line "_s, detectedAt, ')'), state.line, state.column, state.line, state.column);
                    m_error.isAtEndOfSource = true;
                    return false;
                }
                return fail(concatenate("unterminated "_s, state.prefix(), "-string literal (detected at line "_s, detectedAt, ')'), state.line, state.column, state.line, state.column);
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
                if (m_stream && next == '\r')
                    next = at(++m_position);
                // Before a brace it is only a backslash, and the brace is still a brace.
                if (next == '{' || next == '}') {
                    if (!state.isRaw)
                        warnAboutEscape(next, m_line, m_position - 1);
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
                    if ((state->isInDebugExpression || state->isTemplate) && !m_stream)
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
                return fail(concatenate(state->prefix(), "-string: single '}' is not allowed"_s));
            if (m_brackets.size() <= (m_hasEnclosingBracket ? 1u : 0u)) {
                // What closes the bracket that a lambda is in is where the lambda ends.
                if (m_hasEnclosingBracket) {
                    m_position = start;
                    m_end = start;
                    return finish(m_position);
                }
                if (!hasExtraTokens())
                    return fail(concatenate("unmatched '"_s, character, '\''));
            }
            Bracket opening = m_brackets.isEmpty() ? Bracket { } : m_brackets.takeLast();
            if (!hasExtraTokens() && !((opening.character == '(' && c == ')') || (opening.character == '[' && c == ']') || (opening.character == '{' && c == '}'))) {
                if (state && opening.character == '{' && state->braceDepth - 1 == state->expressionStartDepth)
                    return fail(concatenate(state->prefix(), "-string: unmatched '"_s, character, '\''));
                if (opening.line != m_line)
                    return fail(concatenate("closing parenthesis '"_s, character, "' does not match opening parenthesis '"_s, opening.character, "' on line "_s, opening.line));
                return fail(concatenate("closing parenthesis '"_s, character, "' does not match opening parenthesis '"_s, opening.character, '\''));
            }
            if (state) {
                if (--state->braceDepth < 0)
                    return fail(concatenate(state->prefix(), "-string: unmatched '"_s, character, '\''));
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
                return fail(concatenate("invalid non-printable character U+"_s, hex(c, 4)), m_line, column, m_line, column);
            ++m_position;
            add(TokenKind::Invalid, start);
            return true;
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
    ScanRange::LastLine m_lastLine;
    // Where the last line began that was begun with no token under way. CPython's tokenizer lets go of what came before whenever it begins such a line, and so not on going on to another line
    // in a string or after a backslash.
    unsigned m_bufferStart { 0 };
    unsigned m_columnCacheOffset;
    unsigned m_columnCacheValue { 0 };

    bool m_isAtBeginningOfLine { true };
    bool m_isBlankLine { false };
    bool m_lineHasTokens { false };
    bool m_endIsBeginningOfLine { false }; // The last line is nothing but `# type: ignore`, which takes what ends the line with it, though here there is nothing that does.
    bool m_hasEnclosingBracket { false };
    bool m_isDone { false };
    Stream* m_stream { nullptr };

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

WTF_MAKE_TZONE_ALLOCATED_IMPL(TokenStream);

struct TokenStream::Implementation {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(Implementation);

    Implementation(VM& vm, LineSource& lines, bool hasExtraTokens)
        : stream { lines, buffer, streamed, hasExtraTokens }
        , lexer(vm, arena, stream, tokens, warnings, error)
    {
    }

    Arena arena;
    Vector<char16_t> buffer;
    Vector<StreamedToken> streamed;
    size_t next { 0 };
    Vector<Token> tokens;
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    Lexer<char16_t>::Stream stream;
    Lexer<char16_t> lexer;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(TokenStream::Implementation);

TokenStream::TokenStream(VM& vm, LineSource& lines, bool hasExtraTokens)
    : m_implementation(makeUnique<Implementation>(vm, lines, hasExtraTokens))
{
}

TokenStream::~TokenStream() = default;

auto TokenStream::next(StreamedToken& token) -> Result
{
    auto& self = *m_implementation;
    while (self.next >= self.streamed.size()) {
        self.streamed.shrink(0);
        self.tokens.shrink(0);
        self.next = 0;
        ASSERT(!self.lexer.isDone());
        if (!self.lexer.step() || self.stream.hasFailedToRead || self.error)
            return self.stream.hasFailedToRead ? Result::Failed : Result::Error;
    }
    token = self.streamed[self.next++];
    return Result::Token;
}

std::span<const char16_t> TokenStream::source() const { return m_implementation->buffer.span(); }
const SyntaxError& TokenStream::error() const { return m_implementation->error; }
Vector<SyntaxWarning> TokenStream::takeWarnings() { return std::exchange(m_implementation->warnings, { }); }
unsigned TokenStream::line() const { return m_implementation->lexer.line(); }
unsigned TokenStream::bufferStart() const { return m_implementation->lexer.bufferStart(); }

unsigned TokenStream::startOfLine(unsigned line) const
{
    auto& starts = m_implementation->stream.lineStarts;
    return line && line <= starts.size() ? starts[line - 1] : 0;
}

} } // namespace JSC::Python

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
