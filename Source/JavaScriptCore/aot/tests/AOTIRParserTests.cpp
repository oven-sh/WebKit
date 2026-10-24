/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptionWords.h"
#include "AOTIRLexer.h"
#include "AOTIRParser.h"
#include "AOTIRPrinter.h"
#include "AOTIRTestSupport.h"
#include "AOTReaderFixture.h"
#include "AOTVector.h"
#include "LineColumn.h"
#include <cmath>
#include <limits>
#include <memory>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ParseFailure {
    InternalError error;
    String message;
};

struct ExpectedFailure {
    ASCIILiteral text;
    InternalErrorKind kind;
    LineColumn position;
};

struct ExpectedToken {
    TokenKind kind;
    uint32_t line;
    uint32_t column;
};

constexpr ASCIILiteral moduleLine = "module m0 path=\"/a.js\" displayPath=\"a.js\"\n"_s;

std::span<const uint8_t> bytesOf(ASCIILiteral text)
{
    return byteCast<uint8_t>(text.span());
}

Vector<Token> lex(std::span<const uint8_t> text)
{
    Lexer lexer(text);
    Vector<Token> tokens;
    for (Token token = lexer.next(); token.kind != TokenKind::EndOfFile; token = lexer.next())
        tokens.append(token);
    return tokens;
}

Token lexOnlyToken(std::span<const uint8_t> text)
{
    Vector<Token> tokens = lex(text);
    AOT_TEST_CHECK(tokens.size() == 2 && tokens[1].kind == TokenKind::EndOfLine);
    return tokens[0];
}

Token lexOnlyToken(ASCIILiteral text)
{
    return lexOnlyToken(bytesOf(text));
}

Token lexFirstToken(ASCIILiteral text)
{
    return Lexer(bytesOf(text)).next();
}

Vector<TokenKind> kindsOnLine(ASCIILiteral line)
{
    Vector<Token> tokens = lex(bytesOf(line));
    AOT_TEST_CHECK(!tokens.isEmpty() && tokens.last().kind == TokenKind::EndOfLine);
    tokens.removeLast();
    return WTF::map(tokens, [](const Token& token) {
        return token.kind;
    });
}

bool isAt(const Token& token, uint32_t line, uint32_t column)
{
    return token.position.line == line && token.position.column == column;
}

bool hasText(const Token& token, ASCIILiteral text)
{
    return equalSpans(token.text, bytesOf(text));
}

ParseFailure parseInvalidText(StringView text, IRForm form = IRForm::AsRead)
{
    BuildContext build(makeDefaultCompileOptions());
    AOT_TEST_CHECK(!parseProgram(text, form, build));
    Diagnostics& diagnostics = build.diagnostics();
    diagnostics.finish(nullptr);
    AOT_TEST_CHECK(diagnostics.userDiagnostics().isEmpty() && diagnostics.internalErrors().size() == 1);
    return { diagnostics.internalErrors()[0], diagnostics.messageOfInternalError(0) };
}

void checkFailure(StringView text, InternalErrorKind kind, LineColumn position)
{
    InternalError error = parseInvalidText(text).error;
    AOT_TEST_CHECK(error.kind == kind);
    AOT_TEST_CHECK(error.textPosition == position);
    AOT_TEST_CHECK(!error.function && !error.block && !error.value && !error.layout && !error.count);
}

String printedTextOf(const Program& program)
{
    StringPrintStream out;
    printProgram(out, program);
    return out.toString();
}

} // anonymous namespace

static void lexerReportsPositions()
{
    static constexpr ExpectedToken expectedTokens[] = {
        { TokenKind::Word, 1, 1 }, { TokenKind::Word, 1, 3 }, { TokenKind::EndOfLine, 1, 4 },
        { TokenKind::Word, 2, 3 }, { TokenKind::EndOfLine, 2, 14 },
        { TokenKind::Word, 5, 1 }, { TokenKind::Word, 5, 5 }, { TokenKind::EndOfLine, 5, 6 },
    };
    Vector<Token> tokens = lex(bytesOf("a b\n  c # comment\n\n# only a comment\nd\t\r e"_s));
    AOT_TEST_CHECK(tokens.size() == std::size(expectedTokens));
    for (size_t index = 0; index < tokens.size(); ++index)
        AOT_TEST_CHECK(tokens[index].kind == expectedTokens[index].kind && isAt(tokens[index], expectedTokens[index].line, expectedTokens[index].column));
    AOT_TEST_CHECK(hasText(tokens[0], "a"_s) && hasText(tokens[3], "c"_s) && hasText(tokens[6], "e"_s));
}

static void lexerEndsTextOnce()
{
    Lexer lexer(bytesOf("a"_s));
    AOT_TEST_CHECK(lexer.next().kind == TokenKind::Word && lexer.next().kind == TokenKind::EndOfLine);
    for (unsigned repetition = 0; repetition < 2; ++repetition) {
        Token end = lexer.next();
        AOT_TEST_CHECK(end.kind == TokenKind::EndOfFile && isAt(end, 1, 2) && end.text.empty());
    }
    for (ASCIILiteral text : { ""_s, "\n\n"_s, "# a comment"_s, "  \n# a comment\n\t"_s })
        AOT_TEST_CHECK(lexFirstToken(text).kind == TokenKind::EndOfFile);
}

