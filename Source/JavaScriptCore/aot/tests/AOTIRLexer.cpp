/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRLexer.h"

#if ENABLE(AOT)

#include "AOTVector.h"
#include <array>
#include <limits>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/FastFloat.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr ASCIILiteral infinityWord = "Infinity"_s;

static constexpr size_t characterEscapeLength = 2;
static constexpr size_t byteEscapeLength = 4;
static constexpr size_t codePointEscapePrefixLength = 3;
static constexpr size_t maxCodePointDigits = 6;

static constexpr bool isWordStart(uint8_t byte) { return isASCIIAlpha(byte) || byte == '_'; }
static constexpr bool isWordCharacter(uint8_t byte) { return isASCIIAlphanumeric(byte) || byte == '_'; }

static bool startsWithWord(std::span<const uint8_t> text, ASCIILiteral word)
{
    return spanHasPrefix(text, word.span8()) && (text.size() == word.length() || !isWordCharacter(text[word.length()]));
}

static constexpr std::optional<TokenKind> twoBytePunctuatorKindOf(uint8_t first, std::optional<uint8_t> second)
{
    if (first == '-' && second == '>')
        return TokenKind::Arrow;
    if (first == '@' && second == '@')
        return TokenKind::AtAt;
    if (first == '.' && second == '.')
        return TokenKind::DotDot;
    return std::nullopt;
}

static constexpr TokenKind punctuatorKindOf(uint8_t byte)
{
    switch (byte) {
    case ':':
        return TokenKind::Colon;
    case '=':
        return TokenKind::Equal;
    case ',':
        return TokenKind::Comma;
    case '.':
        return TokenKind::Dot;
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
    case '<':
        return TokenKind::LeftAngle;
    case '>':
        return TokenKind::RightAngle;
    case '|':
        return TokenKind::Bar;
    case '~':
        return TokenKind::Tilde;
    case '@':
        return TokenKind::At;
    case '#':
        return TokenKind::NumberSign;
    case '?':
        return TokenKind::Question;
    case '-':
        return TokenKind::Minus;
    case '/':
        return TokenKind::Slash;
    default:
        return TokenKind::Invalid;
    }
}

static size_t digitRunLength(std::span<const uint8_t> text)
{
    size_t length = 0;
    while (length < text.size() && isASCIIDigit(text[length]))
        ++length;
    return length;
}

static size_t fractionLength(std::span<const uint8_t> text)
{
    if (text.empty() || text[0] != '.')
        return 0;
    size_t numberOfDigits = digitRunLength(text.subspan(1));
    return numberOfDigits ? 1 + numberOfDigits : 0;
}

static size_t exponentLength(std::span<const uint8_t> text)
{
    if (text.empty() || (text[0] != 'e' && text[0] != 'E'))
        return 0;
    size_t signLength = text.size() > 1 && (text[1] == '+' || text[1] == '-') ? 1 : 0;
    size_t numberOfDigits = digitRunLength(text.subspan(1 + signLength));
    return numberOfDigits ? 1 + signLength + numberOfDigits : 0;
}

Token Lexer::next()
{
    while (true) {
        skipBlanksAndComments();
        State start = m_state;
        auto byte = peek();
        if (!byte) {
            if (!std::exchange(m_state.hasTokenOnLine, false))
                return finishToken(TokenKind::EndOfFile, start);
            return finishToken(TokenKind::EndOfLine, start);
        }
        if (*byte != '\n') {
            m_state.hasTokenOnLine = true;
            return lexToken(*byte);
        }
        advanceLine();
        if (std::exchange(m_state.hasTokenOnLine, false))
            return finishToken(TokenKind::EndOfLine, start);
    }
}

std::optional<uint8_t> Lexer::peek(size_t distance) const
{
    size_t offset = m_state.offset + distance;
    if (offset >= m_text.size())
        return std::nullopt;
    return m_text[offset];
}

