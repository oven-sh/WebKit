/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSchemaEnums.h"
#include <bit>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

struct TextPosition {
    uint32_t line { 1 };
    uint32_t column { 1 };
};

enum class TokenKind : uint8_t {
    EndOfFile,
    EndOfLine,
    Word,
    ID,
    Integer,
    Decimal,
    String,
    Colon,
    Equal,
    Comma,
    Dot,
    LeftParenthesis,
    RightParenthesis,
    LeftBracket,
    RightBracket,
    LeftBrace,
    RightBrace,
    LeftAngle,
    RightAngle,
    Bar,
    Tilde,
    At,
    AtAt,
    NumberSign,
    Question,
    Minus,
    Arrow,
    DotDot,
    Slash,
    Invalid,
};

enum class IDPrefix : uint8_t { Value, Block, Function, Layout, Variable, Module, ModuleFailure, TypeSite, EvalScope, ExpectedResult, Data };

constexpr std::optional<IDPrefix> idPrefixOf(uint8_t byte)
{
    switch (byte) {
    case 'v':
        return IDPrefix::Value;
    case 'b':
        return IDPrefix::Block;
    case 'f':
        return IDPrefix::Function;
    case 'L':
        return IDPrefix::Layout;
    case 'g':
        return IDPrefix::Variable;
    case 'm':
        return IDPrefix::Module;
    case 'q':
        return IDPrefix::ModuleFailure;
    case 'w':
        return IDPrefix::TypeSite;
    case 'e':
        return IDPrefix::EvalScope;
    case 'x':
        return IDPrefix::ExpectedResult;
    case 'd':
        return IDPrefix::Data;
    default:
        return std::nullopt;
    }
}

constexpr char letterOf(IDPrefix prefix)
{
    switch (prefix) {
    case IDPrefix::Value:
        return 'v';
    case IDPrefix::Block:
        return 'b';
    case IDPrefix::Function:
        return 'f';
    case IDPrefix::Layout:
        return 'L';
    case IDPrefix::Variable:
        return 'g';
    case IDPrefix::Module:
        return 'm';
    case IDPrefix::ModuleFailure:
        return 'q';
    case IDPrefix::TypeSite:
        return 'w';
    case IDPrefix::EvalScope:
        return 'e';
    case IDPrefix::ExpectedResult:
        return 'x';
    case IDPrefix::Data:
        return 'd';
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static_assert(prefixLetterOf(ProgramList::Functions) == letterOf(IDPrefix::Function));
static_assert(prefixLetterOf(ProgramList::Layouts) == letterOf(IDPrefix::Layout));
static_assert(prefixLetterOf(ProgramList::Variables) == letterOf(IDPrefix::Variable));
static_assert(prefixLetterOf(ProgramList::Modules) == letterOf(IDPrefix::Module));
static_assert(prefixLetterOf(ProgramList::ModuleFailures) == letterOf(IDPrefix::ModuleFailure));
static_assert(prefixLetterOf(ProgramList::TypeSites) == letterOf(IDPrefix::TypeSite));
static_assert(prefixLetterOf(ProgramList::EvalScopes) == letterOf(IDPrefix::EvalScope));
static_assert(prefixLetterOf(ProgramList::ExpectedResults) == letterOf(IDPrefix::ExpectedResult));
static_assert(prefixLetterOf(ProgramList::Data) == letterOf(IDPrefix::Data));

struct Token {
    TokenKind kind { TokenKind::EndOfFile };
    IDPrefix prefix { IDPrefix::Value };
    TextPosition position;
    std::span<const uint8_t> text;
    uint64_t number { 0 };

    bool isWord(ASCIILiteral word) const { return kind == TokenKind::Word && equalSpans(text, word.span8()); }
    StringView view() const { return StringView(byteCast<Latin1Character>(text)); }
    int64_t integer() const { return std::bit_cast<int64_t>(number); }
    double decimal() const { return std::bit_cast<double>(number); }
};

class Lexer {
public:
    struct State {
        size_t offset { 0 };
        TextPosition position;
        bool hasTokenOnLine { false };
    };

    explicit Lexer(std::span<const uint8_t> text)
        : m_text(text)
    {
    }

    Token next();

    State state() const { return m_state; }
    void restore(State state) { m_state = state; }

private:
    std::optional<uint8_t> peek(size_t distance = 0) const;
    bool isDigitAt(size_t distance) const;
    bool isHexDigitAt(size_t distance) const;
    bool isWordCharacterAt(size_t distance) const;
    void advance(size_t length);
    void advanceLine();
    Token finishToken(TokenKind, const State& start) const;
    void skipBlanksAndComments();
    Token lexToken(uint8_t byte);
    Token lexWord();
    static void classifyID(Token&);
    Token lexNumber();
    Token lexString();
    std::optional<size_t> escapeLengthAt(size_t distance) const;
    Token finishInvalidString(const State& start);

    std::span<const uint8_t> m_text;
    State m_state;
};

Vector<uint8_t> decodeStringToken(const Token&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