static void lexerRestoresState()
{
    Lexer lexer(bytesOf("a b\nc"_s));
    lexer.next();
    Lexer::State state = lexer.state();
    Token first = lexer.next();
    AOT_TEST_CHECK(lexer.next().kind == TokenKind::EndOfLine && hasText(lexer.next(), "c"_s));
    lexer.restore(state);
    Token again = lexer.next();
    AOT_TEST_CHECK(hasText(first, "b"_s) && hasText(again, "b"_s) && isAt(again, 1, 3) && lexer.next().kind == TokenKind::EndOfLine);
}

static void lexerClassifiesWordsAndIDs()
{
    static constexpr std::pair<ASCIILiteral, IDPrefix> ids[] = {
        { "v12"_s, IDPrefix::Value }, { "b12"_s, IDPrefix::Block }, { "f12"_s, IDPrefix::Function }, { "L12"_s, IDPrefix::Layout },
        { "g12"_s, IDPrefix::Variable }, { "m12"_s, IDPrefix::Module }, { "q12"_s, IDPrefix::ModuleFailure }, { "w12"_s, IDPrefix::TypeSite },
        { "e12"_s, IDPrefix::EvalScope }, { "x12"_s, IDPrefix::ExpectedResult }, { "d12"_s, IDPrefix::Data },
    };
    for (auto [text, prefix] : ids) {
        Token token = lexOnlyToken(text);
        AOT_TEST_CHECK(token.kind == TokenKind::ID && token.prefix == prefix && token.number == 12 && hasText(token, text));
        AOT_TEST_CHECK(letterOf(prefix) == text[0] && idPrefixOf(text[0]) == prefix);
    }
    for (ASCIILiteral word : { "v"_s, "L"_s, "v1a"_s, "v_1"_s, "V1"_s, "l1"_s, "a1"_s, "S1"_s, "_1"_s, "Return"_s, "NaN"_s, "Infinity"_s, "is_1x"_s }) {
        Token token = lexOnlyToken(word);
        AOT_TEST_CHECK(token.kind == TokenKind::Word && token.isWord(word) && token.view() == word);
    }
    AOT_TEST_CHECK(!lexOnlyToken("Return"_s).isWord("Retur"_s) && !lexOnlyToken("v1"_s).isWord("v1"_s));
    AOT_TEST_CHECK(lexOnlyToken("v0"_s).kind == TokenKind::ID && !lexOnlyToken("v0"_s).number);
    AOT_TEST_CHECK(lexOnlyToken("v4294967294"_s).kind == TokenKind::ID && lexOnlyToken("v4294967294"_s).number == UINT32_MAX - 1);
    for (ASCIILiteral tooLarge : { "v4294967295"_s, "v4294967296"_s, "L99999999999999999999999"_s })
        AOT_TEST_CHECK(lexOnlyToken(tooLarge).kind == TokenKind::Invalid);
}

static void lexerIntegers()
{
    static constexpr std::pair<ASCIILiteral, int64_t> integers[] = {
        { "0"_s, 0 }, { "7"_s, 7 }, { "-5"_s, -5 }, { "007"_s, 7 },
        { "9223372036854775807"_s, std::numeric_limits<int64_t>::max() }, { "-9223372036854775808"_s, std::numeric_limits<int64_t>::min() },
    };
    for (auto [text, value] : integers) {
        Token token = lexOnlyToken(text);
        AOT_TEST_CHECK(token.kind == TokenKind::Integer && token.integer() == value && hasText(token, text));
    }
    AOT_TEST_CHECK(hasElements(kindsOnLine("1."_s), { TokenKind::Integer, TokenKind::Dot }));
    AOT_TEST_CHECK(hasElements(kindsOnLine("1..2"_s), { TokenKind::Integer, TokenKind::DotDot, TokenKind::Integer }));
    AOT_TEST_CHECK(hasElements(kindsOnLine("1,-2"_s), { TokenKind::Integer, TokenKind::Comma, TokenKind::Integer }));
}

static void lexerDecimals()
{
    static constexpr std::pair<ASCIILiteral, double> decimals[] = {
        { "1.5"_s, 1.5 }, { "1e21"_s, 1e21 }, { "1e+21"_s, 1e21 }, { "1E3"_s, 1000 }, { "-2.5e-7"_s, -2.5e-7 }, { "0.0"_s, 0 },
        { "9223372036854775808"_s, 0x1p63 }, { "-9223372036854775809"_s, -0x1p63 }, { "-Infinity"_s, -std::numeric_limits<double>::infinity() },
    };
    for (auto [text, value] : decimals) {
        Token token = lexOnlyToken(text);
        AOT_TEST_CHECK(token.kind == TokenKind::Decimal && token.decimal() == value && hasText(token, text));
    }
    Token negativeZero = lexOnlyToken("-0"_s);
    AOT_TEST_CHECK(negativeZero.kind == TokenKind::Decimal && !negativeZero.decimal() && std::signbit(negativeZero.decimal()));
    AOT_TEST_CHECK(!std::signbit(lexOnlyToken("0.0"_s).decimal()));
    AOT_TEST_CHECK(hasElements(kindsOnLine("-Infinityx"_s), { TokenKind::Minus, TokenKind::Word }));
}

