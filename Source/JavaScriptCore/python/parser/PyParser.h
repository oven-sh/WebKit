/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "ParserArena.h"
#include "PyAST.h"
#include "PyLexer.h"
#include <wtf/HashSet.h>
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
    // The tree was made from objects of the module ast: a node need not lie within the one that holds it.
    bool positionsAreArbitrary { false };
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
    enum class ForcedBy : uint8_t { RuleThatIsAlwaysTried, RuleThatExplainsErrors };
    const Token* expectForced(TokenKind, ASCIILiteral expected, ForcedBy);
    void errorIsOfARuleThatIsAlwaysTried();
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
    // What follows an operator is not an operand. True: the operator is left to read, and the expression ends before it.
    bool endsBeforeOperator(const Checkpoint& beforeOperator);

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
    std::nullptr_t invalidSyntaxAtLastTokenSeen();
    std::nullptr_t stackOverflow();
    // For ast.parse(feature_version=...). False: what has just been parsed came with a later version, which is an error.
    bool checkVersion(int version, ASCIILiteral what)
    {
        if (m_featureVersion >= version) [[likely]]
            return true;
        syntaxError(makeString(what, " only supported in Python 3."_s, version, " and greater"_s));
        return false;
    }
    void raiseLexerError();
    void raiseUnclosedBracket();
    void checkRestOfSourceForLexerErrors();
    bool warn(const String& message, const Range&);

    // ---- Statements ----

    AST::Mod* parseFile();
    AST::Mod* parseInteractive();
    AST::Mod* parseEval();
    AST::Mod* parseFunctionType();

    // `isCompound`: set if it was a compound statement.
    bool parseStatement(StmtVector&, bool* isCompound = nullptr);
    void didParseCompoundStatement(Position start);
    bool parseSimpleStatements(StmtVector&);
    Stmt* parseSimpleStatement();
    Stmt* parseCompoundStatement();
    bool atCompoundStatement();
    // `what` and the line of `opener` go in the message if no block follows.
    bool parseBlock(Seq<Stmt*>&, ASCIILiteral what, const Token& opener);

    Stmt* parseExpressionStatement();
    Stmt* parseAnnotatedAssignment(Position start, Expr* target, bool startsWithName);
    std::nullptr_t diagnoseInvalidAssignment(Expr* target, TokenKind firstKind, bool isFirstTarget, const Checkpoint& statementStart);
    bool diagnoseNamedExpressions(const Checkpoint& statementStart);
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
    Stmt* parseMatch(std::optional<Token>& newlineInsteadOfColon, bool& isMatchStatement);
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
    Expr* parseExpressionOnce();
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
    // Of a fatal error. It is the parser's: it came of a rule that is always tried, not of one of those that explain errors, and
    // after it nothing else is tried.
    bool m_fatalErrorIsOfTheParser { false };
    bool m_fatalErrorWasFoundDiagnosing { false }; // It was come upon past a mistake, while that was being explained.
    bool m_quiet { false }; // Nothing is diagnosed: what does not parse just fails.
    // CPython's parser remembers what came of a rule at a token. What it has once tried there without diagnosing anything, it does
    // not try again, so nothing is ever diagnosed there.
    enum class MemoizedRule : uint8_t { Expression, Disjunction, Binary, Primary };
    HashSet<uint64_t> m_parsedQuietly;
    bool wasParsedQuietly(MemoizedRule rule)
    {
        if (!m_quiet && m_parsedQuietly.isEmpty()) [[likely]]
            return false;
        uint64_t key = m_index * 4 + static_cast<unsigned>(rule) + 1;
        if (m_quiet) {
            m_parsedQuietly.add(key);
            return false;
        }
        return m_parsedQuietly.contains(key);
    }
    // `a +` is `a`, with the '+' left to read, as it is to a parser that backtracks: some messages are about that '+'.
    bool m_acceptsPrefix { false };
    bool m_failedExpressionHasPrefix { false }; // After parseExpression() has failed: it would have parsed with m_acceptsPrefix.
    Vector<uint32_t> m_warnedEscapes;
    Vector<AST::TypeIgnore*> m_typeIgnores;
    Position m_lastCompoundStatement { 0, 0 }; // See SyntaxErrorInfo::metadataLineno.
    bool* m_statementIsCompleteSoFar { nullptr }; // Of the compound statement being parsed: it could end here, only optional clauses can follow.
    bool m_errorIsAboutWhatFollows { false }; // The text parsed. It is what comes after it that is wrong.

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
