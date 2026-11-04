/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyLexer.h"

#include "Identifier.h"
#include "ParserArena.h"
#include "PyUnicode.h"
#include "VM.h"
#include <wtf/ASCIICType.h>
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// ---- Text ----

// The length of the UTF-8 sequence at the start of `bytes`, or 0 and the number of bytes that are in error.
static unsigned utf8SequenceLength(std::span<const uint8_t> bytes, unsigned& errorLength)
{
    auto isContinuation = [](uint8_t c) { return (c & 0xC0) == 0x80; };
    auto bad = [&](size_t length) {
        errorLength = static_cast<unsigned>(length);
        return 0u;
    };

    uint8_t c = bytes[0];
    size_t available = bytes.size();
    if (c < 0x80)
        return 1;
    if (c < 0xC2)
        return bad(1);
    if (c < 0xE0) {
        if (available < 2)
            return bad(available);
        return isContinuation(bytes[1]) ? 2 : bad(1);
    }
    if (c < 0xF0) {
        if (available < 2)
            return bad(available);
        // No overlong forms, no surrogates.
        if (!isContinuation(bytes[1]) || (bytes[1] < 0xA0 ? c == 0xE0 : c == 0xED))
            return bad(1);
        if (available < 3)
            return bad(available);
        return isContinuation(bytes[2]) ? 3 : bad(2);
    }
    if (c < 0xF5) {
        if (available < 2)
            return bad(available);
        if (!isContinuation(bytes[1]) || (bytes[1] < 0x90 ? c == 0xF0 : c == 0xF4))
            return bad(1);
        if (available < 3)
            return bad(available);
        if (!isContinuation(bytes[2]))
            return bad(2);
        if (available < 4)
            return bad(available);
        return isContinuation(bytes[3]) ? 4 : bad(3);
    }
    return bad(1);
}

int characterCount(std::span<const uint8_t> line, size_t byteCount)
{
    line = line.first(std::min(byteCount, line.size()));
    int count = 0;
    while (!line.empty()) {
        unsigned errorLength = 0;
        unsigned length = utf8SequenceLength(line, errorLength);
        line = line.subspan(length ? length : errorLength);
        ++count;
    }
    return count;
}

String stringFromUTF8(std::span<const uint8_t> bytes)
{
    return String::fromUTF8ReplacingInvalidSequences(byteCast<char8_t>(bytes));
}

UTF8CString ParserHost::decodeSource(std::span<const uint8_t> bytes, const ASCIICString& encoding, String& error, bool& unknownEncoding)
{
    // Without the codecs of a runtime, the other names of the encodings that need none.
    auto name = makeStringByReplacingAll(String(byteCast<Latin1Character>(encoding.span())).convertToASCIILowercase(), '_', '-');
    if (name == "latin1"_s || name == "latin"_s || name == "l1"_s || name == "iso8859-1"_s || name == "8859"_s || name == "cp819"_s)
        return String(byteCast<Latin1Character>(bytes)).utf8();
    if (name == "ascii"_s || name == "us-ascii"_s || name == "646"_s) {
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (bytes[i] >= 0x80) {
                error = makeString("'ascii' codec can't decode byte 0x"_s, hex(bytes[i], 2, Lowercase), " in position "_s, i, ": ordinal not in range(128)"_s);
                return { };
            }
        }
        return UTF8CString(byteCast<char8_t>(bytes));
    }
    if (name == "utf8"_s || name == "u8"_s || name == "utf"_s || name == "cp65001"_s)
        return UTF8CString(byteCast<char8_t>(bytes));
    unknownEncoding = true;
    error = makeString("unknown encoding: "_s, encoding.span());
    return { };
}

// Every "\r\n" and "\r" becomes "\n".
static void appendTranslatingNewlines(Vector<uint8_t>& out, std::span<const uint8_t> bytes)
{
    out.reserveCapacity(out.size() + bytes.size() + 2);
    while (!bytes.empty()) {
        auto* carriageReturn = static_cast<const uint8_t*>(memchr(bytes.data(), '\r', bytes.size()));
        if (!carriageReturn) {
            out.append(bytes);
            return;
        }
        size_t length = carriageReturn - bytes.data();
        out.append(bytes.first(length));
        out.append('\n');
        bytes = bytes.subspan(length + 1);
        if (!bytes.empty() && bytes[0] == '\n')
            bytes = bytes.subspan(1);
    }
}

// PEP 263: the encoding a comment on this line declares. Sets `onlyComment` if a cookie may still follow on the next line.
static ASCIICString codingCookie(std::span<const uint8_t> line, bool& lineIsBlankOrComment)
{
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\014'))
        ++i;
    lineIsBlankOrComment = i == line.size() || line[i] == '#';
    if (i == line.size() || line[i] != '#')
        return { };

    for (; i + 6 < line.size(); ++i) {
        if (memcmp(line.data() + i, "coding", 6))
            continue;
        size_t t = i + 6;
        if (line[t] != ':' && line[t] != '=')
            continue;
        do {
            ++t;
        } while (t < line.size() && (line[t] == ' ' || line[t] == '\t'));
        size_t begin = t;
        while (t < line.size() && (isASCIIAlphanumeric(line[t]) || line[t] == '-' || line[t] == '_' || line[t] == '.'))
            ++t;
        if (begin == t)
            continue;

        // The names Python itself knows UTF-8 and Latin-1 by.
        auto name = line.subspan(begin, t - begin);
        char normalized[13];
        size_t length = std::min<size_t>(name.size(), 12);
        for (size_t k = 0; k < length; ++k)
            normalized[k] = name[k] == '_' ? '-' : toASCIILower(static_cast<char>(name[k]));
        normalized[length] = 0;
        auto startsWith = [&](const char* prefix) { return !strncmp(normalized, prefix, strlen(prefix)); };
        if (!strcmp(normalized, "utf-8") || startsWith("utf-8-"))
            return "utf-8"_s;
        if (!strcmp(normalized, "latin-1") || !strcmp(normalized, "iso-8859-1") || !strcmp(normalized, "iso-latin-1")
            || startsWith("latin-1-") || startsWith("iso-8859-1-") || startsWith("iso-latin-1-"))
            return "iso-8859-1"_s;
        return ASCIICString(byteCast<char>(name));
    }
    return { };
}

ASCIICString declaredEncoding(std::span<const uint8_t> source)
{
    if (source.size() >= 3 && source[0] == 0xEF && source[1] == 0xBB && source[2] == 0xBF)
        return "utf-8"_s;
    for (int lineno = 1; lineno <= 2 && !source.empty(); ++lineno) {
        size_t length = 0;
        while (length < source.size() && source[length] != '\n' && source[length] != '\r')
            ++length;
        bool lineIsBlankOrComment;
        ASCIICString cookie = codingCookie(source.first(length), lineIsBlankOrComment);
        if (!cookie.isNull() || !lineIsBlankOrComment || length == source.size())
            return cookie;
        source = source.subspan(length + (source[length] == '\r' && length + 1 < source.size() && source[length + 1] == '\n' ? 2 : 1));
    }
    return { };
}