static void lexerRejectsNumberFollowedByWordByte()
{
    for (ASCIILiteral text : { "12ab"_s, "12_"_s, "1e"_s, "1e+"_s, "1.5x"_s, "1e5x"_s, "-3v0"_s }) {
        Token token = lexFirstToken(text);
        AOT_TEST_CHECK(token.kind == TokenKind::Invalid && isAt(token, 1, 1));
    }
}

static void lexerPunctuators()
{
    AOT_TEST_CHECK(hasElements(kindsOnLine(": = , . ( ) [ ] { } < > | ~ @ - /"_s), {
        TokenKind::Colon, TokenKind::Equal, TokenKind::Comma, TokenKind::Dot, TokenKind::LeftParenthesis, TokenKind::RightParenthesis,
        TokenKind::LeftBracket, TokenKind::RightBracket, TokenKind::LeftBrace, TokenKind::RightBrace, TokenKind::LeftAngle, TokenKind::RightAngle,
        TokenKind::Bar, TokenKind::Tilde, TokenKind::At, TokenKind::Minus, TokenKind::Slash,
    }));
    AOT_TEST_CHECK(hasElements(kindsOnLine("-> - > -x --1"_s), { TokenKind::Arrow, TokenKind::Minus, TokenKind::RightAngle, TokenKind::Minus, TokenKind::Word, TokenKind::Minus, TokenKind::Integer }));
    AOT_TEST_CHECK(hasElements(kindsOnLine("@@iterator @@@ @ @"_s), { TokenKind::AtAt, TokenKind::Word, TokenKind::AtAt, TokenKind::At, TokenKind::At, TokenKind::At }));
    AOT_TEST_CHECK(hasElements(kindsOnLine(".. ... . ."_s), { TokenKind::DotDot, TokenKind::DotDot, TokenKind::Dot, TokenKind::Dot, TokenKind::Dot }));
    AOT_TEST_CHECK(hasText(lexOnlyToken("->"_s), "->"_s) && hasText(lexOnlyToken("@@"_s), "@@"_s) && hasText(lexOnlyToken(".."_s), ".."_s));
}

static void lexerNumberSignBeforeQuoteIsAToken()
{
    Vector<Token> tokens = lex(bytesOf("#\"x\"@L2"_s));
    AOT_TEST_CHECK(tokens.size() == 5 && tokens[0].kind == TokenKind::NumberSign && tokens[1].kind == TokenKind::String && hasText(tokens[1], "x"_s));
    AOT_TEST_CHECK(tokens[2].kind == TokenKind::At && tokens[3].kind == TokenKind::ID && tokens[3].prefix == IDPrefix::Layout);
    AOT_TEST_CHECK(hasElements(kindsOnLine("a # \"x\""_s), { TokenKind::Word }));
    AOT_TEST_CHECK(hasElements(kindsOnLine("a #x"_s), { TokenKind::Word }) && hasElements(kindsOnLine("a ##\"x\""_s), { TokenKind::Word }));
}

static void lexerRejectsOtherBytes()
{
    static constexpr uint8_t bytes[] = { '$', '!', '^', '%', '&', '*', '+', ';', '\'', '`', '\\', 0x00, 0x01, 0x7f, 0x80, 0xc3, 0xff };
    for (const uint8_t& byte : bytes) {
        Token token = lexOnlyToken(singleElementSpan(byte));
        AOT_TEST_CHECK(token.kind == TokenKind::Invalid && isAt(token, 1, 1) && token.text.size() == 1);
    }
}

