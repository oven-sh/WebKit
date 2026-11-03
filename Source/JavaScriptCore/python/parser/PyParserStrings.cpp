/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyParser.h"

#include "PyUnicode.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

Constant* Parser::makeStringConstant(const String& string, const Range& range)
{
    auto* constant = make<Constant>(range);
    constant->value.kind = ConstantValue::Kind::Str;
    constant->value.string = m_arena.retain(string.isNull() ? emptyString() : string);
    return constant;
}

static bool isEmptyString(Expr* expression)
{
    auto* constant = expression->tryAs<Constant>();
    return constant && constant->value.isString() && !constant->value.string->length();
}

// `escape` is the offset in `body` of the character after the backslash; `virtualOffset`, the same counted the way the
// reference implementation counts, which shows in the column of the error that the warning may become.
bool Parser::warnInvalidEscape(std::span<const uint8_t> body, size_t escape, size_t virtualOffset, const Token& token)
{
    uint8_t c = body[escape];
    bool inInterpolatedString = token.kind == TokenKind::FStringMiddle || token.kind == TokenKind::FStringEnd || token.kind == TokenKind::TStringMiddle || token.kind == TokenKind::TStringEnd;
    if (inInterpolatedString && (c == '{' || c == '}'))
        return true; // The lexer has warned.

    // CPython gives none while it goes over the text again, so as not to give them twice, except where it is not looking for explanations.
    if (m_diagnosing && !m_quiet)
        return true;

    // What is parsed twice warns once.
    uint32_t sourceOffset = static_cast<uint32_t>(body.data() + escape - m_lexer.source().data());
    if (m_warnedEscapes.contains(sourceOffset))
        return true;
    m_warnedEscapes.append(sourceOffset);

    bool isOctal = c >= '4' && c <= '7';
    auto sequence = byteCast<Latin1Character>(body.subspan(escape, isOctal ? std::min<size_t>(3, body.size() - escape) : 1));
    auto kind = isOctal ? " is an invalid octal escape sequence. "_s : " is an invalid escape sequence. "_s;

    int lineno = token.startPosition.line;
    int column = token.startPosition.column;
    size_t lastNewline = notFound;
    for (size_t i = 0; i < escape; ++i) {
        if (body[i] == '\n') {
            ++lineno;
            lastNewline = i;
        }
    }
    if (lastNewline == notFound) {
        column += static_cast<int>(virtualOffset);
        auto text = textOf(token);
        uint8_t quote = text.empty() ? 0 : text[0];
        if (quote == '\'' || quote == '"') {
            for (uint8_t character : text) {
                if (character != quote)
                    break;
                ++column;
            }
        }
    } else
        column = static_cast<int>(escape - lastNewline - 1);

    auto category = m_featureVersion >= 12 ? ParserHost::WarningCategory::SyntaxWarning : ParserHost::WarningCategory::DeprecationWarning;
    switch (m_host.warn({ category, makeString("\"\\"_s, sequence, '"', kind, "Such sequences will not work in the future. Did you mean \"\\\\"_s, sequence, "\"? A raw string is also an option."_s), lineno })) {
    case ParserHost::WarningResult::Reported:
        return true;
    case ParserHost::WarningResult::BecameException:
        raise(SyntaxErrorInfo::Type::SyntaxError, lineno, column - 1, lineno, column + 1, makeString("\"\\"_s, sequence, '"', kind, "Did you mean \"\\\\"_s, sequence, "\"? A raw string is also an option."_s));
        return false;
    case ParserHost::WarningResult::OtherException:
        m_error = { };
        m_error.type = SyntaxErrorInfo::Type::HostException;
        m_errorIsFatal = true;
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The text between the quotes of a str literal, or of a part of an f-string. `errorRange`: where to report what cannot be
// decoded; null for the last token read.
bool Parser::decodeString(std::span<const uint8_t> body, bool isRaw, const Token& token, const Range* errorRange, String& result)
{
    if (isRaw || !memchr(body.data(), '\\', body.size())) {
        result = stringFromUTF8(body);
        return true;
    }

    // Errors give positions in a form of the text where a character beyond ASCII takes the 10 bytes of \UXXXXXXXX.
    size_t virtualOffset = 0;
    auto fail = [&](size_t start, size_t end, uint8_t firstByte, ASCIILiteral reason) {
        String message;
        if (end == start + 1)
            message = makeString("(unicode error) 'unicodeescape' codec can't decode byte 0x"_s, hex(firstByte, 2, Lowercase), " in position "_s, start, ": "_s, reason);
        else
            message = makeString("(unicode error) 'unicodeescape' codec can't decode bytes in position "_s, start, '-', end - 1, ": "_s, reason);
        if (errorRange)
            syntaxErrorAt(*errorRange, message);
        else
            syntaxError(message);
        return false;
    };

    StringBuilder builder;
    builder.reserveCapacity(body.size());
    size_t firstInvalidEscape = notFound;
    size_t firstInvalidEscapeVirtualOffset = 0;

    size_t i = 0;
    size_t size = body.size();
    while (i < size) {
        uint8_t c = body[i];
        if (c >= 0x80) {
            char32_t character;
            U8_NEXT_UNSAFE(body.data(), i, character);
            builder.append(character);
            virtualOffset += 10;
            continue;
        }
        ++i;
        ++virtualOffset;
        if (c != '\\') {
            builder.append(static_cast<Latin1Character>(c));
            continue;
        }

        size_t escapeStart = virtualOffset - 1;
        if (i >= size || body[i] >= 0x80) {
            // A backslash before the end or before a character beyond ASCII stands for itself.
            builder.append('\\');
            virtualOffset += 5;
            continue;
        }
        c = body[i++];
        ++virtualOffset;

        unsigned count;
        ASCIILiteral truncated;
        switch (c) {
        case '\n':
            continue;
        case '\\':
        case '\'':
        case '"':
            builder.append(static_cast<Latin1Character>(c));
            continue;
        case 'b':
            builder.append('\b');
            continue;
        case 'f':
            builder.append('\f');
            continue;
        case 't':
            builder.append('\t');
            continue;
        case 'n':
            builder.append('\n');
            continue;
        case 'r':
            builder.append('\r');
            continue;
        case 'v':
            builder.append('\v');
            continue;
        case 'a':
            builder.append('\a');
            continue;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            unsigned value = c - '0';
            size_t first = i - 1;
            for (unsigned digits = 1; digits < 3 && i < size && body[i] >= '0' && body[i] <= '7'; ++digits) {
                value = (value << 3) + body[i++] - '0';
                ++virtualOffset;
            }
            if (value > 0377 && firstInvalidEscape == notFound) {
                firstInvalidEscape = first;
                firstInvalidEscapeVirtualOffset = virtualOffset - 3;
            }
            builder.append(static_cast<char16_t>(value));
            continue;
        }
        case 'x':
            count = 2;
            truncated = "truncated \\xXX escape"_s;
            break;
        case 'u':
            count = 4;
            truncated = "truncated \\uXXXX escape"_s;
            break;
        case 'U':
            count = 8;
            truncated = "truncated \\UXXXXXXXX escape"_s;
            break;
        case 'N': {
            if (!m_host.hasCharacterNames()) {
                auto message = "(unicode error) \\N escapes not supported (can't load unicodedata module)"_s;
                if (errorRange)
                    syntaxErrorAt(*errorRange, message);
                else
                    syntaxError(message);
                return false;
            }
            auto malformed = "malformed \\N character escape"_s;
            if (i >= size || body[i] != '{')
                return fail(escapeStart, virtualOffset, '\\', malformed);
            ++i;
            ++virtualOffset;
            size_t nameStart = i;
            while (i < size && body[i] != '}') {
                // A name is ASCII; what is not fails the lookup, and only its length counts.
                if (body[i] >= 0x80) {
                    U8_FWD_1_UNSAFE(body.data(), i);
                    virtualOffset += 10;
                } else {
                    // Here too, a backslash before a character beyond ASCII or before the end took 6 bytes.
                    virtualOffset += body[i] == '\\' && (i + 1 >= size || body[i + 1] >= 0x80) ? 6 : 1;
                    ++i;
                }
            }
            if (i >= size)
                return fail(escapeStart, virtualOffset, '\\', malformed);
            auto name = body.subspan(nameStart, i - nameStart);
            if (name.empty())
                return fail(escapeStart, virtualOffset, '\\', malformed);
            ++i;
            ++virtualOffset;
            auto character = codePointFromName(name);
            if (!character)
                return fail(escapeStart, virtualOffset, '\\', "unknown Unicode character name"_s);
            builder.append(*character);
            continue;
        }
        default:
            if (firstInvalidEscape == notFound) {
                firstInvalidEscape = i - 1;
                firstInvalidEscapeVirtualOffset = virtualOffset - 1;
            }
            builder.append('\\');
            builder.append(static_cast<Latin1Character>(c));
            continue;
        }

        uint32_t value = 0;
        for (; count; --count) {
            if (i >= size || !isASCIIHexDigit(body[i]))
                return fail(escapeStart, virtualOffset, '\\', truncated);
            value = (value << 4) | toASCIIHexValue(body[i++]);
            ++virtualOffset;
        }
        if (value > 0x10FFFF)
            return fail(escapeStart, virtualOffset, '\\', "illegal Unicode character"_s);
        if (value <= 0xFFFF)
            builder.append(static_cast<char16_t>(value));
        else
            builder.append(static_cast<char32_t>(value));
    }

    if (firstInvalidEscape != notFound && !warnInvalidEscape(body, firstInvalidEscape, firstInvalidEscapeVirtualOffset, token))
        return false;
    result = builder.toString();
    return true;
}

bool Parser::decodeBytes(std::span<const uint8_t> body, bool isRaw, const Token& token, ConstantValue& result)
{
    result.kind = ConstantValue::Kind::Bytes;
    if (isRaw || !memchr(body.data(), '\\', body.size())) {
        auto copy = m_arena.copy(body);
        result.bytes = { copy.begin(), copy.size() };
        return true;
    }

    Vector<uint8_t, 64> bytes;
    bytes.reserveInitialCapacity(body.size());
    size_t firstInvalidEscape = notFound;
    size_t i = 0;
    size_t size = body.size();
    while (i < size) {
        uint8_t c = body[i++];
        if (c != '\\') {
            bytes.append(c);
            continue;
        }
        if (i == size) {
            syntaxError("(value error) Trailing \\ in string"_s);
            return false;
        }
        c = body[i++];
        switch (c) {
        case '\n':
            break;
        case '\\':
        case '\'':
        case '"':
            bytes.append(c);
            break;
        case 'b':
            bytes.append('\b');
            break;
        case 'f':
            bytes.append('\f');
            break;
        case 't':
            bytes.append('\t');
            break;
        case 'n':
            bytes.append('\n');
            break;
        case 'r':
            bytes.append('\r');
            break;
        case 'v':
            bytes.append('\v');
            break;
        case 'a':
            bytes.append('\a');
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            unsigned value = c - '0';
            size_t first = i - 1;
            for (unsigned digits = 1; digits < 3 && i < size && body[i] >= '0' && body[i] <= '7'; ++digits)
                value = (value << 3) + body[i++] - '0';
            if (value > 0377 && firstInvalidEscape == notFound)
                firstInvalidEscape = first;
            bytes.append(static_cast<uint8_t>(value));
            break;
        }
        case 'x':
            if (i + 1 < size && isASCIIHexDigit(body[i]) && isASCIIHexDigit(body[i + 1])) {
                bytes.append(static_cast<uint8_t>(toASCIIHexValue(body[i], body[i + 1])));
                i += 2;
                break;
            }
            syntaxError(makeString("(value error) invalid \\x escape at position "_s, i - 2));
            return false;
        default:
            if (firstInvalidEscape == notFound)
                firstInvalidEscape = i - 1;
            bytes.append('\\');
            --i;
        }
    }

    if (firstInvalidEscape != notFound && !warnInvalidEscape(body, firstInvalidEscape, firstInvalidEscape, token))
        return false;
    auto copy = m_arena.copy(bytes);
    result.bytes = { copy.begin(), copy.size() };
    return true;
}

