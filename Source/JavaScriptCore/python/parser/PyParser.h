/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "ParserArena.h"
#include "PyAST.h"
#include "PyLexer.h"
#include <wtf/SetForScope.h>
#include <wtf/TZoneMalloc.h>

namespace JSC {
namespace Python {

enum class ParseMode : uint8_t {
    File, // compile(..., "exec")
    Single, // "single": one interactive statement
    Eval, // "eval"
    FunctionType, // ast.parse(..., mode="func_type")
};

enum class ParserFlag : uint8_t {
    TypeComments = 1 << 0,
    BarryAsBDFL = 1 << 1,
    AllowIncompleteInput = 1 << 2, // Text that ends too early is an _IncompleteInputError, which tells a REPL to read on.
    DontImplyDedent = 1 << 3,
};

// Everything a parse leaves behind, which the compiler reads in place.
class ParseTree {
    WTF_MAKE_TZONE_ALLOCATED(ParseTree);
    WTF_MAKE_NONCOPYABLE(ParseTree);
public:
    ParseTree() = default;

    SourceText source;
    Arena arena;
    IdentifierArena identifiers;
    AST::Mod* root { nullptr };
    Vector<uint32_t> lineStarts; // By line, from 0: its offset in the source.
};

// A recursive descent parser for the grammar of Python 3.15 (Grammar/python.gram in CPython is the reference).
class Parser {
    WTF_MAKE_NONCOPYABLE(Parser);
public:
    Parser(VM&, ParseTree&, ParserHost&, ParseMode, OptionSet<ParserFlag>, int featureVersion = 15);

    // Null: error() says why.
    AST::Mod* parse();
    const SyntaxErrorInfo& error() const { return m_error; }

private:
    using Expr = AST::Expr;
    using Stmt = AST::Stmt;
    using Pattern = AST::Pattern;
    using Range = AST::Range;
    using Name = AST::Name;
    using ExprVector = Vector<Expr*, 8>;
    using StmtVector = Vector<Stmt*, 8>;

    // ---- Tokens ----

    const Token& peek(unsigned ahead = 0)
    {
        if (m_index + ahead >= m_tokensSeen) [[unlikely]]
            reveal(m_index + ahead);
        return m_tokens[m_index + ahead];
    }
    void reveal(size_t index);
    TokenKind kind() { return peek().kind; }
    bool at(TokenKind kind) { return peek().kind == kind; }
    bool atSoftKeyword(const Identifier& keyword, unsigned ahead = 0)
    {
        auto& token = peek(ahead);
        return token.kind == TokenKind::Name && *token.identifier == keyword;
    }
    void fill();
    const Token& advance();
    bool accept(TokenKind kind)
    {
        if (!at(kind))
            return false;
        advance();
        return true;
    }
    // The token, or null after "invalid syntax".
    const Token* expect(TokenKind);
    // The token, or null after "expected 'x'", which is an error whatever else could have been tried.
    const Token* expectForced(TokenKind, ASCIILiteral expected);
    Name expectName();

    Position startPosition() { return peek().startPosition; }
    Range rangeFrom(Position start) const { return { start.line, start.column, m_lastEnd.line, m_lastEnd.column }; }
    static Range rangeOf(const Token& token) { return { token.startPosition.line, token.startPosition.column, token.endPosition.line, token.endPosition.column }; }
    static Range rangeBetween(const Range& first, const Range& last) { return { first.lineno, first.colOffset, last.endLineno, last.endColOffset }; }
    std::span<const uint8_t> textOf(const Token& token) { return m_lexer.source().subspan(token.start, token.end - token.start); }

    // Trying a production that may turn out not to be the one: what it raises is forgotten on rewind(), unless the lexer raised it.
    struct Checkpoint {
        size_t index;
        Position lastEnd;
    };
    Checkpoint checkpoint() const { return { m_index, m_lastEnd }; }
    void rewind(const Checkpoint&);

    bool isSafeToRecurse();

    // ---- Errors. All return null, to be returned in turn. The first error stands. ----

