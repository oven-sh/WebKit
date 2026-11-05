/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyArena.h"
#include "PyParserHost.h"
#include "PyToken.h"
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC {

class IdentifierArena;
class VM;

namespace Python {

// Python source as the lexer reads it: UTF-8, every line ending a "\n", a NUL after the last byte.
class SourceText {
public:
    enum class Flag : uint8_t {
        EndWithNewline = 1 << 0, // Files: a last line without a line ending gets one.
        IgnoreCookie = 1 << 1, // The bytes are UTF-8 whatever a coding cookie says (the source was a str).
    };

    // In bytes, as it comes and in UTF-8. The lexer and the parser make strings of parts of the source by ways that are for text that is
    // not long: through Vectors, which hold 2**30 - 1 units of 16 bits at most, and end the process beyond.
    static constexpr size_t maxLength = (1u << 30) - 1;

    // False: `error` says why. (A MemoryError: it is longer than that.)
    bool initialize(std::span<const uint8_t>, OptionSet<Flag>, const String& filename, ParserHost&, SyntaxErrorInfo& error);

    std::span<const uint8_t> bytes() const { return m_bytes.span().first(m_bytes.size() - 1); }
    const ASCIICString& encoding() const { return m_encoding; } // Null unless a BOM or a cookie named one.

private:
    Vector<uint8_t> m_bytes;
    ASCIICString m_encoding;
};

// The encoding a BOM or a coding cookie in the first two lines declares, normalized as SourceText::encoding() is. Null: none.
ASCIICString declaredEncoding(std::span<const uint8_t> source);

class Lexer {
    WTF_MAKE_NONCOPYABLE(Lexer);
public:
    enum class Flag : uint8_t {
        TypeComments = 1 << 0,
        BarryAsBDFL = 1 << 1, // from __future__ import barry_as_FLUFL: <> is an operator
        ExtraTokens = 1 << 2, // For the tokenize module: comments and non-logical newlines are tokens, and less is rejected.
        WithoutIdentifiers = 1 << 3, // Only the text of a name is wanted.
    };

    // Text that comes a line at a time, as the lexer asks for it.
    class LineSource {
    public:
        virtual ~LineSource() = default;
        enum class Result : uint8_t {
            Line,
            EndOfText,
            Failed, // The host has a pending exception.
        };
        // Adds a line, with its "\n", to the text, all of which is then `source`.
        virtual Result readLine(std::span<const uint8_t>& source) = 0;
    };

    // How the text ended, or why lexing stopped. Besides Error, it is the parser that words the message.
    enum class Status : uint8_t {
        Ok,
        EndOfFile,
        Error, // error() is set.
        Dedent, // A dedent to no enclosing level.
        TabSpace, // Tabs and spaces that do not compare the same for every tab size.
        TooDeep,
        LineContinuation, // Something follows a backslash.
        EndOfFileInTripleQuotedString, // error() is set too.
        EndOfLineInString, // error() is set too.
    };

    Lexer(VM&, IdentifierArena&, ParserHost&, std::span<const uint8_t> source, OptionSet<Flag>);

    void lex(Token&);

    Status status() const { return m_status; }
    const SyntaxErrorInfo& error() const { return m_error; }
    void setBarryAsBDFL() { m_flags.add(Flag::BarryAsBDFL); }
    // What follows the source given at first.
    void setLineSource(LineSource* lineSource) { m_lineSource = lineSource; }
    // The text is over: whatever is indented ends here.
    void implyDedents()
    {
        m_pendingIndents = -static_cast<int>(m_indents.size() - 1);
        m_indents.shrink(1);
        m_alternateIndents.shrink(1);
    }

    std::span<const uint8_t> source() const { return m_source; }
    // By line number, from 1: where the line starts. Known for the lines read so far.
    uint32_t lineStartOffset(int line) const { return m_lineStarts[line - 1]; }
    unsigned linesRead() const { return m_lineStarts.size(); }

    // Where the lexer is: what an error that names no token points at.
    int currentLine() const { return m_line; }
    uint32_t currentOffset() const { return m_cursor; }
    uint32_t currentLineStart() const { return m_lineStart; }
    uint32_t currentLineEnd() const { return m_lineEnd; }
    // For the tokenize module, whose errors show it: the first of the lines CPython's tokenizer would have in its buffer, which
    // it empties when it reads a line with no token begun, outside of f-strings. One more than currentLine(): none.
    int firstBufferedLine() const { return m_firstBufferedLine; }

    struct OpenBracket {
        uint8_t character;
        int line;
        int column;
    };
    const Vector<OpenBracket, 16>& openBrackets() const { return m_brackets; }
    bool isInsideFString() const { return !m_strings.isEmpty(); }

    // A line of the source without its line ending. `line` may not have been read yet.
    std::span<const uint8_t> lineText(int line) const;

    static constexpr unsigned maxIndent = 100;
    static constexpr unsigned maxBracketDepth = 200;
    static constexpr unsigned maxStringDepth = 150;
    static constexpr int maxFieldNesting = 3;

private:
    static constexpr int endOfFile = -1;

    // An f-string or t-string being read. Between its replacement fields the text is string; inside them, code.
    struct InterpolatedString {
        bool inCode { false };
        bool isTemplate;
        bool isRaw;
        bool inFormatSpec { false };
        bool inDebug { false }; // An '=' was seen at the top of the field.
        uint8_t quote;
        uint8_t quoteSize;
        int bracketDepth { 0 }; // Brackets open inside this string, its fields' braces included.
        int fieldDepth { -1 }; // Fields open, less one: a format spec may hold fields of its own.
        uint32_t start;
        uint32_t startLineStart;
        int startLine;
        uint32_t expressionStart { 0 };

        bool inField() const { return fieldDepth >= 0; }
        bool atTopOfField() const { return bracketDepth - fieldDepth == 1; }
        char prefix() const { return isTemplate ? 't' : 'f'; }
    };

    int next();
    void back(int character);
    bool readLine();
    int peek()
    {
        int c = next();
        back(c);
        return c;
    }

    void lexCode(Token&);
    void lexStringPart(Token&, InterpolatedString&);
    bool lexIndentation(Token&, bool& blankLine);
    bool lexNumber(int first);
    bool lexFraction(int c);
    bool lexExponent(int c);
    bool lexImaginary();
    int decimalTail();
    bool verifyEndOfNumber(int c, ASCIILiteral kind);
    bool lookingAt(const char* rest);
    bool verifyIdentifier();
    bool continuationLine();
    bool lexString(Token&, int quote);
    void startInterpolatedString(Token&, int quote);
    bool checkStringPrefixes(bool b, bool r, bool u, bool f, bool t);
    const Identifier* makeIdentifier(uint32_t start, uint32_t end, bool isASCII);
    TokenKind keywordOrName(std::span<const uint8_t>);

    void finish(Token&, TokenKind);
    void finish(Token&, TokenKind, uint32_t start, uint32_t end);
    void finishWithoutText(Token&, TokenKind);
    void finishError(Token& token) { finishWithoutText(token, TokenKind::ErrorToken); }

    // An error where the cursor is.
    void fail(const String& message);
    // Between two columns of the current line, in bytes from 1.
    void fail(int column, int endColumn, const String& message);
    bool warn(ParserHost::WarningCategory, const String& message, const String& messageAsError = { });
    void stop(Status);

    VM& m_vm;
    IdentifierArena& m_identifiers;
    ParserHost& m_host;
    std::span<const uint8_t> m_source;
    LineSource* m_lineSource { nullptr };
    OptionSet<Flag> m_flags;

    // The text is read a line at a time: the line number moves when the first character of a line is asked for.
    uint32_t m_cursor { 0 };
    uint32_t m_lineStart { 0 };
    uint32_t m_lineEnd { 0 }; // Past the "\n".
    int m_line { 0 };
    Vector<uint32_t> m_lineStarts;

    uint32_t m_tokenStart { 0 };
    int m_tokenStartColumn { 0 };
    int m_tokenStartLine { 0 }; // Of a string, which may span lines.
    uint32_t m_tokenStartLineStart { 0 };
    bool m_tokenHasBegun { false };
    int m_firstBufferedLine { 1 };

    bool m_atLineStart { true };
    bool m_commentOnOwnLine { false };
    int m_pendingIndents { 0 }; // Negative: dedents.
    Vector<int, 16> m_indents { 0 };
    Vector<int, 16> m_alternateIndents { 0 }; // With tabs one column wide.

    Vector<OpenBracket, 16> m_brackets;
    Vector<InterpolatedString, 4> m_strings;

    Status m_status { Status::Ok };
    SyntaxErrorInfo m_error;
};

// The number of characters in the first `byteCount` bytes of a line; what is not UTF-8 counts as U+FFFD would.
int characterCount(std::span<const uint8_t> line, size_t byteCount);

// UTF-8 that is known to be valid.
String stringFromUTF8(std::span<const uint8_t>);

} // namespace Python
} // namespace JSC