// A String token: a str or bytes literal with its prefix and quotes.
Expr* Parser::parseStringLiteral(const Token& token)
{
    auto text = textOf(token);
    bool isBytes = false;
    bool isRaw = false;
    size_t prefixLength = 0;
    for (; text[prefixLength] != '\'' && text[prefixLength] != '"'; ++prefixLength) {
        switch (toASCIILower(text[prefixLength])) {
        case 'b':
            isBytes = true;
            break;
        case 'r':
            isRaw = true;
            break;
        }
    }
    uint8_t quote = text[prefixLength];
    auto body = text.subspan(prefixLength);
    size_t quoteSize = body.size() >= 6 && body[1] == quote && body[2] == quote ? 3 : 1;
    body = body.subspan(quoteSize, body.size() - 2 * quoteSize);

    auto* constant = make<Constant>(rangeOf(token));
    if (isBytes) {
        for (uint8_t c : body) {
            if (c >= 0x80)
                return syntaxErrorAt(token, "bytes can only contain ASCII literal characters"_s);
        }
        if (!decodeBytes(body, isRaw, token, constant->value))
            return nullptr;
        return constant;
    }

    String string;
    if (!decodeString(body, isRaw, token, nullptr, string))
        return nullptr;
    constant->value.kind = ConstantValue::Kind::Str;
    constant->value.string = m_arena.retain(string.isNull() ? emptyString() : string);
    constant->kindIsU = text[0] == 'u';
    return constant;
}

