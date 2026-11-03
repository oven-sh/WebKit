/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <wtf/Forward.h>

namespace JSC {

class Identifier;

namespace Python {

// Up to ErrorToken these are the numbers of Lib/token.py, which the tokenize module shows to programs.
enum class TokenKind : uint8_t {
    EndMarker = 0,
    Name,
    Number,
    String,
    Newline,
    Indent,
    Dedent,
    LPar,
    RPar,
    LSqb,
    RSqb,
    Colon,
    Comma,
    Semi,
    Plus,
    Minus,
    Star,
    Slash,
    VBar,
    Amper,
    Less,
    Greater,
    Equal,
    Dot,
    Percent,
    LBrace,
    RBrace,
    EqEqual,
    NotEqual,
    LessEqual,
    GreaterEqual,
    Tilde,
    Circumflex,
    LeftShift,
    RightShift,
    DoubleStar,
    PlusEqual,
    MinEqual,
    StarEqual,
    SlashEqual,
    PercentEqual,
    AmperEqual,
    VBarEqual,
    CircumflexEqual,
    LeftShiftEqual,
    RightShiftEqual,
    DoubleStarEqual,
    DoubleSlash,
    DoubleSlashEqual,
    At,
    AtEqual,
    RArrow,
    Ellipsis,
    ColonEqual,
    Exclamation,
    Op,
    TypeIgnore,
    TypeComment,
    SoftKeyword,
    FStringStart,
    FStringMiddle,
    FStringEnd,
    TStringStart,
    TStringMiddle,
    TStringEnd,
    Comment,
    NL,
    ErrorToken,

    // Keywords: a Name to the tokenize module.
    FirstKeyword,
    KwFalse = FirstKeyword,
    KwNone,
    KwTrue,
    KwAnd,
    KwAs,
    KwAssert,
    KwAsync,
    KwAwait,
    KwBreak,
    KwClass,
    KwContinue,
    KwDef,
    KwDel,
    KwElif,
    KwElse,
    KwExcept,
    KwFinally,
    KwFor,
    KwFrom,
    KwGlobal,
    KwIf,
    KwImport,
    KwIn,
    KwIs,
    KwLambda,
    KwNonlocal,
    KwNot,
    KwOr,
    KwPass,
    KwRaise,
    KwReturn,
    KwTry,
    KwWhile,
    KwWith,
    KwYield,
};

inline bool isKeyword(TokenKind kind) { return kind >= TokenKind::FirstKeyword; }

// Lines count from 1. Columns are bytes of UTF-8 from the start of the line, from 0, as in the ast module.
struct Position {
    int line { 0 };
    int column { 0 };

    friend bool operator==(const Position&, const Position&) = default;
};

struct Token {
    TokenKind kind { TokenKind::EndMarker };
    // An Indent, Dedent or EndMarker is at no column: CPython gives those -1, which shows in where errors point.
    bool hasColumns { true };
    uint8_t level { 0 }; // Brackets open after the token.
    uint32_t start { 0 }; // Offsets into the source text.
    uint32_t end { 0 };
    Position startPosition;
    Position endPosition;
    union {
        const Identifier* identifier { nullptr }; // Name, and keywords
        // The token that ends the expression of a replacement field ('}', ':' or '!') in a t-string or after '=':
        // where the text of the expression starts. 0: none.
        uint32_t expressionStart;
    };
};

} // namespace Python
} // namespace JSC