bool Lexer::isDigitAt(size_t distance) const
{
    auto byte = peek(distance);
    return byte && isASCIIDigit(*byte);
}

bool Lexer::isHexDigitAt(size_t distance) const
{
    auto byte = peek(distance);
    return byte && isASCIIHexDigit(*byte);
}

bool Lexer::isWordCharacterAt(size_t distance) const
{
    auto byte = peek(distance);
    return byte && isWordCharacter(*byte);
}

void Lexer::advance(size_t length)
{
    m_state.offset += length;
    m_state.position.column += static_cast<uint32_t>(length);
}

void Lexer::advanceLine()
{
    ++m_state.offset;
    ++m_state.position.line;
    m_state.position.column = 1;
}

Token Lexer::finishToken(TokenKind kind, const State& start) const
{
    Token token;
    token.kind = kind;
    token.position = start.position;
    token.text = m_text.subspan(start.offset, m_state.offset - start.offset);
    return token;
}

void Lexer::skipBlanksAndComments()
{
    while (auto byte = peek()) {
        if (*byte == ' ' || *byte == '\t' || *byte == '\r') {
            advance(1);
            continue;
        }
        if (*byte != '#' || peek(1) == '"')
            return;
        while (peek() && peek() != '\n')
            advance(1);
    }
}

Token Lexer::lexToken(uint8_t byte)
{
    if (isWordStart(byte))
        return lexWord();
    if (isASCIIDigit(byte) || (byte == '-' && isDigitAt(1)))
        return lexNumber();
    if (byte == '"')
        return lexString();
    State start = m_state;
    if (byte == '-' && startsWithWord(m_text.subspan(m_state.offset + 1), infinityWord)) {
        advance(1 + infinityWord.length());
        Token token = finishToken(TokenKind::Decimal, start);
        token.number = std::bit_cast<uint64_t>(-std::numeric_limits<double>::infinity());
        return token;
    }
    if (auto kind = twoBytePunctuatorKindOf(byte, peek(1))) {
        advance(2);
        return finishToken(*kind, start);
    }
    advance(1);
    return finishToken(punctuatorKindOf(byte), start);
}

Token Lexer::lexWord()
{
    State start = m_state;
    size_t length = 1;
    while (isWordCharacterAt(length))
        ++length;
    advance(length);
    Token token = finishToken(TokenKind::Word, start);
    classifyID(token);
    return token;
}

void Lexer::classifyID(Token& token)
{
    auto prefix = idPrefixOf(token.text[0]);
    if (!prefix || token.text.size() == 1)
        return;
    uint64_t number = 0;
    for (uint8_t byte : token.text.subspan(1)) {
        if (!isASCIIDigit(byte))
            return;
        if (number < UINT32_MAX)
            number = number * 10 + (byte - '0');
    }
    if (number >= UINT32_MAX) {
        token.kind = TokenKind::Invalid;
        return;
    }
    token.kind = TokenKind::ID;
    token.prefix = *prefix;
    token.number = number;
}

Token Lexer::lexNumber()
{
    State start = m_state;
    bool isNegative = peek() == '-';
    size_t length = isNegative ? 1 : 0;
    CheckedInt64 integer = 0;
    for (; isDigitAt(length); ++length) {
        int64_t digit = *peek(length) - '0';
        integer *= 10;
        integer += isNegative ? -digit : digit;
    }
    size_t integerLength = length;
    length += fractionLength(m_text.subspan(m_state.offset + length));
    length += exponentLength(m_text.subspan(m_state.offset + length));
    bool isFollowedByWordCharacter = isWordCharacterAt(length);
    advance(length);
    if (isFollowedByWordCharacter)
        return finishToken(TokenKind::Invalid, start);

    bool isNegativeZero = isNegative && !integer.hasOverflowed() && !integer.value();
    if (length == integerLength && !integer.hasOverflowed() && !isNegativeZero) {
        Token token = finishToken(TokenKind::Integer, start);
        token.number = std::bit_cast<uint64_t>(integer.value());
        return token;
    }

    Token token = finishToken(TokenKind::Decimal, start);
    size_t parsedLength = 0;
    token.number = std::bit_cast<uint64_t>(WTF::parseDouble(byteCast<Latin1Character>(token.text), parsedLength));
    if (parsedLength != token.text.size())
        token.kind = TokenKind::Invalid;
    return token;
}