// The source of the expression of a replacement field, without its comments.
String Parser::expressionText(uint32_t start, uint32_t end)
{
    auto text = m_lexer.source().subspan(start, end - start);

    // Quotes come in odd numbers, so toggling on each tells inside from outside even for triple quotes.
    bool hasComment = false;
    bool inString = false;
    uint8_t quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        uint8_t c = text[i];
        if (c == '\\') {
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            if (!inString) {
                inString = true;
                quote = c;
            } else if (c == quote)
                inString = false;
            continue;
        }
        if (c == '#' && !inString) {
            hasComment = true;
            break;
        }
    }
    if (!hasComment)
        return stringFromUTF8(text);

    Vector<uint8_t, 64> result;
    inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        uint8_t c = text[i];
        if (c == '\\') {
            result.append(c);
            if (++i < text.size())
                result.append(text[i]);
        } else if (c == '"' || c == '\'') {
            if (!inString) {
                inString = true;
                quote = c;
            } else if (c == quote)
                inString = false;
            result.append(c);
        } else if (c == '#' && !inString) {
            while (i < text.size() && text[i] != '\n')
                ++i;
            if (i < text.size())
                result.append('\n');
        } else
            result.append(c);
    }
    return stringFromUTF8(result.span());
}

// "x = " without the '=' and what follows it: blanks, and line continuations.
static String withoutDebugSuffix(const String& text)
{
    unsigned length = text.length();
    while (length) {
        bool hasNewline = false;
        while (length && isWhitespace(text[length - 1])) {
            if (text[length - 1] == '\r' || text[length - 1] == '\n')
                hasNewline = true;
            --length;
        }
        if (!hasNewline || !length || text[length - 1] != '\\')
            break;
        --length;
    }
    if (!length || text[length - 1] != '=')
        return text;
    return text.left(length - 1);
}

