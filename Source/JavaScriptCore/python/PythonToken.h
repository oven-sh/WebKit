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

#pragma once

#include "PythonSyntaxError.h"
#include "Identifier.h"
#include <wtf/text/WTFString.h>

namespace JSC { namespace Python {

#define FOR_EACH_PYTHON_OPERATOR_TOKEN(macro) \
    macro(LeftParenthesis, "(") \
    macro(RightParenthesis, ")") \
    macro(LeftBracket, "[") \
    macro(RightBracket, "]") \
    macro(LeftBrace, "{") \
    macro(RightBrace, "}") \
    macro(Colon, ":") \
    macro(Comma, ",") \
    macro(Semicolon, ";") \
    macro(Plus, "+") \
    macro(Minus, "-") \
    macro(Star, "*") \
    macro(Slash, "/") \
    macro(VerticalBar, "|") \
    macro(Ampersand, "&") \
    macro(Less, "<") \
    macro(Greater, ">") \
    macro(Equal, "=") \
    macro(Dot, ".") \
    macro(Percent, "%") \
    macro(EqualEqual, "==") \
    macro(NotEqual, "!=") \
    macro(LessEqual, "<=") \
    macro(GreaterEqual, ">=") \
    macro(Tilde, "~") \
    macro(Circumflex, "^") \
    macro(LeftShift, "<<") \
    macro(RightShift, ">>") \
    macro(DoubleStar, "**") \
    macro(PlusEqual, "+=") \
    macro(MinusEqual, "-=") \
    macro(StarEqual, "*=") \
    macro(SlashEqual, "/=") \
    macro(PercentEqual, "%=") \
    macro(AmpersandEqual, "&=") \
    macro(VerticalBarEqual, "|=") \
    macro(CircumflexEqual, "^=") \
    macro(LeftShiftEqual, "<<=") \
    macro(RightShiftEqual, ">>=") \
    macro(DoubleStarEqual, "**=") \
    macro(DoubleSlash, "//") \
    macro(DoubleSlashEqual, "//=") \
    macro(At, "@") \
    macro(AtEqual, "@=") \
    macro(Arrow, "->") \
    macro(Ellipsis, "...") \
    macro(ColonEqual, ":=") \
    macro(Exclamation, "!")

#define FOR_EACH_PYTHON_KEYWORD(macro) \
    macro(False, "False") \
    macro(None, "None") \
    macro(True, "True") \
    macro(And, "and") \
    macro(As, "as") \
    macro(Assert, "assert") \
    macro(Async, "async") \
    macro(Await, "await") \
    macro(Break, "break") \
    macro(Class, "class") \
    macro(Continue, "continue") \
    macro(Def, "def") \
    macro(Del, "del") \
    macro(Elif, "elif") \
    macro(Else, "else") \
    macro(Except, "except") \
    macro(Finally, "finally") \
    macro(For, "for") \
    macro(From, "from") \
    macro(Global, "global") \
    macro(If, "if") \
    macro(Import, "import") \
    macro(In, "in") \
    macro(Is, "is") \
    macro(Lambda, "lambda") \
    macro(Nonlocal, "nonlocal") \
    macro(Not, "not") \
    macro(Or, "or") \
    macro(Pass, "pass") \
    macro(Raise, "raise") \
    macro(Return, "return") \
    macro(Try, "try") \
    macro(While, "while") \
    macro(With, "with") \
    macro(Yield, "yield")

enum class TokenKind : uint8_t {
    EndMarker,
    Error, // Where the scanner could go no further.
    Newline,
    Indent,
    Dedent,
    Name,
    Number,
    String,
    // f"..." and t"..." come in pieces: the opening quotes, then text and the tokens of what is between braces, then the closing quotes.
    FStringStart,
    FStringMiddle,
    FStringEnd,
    TStringStart,
    TStringMiddle,
    TStringEnd,
#define DECLARE(name, text) name,
    FOR_EACH_PYTHON_OPERATOR_TOKEN(DECLARE)
#undef DECLARE
#define DECLARE(name, text) Keyword##name,
    FOR_EACH_PYTHON_KEYWORD(DECLARE)
#undef DECLARE
};

// Names that are keywords only where the grammar says so.
enum class SoftKeyword : uint8_t {
    None,
    Match,
    Case,
    Type,
    Underscore,
};

enum class NumberKind : uint8_t {
    Integer, // It is in Token::integer.
    BigInteger, // Its digits are in Token::text, in Token::radix.
    Float,
    Imaginary,
};

struct Token {
    TokenKind kind { TokenKind::EndMarker };
    SoftKeyword softKeyword { SoftKeyword::None };
    NumberKind numberKind { NumberKind::Integer };
    uint8_t radix { 10 };
    bool isBytes : 1 { false }; // b"..."
    bool hasUnicodePrefix : 1 { false }; // u"..."
    bool isLessGreater : 1 { false }; // NotEqual, spelled <>

    // In code units of the source.
    unsigned start { 0 };
    unsigned end { 0 };
    // Lines from 1. Columns from 0 and in bytes of UTF-8, which is how Python counts them.
    unsigned line { 0 };
    unsigned column { 0 };
    unsigned endLine { 0 };
    unsigned endColumn { 0 };

    // Name: the name, normalized. String and the middles: the value, with escapes undone (of bytes, a character for each).
    const Identifier* text { nullptr };
    // On the token that ends the expression between braces: the source of that expression, for f"{x=}" and for t"{x}".
    const Identifier* expressionSource { nullptr };
    union {
        uint64_t integer { 0 };
        double real;
    };
};

ASCIILiteral NODELETE tokenKindName(TokenKind);

// SyntaxWarning, which is for whoever asked for the compilation to issue.
struct SyntaxWarning {
    String message;
    // What to say if the program has asked for such warnings to be errors, in which case it is a SyntaxError. Null if it is the same.
    String errorMessage;
    // Where that would be, as in SyntaxError.
    unsigned line { 0 };
    unsigned column { 0 };
    unsigned endLine { 0 };
    unsigned endColumn { 0 };
    bool isFoundInParsing { false };
    bool isFromTokenizer { false };
};

} } // namespace JSC::Python