bool SourceText::initialize(std::span<const uint8_t> input, OptionSet<Flag> flags, const String& filename, ParserHost& host, SyntaxErrorInfo& error)
{
    auto failAt = [&](int lineno, int offset, int endOffset, std::span<const uint8_t> line, String&& message) {
        error.type = SyntaxErrorInfo::Type::SyntaxError;
        error.message = WTF::move(message);
        error.hasLocation = true;
        error.lineno = lineno;
        error.offset = offset;
        error.text = stringFromUTF8(line);
        error.hasEnd = true;
        error.endLineno = lineno;
        error.endOffset = endOffset;
        return false;
    };

    if (auto* null = static_cast<const uint8_t*>(memchr(input.data(), 0, input.size()))) {
        if (!filename.isNull()) {
            // A file is read line by line, so it is known where: what is shown of the line ends there.
            auto before = input.first(null - input.data());
            int lineno = 1;
            size_t lineStart = 0;
            for (size_t i = 0; i < before.size(); ++i) {
                if (before[i] == '\n' || (before[i] == '\r' && (i + 1 == before.size() || before[i + 1] != '\n'))) {
                    ++lineno;
                    lineStart = i + 1;
                }
            }
            return failAt(lineno, 0, 0, before.subspan(lineStart), "source code cannot contain null bytes"_s);
        }
        error.type = SyntaxErrorInfo::Type::SyntaxError;
        error.message = "source code cannot contain null bytes"_s;
        return false;
    }

    m_bytes.clear();
    appendTranslatingNewlines(m_bytes, input);
    // Before anything is decoded, as in CPython: in UTF-16 that newline is half a character.
    if (flags.contains(Flag::EndWithNewline) && !m_bytes.isEmpty() && m_bytes.last() != '\n')
        m_bytes.append('\n');

    if (!flags.contains(Flag::IgnoreCookie)) {
        bool hasBOM = m_bytes.size() >= 3 && m_bytes[0] == 0xEF && m_bytes[1] == 0xBB && m_bytes[2] == 0xBF;
        if (hasBOM) {
            m_bytes.removeAt(0, 3);
            m_encoding = "utf-8"_s;
        }

        // A cookie is on the first line, or on the second if the first is blank or a comment. A line counts once it has ended.
        ASCIICString cookie;
        int cookieLine = 0;
        std::span<const uint8_t> cookieLineText;
        std::span<const uint8_t> rest = m_bytes.span();
        for (int lineno = 1; lineno <= 2; ++lineno) {
            auto* newline = static_cast<const uint8_t*>(memchr(rest.data(), '\n', rest.size()));
            if (!newline)
                break;
            auto line = rest.first(newline - rest.data());
            bool lineIsBlankOrComment;
            cookie = codingCookie(line, lineIsBlankOrComment);
            if (!cookie.isNull()) {
                cookieLine = lineno;
                cookieLineText = line;
                break;
            }
            if (!lineIsBlankOrComment)
                break;
            rest = rest.subspan(line.size() + 1);
        }

        if (!cookie.isNull()) {
            if (hasBOM) {
                if (cookie != "utf-8"_s) {
                    // CPython's end column: the length of the line for the first line, one more for the second.
                    int endColumn = cookieLine;
                    for (size_t i = 0; i + 1 < cookieLineText.size(); ++i)
                        endColumn += (cookieLineText[i] & 0xC0) != 0x80;
                    return failAt(cookieLine, 0, endColumn, cookieLineText, makeString("encoding problem: "_s, cookie.span(), " with BOM"_s));
                }
            } else {
                m_encoding = cookie;
                if (cookie == "iso-8859-1"_s) {
                    Vector<uint8_t> utf8;
                    utf8.reserveInitialCapacity(m_bytes.size() + m_bytes.size() / 8);
                    for (uint8_t c : m_bytes) {
                        if (c < 0x80)
                            utf8.append(c);
                        else {
                            utf8.append(static_cast<uint8_t>(0xC0 | (c >> 6)));
                            utf8.append(static_cast<uint8_t>(0x80 | (c & 0x3F)));
                        }
                    }
                    m_bytes = WTF::move(utf8);
                } else if (cookie != "utf-8"_s) {
                    String message;
                    bool unknownEncoding = false;
                    UTF8CString decoded = host.decodeSource(m_bytes.span(), cookie, message, unknownEncoding);
                    if (decoded.isNull()) {
                        // Of a file, CPython has what follows the declaration decoded by a reader that starts at the end of that line:
                        // that is where the position in the message counts from.
                        if (!filename.isNull() && !unknownEncoding) {
                            String messageOfReader;
                            if (host.decodeSource(m_bytes.span().subspan(cookieLineText.data() + cookieLineText.size() - m_bytes.span().data()), cookie, messageOfReader, unknownEncoding).isNull())
                                message = WTF::move(messageOfReader);
                        }
                        // SyntaxError(message, (filename, 0, -1, None))
                        error.type = SyntaxErrorInfo::Type::SyntaxError;
                        error.message = WTF::move(message);
                        error.hasLocation = true;
                        error.lineno = 0;
                        error.offset = -1;
                        return false;
                    }
                    m_bytes.clear();
                    appendTranslatingNewlines(m_bytes, byteCast<uint8_t>(decoded.span()));
                }
            }
        }
    }

    // Everything after this trusts the text to be UTF-8.
    {
        std::span<const uint8_t> rest = m_bytes.span();
        int lineno = 1;
        size_t lineStart = 0;
        size_t offset = 0;
        while (offset < rest.size()) {
            uint8_t c = rest[offset];
            if (c < 0x80) [[likely]] {
                ++offset;
                if (c == '\n') {
                    ++lineno;
                    lineStart = offset;
                }
                continue;
            }
            unsigned errorLength = 0;
            unsigned length = utf8SequenceLength(rest.subspan(offset), errorLength);
            if (!length) {
                auto* newline = static_cast<const uint8_t*>(memchr(rest.data() + lineStart, '\n', rest.size() - lineStart));
                auto line = rest.subspan(lineStart, newline ? newline - (rest.data() + lineStart) : rest.size() - lineStart);
                int column = 1;
                for (size_t i = lineStart; i < offset; ++i)
                    column += (rest[i] & 0xC0) != 0x80;
                return failAt(lineno, column, column, line, makeString("Non-UTF-8 code starting with '\\x"_s, hex(c, 2, Lowercase), '\'', filename.isNull() ? ""_s : " in file "_s, filename, " on line "_s, lineno, ", but no encoding declared; see https://peps.python.org/pep-0263/ for details"_s));
            }
            offset += length;
        }
    }

    if (flags.contains(Flag::EndWithNewline) && !m_bytes.isEmpty() && m_bytes.last() != '\n')
        m_bytes.append('\n');
    m_bytes.append(0);
    return true;
}

// ---- Lexer ----

Lexer::Lexer(VM& vm, IdentifierArena& identifiers, ParserHost& host, std::span<const uint8_t> source, OptionSet<Flag> flags)
    : m_vm(vm)
    , m_identifiers(identifiers)
    , m_host(host)
    , m_source(source)
    , m_flags(flags)
{
}

static inline bool isIdentifierStart(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 128;
}

static inline bool isIdentifierPart(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 128;
}