// '{' annotated_rhs '='? ['!' NAME] [':' format_spec*] '}'
// With '=', the result is a JoinedStr of the text of the expression and the field, for the caller to splice in.
Expr* Parser::parseReplacementField(bool isTemplate, bool isRaw, bool inFormatSpec)
{
    char prefix = isTemplate ? 't' : 'f';
    Position start = startPosition();
    advance();

    switch (kind()) {
    case TokenKind::Equal:
    case TokenKind::Exclamation:
    case TokenKind::Colon:
    case TokenKind::RBrace:
        return syntaxErrorAt(peek(), makeString(prefix, "-string: valid expression required before '"_s, byteCast<Latin1Character>(textOf(peek())), '\''));
    default:
        break;
    }

    const Token firstToken = peek();
    auto beforeExpression = checkpoint();
    Expr* value = parseAnnotatedRHS();
    if (!value && m_errorIsGeneric && !m_errorIsFatal) {
        // If it starts with an expression, the complaint is about what follows that.
        rewind(beforeExpression);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        SetForScope diagnosing(m_diagnosing, true);
        value = parseAnnotatedRHS();
    }
    if (!value) {
        if (m_errorIsGeneric && !m_errorIsFatal) {
            m_error = { };
            syntaxErrorAt(firstToken, makeString(prefix, "-string: expecting a valid expression after '{'"_s));
        }
        return nullptr;
    }

    bool isDebug = accept(TokenKind::Equal);

    // The token that follows the expression knows where its text starts.
    uint32_t textStart = 0;
    uint32_t textEnd = 0;
    auto noteText = [&](const Token& token) {
        if (!textStart && token.expressionStart) {
            textStart = token.expressionStart;
            textEnd = token.start;
        }
    };

    int conversion = -1;
    Position debugEnd;
    bool hasConversion = false;
    if (at(TokenKind::Exclamation)) {
        const Token exclamation = advance();
        noteText(exclamation);
        if (at(TokenKind::Colon) || at(TokenKind::RBrace))
            return syntaxErrorOnNextToken(makeString(prefix, "-string: missing conversion character"_s));
        if (!at(TokenKind::Name))
            return syntaxErrorOnNextToken(makeString(prefix, "-string: invalid conversion character"_s));
        const Token name = advance();
        if (exclamation.endPosition != name.startPosition)
            return syntaxErrorAt(rangeBetween(rangeOf(exclamation), rangeOf(name)), makeString(prefix, "-string: conversion type must come right after the exclamation mark"_s));
        auto& string = name.identifier->string();
        if (string.length() != 1 || (string[0] != 's' && string[0] != 'r' && string[0] != 'a'))
            return syntaxErrorAt(name, makeString(prefix, "-string: invalid conversion character '"_s, string, "': expected 's', 'r', or 'a'"_s));
        conversion = string[0];
        hasConversion = true;
        debugEnd = name.startPosition;
    }

    Expr* formatSpec = nullptr;
    if (at(TokenKind::Colon)) {
        const Token colon = advance();
        noteText(colon);
        ExprVector parts;
        for (;;) {
            if (at(TokenKind::FStringMiddle) || at(TokenKind::TStringMiddle)) {
                const Token middle = advance();
                String string;
                if (!decodeString(textOf(middle), isRaw, middle, nullptr, string))
                    return nullptr;
                if (!string.isEmpty())
                    parts.append(makeStringConstant(string, rangeOf(middle)));
            } else if (at(TokenKind::LBrace)) {
                Expr* field = parseReplacementField(isTemplate, isRaw, true);
                if (!field)
                    return nullptr;
                parts.append(field);
            } else
                break;
        }
        Range range = rangeFrom(colon.startPosition);
        if (parts.isEmpty() || (parts.size() == 1 && parts[0]->is<Constant>())) {
            auto* joined = make<JoinedStr>(range);
            joined->values = m_arena.copy(parts);
            formatSpec = joined;
        } else {
            auto* joined = make<JoinedStr>(range);
            joined->values = flattenAndFoldStrings(parts);
            formatSpec = joined;
        }
        if (!hasConversion)
            debugEnd = { colon.startPosition.line, colon.startPosition.column + 1 };
        if (!at(TokenKind::RBrace))
            return failed() ? nullptr : syntaxErrorOnNextToken(makeString(prefix, "-string: expecting '}', or format specs"_s));
    }

    if (!at(TokenKind::RBrace)) {
        if (failed())
            return nullptr;
        if (hasConversion)
            return syntaxErrorOnNextToken(makeString(prefix, "-string: expecting ':' or '}'"_s));
        if (isDebug)
            return syntaxErrorOnNextToken(makeString(prefix, "-string: expecting '!', or ':', or '}'"_s));
        return syntaxErrorOnNextToken(makeString(prefix, "-string: expecting '=', or '!', or ':', or '}'"_s));
    }
    const Token close = advance();
    noteText(close);
    Range range = rangeFrom(start);
    if (!hasConversion && !formatSpec)
        debugEnd = close.endPosition;

    // Without a conversion or a format, x= shows the repr.
    if (isDebug && !hasConversion && !formatSpec)
        conversion = 'r';

    String text;
    if (isDebug || (isTemplate && !inFormatSpec))
        text = expressionText(textStart, textEnd);

    Expr* field;
    if (isTemplate && !inFormatSpec) {
        auto* interpolation = make<Interpolation>(range);
        interpolation->value = value;
        interpolation->str.kind = ConstantValue::Kind::Str;
        String expression = isDebug ? withoutDebugSuffix(text) : text;
        interpolation->str.string = m_arena.retain(expression.isNull() ? emptyString() : expression);
        interpolation->conversion = conversion;
        interpolation->formatSpec = formatSpec;
        field = interpolation;
    } else {
        auto* formatted = make<FormattedValue>(range);
        formatted->value = value;
        formatted->conversion = conversion;
        formatted->formatSpec = formatSpec;
        field = formatted;
    }
    if (!isDebug)
        return field;

    Expr* pair[2] = {
        makeStringConstant(text, { range.lineno, range.colOffset + 1, debugEnd.line, debugEnd.column - 1 }),
        field,
    };
    auto* joined = make<JoinedStr>(Range { range.lineno, range.colOffset, debugEnd.line, debugEnd.column });
    joined->values = m_arena.copy(std::span<Expr* const> { pair });
    return joined;
}