static void lexerKeepsEscapedQuoteInsideString()
{
    Vector<Token> tokens = lex(bytesOf(R"(x="a\"b" "")"_s));
    AOT_TEST_CHECK(tokens.size() == 5 && tokens[2].kind == TokenKind::String && isAt(tokens[2], 1, 3) && hasText(tokens[2], R"(a\"b)"_s));
    AOT_TEST_CHECK(equalSpans(decodeStringToken(tokens[2]).span(), bytesOf("a\"b"_s)));
    AOT_TEST_CHECK(tokens[3].kind == TokenKind::String && isAt(tokens[3], 1, 10) && tokens[3].text.empty() && decodeStringToken(tokens[3]).isEmpty());
}

static void lexerDecodesEveryEscape()
{
    static constexpr uint8_t expectedBytes[] = { '"', '\\', '\n', 'A', 0x80, 0xc3, 0xa9, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80, 0xf4, 0x8f, 0xbf, 0xbf, 0x00, '#' };
    Token token = lexOnlyToken(R"("\"\\\n\x41\x80\u{e9}\u{20AC}\u{1f600}\u{10FFFF}\u{0}#")"_s);
    AOT_TEST_CHECK(token.kind == TokenKind::String);
    AOT_TEST_CHECK(equalSpans(decodeStringToken(token).span(), std::span<const uint8_t> { expectedBytes }));
}

static void lexerEncodesSurrogateEscapeAsThreeBytes()
{
    static constexpr uint8_t expectedBytes[] = { 0xed, 0xa0, 0x80, 0xed, 0xbf, 0xbf };
    Token token = lexOnlyToken(R"("\u{d800}\u{dfff}")"_s);
    AOT_TEST_CHECK(token.kind == TokenKind::String && equalSpans(decodeStringToken(token).span(), std::span<const uint8_t> { expectedBytes }));
}

static void lexerCopiesBytesAboveASCII()
{
    static constexpr uint8_t text[] = { '"', 0xc3, 0xa9, 0xff, '"' };
    Token token = lexOnlyToken(std::span<const uint8_t> { text });
    AOT_TEST_CHECK(token.kind == TokenKind::String && equalSpans(decodeStringToken(token).span(), std::span<const uint8_t> { text }.subspan(1, 3)));
}

static void lexerReportsOffendingByteOfString()
{
    static constexpr std::pair<ASCIILiteral, uint32_t> strings[] = {
        { R"("\q")"_s, 2 }, { R"("ab\t")"_s, 4 }, { R"("\u{}")"_s, 2 }, { R"("\u{110000}")"_s, 2 }, { R"("\u{1234567}")"_s, 2 }, { R"("\u{12")"_s, 2 },
        { R"("\u12")"_s, 2 }, { R"("\x4")"_s, 2 }, { R"("\x4g")"_s, 2 }, { R"("ab)"_s, 4 }, { R"("ab\)"_s, 4 }, { "\"ab\ncd\""_s, 4 }, { "\"a\tb\""_s, 3 },
    };
    for (auto [text, column] : strings) {
        Token token = lexFirstToken(text);
        AOT_TEST_CHECK(token.kind == TokenKind::Invalid && isAt(token, 1, column));
    }
}

static void parserReportsPositionInRecordLines()
{
    static constexpr ExpectedFailure cases[] = {
        { "bogus g0\n"_s, InternalErrorKind::IRTextError, { 1, 1 } },
        { "variable g1\n"_s, InternalErrorKind::IRTextError, { 1, 10 } },
        { "variable f0\n"_s, InternalErrorKind::IRTextError, { 1, 10 } },
        { "variable\n"_s, InternalErrorKind::IRTextError, { 1, 9 } },
        { "layout L0\nlayout L0\n"_s, InternalErrorKind::IRTextError, { 2, 8 } },
        { "layout L0\n\n# a comment\nlayout L2\n"_s, InternalErrorKind::IRTextError, { 4, 8 } },
        { "variable g0 declared=\n"_s, InternalErrorKind::IRTextError, { 1, 22 } },
        { "variable g0 declared={type=anything\n"_s, InternalErrorKind::IRTextError, { 1, 36 } },
        { "variable g0 declared={type=anything expected=number}\n"_s, InternalErrorKind::IRTextError, { 1, 37 } },
        { "variable g0 bogus=1\n"_s, InternalErrorKind::IRTextError, { 1, 13 } },
        { "variable g0 name=\"a\" name=\"b\"\n"_s, InternalErrorKind::IRTextError, { 1, 22 } },
        { "variable g0 declared={type=anything} name=\"g\"\n"_s, InternalErrorKind::IRTextError, { 1, 38 } },
        { "variable g0 name=g\n"_s, InternalErrorKind::IRTextError, { 1, 18 } },
        { "variable g0 module=f0\n"_s, InternalErrorKind::IRTextError, { 1, 20 } },
        { "variable g0 initialization=Bogus\n"_s, InternalErrorKind::IRTextError, { 1, 28 } },
        { "variable g0 flags=[Bogus]\n"_s, InternalErrorKind::IRTextError, { 1, 20 } },
        { "variable g0 flags=[IsImmutable IsImmutable]\n"_s, InternalErrorKind::IRTextError, { 1, 32 } },
        { "variable g0 .\n"_s, InternalErrorKind::IRTextError, { 1, 13 } },
        { "function f0 position=-1\n"_s, InternalErrorKind::IRTextError, { 1, 22 } },
        { "function f0 position=4294967296\n"_s, InternalErrorKind::IRTextError, { 1, 22 } },
        { "function f0 text={start=3.9}\n"_s, InternalErrorKind::IRTextError, { 1, 25 } },
    };
    for (const ExpectedFailure& expected : cases)
        checkFailure(expected.text, expected.kind, expected.position);
}

static void parserReportsPositionOfInvalidTokens()
{
    static constexpr ExpectedFailure cases[] = {
        { "variable g0 name=$\n"_s, InternalErrorKind::IRTextError, { 1, 18 } },
        { "variable g0 name=\"abc\nvariable g1\n"_s, InternalErrorKind::IRTextError, { 1, 22 } },
        { "variable g0 name=\"a\\q\"\n"_s, InternalErrorKind::IRTextError, { 1, 20 } },
        { "variable g0 name=\"ab"_s, InternalErrorKind::IRTextError, { 1, 21 } },
        { "variable g0\nvariable g4294967295\n"_s, InternalErrorKind::IRTextError, { 2, 10 } },
        { "function f0 length=12ab\n"_s, InternalErrorKind::IRTextError, { 1, 20 } },
    };
    for (const ExpectedFailure& expected : cases)
        checkFailure(expected.text, expected.kind, expected.position);

    static constexpr char textWithNull[] = "variable g0\0 name=\"a\"\n";
    checkFailure(StringView(byteCast<Latin1Character>(std::span { textWithNull }.first(sizeof(textWithNull) - 1))), InternalErrorKind::IRTextError, { 1, 12 });
}

static void parserReportsPositionInTypes()
{
    static constexpr ExpectedFailure cases[] = {
        { "variable g0 declared={type=never|null}\n"_s, InternalErrorKind::IRTextError, { 1, 33 } },
        { "variable g0 declared={type=number|}\n"_s, InternalErrorKind::IRTextError, { 1, 35 } },
        { "variable g0 declared={type=Bogus}\n"_s, InternalErrorKind::IRTextError, { 1, 28 } },
        { "variable g0 declared={type=Map<string>}\n"_s, InternalErrorKind::IRTextError, { 1, 31 } },
        { "variable g0 declared={type=Array<number>|Array<string>}\n"_s, InternalErrorKind::IRTextError, { 1, 47 } },
        { "variable g0 declared={type=int[1, x]}\n"_s, InternalErrorKind::IRTextError, { 1, 35 } },
        { "variable g0 declared={type=env L0|number}\n"_s, InternalErrorKind::IRTextError, { 1, 28 } },
        { "variable g0 declared={type=builtin(Bogus)}\n"_s, InternalErrorKind::IRTextError, { 1, 36 } },
        { "variable g0 declared={type=f0}\n"_s, InternalErrorKind::IdOutOfRange, { 1, 28 } },
    };
    for (const ExpectedFailure& expected : cases)
        checkFailure(expected.text, expected.kind, expected.position);
}

static void parserLimitsNestingOfTypes()
{
    constexpr unsigned maxNesting = 32;
    auto createText = [](unsigned nesting) {
        StringBuilder text;
        text.append(moduleLine, "variable g0 declared={type="_s);
        for (unsigned level = 1; level < nesting; ++level)
            text.append("Array<"_s);
        text.append("number"_s);
        for (unsigned level = 1; level < nesting; ++level)
            text.append('>');
        text.append("}\n"_s);
        return text.toString();
    };
    BuildContext build(makeDefaultCompileOptions());
    AOT_TEST_CHECK(parseProgram(createText(maxNesting), IRForm::AsRead, build));
    checkFailure(createText(maxNesting + 1), InternalErrorKind::IRTextError, { 2, 28 + 6 * maxNesting });
}

static void parserReportsPositionInBodies()
{
    static constexpr ExpectedFailure lines[] = {
        { "v1 = Undefined $"_s, InternalErrorKind::IRTextError, { 3, 20 } },
        { "v1 = String string=\"\\q\""_s, InternalErrorKind::IRTextError, { 3, 25 } },
        { "v1 = String string=\"a"_s, InternalErrorKind::IRTextError, { 3, 26 } },
        { "v1 = Undefinedx"_s, InternalErrorKind::IRTextError, { 3, 10 } },
        { "v1 = PropGet v0 nam=\"p\""_s, InternalErrorKind::IRTextError, { 3, 21 } },
        { "v1 = PropGet v0 key=\"p\" key=\"p\""_s, InternalErrorKind::IRTextError, { 3, 29 } },
        { "v1 = Call v0, - [isFast]"_s, InternalErrorKind::IRTextError, { 3, 22 } },
        { "v1 = Call v0, - [isAnnotatedPure, isAnnotatedPure]"_s, InternalErrorKind::IRTextError, { 3, 39 } },
        { "v1 = Undefined [isFast]"_s, InternalErrorKind::IRTextError, { 3, 20 } },
        { "v0 = Undefined"_s, InternalErrorKind::IRTextError, { 3, 5 } },
        { "v100000 = Undefined"_s, InternalErrorKind::IRTextError, { 3, 5 } },
        { "v1 Undefined"_s, InternalErrorKind::IRTextError, { 3, 8 } },
        { "v1 = Integer integer=9223372036854775808"_s, InternalErrorKind::IRTextError, { 3, 26 } },
        { "v1 = Undefined v0"_s, InternalErrorKind::IRTextError, { 3, 22 } },
        { "v1 = Negate"_s, InternalErrorKind::IRTextError, { 3, 16 } },
        { "v1 = Negate v0, v0"_s, InternalErrorKind::IRTextError, { 3, 23 } },
        { "v1 = Negate v9"_s, InternalErrorKind::IdOutOfRange, { 3, 17 } },
        { "Undefined"_s, InternalErrorKind::IRTextError, { 3, 5 } },
        { "v1 = LoopHint"_s, InternalErrorKind::IRTextError, { 3, 10 } },
        { "v1 = Undefined @4294967295"_s, InternalErrorKind::IRTextError, { 3, 21 } },
        { "v1: Bogus = Undefined"_s, InternalErrorKind::IRTextError, { 3, 9 } },
        { "v1: number ~ Bogus = Undefined"_s, InternalErrorKind::IRTextError, { 3, 18 } },
        { "v1: number ~ int ~ i32 = Undefined"_s, InternalErrorKind::IRTextError, { 3, 22 } },
        { "v1 @Bogus = Undefined"_s, InternalErrorKind::IRTextError, { 3, 9 } },
    };
    for (const ExpectedFailure& expected : lines) {
        String text = makeString("function f0 parameters=[{type=anything}] results=[{type=anything}] {\n  b0(v0: anything):\n    "_s, expected.text, "\n    Return\n}\n"_s);
        checkFailure(text, expected.kind, expected.position);
    }
}

static void parserReportsPositionInBlocksAndTerminators()
{
    static constexpr ExpectedFailure cases[] = {
        { "function f0 {\n}\n"_s, InternalErrorKind::IRTextError, { 2, 1 } },
        { "function f0 {\n  b0():\n    Return\n"_s, InternalErrorKind::IRTextError, { 4, 1 } },
        { "function f0 {\n  b0():\n    Return\n} x\n"_s, InternalErrorKind::IRTextError, { 4, 3 } },
        { "function f0 {\n  b1():\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 2, 3 } },
        { "function f0 {\n  b0()\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 2, 7 } },
        { "function f0 {\n  b0(v1):\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 2, 6 } },
        { "function f0 {\n  b0() role=Bogus:\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 2, 13 } },
        { "function f0 {\n  b0() frequency=Rare frequency=Rare:\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 2, 23 } },
        { "function f0 {\n  b0() bogus=1:\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 2, 8 } },
        { "function f0 {\n  b0():\n    Jump -> taken b0()\n}\n"_s, InternalErrorKind::IRTextError, { 3, 13 } },
        { "function f0 {\n  b0():\n    Jump -> next b0(), next b0()\n}\n"_s, InternalErrorKind::IRTextError, { 3, 24 } },
        { "function f0 {\n  b0():\n    Jump -> next b7()\n}\n"_s, InternalErrorKind::IdOutOfRange, { 3, 18 } },
        { "function f0 {\n  b0():\n    Jump -> next b0(v3)\n}\n"_s, InternalErrorKind::IdOutOfRange, { 3, 21 } },
        { "function f0 {\n  b0():\n    Jump -> next b0(| x)\n}\n"_s, InternalErrorKind::IRTextError, { 3, 23 } },
        { "function f0 {\n  b0():\n    Return v9\n}\n"_s, InternalErrorKind::IdOutOfRange, { 3, 12 } },
        { "function f0 {\n  b0():\n    Return\n    Return\n}\n"_s, InternalErrorKind::IRTextError, { 4, 5 } },
    };
    for (const ExpectedFailure& expected : cases)
        checkFailure(expected.text, expected.kind, expected.position);
}

static void parserPutsTextPositionInMessage()
{
    AOT_TEST_CHECK(parseInvalidText("variable g0 declared=\n"_s).message == "the IR text does not parse; line 1, column 22 of the IR text"_s);
    AOT_TEST_CHECK(parseInvalidText("function f0 {\n  b0():\n    Return v9\n}\n"_s).message == "an id is out of range, in record 0; line 3, column 12 of the IR text"_s);
}

static void parserLeavesIDsOfListsToTheReader()
{
    InternalError error = parseInvalidText(makeString(moduleLine, "variable g0 declared={type=anything}\nvariable g1 declared={type=anything} function=f7\n"_s)).error;
    AOT_TEST_CHECK(error.kind == InternalErrorKind::IdOutOfRange && error.count == 1 && !error.textPosition);
    error = parseInvalidText("variable g0 declared={type=anything}\n"_s).error;
    AOT_TEST_CHECK(error.kind == InternalErrorKind::IdOutOfRange && !error.count && !error.textPosition);
}

static void parserGivesNoPositionToErrorsOfTheReader()
{
    static constexpr std::pair<ASCIILiteral, InternalErrorKind> cases[] = {
        { "layout L0 supertypes=[L1, L1]\nlayout L1\n"_s, InternalErrorKind::DuplicateListEntry },
        { "layout L0 supertypes=[L2, L1]\nlayout L1\nlayout L2\n"_s, InternalErrorKind::UnsortedList },
        { "variable g0 declared={type=anything}\nmodule m1 path=\"/b.js\" displayPath=\"b.js\" exports=[{name=\"b\" variable=g0}, {name=\"a\" variable=g0}]\n"_s, InternalErrorKind::UnsortedList },
        { "variable g0 declared={type=anything}\nmodule m1 path=\"/b.js\" displayPath=\"b.js\" exports=[{name=\"a\" variable=g0}, {name=\"a\" variable=g0}]\n"_s, InternalErrorKind::DuplicateListEntry },
        { "function f0\n"_s, InternalErrorKind::ListOutOfBounds },
        { "function f0 results=[{type=anything}] {\n  b0():\n    v0 = GetSlot v0\n    Return\n}\n"_s, InternalErrorKind::EngineOnlyOnWire },
    };
    for (auto [text, kind] : cases) {
        InternalError error = parseInvalidText(makeString(moduleLine, text)).error;
        AOT_TEST_CHECK(error.kind == kind && !error.textPosition);
    }
}

static void parserLeavesCaptureLimitToTheReader()
{
    BuildContext build(makeDefaultCompileOptions());
    auto createText = [](uint64_t numberOfCaptures) {
        StringBuilder text;
        text.append(moduleLine, "function f0 captures=["_s);
        for (uint64_t index = 0; index < numberOfCaptures; ++index)
            text.append(index ? ", "_s : ""_s, "{type=anything}"_s);
        text.append("] results=[{type=anything}]\n"_s);
        return text.toString();
    };
    uint64_t maxCaptures = build.limit(LimitID::MaxCaptures);
    auto program = parseProgram(createText(maxCaptures), IRForm::AsRead, build);
    AOT_TEST_CHECK(program && program->functions[0].captures.size() == maxCaptures);
    InternalError error = parseInvalidText(createText(maxCaptures + 1)).error;
    AOT_TEST_CHECK(error.kind == InternalErrorKind::ListOutOfBounds && error.function == FunctionID(0u) && !error.textPosition);
}

static void parserSurvivesDeepData()
{
    constexpr unsigned depth = 200000;
    StringBuilder text;
    text.append("data d0 root="_s);
    for (unsigned level = 0; level < depth; ++level)
        text.append("Array{elements=["_s);
    InternalError error = parseInvalidText(text.toString()).error;
    AOT_TEST_CHECK(error.kind == InternalErrorKind::IRTextError && error.textPosition && error.textPosition->line == 1);
}

static void parserOwnsFunctionText()
{
    static constexpr uint8_t expectedText[] = { 'a', ' ', '=', '>', ' ', '"', 0xc3, 0xa9, '"', '\n' };
    BuildContext build(makeDefaultCompileOptions());
    auto program = parseProgram(String(R"IR(module m0 path="/a.js" displayPath="a.js" functionText="a => \x22\xc3\xa9\x22\x0a"
module m1 path="/b.js" displayPath="b.js"
entry module=m0
function f0 name="a" module=m0 text={end=9} results=[{type=undefined}]
function f1 name="pastTheText" module=m1 text={start=4 end=5} results=[{type=undefined}]
function f2 name="withoutText" module=m0 results=[{type=undefined}]
)IR"_s), IRForm::AsRead, build);
    AOT_TEST_CHECK(program);
    std::span<const uint8_t> functionText = program->modules[0].functionText;
    std::span<const uint8_t> buffer = program->buffer();
    AOT_TEST_CHECK(equalSpans(functionText, std::span<const uint8_t> { expectedText }));
    AOT_TEST_CHECK(functionText.data() >= buffer.data() && functionText.data() + functionText.size() <= buffer.data() + buffer.size());
    AOT_TEST_CHECK(program->modules[1].functionText.empty());
    AOT_TEST_CHECK(!program->functions[0].text.start && program->functions[0].text.end == 9);
    AOT_TEST_CHECK(program->functions[1].text.start == 4 && program->functions[1].text.end == 5);
    AOT_TEST_CHECK(!program->functions[2].text.start && !program->functions[2].text.end);
    AOT_TEST_CHECK(program->form() == IRForm::AsRead && program->entries.size() == 1 && program->entries[0].module == ModuleID(0u));
}

static void parserRejectsCharacterEscapesInBytes()
{
    checkFailure("module m0 functionText=\"a\\n\"\n"_s, InternalErrorKind::IRTextError, { 1, 24 });
    checkFailure("module m0 functionText=\"\\u{41}\"\n"_s, InternalErrorKind::IRTextError, { 1, 24 });
}

static void parserFillsUntypedThis()
{
    BuildContext build(makeDefaultCompileOptions());
    auto program = parseProgram(makeString(moduleLine, "function f0 results=[{type=number}]\nfunction f1 flags=[HasThis] thisType={type=\"a\" flags=[IsExpected]} results=[{type=number}]\n"_s), IRForm::AsRead, build);
    AOT_TEST_CHECK(program);
    DeclaredType untyped;
    untyped.type = TypeTable::anything;
    AOT_TEST_CHECK(program->functions[0].thisType == untyped);
    AOT_TEST_CHECK(program->at(program->functions[1].thisType.type).singleStringLiteral() && program->functions[1].thisType.flags == OptionSet<PositionFlags> { PositionFlags::IsExpected });
}

static void parserChecksRuntimeFunctionNumbers()
{
    const BuiltinTable& table = BuiltinTable::singleton();
    AOT_TEST_CHECK(!table.runtimeFunctions().empty());
    RuntimeFunctionID id = table.runtimeFunctions().front().id;
    BuildContext build(makeDefaultCompileOptions());
    auto program = parseProgram(makeString(moduleLine, "function f0 role=Library enclosingRuntimeFunction="_s, id.index(), " results=[{type=anything}]\nruntimefunction id="_s, id.index(), " function=f0\n"_s), IRForm::AsRead, build);
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(program->functions[0].role == FunctionRole::Library && program->functions[0].enclosingRuntimeFunction == id);
    const RuntimeFunctionEntry* entry = findRuntimeFunction(*program, id);
    AOT_TEST_CHECK(entry && entry->function == FunctionID(0u) && entry->visibility == FunctionVisibility::Public);

    AOT_TEST_CHECK(!table.contains(RuntimeFunctionID(4000000000u)));
    checkFailure("function f0 role=Library enclosingRuntimeFunction=4000000000\n"_s, InternalErrorKind::IRTextError, { 1, 51 });
}

static void parserGoesThroughTheReader()
{
    BuildContext build(makeDefaultCompileOptions());
    auto parsed = parseProgram(readerFixtureText, IRForm::AsRead, build);
    AOT_TEST_CHECK(parsed && !parsed->buffer().empty());
    auto read = Program::read(parsed->buffer(), build);
    AOT_TEST_CHECK(read);
    String parsedText = printedTextOf(*parsed);
    AOT_TEST_CHECK(!parsedText.isEmpty() && parsedText == printedTextOf(*read));

    auto parsedAgain = parseProgram(parsedText, IRForm::AsRead, build);
    AOT_TEST_CHECK(parsedAgain && printedTextOf(*parsedAgain) == parsedText);
}

static void parserAppliesEngineFieldsAfterTheRead()
{
    BuildContext build(makeDefaultCompileOptions());
    auto program = parseProgram(makeString(moduleLine, R"IR(variable g0 declared={type=anything} flow={writers=AllKnown}
variable g1 declared={type=anything}
function f0 results=[{type=anything}] capabilities=[MayBeInlined] {
  b0():
    v0: number ~ i32 @Double = Integer integer=5 @12:3
    v1 = Integer integer=6 @13
    Return v0
}
function f1 results=[{type=anything}]
)IR"_s), IRForm::AsRead, build);
    AOT_TEST_CHECK(program);
    AOT_TEST_CHECK(program->variables[0].flow.writers == WriterState::AllKnown && program->variables[1].flow == LocationFlow { });
    AOT_TEST_CHECK(program->functions[0].capabilities == OptionSet<FunctionCapabilities> { FunctionCapabilities::MayBeInlined });
    AOT_TEST_CHECK(program->functions[1].capabilities == initialCapabilitiesOf(program->functions[1]) && !program->functions[1].capabilities.isEmpty());

    const Value& stated = program->functions[0].at(ValueID(0u));
    AOT_TEST_CHECK(stated.type == TypeTable::number && stated.expected() == TypeTable::int32 && stated.expectedOrType() == TypeTable::int32);
    AOT_TEST_CHECK(stated.representation == Representation::Double && stated.position == 12 && stated.inlineFrame == 3 && stated.integer() == 5);
    const Value& unstated = program->functions[0].at(ValueID(1u));
    AOT_TEST_CHECK(!unstated.type && !unstated.expected() && unstated.representation == Representation::Unchosen);
    AOT_TEST_CHECK(unstated.position == 13 && !unstated.inlineFrame && unstated.integer() == 6);

    auto read = Program::read(program->buffer(), build);
    AOT_TEST_CHECK(read && !read->functions[0].at(ValueID(0u)).type && read->variables[0].flow == LocationFlow { });
}

static void parserLetsStatedTypeWinOverRule()
{
    String ruleText = makeString(moduleLine, "function f0 results=[{type=anything}] {\n  b0():\n    v0 = Integer integer=5\n    Return v0\n}\n"_s);
    String statedText = makeString(moduleLine, "function f0 results=[{type=anything}] {\n  b0():\n    v0: number = Integer integer=5\n    Return v0\n}\n"_s);
    BuildContext build(makeDefaultCompileOptions());
    auto typedByRule = parseProgram(ruleText, IRForm::Prepared, build);
    AOT_TEST_CHECK(typedByRule && typedByRule->form() == IRForm::Prepared);
    AOT_TEST_CHECK(typedByRule->typeOf(typedByRule->functions[0], ValueID(0u)) == Type::integer(Range::one(5)));

    VerifierInputs optimizing;
    optimizing.isOptimizing = true;
    auto stated = parseProgram(statedText, IRForm::Prepared, build, optimizing);
    AOT_TEST_CHECK(stated && stated->functions[0].at(ValueID(0u)).type == TypeTable::number);

    InternalError error = parseInvalidText(statedText, IRForm::Prepared).error;
    AOT_TEST_CHECK(error.kind == InternalErrorKind::TypeOutOfDate && error.function == FunctionID(0u) && error.value == ValueID(0u) && !error.textPosition);
}

#define FOR_EACH_AOT_IR_PARSER_TEST(v) \
    v(lexerReportsPositions) v(lexerEndsTextOnce) v(lexerRestoresState) v(lexerClassifiesWordsAndIDs) v(lexerIntegers) v(lexerDecimals) \
    v(lexerRejectsNumberFollowedByWordByte) v(lexerPunctuators) v(lexerNumberSignBeforeQuoteIsAToken) v(lexerRejectsOtherBytes) \
    v(lexerKeepsEscapedQuoteInsideString) v(lexerDecodesEveryEscape) v(lexerEncodesSurrogateEscapeAsThreeBytes) v(lexerCopiesBytesAboveASCII) \
    v(lexerReportsOffendingByteOfString) \
    v(parserReportsPositionInRecordLines) v(parserReportsPositionOfInvalidTokens) v(parserReportsPositionInTypes) v(parserLimitsNestingOfTypes) \
    v(parserReportsPositionInBodies) v(parserReportsPositionInBlocksAndTerminators) v(parserPutsTextPositionInMessage) \
    v(parserLeavesIDsOfListsToTheReader) v(parserGivesNoPositionToErrorsOfTheReader) v(parserLeavesCaptureLimitToTheReader) \
    v(parserSurvivesDeepData) v(parserOwnsFunctionText) v(parserRejectsCharacterEscapesInBytes) \
    v(parserFillsUntypedThis) v(parserChecksRuntimeFunctionNumbers) v(parserGoesThroughTheReader) \
    v(parserAppliesEngineFieldsAfterTheRead) v(parserLetsStatedTypeWinOverRule)

std::span<const NativeTest> irParserTests()
{
#define AOT_IR_PARSER_TEST_ENTRY(name) { #name ""_s, name },
    static constexpr NativeTest tests[] = { FOR_EACH_AOT_IR_PARSER_TEST(AOT_IR_PARSER_TEST_ENTRY) };
#undef AOT_IR_PARSER_TEST_ENTRY
    return tests;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