static inline bool isDigit(int c) { return c >= '0' && c <= '9'; }
static inline bool isHexDigit(int c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool Lexer::readLine()
{
    ASSERT(m_cursor == m_lineEnd);
    if (!m_tokenHasBegun && m_strings.isEmpty())
        m_firstBufferedLine = m_line + 1;
    if (m_lineEnd == m_source.size()) {
        auto result = m_lineSource ? m_lineSource->readLine(m_source) : LineSource::Result::EndOfText;
        if (result == LineSource::Result::Failed) {
            m_error = { };
            m_error.type = SyntaxErrorInfo::Type::HostException;
            m_status = Status::Error;
            return false;
        }
        if (result == LineSource::Result::EndOfText) {
            m_status = Status::EndOfFile;
            return false;
        }
        ASSERT(m_lineEnd < m_source.size());
    }
    auto* newline = static_cast<const uint8_t*>(memchr(m_source.data() + m_lineEnd, '\n', m_source.size() - m_lineEnd));
    m_lineStart = m_cursor;
    m_lineEnd = newline ? static_cast<uint32_t>(newline - m_source.data()) + 1 : static_cast<uint32_t>(m_source.size());
    ++m_line;
    m_lineStarts.append(m_lineStart);
    return true;
}

ALWAYS_INLINE int Lexer::next()
{
    if (m_cursor != m_lineEnd) [[likely]]
        return m_source[m_cursor++];
    if (m_status != Status::Ok || !readLine())
        return endOfFile;
    return m_source[m_cursor++];
}

ALWAYS_INLINE void Lexer::back(int character)
{
    if (character != endOfFile) {
        ASSERT(m_cursor && m_source[m_cursor - 1] == character);
        --m_cursor;
    }
}

std::span<const uint8_t> Lexer::lineText(int line) const
{
    uint32_t start;
    if (line >= 1 && static_cast<unsigned>(line) <= m_lineStarts.size())
        start = m_lineStarts[line - 1];
    else {
        // Not read yet: count on from the last line that was.
        start = m_lineEnd;
        for (int current = m_line + 1; current < line && start < m_source.size(); ++current) {
            auto* newline = static_cast<const uint8_t*>(memchr(m_source.data() + start, '\n', m_source.size() - start));
            start = newline ? static_cast<uint32_t>(newline - m_source.data()) + 1 : static_cast<uint32_t>(m_source.size());
        }
    }
    auto rest = m_source.subspan(start);
    auto* newline = static_cast<const uint8_t*>(memchr(rest.data(), '\n', rest.size()));
    return newline ? rest.first(newline - rest.data()) : rest;
}

void Lexer::stop(Status status)
{
    m_status = status;
    m_cursor = m_lineEnd;
}

void Lexer::fail(int column, int endColumn, const String& message)
{
    // The first error stands.
    if (m_status == Status::Error)
        return;

    auto rest = m_source.subspan(m_lineStart);
    auto columnInCharacters = [&](int byteColumn) {
        int result = 1;
        for (int i = 0; i < byteColumn - 1; ++i)
            result += (rest[i] & 0xC0) != 0x80;
        return result;
    };

    if (column == -1)
        column = characterCount(rest, m_cursor - m_lineStart);
    else if (column > 0)
        column = columnInCharacters(column);
    if (endColumn == -1)
        endColumn = column;
    else if (endColumn > 0)
        endColumn = columnInCharacters(endColumn);

    auto* newline = static_cast<const uint8_t*>(memchr(rest.data(), '\n', rest.size()));
    m_error = { };
    m_error.type = SyntaxErrorInfo::Type::SyntaxError;
    m_error.message = message;
    m_error.hasLocation = true;
    m_error.lineno = m_line;
    m_error.offset = column;
    m_error.text = stringFromUTF8(newline ? rest.first(newline - rest.data()) : rest);
    m_error.hasEnd = true;
    m_error.endLineno = m_line;
    m_error.endOffset = endColumn;
    m_status = Status::Error;
}

void Lexer::fail(const String& message)
{
    fail(-1, -1, message);
}

// False: lexing stops, with an error.
bool Lexer::warn(ParserHost::WarningCategory category, const String& message, const String& messageAsError)
{
    if (m_flags.contains(Flag::ExtraTokens))
        return true;
    switch (m_host.warn({ category, message, m_line })) {
    case ParserHost::WarningResult::Reported:
        return true;
    case ParserHost::WarningResult::BecameException:
        fail(messageAsError.isNull() ? message : messageAsError);
        return false;
    case ParserHost::WarningResult::OtherException:
        m_error = { };
        m_error.type = SyntaxErrorInfo::Type::HostException;
        m_status = Status::Error;
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Lexer::finish(Token& token, TokenKind kind, uint32_t start, uint32_t end)
{
    bool spansLines = kind == TokenKind::String || kind == TokenKind::FStringMiddle || kind == TokenKind::TStringMiddle;
    token.kind = kind;
    token.hasColumns = true;
    token.level = static_cast<uint8_t>(m_brackets.size());
    token.start = start;
    token.end = end;
    token.startPosition = { spansLines ? m_tokenStartLine : m_line, m_tokenStartColumn };
    token.endPosition = { m_line, static_cast<int>(m_cursor - m_lineStart) };
}

void Lexer::finish(Token& token, TokenKind kind)
{
    finish(token, kind, m_tokenStart, m_cursor);
}

void Lexer::finishWithoutText(Token& token, TokenKind kind)
{
    token.kind = kind;
    token.hasColumns = false;
    token.level = static_cast<uint8_t>(m_brackets.size());
    token.start = token.end = m_cursor;
    token.startPosition = { m_line, -1 };
    token.endPosition = { m_line, -1 };
}

void Lexer::lex(Token& token)
{
    token.identifier = nullptr;
    if (m_strings.isEmpty() || m_strings.last().inCode)
        lexCode(token);
    else
        lexStringPart(token, m_strings.last());
}

TokenKind Lexer::keywordOrName(std::span<const uint8_t> text)
{
    auto is = [&](const char* keyword) { return !memcmp(text.data(), keyword, text.size()); };
    switch (text.size()) {
    case 2:
        switch (text[0]) {
        case 'i':
            if (text[1] == 'f')
                return TokenKind::KwIf;
            if (text[1] == 'n')
                return TokenKind::KwIn;
            if (text[1] == 's')
                return TokenKind::KwIs;
            break;
        case 'a':
            if (text[1] == 's')
                return TokenKind::KwAs;
            break;
        case 'o':
            if (text[1] == 'r')
                return TokenKind::KwOr;
            break;
        }
        break;
    case 3:
        switch (text[0]) {
        case 'a':
            if (is("and"))
                return TokenKind::KwAnd;
            break;
        case 'd':
            if (is("def"))
                return TokenKind::KwDef;
            if (is("del"))
                return TokenKind::KwDel;
            break;
        case 'f':
            if (is("for"))
                return TokenKind::KwFor;
            break;
        case 'n':
            if (is("not"))
                return TokenKind::KwNot;
            break;
        case 't':
            if (is("try"))
                return TokenKind::KwTry;
            break;
        }
        break;
    case 4:
        switch (text[0]) {
        case 'N':
            if (is("None"))
                return TokenKind::KwNone;
            break;
        case 'T':
            if (is("True"))
                return TokenKind::KwTrue;
            break;
        case 'e':
            if (is("else"))
                return TokenKind::KwElse;
            if (is("elif"))
                return TokenKind::KwElif;
            break;
        case 'f':
            if (is("from"))
                return TokenKind::KwFrom;
            break;
        case 'p':
            if (is("pass"))
                return TokenKind::KwPass;
            break;
        case 'w':
            if (is("with"))
                return TokenKind::KwWith;
            break;
        }
        break;
    case 5:
        switch (text[0]) {
        case 'F':
            if (is("False"))
                return TokenKind::KwFalse;
            break;
        case 'a':
            if (is("async"))
                return TokenKind::KwAsync;
            if (is("await"))
                return TokenKind::KwAwait;
            break;
        case 'b':
            if (is("break"))
                return TokenKind::KwBreak;
            break;
        case 'c':
            if (is("class"))
                return TokenKind::KwClass;
            break;
        case 'r':
            if (is("raise"))
                return TokenKind::KwRaise;
            break;
        case 'w':
            if (is("while"))
                return TokenKind::KwWhile;
            break;
        case 'y':
            if (is("yield"))
                return TokenKind::KwYield;
            break;
        }
        break;
    case 6:
        switch (text[0]) {
        case 'a':
            if (is("assert"))
                return TokenKind::KwAssert;
            break;
        case 'e':
            if (is("except"))
                return TokenKind::KwExcept;
            break;
        case 'g':
            if (is("global"))
                return TokenKind::KwGlobal;
            break;
        case 'i':
            if (is("import"))
                return TokenKind::KwImport;
            break;
        case 'l':
            if (is("lambda"))
                return TokenKind::KwLambda;
            break;
        case 'r':
            if (is("return"))
                return TokenKind::KwReturn;
            break;
        }
        break;
    case 7:
        if (is("finally"))
            return TokenKind::KwFinally;
        break;
    case 8:
        if (is("continue"))
            return TokenKind::KwContinue;
        if (is("nonlocal"))
            return TokenKind::KwNonlocal;
        break;
    }
    return TokenKind::Name;
}

const Identifier* Lexer::makeIdentifier(uint32_t start, uint32_t end, bool isASCII)
{
    auto text = m_source.subspan(start, end - start);
    if (isASCII) [[likely]]
        return &m_identifiers.makeIdentifier(m_vm, byteCast<Latin1Character>(text));
    // PEP 3131: identifiers are compared in NFKC.
    String normalized = normalizeNFKC(stringFromUTF8(text));
    if (normalized.is8Bit())
        return &m_identifiers.makeIdentifier(m_vm, normalized.span8());
    return &m_identifiers.makeIdentifier(m_vm, normalized.span16());
}

// PEP 3131, for a name with anything but ASCII in it.
bool Lexer::verifyIdentifier()
{
    if (m_flags.contains(Flag::ExtraTokens))
        return true;
    auto text = m_source.subspan(m_tokenStart, m_cursor - m_tokenStart);
    size_t offset = 0;
    bool first = true;
    while (offset < text.size()) {
        char32_t c;
        U8_NEXT_UNSAFE(text.data(), offset, c);
        if (first ? isXIDStart(c) || c == '_' : isXIDContinue(c)) {
            first = false;
            continue;
        }
        m_cursor = m_tokenStart + static_cast<uint32_t>(offset);
        if (isPrintable(c)) {
            char16_t units[2];
            unsigned length = 0;
            U16_APPEND_UNSAFE(units, length, c);
            fail(makeString("invalid character '"_s, StringView(std::span<const char16_t> { units, length }), "' (U+"_s, hex(static_cast<unsigned>(c), 4), ')'));
        } else
            fail(makeString("invalid non-printable character U+"_s, hex(static_cast<unsigned>(c), 4)));
        return false;
    }
    return true;
}

// After a backslash. False: lexing stops.
bool Lexer::continuationLine()
{
    int c = next();
    if (c != '\n') {
        m_status = Status::LineContinuation;
        return false;
    }
    c = next();
    if (c == endOfFile) {
        stop(Status::EndOfFile);
        return false;
    }
    back(c);
    return true;
}

// At the start of a line: measures the indentation and queues Indent and Dedent tokens. False: `token` is an error.
bool Lexer::lexIndentation(Token& token, bool& blankLine)
{
    constexpr int tabSize = 8;
    int column = 0;
    int alternateColumn = 0;
    int continuationColumn = 0;
    m_atLineStart = false;
    int c;
    for (;;) {
        c = next();
        if (c == ' ') {
            ++column;
            ++alternateColumn;
        } else if (c == '\t') {
            column = (column / tabSize + 1) * tabSize;
            ++alternateColumn;
        } else if (c == '\014')
            column = alternateColumn = 0;
        else if (c == '\\') {
            // Indentation does not continue over lines: what precedes the first backslash is all of it.
            if (!continuationColumn)
                continuationColumn = column;
            if (!continuationLine()) {
                finishError(token);
                return false;
            }
        } else
            break;
    }
    back(c);

    // A line of only white space or a comment says nothing about indentation.
    blankLine = c == '#' || c == '\n' || c == '\r';
    if (blankLine || !m_brackets.isEmpty())
        return true;

    if (continuationColumn)
        column = alternateColumn = continuationColumn;

    auto inconsistent = [&] {
        stop(Status::TabSpace);
        finishError(token);
        return false;
    };

    if (column == m_indents.last()) {
        if (alternateColumn != m_alternateIndents.last())
            return inconsistent();
    } else if (column > m_indents.last()) {
        if (m_indents.size() >= maxIndent) {
            stop(Status::TooDeep);
            finishError(token);
            return false;
        }
        if (alternateColumn <= m_alternateIndents.last())
            return inconsistent();
        ++m_pendingIndents;
        m_indents.append(column);
        m_alternateIndents.append(alternateColumn);
    } else {
        while (m_indents.size() > 1 && column < m_indents.last()) {
            --m_pendingIndents;
            m_indents.removeLast();
            m_alternateIndents.removeLast();
        }
        if (column != m_indents.last()) {
            stop(Status::Dedent);
            finishError(token);
            return false;
        }
        if (alternateColumn != m_alternateIndents.last())
            return inconsistent();
    }
    return true;
}

// Whether `rest` and then the end of a word follow. Consumes nothing.
bool Lexer::lookingAt(const char* rest)
{
    uint32_t cursor = m_cursor;
    for (; *rest; ++rest, ++cursor) {
        if (cursor >= m_lineEnd || m_source[cursor] != *rest)
            return false;
    }
    return cursor >= m_lineEnd || !isIdentifierPart(m_source[cursor]);
}

// `c` follows a number. A keyword may, for now, with a warning; another word may not.
bool Lexer::verifyEndOfNumber(int c, ASCIILiteral kind)
{
    if (m_flags.contains(Flag::ExtraTokens))
        return true;
    bool isKeyword = false;
    switch (c) {
    case 'a':
        isKeyword = lookingAt("nd");
        break;
    case 'e':
        isKeyword = lookingAt("lse");
        break;
    case 'f':
        isKeyword = lookingAt("or");
        break;
    case 'i': {
        int c2 = peek();
        isKeyword = c2 == 'f' || c2 == 'n' || c2 == 's';
        break;
    }
    case 'o':
        isKeyword = lookingAt("r");
        break;
    case 'n':
        isKeyword = lookingAt("ot");
        break;
    }
    if (isKeyword) {
        back(c);
        if (!warn(ParserHost::WarningCategory::SyntaxWarning, makeString("invalid "_s, kind, " literal"_s)))
            return false;
        next();
    } else if (c < 128 && isIdentifierPart(c)) {
        back(c);
        fail(makeString("invalid "_s, kind, " literal"_s));
        return false;
    }
    return true;
}

// Digits, with single underscores between them. The character after them, or 0 after an error.
int Lexer::decimalTail()
{
    int c;
    for (;;) {
        do {
            c = next();
        } while (isDigit(c));
        if (c != '_')
            return c;
        c = next();
        if (!isDigit(c)) {
            back(c);
            fail("invalid decimal literal"_s);
            return 0;
        }
    }
}

bool Lexer::lexImaginary()
{
    int c = next();
    if (!verifyEndOfNumber(c, "imaginary"_s))
        return false;
    back(c);
    return true;
}

// `c` is an 'e' or 'E' after digits.
bool Lexer::lexExponent(int e)
{
    int c = next();
    if (c == '+' || c == '-') {
        c = next();
        if (!isDigit(c)) {
            back(c);
            fail("invalid decimal literal"_s);
            return false;
        }
    } else if (!isDigit(c)) {
        // Not an exponent: the number ended before the 'e'.
        back(c);
        if (!verifyEndOfNumber(e, "decimal"_s))
            return false;
        back(e);
        return true;
    }
    c = decimalTail();
    if (!c)
        return false;
    if (c == 'j' || c == 'J')
        return lexImaginary();
    if (!verifyEndOfNumber(c, "decimal"_s))
        return false;
    back(c);
    return true;
}

// `c` follows the '.'.
bool Lexer::lexFraction(int c)
{
    if (isDigit(c)) {
        c = decimalTail();
        if (!c)
            return false;
    }
    if (c == 'e' || c == 'E')
        return lexExponent(c);
    if (c == 'j' || c == 'J')
        return lexImaginary();
    if (!verifyEndOfNumber(c, "decimal"_s))
        return false;
    back(c);
    return true;
}

bool Lexer::lexNumber(int c)
{
    auto lexRadix = [&](auto isDigitOfRadix, ASCIILiteral kind) {
        int c = next();
        do {
            if (c == '_')
                c = next();
            if (!isDigitOfRadix(c)) {
                if (isDigit(c))
                    fail(makeString("invalid digit '"_s, static_cast<char>(c), "' in "_s, kind, " literal"_s));
                else {
                    back(c);
                    fail(makeString("invalid "_s, kind, " literal"_s));
                }
                return false;
            }
            do {
                c = next();
            } while (isDigitOfRadix(c));
        } while (c == '_');
        if (isDigit(c)) {
            fail(makeString("invalid digit '"_s, static_cast<char>(c), "' in "_s, kind, " literal"_s));
            return false;
        }
        if (!verifyEndOfNumber(c, kind))
            return false;
        back(c);
        return true;
    };

    if (c == '0') {
        c = next();
        if (c == 'x' || c == 'X')
            return lexRadix(isHexDigit, "hexadecimal"_s);
        if (c == 'o' || c == 'O')
            return lexRadix([](int c) { return c >= '0' && c < '8'; }, "octal"_s);
        if (c == 'b' || c == 'B')
            return lexRadix([](int c) { return c == '0' || c == '1'; }, "binary"_s);

        // Zeros are a zero. Other digits after them are only the start of a float.
        for (;;) {
            if (c == '_') {
                c = next();
                if (!isDigit(c)) {
                    back(c);
                    fail("invalid decimal literal"_s);
                    return false;
                }
            }
            if (c != '0')
                break;
            c = next();
        }
        uint32_t zerosEnd = m_cursor;
        bool nonzero = false;
        if (isDigit(c)) {
            nonzero = true;
            c = decimalTail();
            if (!c)
                return false;
        }
        if (c == '.')
            return lexFraction(next());
        if (c == 'e' || c == 'E')
            return lexExponent(c);
        if (c == 'j' || c == 'J')
            return lexImaginary();
        if (nonzero && !m_flags.contains(Flag::ExtraTokens)) {
            back(c);
            fail(static_cast<int>(m_tokenStart + 1 - m_lineStart), static_cast<int>(zerosEnd - m_lineStart), "leading zeros in decimal integer literals are not permitted; use an 0o prefix for octal integers"_s);
            return false;
        }
        if (!verifyEndOfNumber(c, "decimal"_s))
            return false;
        back(c);
        return true;
    }

    c = decimalTail();
    if (!c)
        return false;
    if (c == '.')
        return lexFraction(next());
    if (c == 'e' || c == 'E')
        return lexExponent(c);
    if (c == 'j' || c == 'J')
        return lexImaginary();
    if (!verifyEndOfNumber(c, "decimal"_s))
        return false;
    back(c);
    return true;
}

bool Lexer::checkStringPrefixes(bool b, bool r, bool u, bool f, bool t)
{
    auto incompatible = [&](char first, char second) {
        fail(static_cast<int>(m_tokenStart + 1 - m_lineStart), static_cast<int>(m_cursor - m_lineStart), makeString('\'', first, "' and '"_s, second, "' prefixes are incompatible"_s));
        return false;
    };
    if (u && b)
        return incompatible('u', 'b');
    if (u && r)
        return incompatible('u', 'r');
    if (u && f)
        return incompatible('u', 'f');
    if (u && t)
        return incompatible('u', 't');
    if (b && f)
        return incompatible('b', 'f');
    if (b && t)
        return incompatible('b', 't');
    if (f && t)
        return incompatible('f', 't');
    return true;
}

// The opening quote has been read.
void Lexer::startInterpolatedString(Token& token, int quote)
{
    unsigned quoteSize = 1;
    int afterQuote = next();
    if (afterQuote == quote) {
        int afterAfterQuote = next();
        if (afterAfterQuote == quote)
            quoteSize = 3;
        else {
            back(afterAfterQuote);
            back(afterQuote);
        }
    } else
        back(afterQuote);

    if (m_strings.size() + 1 >= maxStringDepth) {
        fail("too many nested f-strings or t-strings"_s);
        finish(token, TokenKind::ErrorToken);
        return;
    }

    InterpolatedString string;
    uint8_t first = toASCIILower(m_source[m_tokenStart]);
    uint8_t second = toASCIILower(m_source[m_tokenStart + 1]);
    string.isTemplate = first == 't' || second == 't';
    string.isRaw = first == 'r' || second == 'r';
    string.quote = static_cast<uint8_t>(quote);
    string.quoteSize = static_cast<uint8_t>(quoteSize);
    string.start = m_tokenStart;
    string.startLineStart = m_lineStart;
    string.startLine = m_line;
    m_strings.append(string);
    finish(token, string.isTemplate ? TokenKind::TStringStart : TokenKind::FStringStart);
}

// The opening quote has been read. False: `token` is an error.
bool Lexer::lexString(Token& token, int quote)
{
    unsigned quoteSize = 1;
    unsigned endQuoteSize = 0;
    bool hasEscapedQuote = false;

    m_tokenStartLine = m_line;
    m_tokenStartLineStart = m_lineStart;

    int c = next();
    if (c == quote) {
        c = next();
        if (c == quote)
            quoteSize = 3;
        else
            endQuoteSize = 1; // An empty string.
    }
    if (c != quote)
        back(c);

    while (endQuoteSize != quoteSize) {
        c = next();
        if (c == endOfFile || (quoteSize == 1 && c == '\n')) {
            // The error is reported where the string starts.
            int detectedAt = m_line;
            m_cursor = m_tokenStart + 1;
            m_lineStart = m_tokenStartLineStart;
            m_line = m_tokenStartLine;

            if (!m_strings.isEmpty()) {
                // A quote like that of the enclosing f-string is more likely its end, with a '}' missing before it.
                auto& string = m_strings.last();
                if (string.quote == quote && string.quoteSize == quoteSize) {
                    int level = static_cast<int>(m_brackets.size()) - string.bracketDepth + string.fieldDepth;
                    int lineno = m_brackets[level].line;
                    if (lineno != m_line)
                        fail(makeString(string.prefix(), "-string: expecting '}' to close '{' on line "_s, lineno));
                    else
                        fail(makeString(string.prefix(), "-string: expecting '}'"_s));
                    finish(token, TokenKind::ErrorToken);
                    return false;
                }
            }

            if (quoteSize == 3) {
                fail(makeString("unterminated triple-quoted string literal (detected at line "_s, detectedAt, ')'));
                if (c != '\n')
                    m_status = Status::EndOfFileInTripleQuotedString;
            } else {
                fail(makeString("unterminated string literal (detected at line "_s, detectedAt, ')', hasEscapedQuote ? "; perhaps you escaped the end quote?"_s : ""_s));
                if (c != '\n')
                    m_status = Status::EndOfLineInString;
            }
            finish(token, TokenKind::ErrorToken);
            return false;
        }
        if (c == quote)
            ++endQuoteSize;
        else {
            endQuoteSize = 0;
            if (c == '\\') {
                c = next();
                if (c == quote)
                    hasEscapedQuote = true;
            }
        }
    }
    finish(token, TokenKind::String);
    return true;
}

static TokenKind oneCharacterToken(int c)
{
    switch (c) {
    case '!': return TokenKind::Exclamation;
    case '%': return TokenKind::Percent;
    case '&': return TokenKind::Amper;
    case '(': return TokenKind::LPar;
    case ')': return TokenKind::RPar;
    case '*': return TokenKind::Star;
    case '+': return TokenKind::Plus;
    case ',': return TokenKind::Comma;
    case '-': return TokenKind::Minus;
    case '.': return TokenKind::Dot;
    case '/': return TokenKind::Slash;
    case ':': return TokenKind::Colon;
    case ';': return TokenKind::Semi;
    case '<': return TokenKind::Less;
    case '=': return TokenKind::Equal;
    case '>': return TokenKind::Greater;
    case '@': return TokenKind::At;
    case '[': return TokenKind::LSqb;
    case ']': return TokenKind::RSqb;
    case '^': return TokenKind::Circumflex;
    case '{': return TokenKind::LBrace;
    case '|': return TokenKind::VBar;
    case '}': return TokenKind::RBrace;
    case '~': return TokenKind::Tilde;
    default: return TokenKind::Op;
    }
}

static TokenKind twoCharacterToken(int c1, int c2)
{
    switch (c1) {
    case '!':
        if (c2 == '=')
            return TokenKind::NotEqual;
        break;
    case '%':
        if (c2 == '=')
            return TokenKind::PercentEqual;
        break;
    case '&':
        if (c2 == '=')
            return TokenKind::AmperEqual;
        break;
    case '*':
        if (c2 == '*')
            return TokenKind::DoubleStar;
        if (c2 == '=')
            return TokenKind::StarEqual;
        break;
    case '+':
        if (c2 == '=')
            return TokenKind::PlusEqual;
        break;
    case '-':
        if (c2 == '=')
            return TokenKind::MinEqual;
        if (c2 == '>')
            return TokenKind::RArrow;
        break;
    case '/':
        if (c2 == '/')
            return TokenKind::DoubleSlash;
        if (c2 == '=')
            return TokenKind::SlashEqual;
        break;
    case ':':
        if (c2 == '=')
            return TokenKind::ColonEqual;
        break;
    case '<':
        if (c2 == '<')
            return TokenKind::LeftShift;
        if (c2 == '=')
            return TokenKind::LessEqual;
        if (c2 == '>')
            return TokenKind::NotEqual;
        break;
    case '=':
        if (c2 == '=')
            return TokenKind::EqEqual;
        break;
    case '>':
        if (c2 == '=')
            return TokenKind::GreaterEqual;
        if (c2 == '>')
            return TokenKind::RightShift;
        break;
    case '@':
        if (c2 == '=')
            return TokenKind::AtEqual;
        break;
    case '^':
        if (c2 == '=')
            return TokenKind::CircumflexEqual;
        break;
    case '|':
        if (c2 == '=')
            return TokenKind::VBarEqual;
        break;
    }
    return TokenKind::Op;
}

static TokenKind threeCharacterToken(int c1, int c2, int c3)
{
    if (c3 != '=' || c1 != c2)
        return TokenKind::Op;
    switch (c1) {
    case '*': return TokenKind::DoubleStarEqual;
    case '/': return TokenKind::DoubleSlashEqual;
    case '<': return TokenKind::LeftShiftEqual;
    case '>': return TokenKind::RightShiftEqual;
    default: return TokenKind::Op;
    }
}

void Lexer::lexCode(Token& token)
{
    InterpolatedString* string = m_strings.isEmpty() ? nullptr : &m_strings.last();
    int c;
    bool blankLine;

nextLine:
    blankLine = false;
    m_tokenHasBegun = false;
    if (m_atLineStart && !lexIndentation(token, blankLine))
        return;

    m_tokenStart = m_cursor;
    m_tokenStartColumn = static_cast<int>(m_cursor - m_lineStart);
    m_tokenHasBegun = true;

    if (m_pendingIndents) {
        bool isDedent = m_pendingIndents < 0;
        m_pendingIndents += isDedent ? 1 : -1;
        if (m_flags.contains(Flag::ExtraTokens)) {
            if (isDedent)
                finish(token, TokenKind::Dedent, m_cursor, m_cursor);
            else
                finish(token, TokenKind::Indent, m_lineStart, m_cursor);
        } else
            finishWithoutText(token, isDedent ? TokenKind::Dedent : TokenKind::Indent);
        return;
    }

again:
    m_tokenHasBegun = false;
    do {
        c = next();
    } while (c == ' ' || c == '\t' || c == '\014');

    m_tokenStart = c == endOfFile ? m_cursor : m_cursor - 1;
    m_tokenStartColumn = static_cast<int>(m_cursor - m_lineStart) - 1;
    m_tokenHasBegun = true;

    if (c == '#') {
        do {
            c = next();
        } while (c != endOfFile && c != '\n' && c != '\r');

        if (m_flags.contains(Flag::TypeComments)) {
            // "# type: " with any amount of blanks where it has one, none included.
            uint32_t p = m_tokenStart;
            const char* prefix = "# type: ";
            for (; *prefix && p < m_cursor; ++prefix) {
                if (*prefix == ' ') {
                    while (m_source[p] == ' ' || m_source[p] == '\t')
                        ++p;
                } else if (m_source[p] == *prefix)
                    ++p;
                else
                    break;
            }
            if (!*prefix) {
                back(c);
                uint32_t ignoreEnd = p + 6;
                bool isTypeIgnore = m_cursor >= ignoreEnd && !memcmp(m_source.data() + p, "ignore", 6)
                    && !(m_cursor > ignoreEnd && (m_source[ignoreEnd] >= 128 || isASCIIAlphanumeric(m_source[ignoreEnd])));
                uint32_t textStart = isTypeIgnore ? ignoreEnd : p;
                uint32_t textEnd = m_cursor;
                int endColumn = static_cast<int>(m_cursor - m_lineStart);
                int line = m_line;
                if (isTypeIgnore && blankLine) {
                    // Alone on its line: the line ending goes with it, and is part of the tag.
                    if (next() != endOfFile)
                        ++textEnd;
                    m_atLineStart = true;
                }
                token.kind = isTypeIgnore ? TokenKind::TypeIgnore : TokenKind::TypeComment;
                token.hasColumns = true;
                token.level = static_cast<uint8_t>(m_brackets.size());
                token.start = textStart;
                token.end = textEnd;
                token.startPosition = { line, static_cast<int>(textStart - m_lineStarts[line - 1]) };
                token.endPosition = { line, endColumn };
                return;
            }
        }
        if (m_flags.contains(Flag::ExtraTokens)) {
            back(c);
            m_commentOnOwnLine = blankLine;
            finish(token, TokenKind::Comment);
            return;
        }
    }

    if (c == endOfFile) {
        if (!m_brackets.isEmpty() || m_status != Status::EndOfFile)
            finishError(token);
        else
            finishWithoutText(token, TokenKind::EndMarker);
        return;
    }

    if (isIdentifierStart(c)) {
        // A name, unless it turns out to be the prefix of a string.
        bool sawB = false;
        bool sawR = false;
        bool sawU = false;
        bool sawF = false;
        bool sawT = false;
        for (;;) {
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
            c = next();
            if (c == '"' || c == '\'') {
                if (!checkStringPrefixes(sawB, sawR, sawU, sawF, sawT)) {
                    finish(token, TokenKind::ErrorToken);
                    return;
                }
                if (sawF || sawT)
                    startInterpolatedString(token, c);
                else
                    lexString(token, c);
                return;
            }
        }
        bool isASCII = true;
        while (isIdentifierPart(c)) {
            if (c >= 128)
                isASCII = false;
            c = next();
        }
        back(c);
        if (!isASCII && !verifyIdentifier()) {
            finish(token, TokenKind::ErrorToken);
            return;
        }
        finish(token, isASCII ? keywordOrName(m_source.subspan(m_tokenStart, m_cursor - m_tokenStart)) : TokenKind::Name);
        if (!m_flags.contains(Flag::WithoutIdentifiers)) {
            token.identifier = makeIdentifier(m_tokenStart, m_cursor, isASCII);
            if (!isASCII) [[unlikely]] {
                // Not the keyword, and not a name a variable can have.
                for (auto constant : { "None"_s, "True"_s, "False"_s }) {
                    if (token.identifier->string() != constant)
                        continue;
                    m_error = { };
                    m_error.type = SyntaxErrorInfo::Type::ValueError;
                    m_error.message = makeString("identifier field can't represent '"_s, constant, "' constant"_s);
                    m_status = Status::Error;
                    token.kind = TokenKind::ErrorToken;
                }
            }
        }
        return;
    }

    // Only `tokenize` gets here with one, in the middle of a line that it was given. It belongs to the token that follows, which is
    // no longer looked at as a name.
    if (c == '\r')
        c = next();

    if (c == '\n') {
        m_atLineStart = true;
        if (blankLine || !m_brackets.isEmpty()) {
            if (m_flags.contains(Flag::ExtraTokens)) {
                m_commentOnOwnLine = false;
                finish(token, TokenKind::NL);
                return;
            }
            goto nextLine;
        }
        if (m_commentOnOwnLine && m_flags.contains(Flag::ExtraTokens)) {
            m_commentOnOwnLine = false;
            finish(token, TokenKind::NL);
            return;
        }
        finish(token, TokenKind::Newline, m_tokenStart, m_cursor - 1);
        return;
    }

    if (c == '.') {
        c = next();
        if (isDigit(c)) {
            finish(token, lexFraction(c) ? TokenKind::Number : TokenKind::ErrorToken);
            return;
        }
        if (c == '.') {
            c = next();
            if (c == '.') {
                finish(token, TokenKind::Ellipsis);
                return;
            }
            back(c);
            back('.');
        } else
            back(c);
        finish(token, TokenKind::Dot);
        return;
    }

    if (isDigit(c)) {
        finish(token, lexNumber(c) ? TokenKind::Number : TokenKind::ErrorToken);
        return;
    }

    if (c == '\'' || c == '"') {
        lexString(token, c);
        return;
    }

    if (c == '\\') {
        if (!continuationLine()) {
            finishError(token);
            return;
        }
        goto again;
    }

    if (string && string->inField() && (c == ':' || c == '}' || c == '!' || c == '{')) {
        // Brackets open around this character, the field's own brace aside.
        int depth = string->bracketDepth - (c != '{');
        bool endsExpression = !depth || (depth == 1 && (string->inDebug || string->inFormatSpec));
        // The text of the expression is part of a t-string's value, and of what f"{x=}" shows.
        if (endsExpression && c != '{' && (string->inDebug || string->isTemplate))
            token.expressionStart = string->expressionStart;

        if (c == ':' && depth == string->fieldDepth) {
            string->inCode = false;
            string->inFormatSpec = true;
            finish(token, TokenKind::Colon);
            return;
        }
    }

    {
        int c2 = next();
        TokenKind kind = twoCharacterToken(c, c2);
        if (c == '<' && c2 == '>' && !m_flags.contains(Flag::BarryAsBDFL))
            kind = TokenKind::Op;
        if (kind != TokenKind::Op) {
            int c3 = next();
            TokenKind kind3 = threeCharacterToken(c, c2, c3);
            if (kind3 != TokenKind::Op)
                kind = kind3;
            else
                back(c3);
            finish(token, kind);
            return;
        }
        back(c2);
    }

    switch (c) {
    case '(':
    case '[':
    case '{':
        if (m_brackets.size() >= maxBracketDepth) {
            fail("too many nested parentheses"_s);
            finish(token, TokenKind::ErrorToken);
            return;
        }
        m_brackets.append({ static_cast<uint8_t>(c), m_line, static_cast<int>(m_tokenStart - m_lineStart) });
        if (string)
            ++string->bracketDepth;
        break;
    case ')':
    case ']':
    case '}':
        if (string && !string->bracketDepth && c == '}') {
            fail(makeString(string->prefix(), "-string: single '}' is not allowed"_s));
            finish(token, TokenKind::ErrorToken);
            return;
        }
        if (m_brackets.isEmpty() && !m_flags.contains(Flag::ExtraTokens)) {
            fail(makeString("unmatched '"_s, static_cast<char>(c), '\''));
            finish(token, TokenKind::ErrorToken);
            return;
        }
        if (!m_brackets.isEmpty()) {
            OpenBracket open = m_brackets.takeLast();
            bool matches = (open.character == '(' && c == ')') || (open.character == '[' && c == ']') || (open.character == '{' && c == '}');
            if (!matches) {
                // What does not close the brace of a replacement field is unmatched, rather than mismatched with it.
                if (string && open.character == '{' && string->bracketDepth - 1 == string->fieldDepth) {
                    fail(makeString(string->prefix(), "-string: unmatched '"_s, static_cast<char>(c), '\''));
                    finish(token, TokenKind::ErrorToken);
                    return;
                }
                if (!m_flags.contains(Flag::ExtraTokens)) {
                    if (open.line != m_line)
                        fail(makeString("closing parenthesis '"_s, static_cast<char>(c), "' does not match opening parenthesis '"_s, static_cast<char>(open.character), "' on line "_s, open.line));
                    else
                        fail(makeString("closing parenthesis '"_s, static_cast<char>(c), "' does not match opening parenthesis '"_s, static_cast<char>(open.character), '\''));
                    finish(token, TokenKind::ErrorToken);
                    return;
                }
            }
        }
        if (string) {
            if (--string->bracketDepth < 0) {
                fail(makeString(string->prefix(), "-string: unmatched '"_s, static_cast<char>(c), '\''));
                finish(token, TokenKind::ErrorToken);
                return;
            }
            if (c == '}' && string->bracketDepth == string->fieldDepth) {
                --string->fieldDepth;
                string->inCode = false;
                string->inFormatSpec = false;
                string->inDebug = false;
            }
        }
        break;
    default:
        break;
    }

    if (c < 0x20 || c == 0x7F) {
        fail(makeString("invalid non-printable character U+"_s, hex(static_cast<unsigned>(c), 4)));
        finish(token, TokenKind::ErrorToken);
        return;
    }

    if (c == '=' && string && string->atTopOfField())
        string->inDebug = true;

    finish(token, oneCharacterToken(c));
}

// Between the replacement fields of an f-string or t-string, and in their format specs.
void Lexer::lexStringPart(Token& token, InterpolatedString& string)
{
    TokenKind middle = string.isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle;

    m_tokenStart = m_cursor;
    m_tokenStartLine = m_line;
    m_tokenStartColumn = static_cast<int>(m_cursor - m_lineStart);
    m_tokenHasBegun = true;

    auto enterField = [&]() -> bool {
        if (++string.fieldDepth >= maxFieldNesting) {
            fail(makeString(string.prefix(), "-string: expressions nested too deeply"_s));
            finish(token, TokenKind::ErrorToken);
            return false;
        }
        string.inCode = true;
        return true;
    };

    // A field right here leaves no text before it.
    int startCharacter = next();
    if (startCharacter == '{') {
        int afterBrace = peek();
        if (afterBrace != '{')
            string.expressionStart = m_cursor;
        back(startCharacter);
        if (afterBrace != '{') {
            if (enterField())
                lexCode(token);
            return;
        }
    } else
        back(startCharacter);

    {
        unsigned i = 0;
        for (; i < string.quoteSize; ++i) {
            int quote = next();
            if (quote != string.quote) {
                back(quote);
                break;
            }
        }
        if (i == string.quoteSize) {
            TokenKind end = string.isTemplate ? TokenKind::TStringEnd : TokenKind::FStringEnd;
            m_strings.removeLast();
            finish(token, end);
            return;
        }
        // The quotes read so far are text.
    }

    unsigned endQuoteSize = 0;
    bool inNamedEscape = false;
    while (endQuoteSize != string.quoteSize) {
        int c = next();
        bool inFormatSpec = string.inFormatSpec && string.inField();

        if (c == endOfFile || (string.quoteSize == 1 && c == '\n')) {
            if (inFormatSpec && c == '\n') {
                fail(makeString(string.prefix(), "-string: newlines are not allowed in format specifiers for single quoted "_s, string.prefix(), "-strings"_s));
                finish(token, TokenKind::ErrorToken);
                return;
            }
            // The error is reported where the string starts.
            int detectedAt = m_line;
            m_cursor = string.start + 1;
            m_lineStart = string.startLineStart;
            m_line = string.startLine;
            if (string.quoteSize == 3) {
                fail(makeString("unterminated triple-quoted "_s, string.prefix(), "-string literal (detected at line "_s, detectedAt, ')'));
                if (c != '\n')
                    m_status = Status::EndOfFileInTripleQuotedString;
            } else
                fail(makeString("unterminated "_s, string.prefix(), "-string literal (detected at line "_s, detectedAt, ')'));
            finish(token, TokenKind::ErrorToken);
            return;
        }

        if (c == string.quote) {
            ++endQuoteSize;
            continue;
        }
        endQuoteSize = 0;

        if (c == '{') {
            int afterBrace = next();
            if (afterBrace != '{' || inFormatSpec) {
                back(afterBrace);
                string.expressionStart = m_cursor;
                back(c);
                if (!enterField())
                    return;
                string.inFormatSpec = false;
                finish(token, middle);
            } else
                finish(token, middle, m_tokenStart, m_cursor - 1); // "{{" is a brace.
            return;
        }
        if (c == '}') {
            if (inNamedEscape) {
                finish(token, middle);
                return;
            }
            int afterBrace = next();
            if (afterBrace == '}' && !inFormatSpec && !string.bracketDepth)
                finish(token, middle, m_tokenStart, m_cursor - 1); // "}}" is a brace.
            else {
                back(afterBrace);
                back(c);
                string.inCode = true;
                string.inFormatSpec = false;
                finish(token, middle);
            }
            return;
        }
        if (c == '\\') {
            int escaped = next();
            if (escaped == '{' || escaped == '}') {
                // The brace is still a brace.
                char brace = static_cast<char>(escaped);
                if (!string.isRaw && !warn(ParserHost::WarningCategory::SyntaxWarning,
                    makeString("\"\\"_s, brace, "\" is an invalid escape sequence. Such sequences will not work in the future. Did you mean \"\\\\"_s, brace, "\"? A raw string is also an option."_s),
                    makeString("\"\\"_s, brace, "\" is an invalid escape sequence. Did you mean \"\\\\"_s, brace, "\"? A raw string is also an option."_s))) {
                    finish(token, TokenKind::ErrorToken);
                    return;
                }
                back(escaped);
                continue;
            }
            if (!string.isRaw && escaped == 'N') {
                // \N{name}: these braces are not a field.
                int afterN = next();
                if (afterN == '{')
                    inNamedEscape = true;
                else
                    back(afterN);
            }
        }
    }

    // The closing quotes are the next token.
    for (unsigned i = 0; i < string.quoteSize; ++i)
        back(string.quote);
    finish(token, middle);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