// FSTRING_START middle* FSTRING_END, or the same of a t-string.
Expr* Parser::parseInterpolatedString()
{
    const Token startToken = advance();
    bool isTemplate = startToken.kind == TokenKind::TStringStart;
    bool isRaw = false;
    for (uint8_t c : textOf(startToken))
        isRaw |= c == 'r' || c == 'R';
    TokenKind middleKind = isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle;
    TokenKind endKind = isTemplate ? TokenKind::TStringEnd : TokenKind::FStringEnd;

    // The text is decoded once the end is known, which is where warnings about it point.
    struct Part {
        Expr* field;
        Token middle;
    };
    Vector<Part, 8> parts;
    for (;;) {
        if (at(middleKind))
            parts.append({ nullptr, advance() });
        else if (at(TokenKind::LBrace)) {
            Expr* field = parseReplacementField(isTemplate, isRaw, false);
            if (!field)
                return nullptr;
            parts.append({ field, { } });
        } else
            break;
    }
    if (!at(endKind))
        return invalidSyntax();
    const Token endToken = advance();

    ExprVector values;
    for (auto& part : parts) {
        if (part.field) {
            if (auto* pair = part.field->tryAs<JoinedStr>()) {
                values.append(pair->values[0]);
                values.append(pair->values[1]);
            } else
                values.append(part.field);
            continue;
        }
        Range range = rangeOf(part.middle);
        String string;
        if (!decodeString(textOf(part.middle), isRaw, endToken, &range, string))
            return nullptr;
        if (!string.isEmpty())
            values.append(makeStringConstant(string, range));
    }

    Range range = rangeBetween(rangeOf(startToken), rangeOf(endToken));
    if (isTemplate) {
        if (!checkVersion(14, "t-strings are"_s))
            return nullptr;
        auto* result = make<TemplateStr>(range);
        result->values = m_arena.copy(values);
        return result;
    }
    auto* result = make<JoinedStr>(range);
    result->values = m_arena.copy(values);
    return result;
}