Token Lexer::lexString()
{
    State start = m_state;
    advance(1);
    while (auto byte = peek()) {
        if (*byte == '"') {
            Token token = finishToken(TokenKind::String, start);
            token.text = token.text.subspan(1);
            advance(1);
            return token;
        }
        if (*byte == '\\') {
            auto length = escapeLengthAt(0);
            if (!length)
                return finishInvalidString(start);
            advance(*length);
            continue;
        }
        if (*byte < 0x20)
            return finishInvalidString(start);
        advance(1);
    }
    return finishInvalidString(start);
}

std::optional<size_t> Lexer::escapeLengthAt(size_t distance) const
{
    auto escaped = peek(distance + 1);
    if (escaped == '"' || escaped == '\\' || escaped == 'n')
        return characterEscapeLength;
    if (escaped == 'x') {
        if (!isHexDigitAt(distance + 2) || !isHexDigitAt(distance + 3))
            return std::nullopt;
        return byteEscapeLength;
    }
    if (escaped != 'u' || peek(distance + 2) != '{')
        return std::nullopt;

    size_t firstDigit = distance + codePointEscapePrefixLength;
    size_t numberOfDigits = 0;
    char32_t codePoint = 0;
    for (; numberOfDigits < maxCodePointDigits && isHexDigitAt(firstDigit + numberOfDigits); ++numberOfDigits)
        codePoint = codePoint << 4 | toASCIIHexValue(*peek(firstDigit + numberOfDigits));
    if (!numberOfDigits || codePoint > UCHAR_MAX_VALUE || peek(firstDigit + numberOfDigits) != '}')
        return std::nullopt;
    return codePointEscapePrefixLength + numberOfDigits + 1;
}

Token Lexer::finishInvalidString(const State& start)
{
    TextPosition offendingPosition = m_state.position;
    if (peek() && peek() != '\n')
        advance(1);
    Token token = finishToken(TokenKind::Invalid, start);
    token.position = offendingPosition;
    return token;
}

static void appendCodePoint(Vector<uint8_t>& bytes, char32_t codePoint)
{
    std::array<uint8_t, U8_MAX_LENGTH> encoded;
    size_t length = 0;
    // U8_APPEND rejects surrogates, and a program string may hold a lone one.
    U8_APPEND_UNSAFE(encoded, length, codePoint);
    bytes.append(std::span<const uint8_t> { encoded }.first(length));
}

static size_t appendDecodedEscape(Vector<uint8_t>& bytes, std::span<const uint8_t> escape)
{
    switch (escape[1]) {
    case '"':
    case '\\':
        bytes.append(escape[1]);
        return characterEscapeLength;
    case 'n':
        bytes.append(static_cast<uint8_t>('\n'));
        return characterEscapeLength;
    case 'x':
        bytes.append(toASCIIHexValue(escape[2], escape[3]));
        return byteEscapeLength;
    case 'u': {
        size_t index = codePointEscapePrefixLength;
        char32_t codePoint = 0;
        for (; escape[index] != '}'; ++index)
            codePoint = codePoint << 4 | toASCIIHexValue(escape[index]);
        appendCodePoint(bytes, codePoint);
        return index + 1;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

Vector<uint8_t> decodeStringToken(const Token& token)
{
    RELEASE_ASSERT(token.kind == TokenKind::String);
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(token.text.size());
    for (size_t index = 0; index < token.text.size();) {
        uint8_t byte = token.text[index];
        if (byte != '\\') {
            bytes.append(byte);
            ++index;
            continue;
        }
        index += appendDecodedEscape(bytes, token.text.subspan(index));
    }
    return bytes;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