    bool failed() const { return !!m_error; }
    std::nullptr_t raise(SyntaxErrorInfo::Type, int lineno, int colOffset, int endLineno, int endColOffset, const String& message);
    // At the last token read, which is as far as the parser got.
    std::nullptr_t syntaxError(const String& message, SyntaxErrorInfo::Type = SyntaxErrorInfo::Type::SyntaxError);
    std::nullptr_t syntaxErrorOnNextToken(const String& message);
    std::nullptr_t syntaxErrorAt(const Range&, const String& message);
    std::nullptr_t syntaxErrorAt(const Token& token, const String& message) { return syntaxErrorAt(rangeOf(token), message); }
    // From there to where the lexer is.
    std::nullptr_t syntaxErrorStartingFrom(Position, const String& message);
    std::nullptr_t indentationError(const String& message) { return syntaxError(message, SyntaxErrorInfo::Type::IndentationError); }
    // What no message explains. An enclosing production that knows better may replace it.
    std::nullptr_t invalidSyntax();
    std::nullptr_t stackOverflow();
    void raiseLexerError();
    void raiseUnclosedBracket();
    void checkRestOfSourceForLexerErrors();
    bool warn(const String& message, const Range&);

    // ---- Statements ----

    AST::Mod* parseFile();
    AST::Mod* parseInteractive();
    AST::Mod* parseEval();
    AST::Mod* parseFunctionType();

    bool parseStatement(StmtVector&);
    bool parseSimpleStatements(StmtVector&);
    Stmt* parseSimpleStatement();
    Stmt* parseCompoundStatement();
    bool atCompoundStatement();
    // `what` and the line of `opener` go in the message if no block follows.
    bool parseBlock(Seq<Stmt*>&, ASCIILiteral what, const Token& opener);

    Stmt* parseExpressionStatement();
    Stmt* parseAnnotatedAssignment(Position start, Expr* target, bool startsWithName);
    std::nullptr_t diagnoseInvalidAssignment(Expr* target, TokenKind firstKind, bool isFirstTarget);
    std::nullptr_t diagnoseStatementBeforeIf();
    Stmt* parseReturn();
    Stmt* parseRaise();
    Stmt* parseDelete();
    Stmt* parseAssert();
    Stmt* parseGlobalOrNonlocal();
    Stmt* parseImport(bool isLazy, Position start);
    Stmt* parseImportFrom(const Token* lazyToken, Position start);
    AST::Alias* parseImportFromAsName();
    Name parseDottedName();
    Stmt* parseTypeAlias();

    Stmt* parseIf();
    Stmt* parseWhile();
    Stmt* parseFor(bool isAsync, Position start);
    Stmt* parseWith(bool isAsync, Position start);
    bool parseWithItems(Vector<AST::WithItem*, 4>&, bool parenthesized);
    Stmt* parseTry();
    Stmt* parseFunctionDef(bool isAsync, Position start, Seq<Expr*> decorators);
    Stmt* parseClassDef(Seq<Expr*> decorators);
    Stmt* parseDecorated();
    Stmt* parseMatch(std::optional<Token>& newlineInsteadOfColon);
    bool parseElse(Seq<Stmt*>&);
    StringImpl* acceptTypeComment();

    bool parseTypeParams(Seq<AST::TypeParam*>&);
    AST::Arguments* parseParameters(bool isLambda);
    AST::Arguments* emptyArguments();

    // ---- Expressions ----

    Expr* parseStarExpressions(); // star_expressions
    Expr* parseStarExpression(); // star_expression
    Expr* parseExpressions(); // expressions
    Expr* parseExpression(); // expression
    Expr* parseNamedExpression(); // named_expression
    Expr* parseStarNamedExpression(); // star_named_expression
    Expr* parseSequenceElement(); // star_named_expression_sequence
    Expr* parseExpressionQuietly();
    std::nullptr_t diagnoseJuxtaposition(Expr* first, size_t startIndex);
    std::nullptr_t diagnoseComprehensionTarget(const ExprVector&, const Token& lastComma, bool endsWithComma);
    bool isSoftKeyword(const Token&);
    Expr* parseAnnotatedRHS(); // annotated_rhs: yield_expr | star_expressions
    Expr* parseYield();
    Expr* parseLambda();
    Expr* parseDisjunction();
    Expr* parseConjunction();
    Expr* parseInversion();
    Expr* parseComparison();
    Expr* parseBitwiseOr() { return parseBinary(1); }
    Expr* parseBinary(int minimumPrecedence);
    Expr* parseFactor();
    Expr* parsePower();
    Expr* parseAwaitPrimary();
    Expr* parsePrimary();
    Expr* parseTrailers(Expr*, Position start);
    Expr* parseAtom();
    Expr* parseParenthesized();
    Expr* parseListDisplay();
    Expr* parseBraceDisplay();
    Expr* parseSlices();
    Expr* parseSlice();
    bool parseArguments(Seq<Expr*>& arguments, Seq<AST::Keyword*>& keywords, Position openParenthesis, Expr*& soleGenerator);
    bool parseComprehensionClauses(Seq<AST::Comprehension*>&);
    bool atComprehension() { return at(TokenKind::KwFor) || (at(TokenKind::KwAsync) && peek(1).kind == TokenKind::KwFor); }
    Expr* parseForTargets(bool inComprehension);
    Expr* parseStarTargets(); // star_targets
    Expr* parseStarTarget();
    Expr* parseNumber();
    bool atStartOfExpression(unsigned ahead = 0);