// The values of adjacent literals as those of one: nested values spliced in, adjacent constants joined, empty ones dropped.
Seq<Expr*> Parser::flattenAndFoldStrings(const ExprVector& strings)
{
    ExprVector flattened;
    for (Expr* string : strings) {
        if (auto* joined = string->tryAs<JoinedStr>())
            flattened.append(std::span<Expr* const> { joined->values.begin(), joined->values.size() });
        else if (auto* templateString = string->tryAs<TemplateStr>())
            flattened.append(std::span<Expr* const> { templateString->values.begin(), templateString->values.size() });
        else
            flattened.append(string);
    }

    ExprVector values;
    for (size_t i = 0; i < flattened.size(); ++i) {
        Expr* element = flattened[i];
        if (element->is<Constant>()) {
            if (i + 1 < flattened.size() && flattened[i + 1]->is<Constant>()) {
                auto* first = element->as<Constant>();
                Expr* last = element;
                StringBuilder builder;
                size_t j = i;
                for (; j < flattened.size() && flattened[j]->is<Constant>(); ++j) {
                    last = flattened[j];
                    builder.append(StringView { *last->as<Constant>()->value.string });
                }
                i = j - 1;
                Constant* folded = makeStringConstant(builder.toString(), rangeBetween(first->range, last->range));
                folded->kindIsU = first->kindIsU;
                element = folded;
            }
            if (isEmptyString(element))
                continue;
        }
        values.append(element);
    }
    return m_arena.copy(values);
}

Expr* Parser::concatenateStrings(const ExprVector& strings, const Range& range, bool isTemplate)
{
    if (isTemplate) {
        auto* result = make<TemplateStr>(range);
        result->values = flattenAndFoldStrings(strings);
        return result;
    }

    bool sawInterpolated = false;
    bool sawString = false;
    bool sawBytes = false;
    for (Expr* string : strings) {
        if (auto* constant = string->tryAs<Constant>())
            (constant->value.isBytes() ? sawBytes : sawString) = true;
        else
            sawInterpolated = true;
    }
    if ((sawString || sawInterpolated) && sawBytes)
        return syntaxError("cannot mix bytes and nonbytes literals"_s);

    if (sawInterpolated) {
        auto* result = make<JoinedStr>(range);
        result->values = flattenAndFoldStrings(strings);
        return result;
    }
    if (strings.size() == 1)
        return strings[0];

    auto* first = strings[0]->as<Constant>();
    if (sawBytes) {
        Vector<uint8_t, 64> bytes;
        for (Expr* string : strings) {
            auto& value = string->as<Constant>()->value;
            bytes.append(std::span { value.bytes.data, value.bytes.size });
        }
        auto copy = m_arena.copy(bytes);
        auto* result = make<Constant>(range);
        result->value.kind = ConstantValue::Kind::Bytes;
        result->value.bytes = { copy.begin(), copy.size() };
        result->kindIsU = first->kindIsU;
        return result;
    }

    StringBuilder builder;
    for (Expr* string : strings)
        builder.append(StringView { *string->as<Constant>()->value.string });
    Constant* result = makeStringConstant(builder.toString(), range);
    result->kindIsU = first->kindIsU;
    return result;
}

// strings: (fstring | string)+ | tstring+
Expr* Parser::parseStrings()
{
    Position start = startPosition();
    bool isTemplate = at(TokenKind::TStringStart);
    ExprVector strings;
    for (;;) {
        Expr* string;
        if (at(TokenKind::String)) {
            const Token token = advance();
            string = parseStringLiteral(token);
        } else if (at(TokenKind::FStringStart) || at(TokenKind::TStringStart))
            string = parseInterpolatedString();
        else
            break;
        if (!string)
            return nullptr;
        if (string->is<TemplateStr>() != isTemplate)
            return syntaxErrorAt(rangeBetween(strings.last()->range, string->range), "cannot mix t-string literals with string or bytes literals"_s);
        strings.append(string);
    }
    return concatenateStrings(strings, rangeFrom(start), isTemplate);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