    // ---- Strings ----

    Expr* parseStrings();
    Expr* parseStringLiteral(const Token&);
    Expr* parseInterpolatedString();
    Expr* parseReplacementField(bool isTemplate, bool isRaw, bool inFormatSpec);
    Expr* concatenateStrings(const ExprVector&, const Range&, bool isTemplate);
    Seq<Expr*> flattenAndFoldStrings(const ExprVector&);
    bool decodeString(std::span<const uint8_t> body, bool isRaw, const Token&, const Range* errorRange, String& result);
    bool decodeBytes(std::span<const uint8_t> body, bool isRaw, const Token&, AST::ConstantValue&);
    bool warnInvalidEscape(std::span<const uint8_t> body, size_t escape, size_t virtualOffset, const Token&);
    String expressionText(uint32_t start, uint32_t end);
    AST::Constant* makeStringConstant(const String&, const Range&);

    // ---- Patterns ----

    Pattern* parsePatterns();
    Pattern* parsePattern();
    Pattern* parseOrPattern();
    Pattern* parseClosedPattern();
    Pattern* parseMaybeStarPattern();
    Pattern* parseSequencePattern(TokenKind close);
    Pattern* parseMappingPattern();
    Pattern* parseClassPattern(Expr* cls, Position start);
    Expr* parseSignedNumberOrComplex();
    Expr* parseNameOrAttribute();
    bool parseOpenSequence(Vector<Pattern*, 8>&, Pattern* first, TokenKind close);

    // ---- Targets ----

    enum class TargetKind : uint8_t { Star, Del, For };
    Expr* findInvalidTarget(Expr*, TargetKind);
    std::nullptr_t raiseInvalidTarget(Expr*, TargetKind);
    void setContext(Expr*, AST::ExprContext);
    static ASCIILiteral describe(Expr*);
    static bool isAtBitwiseOrLevel(Expr*);

    template<typename T, typename... Arguments>
    T* make(Arguments&&... arguments) { return m_arena.create<T>(std::forward<Arguments>(arguments)...); }
    AST::NameExpr* makeName(const Token&, AST::ExprContext = AST::ExprContext::Load);
    AST::Tuple* makeTuple(const ExprVector&, const Range&);

    VM& m_vm;
    ParseTree& m_tree;
    Arena& m_arena;
    ParserHost& m_host;
    Lexer m_lexer;
    ParseMode m_mode;
    OptionSet<ParserFlag> m_flags;
    int m_featureVersion;

    Vector<Token, 64> m_tokens;
    size_t m_index { 0 };
    // How many of the tokens the parser has looked at, not counting what it read past a mistake only to explain it:
    // "invalid syntax" points at the last of these.
    size_t m_tokensSeen { 0 };
    bool m_diagnosing { false };
    Position m_lastEnd; // Where the last token consumed ends, newlines and indentation aside.
    bool m_parsingStarted { false };

    SyntaxErrorInfo m_error;
    bool m_errorIsFatal { false };
    bool m_errorIsGeneric { false };
    bool m_errorIsFinal { false }; // No error of the lexer further on replaces it.
    bool m_quiet { false }; // Nothing is diagnosed: what does not parse just fails.
    Vector<uint32_t> m_warnedEscapes;
    Vector<AST::TypeIgnore*> m_typeIgnores;
    Position m_lastStatementStart;

    const Identifier& m_matchKeyword;
    const Identifier& m_caseKeyword;
    const Identifier& m_typeKeyword;
    const Identifier& m_lazyKeyword;
    const Identifier& m_underscore;
};

// Parses `bytes`. False: `error` says why.
bool parse(VM&, ParseTree&, ParserHost&, std::span<const uint8_t> bytes, const String& filename, ParseMode, OptionSet<ParserFlag>, bool sourceIsUTF8, SyntaxErrorInfo& error, int featureVersion = 15);

} // namespace Python
} // namespace JSC
