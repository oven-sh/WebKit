/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyParser.h"

#include "VM.h"
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

WTF_MAKE_TZONE_ALLOCATED_IMPL(ParseTree);

// "Where the lexer is", as a line or a column of an error.
static constexpr int currentPosition = -5;

static OptionSet<Lexer::Flag> lexerFlags(OptionSet<ParserFlag> flags)
{
    OptionSet<Lexer::Flag> result;
    if (flags.contains(ParserFlag::TypeComments))
        result.add(Lexer::Flag::TypeComments);
    if (flags.contains(ParserFlag::BarryAsBDFL))
        result.add(Lexer::Flag::BarryAsBDFL);
    return result;
}

Parser::Parser(VM& vm, ParseTree& tree, ParserHost& host, ParseMode mode, OptionSet<ParserFlag> flags, int featureVersion)
    : m_vm(vm)
    , m_tree(tree)
    , m_arena(tree.arena)
    , m_host(host)
    , m_lexer(vm, tree.identifiers, host, tree.source.bytes(), lexerFlags(flags))
    , m_mode(mode)
    , m_flags(flags)
    , m_featureVersion(featureVersion)
    , m_matchKeyword(tree.identifiers.makeIdentifier(vm, "match"_span8))
    , m_caseKeyword(tree.identifiers.makeIdentifier(vm, "case"_span8))
    , m_typeKeyword(tree.identifiers.makeIdentifier(vm, "type"_span8))
    , m_lazyKeyword(tree.identifiers.makeIdentifier(vm, "lazy"_span8))
    , m_underscore(tree.identifiers.makeIdentifier(vm, "_"_span8))
{
}

bool Parser::isSafeToRecurse()
{
    return m_vm.isSafeToRecurse();
}

// ---- Tokens ----

void Parser::fill()
{
    // Nothing follows the end or an error.
    if (!m_tokens.isEmpty() && (m_tokens.last().kind == TokenKind::ErrorToken || (m_tokens.last().kind == TokenKind::EndMarker))) {
        m_tokens.append(Token { m_tokens.last() });
        return;
    }

    Token token;
    m_lexer.lex(token);
    while (token.kind == TokenKind::TypeIgnore) {
        auto* ignore = make<TypeIgnore>();
        ignore->lineno = token.startPosition.line;
        ignore->tag = m_arena.retain(stringFromUTF8(textOf(token)));
        m_typeIgnores.append(ignore);
        m_lexer.lex(token);
    }

    // An interactive statement ends where the text does, whatever is still indented.
    if (m_mode == ParseMode::Single && token.kind == TokenKind::EndMarker && m_parsingStarted) {
        token.kind = TokenKind::Newline;
        m_parsingStarted = false;
        if (!m_flags.contains(ParserFlag::DontImplyDedent))
            m_lexer.implyDedents();
    } else
        m_parsingStarted = true;

    m_tokens.append(token);
    if (token.kind == TokenKind::ErrorToken)
        raiseLexerError();
}

void Parser::reveal(size_t index)
{
    while (index >= m_tokens.size())
        fill();
    if (!m_diagnosing)
        m_tokensSeen = index + 1;
}

const Token& Parser::advance()
{
    const Token& token = peek();
    switch (token.kind) {
    case TokenKind::Newline:
    case TokenKind::Indent:
    case TokenKind::Dedent:
    case TokenKind::EndMarker:
    case TokenKind::ErrorToken:
        break;
    default:
        m_lastEnd = token.endPosition;
    }
    if (token.kind != TokenKind::ErrorToken)
        ++m_index;
    return token;
}

const Token* Parser::expect(TokenKind kind)
{
    if (at(kind))
        return &advance();
    return invalidSyntax();
}

const Token* Parser::expectForced(TokenKind kind, ASCIILiteral expected, ForcedBy forcedBy)
{
    if (at(kind))
        return &advance();
    if (failed())
        return nullptr;
    syntaxErrorAt(peek(), makeString("expected '"_s, expected, '\''));
    if (forcedBy == ForcedBy::RuleThatIsAlwaysTried)
        errorIsOfARuleThatIsAlwaysTried();
    return nullptr;
}

void Parser::errorIsOfARuleThatIsAlwaysTried()
{
    if (m_errorIsFatal)
        return;
    m_errorIsFatal = true;
    m_errorIsGeneric = false;
    m_fatalErrorIsOfTheParser = true;
    m_fatalErrorWasFoundDiagnosing = m_diagnosing;
}

Name Parser::expectName()
{
    auto* token = expect(TokenKind::Name);
    return token ? token->identifier : nullptr;
}

void Parser::rewind(const Checkpoint& checkpoint)
{
    // Once the parser is past a mistake and looks for what to say about it, the first thing it finds is what it says.
    bool isExplained = failed() && !m_errorIsGeneric && m_errorWasFoundDiagnosing;
    if (!m_errorIsFatal && !isExplained) {
        m_error = { };
        m_errorIsGeneric = false;
        m_errorIsFinal = false;
    }
    m_index = checkpoint.index;
    m_lastEnd = checkpoint.lastEnd;
}

bool Parser::endsBeforeOperator(const Checkpoint& beforeOperator)
{
    if (!m_acceptsPrefix || m_errorIsFatal || !m_errorIsGeneric)
        return false;
    rewind(beforeOperator);
    return true;
}

// ---- Errors ----

// Columns are in bytes, from 0.
std::nullptr_t Parser::raise(SyntaxErrorInfo::Type type, int lineno, int colOffset, int endLineno, int endColOffset, const String& message)
{
    if (failed())
        return nullptr;

    // From here on they count from 1.
    if (colOffset != currentPosition)
        ++colOffset;
    if (endColOffset != currentPosition)
        ++endColOffset;
    if (endLineno == currentPosition)
        endLineno = m_lexer.currentLine();
    if (endColOffset == currentPosition)
        endColOffset = static_cast<int>(m_lexer.currentOffset() - m_lexer.currentLineStart());

    std::span<const uint8_t> line;
    Vector<uint8_t> lineOfFile;
    if (m_mode == ParseMode::File && m_host.lineOfFile(lineno, m_tree.source.encoding(), lineOfFile))
        line = lineOfFile.span();
    else if (m_lexer.currentLine() <= lineno && m_lexer.currentLineEnd() > m_lexer.currentLineStart())
        line = m_lexer.source().subspan(m_lexer.currentLineStart(), m_lexer.currentLineEnd() - m_lexer.currentLineStart());
    else
        line = m_lexer.lineText(lineno);

    auto inCharacters = [&](int byteOffset) {
        size_t bytes = std::min<size_t>(std::max(byteOffset, 0), line.size() + 1);
        return characterCount(line, bytes) + (bytes > line.size());
    };

    m_errorIsGeneric = m_quiet;
    m_errorWasFoundDiagnosing = m_diagnosing;
    m_error.type = type;
    m_error.message = message;
    m_error.hasLocation = true;
    m_error.lineno = lineno;
    m_error.offset = inCharacters(colOffset);
    m_error.text = stringFromUTF8(line);
    m_error.hasEnd = true;
    m_error.endLineno = endLineno;
    m_error.endOffset = endColOffset > 0 ? inCharacters(endColOffset) : endColOffset;
    return nullptr;
}

static std::pair<int, int> columnsOf(const Token& token, const Lexer& lexer)
{
    // A token without columns stands for where the lexer is.
    int column = token.hasColumns ? token.startPosition.column : static_cast<int>(lexer.currentOffset() - lexer.currentLineStart()) - 1;
    int endColumn = token.hasColumns ? token.endPosition.column : -2;
    return { column, endColumn };
}

std::nullptr_t Parser::syntaxError(const String& message, SyntaxErrorInfo::Type type)
{
    if (failed())
        return nullptr;
    if (m_tokens.isEmpty())
        return raise(type, 0, -1, 0, -2, message);
    auto& token = m_tokens.last();
    auto [column, endColumn] = columnsOf(token, m_lexer);
    return raise(type, token.startPosition.line, column, token.endPosition.line, endColumn, message);
}

std::nullptr_t Parser::syntaxErrorOnNextToken(const String& message)
{
    if (failed())
        return nullptr;
    auto& token = peek();
    if (failed())
        return nullptr;
    auto [column, endColumn] = columnsOf(token, m_lexer);
    return raise(SyntaxErrorInfo::Type::SyntaxError, token.startPosition.line, column, token.endPosition.line, endColumn, message);
}

std::nullptr_t Parser::syntaxErrorAt(const Range& range, const String& message)
{
    return raise(SyntaxErrorInfo::Type::SyntaxError, range.lineno, range.colOffset, range.endLineno, range.endColOffset, message);
}

std::nullptr_t Parser::syntaxErrorStartingFrom(Position start, const String& message)
{
    return raise(SyntaxErrorInfo::Type::SyntaxError, start.line, start.column, currentPosition, currentPosition, message);
}

void Parser::raiseUnclosedBracket()
{
    auto& bracket = m_lexer.openBrackets().last();
    raise(SyntaxErrorInfo::Type::SyntaxError, bracket.line, bracket.column, bracket.line, -1, makeString('\'', static_cast<char>(bracket.character), "' was never closed"_s));
}

// What no production accounts for.
std::nullptr_t Parser::invalidSyntax()
{
    if (failed())
        return nullptr;
    peek();
    if (failed())
        return nullptr;
    return invalidSyntaxAtLastTokenSeen();
}

// An attempt to explain the error has come to nothing.
std::nullptr_t Parser::invalidSyntaxAfterAttempt()
{
    if (m_errorIsFatal)
        return nullptr;
    m_error = { };
    return invalidSyntaxAtLastTokenSeen();
}

std::nullptr_t Parser::invalidSyntaxAtLastTokenSeen()
{
    auto& last = m_tokens[m_tokensSeen - 1];
    if (last.kind == TokenKind::Indent || last.kind == TokenKind::Dedent) {
        indentationError(last.kind == TokenKind::Indent ? "unexpected indent"_s : "unexpected unindent"_s);
        m_errorIsFinal = true;
        m_errorIsGeneric = true;
        return nullptr;
    }
    syntaxErrorAt(last, "invalid syntax"_s);
    m_errorIsGeneric = true;
    return nullptr;
}

std::nullptr_t Parser::stackOverflow()
{
    if (!m_errorIsFatal) {
        m_error = { };
        m_error.type = SyntaxErrorInfo::Type::MemoryError;
        m_error.message = "Parser stack overflowed - Python source too complex to parse"_s;
        m_errorIsFatal = true;
        m_errorIsGeneric = false;
    }
    return nullptr;
}

void Parser::raiseLexerError()
{
    if (failed() && m_errorIsFatal)
        return;
    m_error = { };
    m_errorIsGeneric = false;
    m_errorIsFinal = false;
    m_fatalErrorWasFoundDiagnosing = m_diagnosing;
    int line = m_lexer.currentLine();
    switch (m_lexer.status()) {
    case Lexer::Status::Error:
    case Lexer::Status::EndOfFileInTripleQuotedString:
    case Lexer::Status::EndOfLineInString:
        m_error = m_lexer.error();
        break;
    case Lexer::Status::EndOfFile:
        if (!m_lexer.openBrackets().isEmpty())
            raiseUnclosedBracket();
        else
            syntaxError("unexpected EOF while parsing"_s);
        break;
    case Lexer::Status::Dedent:
        indentationError("unindent does not match any outer indentation level"_s);
        break;
    case Lexer::Status::TabSpace:
        raise(SyntaxErrorInfo::Type::TabError, line, 0, line, -1, "inconsistent use of tabs and spaces in indentation"_s);
        break;
    case Lexer::Status::TooDeep:
        raise(SyntaxErrorInfo::Type::IndentationError, line, 0, line, -1, "too many levels of indentation"_s);
        break;
    case Lexer::Status::LineContinuation:
        // CPython counts the column from where its buffer begins, which on a line that continues another one is the first of them.
        // So there it is past the end of the line, which is as far as a column goes.
        raise(SyntaxErrorInfo::Type::SyntaxError, line, std::max(static_cast<int>(m_lexer.currentOffset() - m_lexer.lineStartOffset(std::min(m_lexer.firstBufferedLine(), line))) - 1, 0), line, -1, "unexpected character after line continuation character"_s);
        break;
    case Lexer::Status::Ok:
        raise(SyntaxErrorInfo::Type::SyntaxError, line, 0, line, -1, "unknown parsing error"_s);
        break;
    }
    m_errorIsFatal = true;
    m_errorIsGeneric = false;
}

// After a syntax error: an error of the lexer further on, such as a bracket that does not match, explains more.
void Parser::checkRestOfSourceForLexerErrors()
{
    if (m_tokens.isEmpty() || m_tokens.last().kind == TokenKind::ErrorToken || m_tokens.last().kind == TokenKind::EndMarker)
        return;
    int errorLine = m_tokens.last().startPosition.line;
    Token token;
    for (;;) {
        m_lexer.lex(token);
        if (token.kind == TokenKind::EndMarker)
            return;
        if (token.kind != TokenKind::ErrorToken)
            continue;
        switch (m_lexer.status()) {
        case Lexer::Status::Error:
        case Lexer::Status::EndOfFileInTripleQuotedString:
        case Lexer::Status::EndOfLineInString:
            // Inside an f-string it is the error in the expression that says what is wrong.
            if (!m_lexer.isInsideFString())
                m_error = m_lexer.error();
            return;
        default:
            if (!m_lexer.openBrackets().isEmpty() && errorLine > m_lexer.openBrackets().last().line) {
                m_error = { };
                raiseUnclosedBracket();
            }
            return;
        }
    }
}

bool Parser::warn(const String& message, const Range& range)
{
    switch (m_host.warn({ ParserHost::WarningCategory::SyntaxWarning, message, range.lineno, range.colOffset, range.endLineno, range.endColOffset })) {
    case ParserHost::WarningResult::Reported:
        return true;
    case ParserHost::WarningResult::BecameException:
        syntaxErrorAt(range, message);
        return false;
    case ParserHost::WarningResult::OtherException:
        if (!failed())
            m_error.type = SyntaxErrorInfo::Type::HostException;
        m_errorIsFatal = true;
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Entry points ----

Mod* Parser::parse()
{
    Mod* result = nullptr;
    switch (m_mode) {
    case ParseMode::File:
        result = parseFile();
        break;
    case ParseMode::Single:
        result = parseInteractive();
        break;
    case ParseMode::Eval:
        result = parseEval();
        break;
    case ParseMode::FunctionType:
        result = parseFunctionType();
        break;
    }

    if (result && !failed()) {
        m_tree.root = result;
        m_tree.lineStarts.reserveInitialCapacity(m_lexer.linesRead());
        for (unsigned line = 1; line <= m_lexer.linesRead(); ++line)
            m_tree.lineStarts.append(m_lexer.lineStartOffset(line));
        return result;
    }

    if (!failed())
        invalidSyntax();

    if (m_flags.contains(ParserFlag::AllowIncompleteInput)) {
        switch (m_lexer.status()) {
        case Lexer::Status::EndOfFile:
        case Lexer::Status::EndOfFileInTripleQuotedString:
        case Lexer::Status::EndOfLineInString:
            m_error = { };
            m_errorIsFatal = false;
            syntaxError("incomplete input"_s, SyntaxErrorInfo::Type::IncompleteInputError);
            return nullptr;
        default:
            break;
        }
    }

    // What nothing explains is said last of all, and the line is shown as the lexer then has it.
    if (m_errorIsGeneric && !m_errorIsFatal) {
        m_error = { };
        m_errorIsFinal = false;
        invalidSyntaxAtLastTokenSeen();
    }
    if ((!m_errorIsFatal || m_fatalErrorIsOfTheParser) && !m_errorIsFinal && m_error.isSyntaxError())
        checkRestOfSourceForLexerErrors();
    if (m_error.isSyntaxError() && !m_errorIsAboutWhatFollows) {
        m_error.hasMetadata = true;
        // CPython's parser notes the statements when it goes over the text again to explain the error, which it does not do
        // after an error of the tokenizer, or of a rule that is always tried.
        if (!m_errorIsFatal || m_fatalErrorWasFoundDiagnosing) {
            m_error.metadataLineno = m_lastCompoundStatement.line;
            m_error.metadataColOffset = m_lastCompoundStatement.column;
        }
    }
    return nullptr;
}

Mod* Parser::parseFile()
{
    StmtVector body;
    while (!at(TokenKind::EndMarker)) {
        if (!parseStatement(body))
            return nullptr;
    }
    auto* module = make<Mod>();
    module->kind = ModKind::Module;
    module->body = m_arena.copy(body);
    module->typeIgnores = m_arena.copy(m_typeIgnores);
    return module;
}

Mod* Parser::parseInteractive()
{
    StmtVector body;
    if (at(TokenKind::Newline)) {
        Range range = rangeOf(peek());
        advance();
        body.append(make<Pass>(range));
    } else if (at(TokenKind::EndMarker)) {
        // Nothing at all: the caller sees the end of its input.
        return invalidSyntax();
    } else {
        bool isCompound = false;
        if (!parseStatement(body, &isCompound))
            return nullptr;
        if (isCompound && !expect(TokenKind::Newline))
            return nullptr;
    }

    // Only blanks and comments may follow.
    auto rest = m_lexer.source().subspan(m_lexer.currentOffset());
    for (size_t i = 0; i < rest.size();) {
        uint8_t c = rest[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\014') {
            ++i;
            continue;
        }
        if (c != '#') {
            m_errorIsAboutWhatFollows = true;
            m_errorIsFinal = true;
            return syntaxError("multiple statements found while compiling a single statement"_s);
        }
        while (i < rest.size() && rest[i] != '\n')
            ++i;
    }

    auto* module = make<Mod>();
    module->kind = ModKind::Interactive;
    module->body = m_arena.copy(body);
    return module;
}

Mod* Parser::parseEval()
{
    Expr* expression = parseExpressions();
    if (!expression)
        return nullptr;
    while (accept(TokenKind::Newline)) { }
    if (!expect(TokenKind::EndMarker))
        return nullptr;
    auto* module = make<Mod>();
    module->kind = ModKind::Expression;
    module->expression = expression;
    return module;
}

// func_type: '(' [type_expressions] ')' '->' expression NEWLINE* ENDMARKER
Mod* Parser::parseFunctionType()
{
    if (!expect(TokenKind::LPar))
        return nullptr;
    ExprVector argumentTypes;
    // The stars leave no trace, but only `**type` may follow `*type`, and nothing `**type`.
    bool sawStar = false;
    bool sawDoubleStar = false;
    while (!at(TokenKind::RPar) || !argumentTypes.isEmpty()) {
        if (accept(TokenKind::DoubleStar))
            sawDoubleStar = true;
        else if (sawStar)
            return invalidSyntax();
        else if (accept(TokenKind::Star))
            sawStar = true;
        Expr* type = parseExpression();
        if (!type)
            return nullptr;
        argumentTypes.append(type);
        if (sawDoubleStar || !accept(TokenKind::Comma))
            break;
    }
    if (!expect(TokenKind::RPar) || !expect(TokenKind::RArrow))
        return nullptr;
    Expr* returns = parseExpression();
    if (!returns)
        return nullptr;
    while (accept(TokenKind::Newline)) { }
    if (!expect(TokenKind::EndMarker))
        return nullptr;
    auto* module = make<Mod>();
    module->kind = ModKind::FunctionType;
    module->argtypes = m_arena.copy(argumentTypes);
    module->expression = returns;
    return module;
}

// ---- Statements ----

bool Parser::atCompoundStatement()
{
    switch (kind()) {
    case TokenKind::KwDef:
    case TokenKind::At:
    case TokenKind::KwAsync:
    case TokenKind::KwIf:
    case TokenKind::KwClass:
    case TokenKind::KwWith:
    case TokenKind::KwFor:
    case TokenKind::KwTry:
    case TokenKind::KwWhile:
        return true;
    default:
        return false;
    }
}

// What CPython's parser notes while it goes over the text a second time to explain the error.
void Parser::didParseCompoundStatement(Position start)
{
    if (m_lastCompoundStatement.line <= start.line)
        m_lastCompoundStatement = start;
}

bool Parser::parseStatement(StmtVector& out, bool* isCompound)
{
    auto finishCompoundStatement = [&](Stmt* statement) {
        // On that second time a function definition that is in order is matched by a rule that is there to find fault with it, and
        // which has no statement to give.
        if (statement->kind != StmtKind::FunctionDef && statement->kind != StmtKind::AsyncFunctionDef)
            didParseCompoundStatement(statement->range.start());
        out.append(statement);
        if (isCompound)
            *isCompound = true;
        return true;
    };
    if (atCompoundStatement()) {
        Position start = startPosition();
        bool isCompleteSoFar = false;
        SetForScope scope(m_statementIsCompleteSoFar, &isCompleteSoFar);
        Stmt* statement = parseCompoundStatement();
        if (statement)
            return finishCompoundStatement(statement);
        // To a parser that backtracks, a clause that does not parse is not there, and the statement ends before it.
        if (isCompleteSoFar && m_errorIsGeneric && !m_errorIsFatal)
            didParseCompoundStatement(start);
        return false;
    }
    if (atSoftKeyword(m_matchKeyword)) {
        // `match` is a keyword only if what follows makes a match statement.
        auto saved = checkpoint();
        std::optional<Token> newlineInsteadOfColon;
        bool isMatchStatement = false;
        if (Stmt* statement = parseMatch(newlineInsteadOfColon, isMatchStatement))
            return finishCompoundStatement(statement);
        if (m_errorIsFatal || isMatchStatement)
            return false;
        SyntaxErrorInfo matchError = m_error;
        bool matchErrorIsSpecific = failed() && !m_errorIsGeneric;
        bool matchErrorIsFinal = m_errorIsFinal;
        rewind(saved);
        if (parseSimpleStatements(out))
            return true;
        if (m_errorIsFatal)
            return false;
        // It is nothing else either, and what was found wrong with it as a match statement was found first.
        if (matchErrorIsSpecific) {
            m_error = matchError;
            m_errorIsGeneric = false;
            m_errorIsFinal = matchErrorIsFinal;
        } else if (newlineInsteadOfColon) {
            rewind(saved);
            syntaxErrorAt(*newlineInsteadOfColon, "expected ':'"_s);
        }
        return false;
    }
    if (atSoftKeyword(m_caseKeyword)) {
        auto saved = checkpoint();
        if (parseSimpleStatements(out))
            return true;
        if (m_errorIsFatal)
            return false;

        // It may be a case block that is not in a match statement.
        SyntaxErrorInfo error = m_error;
        bool errorIsGeneric = m_errorIsGeneric;
        bool errorIsFinal = m_errorIsFinal;
        rewind(saved);
        SetForScope diagnosing(m_diagnosing, true);
        const Token caseToken = advance();
        if (parsePatterns() && (!accept(TokenKind::KwIf) || parseNamedExpression()) && at(TokenKind::Colon)) {
            const Token colon = advance();
            Seq<Stmt*> body;
            if (parseBlock(body, { }, caseToken))
                syntaxErrorAt(rangeBetween(rangeOf(caseToken), rangeOf(colon)), "case statement must be inside match statement"_s);
            if (failed() && !m_errorIsGeneric)
                return false;
        }
        if (m_errorIsFatal)
            return false;
        m_error = error;
        m_errorIsGeneric = errorIsGeneric;
        m_errorIsFinal = errorIsFinal;
        return false;
    }
    return parseSimpleStatements(out);
}

Stmt* Parser::parseCompoundStatement()
{
    if (!isSafeToRecurse()) [[unlikely]]
        return stackOverflow();
    Position start = startPosition();
    switch (kind()) {
    case TokenKind::KwDef:
        return parseFunctionDef(false, start, { });
    case TokenKind::At:
        return parseDecorated();
    case TokenKind::KwAsync:
        switch (peek(1).kind) {
        case TokenKind::KwDef:
            advance();
            return parseFunctionDef(true, start, { });
        case TokenKind::KwWith:
            advance();
            return parseWith(true, start);
        case TokenKind::KwFor:
            advance();
            return parseFor(true, start);
        default:
            advance();
            return invalidSyntax();
        }
    case TokenKind::KwIf:
        return parseIf();
    case TokenKind::KwClass:
        return parseClassDef({ });
    case TokenKind::KwWith:
        return parseWith(false, start);
    case TokenKind::KwFor:
        return parseFor(false, start);
    case TokenKind::KwTry:
        return parseTry();
    case TokenKind::KwWhile:
        return parseWhile();
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// simple_stmts: ';'.simple_stmt+ [';'] NEWLINE
bool Parser::parseSimpleStatements(StmtVector& out)
{
    for (;;) {
        auto statementStart = checkpoint();
        Stmt* statement = parseSimpleStatement();
        if (!statement)
            return false;
        out.append(statement);
        if (!accept(TokenKind::Semi)) {
            if (!at(TokenKind::Newline) && statement->kind == StmtKind::Expr && !diagnoseNamedExpressions(statementStart)) [[unlikely]]
                return false;
            break;
        }
        if (at(TokenKind::Newline))
            break;
    }
    return !!expect(TokenKind::Newline);
}

Stmt* Parser::parseSimpleStatement()
{
    Position start = startPosition();
    switch (kind()) {
    case TokenKind::KwReturn:
        return parseReturn();
    case TokenKind::KwRaise:
        return parseRaise();
    case TokenKind::KwPass:
    case TokenKind::KwBreak:
    case TokenKind::KwContinue: {
        if (peek(1).kind == TokenKind::KwIf)
            return diagnoseStatementBeforeIf();
        TokenKind kind = advance().kind;
        if (kind == TokenKind::KwPass)
            return make<Pass>(rangeFrom(start));
        if (kind == TokenKind::KwBreak)
            return make<Break>(rangeFrom(start));
        return make<Continue>(rangeFrom(start));
    }
    case TokenKind::KwDel:
        return parseDelete();
    case TokenKind::KwAssert:
        return parseAssert();
    case TokenKind::KwGlobal:
    case TokenKind::KwNonlocal:
        return parseGlobalOrNonlocal();
    case TokenKind::KwImport:
        return parseImport(false, start);
    case TokenKind::KwFrom:
        return parseImportFrom(nullptr, start);
    case TokenKind::Name:
        if (*peek().identifier == m_typeKeyword && peek(1).kind == TokenKind::Name) {
            auto saved = checkpoint();
            Stmt* alias = parseTypeAlias();
            if (alias || m_errorIsFatal) [[likely]]
                return alias;
            // Whatever is wrong with it, it was looked at as an expression before that, with `type` a name.
            SyntaxErrorInfo error = m_error;
            bool errorIsGeneric = m_errorIsGeneric;
            if (diagnoseNamedExpressions(saved)) {
                m_error = error;
                m_errorIsGeneric = errorIsGeneric;
            }
            return nullptr;
        }
        if (*peek().identifier == m_lazyKeyword) {
            if (peek(1).kind == TokenKind::KwImport) {
                advance();
                return parseImport(true, start);
            }
            if (peek(1).kind == TokenKind::KwFrom) {
                const Token& lazyToken = advance();
                return parseImportFrom(&lazyToken, start);
            }
        }
        break;
    default:
        break;
    }
    return parseExpressionStatement();
}

// `pass if x else y`
std::nullptr_t Parser::diagnoseStatementBeforeIf()
{
    const Token statement = advance();
    SetForScope diagnosing(m_diagnosing, true);
    advance();
    if (parseDisjunction() && accept(TokenKind::KwElse) && parseSimpleStatement())
        return syntaxErrorAt(statement, "expected expression before 'if', but statement is given"_s);
    if (failed() && !m_errorIsGeneric)
        return nullptr;
    if (!m_errorIsFatal)
        m_error = { };
    return invalidSyntax();
}

static std::optional<Operator> augmentedOperator(TokenKind kind)
{
    switch (kind) {
    case TokenKind::PlusEqual: return Operator::Add;
    case TokenKind::MinEqual: return Operator::Sub;
    case TokenKind::StarEqual: return Operator::Mult;
    case TokenKind::AtEqual: return Operator::MatMult;
    case TokenKind::SlashEqual: return Operator::Div;
    case TokenKind::PercentEqual: return Operator::Mod;
    case TokenKind::AmperEqual: return Operator::BitAnd;
    case TokenKind::VBarEqual: return Operator::BitOr;
    case TokenKind::CircumflexEqual: return Operator::BitXor;
    case TokenKind::LeftShiftEqual: return Operator::LShift;
    case TokenKind::RightShiftEqual: return Operator::RShift;
    case TokenKind::DoubleStarEqual: return Operator::Pow;
    case TokenKind::DoubleSlashEqual: return Operator::FloorDiv;
    default: return std::nullopt;
    }
}

// An assignment of any kind, or an expression: which one shows only after the first expression.
Stmt* Parser::parseExpressionStatement()
{
    Position start = startPosition();
    TokenKind firstKind = kind();
    Checkpoint statementStart = checkpoint();

    Expr* first = parseAnnotatedRHS();
    if (!first) {
        if (m_errorIsGeneric && !m_errorIsFatal && !m_acceptsPrefix) {
            SyntaxErrorInfo unexplained = m_error;
            if (diagnoseNamedExpressions(statementStart)) {
                m_error = unexplained;
                m_errorIsGeneric = true;
            }
        }
        return nullptr;
    }

    if (at(TokenKind::Colon) && !first->is<Yield>() && !first->is<YieldFrom>())
        return parseAnnotatedAssignment(start, first, firstKind == TokenKind::Name);

    if (at(TokenKind::Equal)) {
        ExprVector targets;
        Expr* value = first;
        while (at(TokenKind::Equal)) {
            if ((value->is<Yield>() || value->is<YieldFrom>()) && !value->parenthesized)
                return syntaxErrorAt(value->range, "assignment to yield expression not possible"_s);
            if (findInvalidTarget(value, TargetKind::Star))
                return diagnoseInvalidAssignment(value, targets.isEmpty(), statementStart);
            advance();
            targets.append(value);
            value = parseAnnotatedRHS();
            if (!value)
                return nullptr;
        }
        for (Expr* target : targets)
            setContext(target, ExprContext::Store);
        // The statement reaches to the end of its type comment.
        StringImpl* typeComment = acceptTypeComment();
        auto* assign = make<Assign>(rangeFrom(start));
        assign->targets = m_arena.copy(targets);
        assign->value = value;
        assign->typeComment = typeComment;
        return assign;
    }

    if (auto op = augmentedOperator(kind())) {
        switch (first->kind) {
        case ExprKind::Name:
        case ExprKind::Attribute:
        case ExprKind::Subscript:
            break;
        default:
            // A `yield` that is not in parentheses is a statement on its own.
            if ((first->is<Yield>() || first->is<YieldFrom>()) && !first->parenthesized)
                return invalidSyntax();
            {
                SetForScope diagnosing(m_diagnosing, true);
                SetForScope acceptsPrefix(m_acceptsPrefix, true);
                auto saved = checkpoint();
                advance();
                if (!parseAnnotatedRHS()) {
                    if (m_errorIsFatal || !m_errorIsGeneric)
                        return nullptr;
                    rewind(saved);
                    return invalidSyntax();
                }
            }
            return syntaxErrorAt(first->range, makeString('\'', describe(first), "' is an illegal expression for augmented assignment"_s));
        }
        advance();
        if (*op == Operator::MatMult && !checkVersion(5, "The '@' operator is"_s))
            return nullptr;
        Expr* value = parseAnnotatedRHS();
        if (!value)
            return nullptr;
        setContext(first, ExprContext::Store);
        auto* assign = make<AugAssign>(rangeFrom(start));
        assign->target = first;
        assign->op = *op;
        assign->value = value;
        return assign;
    }

    if (at(TokenKind::ColonEqual)) {
        if (!diagnoseNamedExpressions(statementStart))
            return nullptr;
        return invalidSyntax();
    }

    auto* statement = make<ExprStmt>(rangeFrom(start));
    statement->value = first;
    return statement;
}

// Before anything else is said about a statement that looked like an assignment and is none, it is tried as
// `a, b: annotation` (see invalid_assignment in the grammar: after the first comma, the elements need no commas between them).
// On the way each element is looked at as a named_expression, which takes `a = b` and `a.b := c` for mistakes.
// False: something was found.
bool Parser::diagnoseNamedExpressions(const Checkpoint& statementStart)
{
    SetForScope diagnosing(m_diagnosing, true);
    SetForScope acceptsPrefix(m_acceptsPrefix, true);
    Checkpoint here = checkpoint();
    rewind(statementStart);
    Expr* first = parseStarNamedExpression();
    if (first && accept(TokenKind::Comma)) {
        while ((atStartOfExpression() || at(TokenKind::Star)) && parseStarNamedExpression())
            accept(TokenKind::Comma);
        if (!failed() && accept(TokenKind::Colon) && parseExpression())
            syntaxErrorAt(first->range, "only single target (not tuple) can be annotated"_s);
    }
    if (failed() && (m_errorIsFatal || !m_errorIsGeneric))
        return false;
    rewind(here);
    return true;
}

// `target`, which cannot be assigned to, is followed by '='.
std::nullptr_t Parser::diagnoseInvalidAssignment(Expr* target, bool isFirstTarget, const Checkpoint& statementStart)
{
    SetForScope diagnosing(m_diagnosing, true);
    // A lone '=' after something that can be compared was probably meant as '=='.
    if (isFirstTarget && isAtBitwiseOrLevel(target) && !startsLikeATarget(target, m_tokens[statementStart.index].startPosition)) {
        advance();
        if (atStartOfExpression() && !at(TokenKind::KwNot) && !at(TokenKind::KwLambda)) {
            auto saved = checkpoint();
            bool parsed = !!parseBitwiseOr();
            if (m_errorIsFatal)
                return nullptr;
            if (!parsed)
                rewind(saved);
            if (!parsed || (!at(TokenKind::Equal) && !at(TokenKind::ColonEqual)))
                return syntaxErrorAt(target->range, makeString("cannot assign to "_s, describe(target), " here. Maybe you meant '==' instead of '='?"_s));
        }
    }
    if (!diagnoseNamedExpressions(statementStart))
        return nullptr;
    return raiseInvalidTarget(target, TargetKind::Star);
}

Stmt* Parser::parseAnnotatedAssignment(Position start, Expr* target, bool startsWithName)
{
    switch (target->kind) {
    case ExprKind::Name:
    case ExprKind::Attribute:
    case ExprKind::Subscript:
        break;
    case ExprKind::Starred:
        return invalidSyntax();
    default: {
        SetForScope diagnosing(m_diagnosing, true);
        advance();
        if (!parseExpression())
            return nullptr;
        if (target->is<List>())
            return syntaxErrorAt(target->range, "only single target (not list) can be annotated"_s);
        // Of a tuple without parentheses, it is the first element that is pointed at.
        if (target->is<Tuple>())
            return syntaxErrorAt(target->parenthesized ? target->range : target->as<Tuple>()->elts.first()->range, "only single target (not tuple) can be annotated"_s);
        return syntaxErrorAt(target->range, "illegal target for annotation"_s);
    }
    }

    advance();
    Expr* annotation = parseExpression();
    if (!annotation)
        return nullptr;
    Expr* value = nullptr;
    if (accept(TokenKind::Equal)) {
        value = parseAnnotatedRHS();
        if (!value)
            return nullptr;
    }
    if (!checkVersion(6, target->is<NameExpr>() && startsWithName ? "Variable annotation syntax is"_s : "Variable annotations syntax is"_s))
        return nullptr;
    setContext(target, ExprContext::Store);
    auto* assign = make<AnnAssign>(rangeFrom(start));
    assign->target = target;
    assign->annotation = annotation;
    assign->value = value;
    assign->simple = target->is<NameExpr>() && startsWithName;
    return assign;
}

StringImpl* Parser::acceptTypeComment()
{
    if (!at(TokenKind::TypeComment))
        return nullptr;
    return m_arena.retain(stringFromUTF8(textOf(advance())));
}

static bool endsSimpleStatement(TokenKind kind)
{
    return kind == TokenKind::Newline || kind == TokenKind::Semi || kind == TokenKind::EndMarker;
}

Stmt* Parser::parseReturn()
{
    Position start = startPosition();
    advance();
    Expr* value = nullptr;
    if (!endsSimpleStatement(kind())) {
        value = parseStarExpressions();
        if (!value)
            return nullptr;
    }
    auto* statement = make<Return>(rangeFrom(start));
    statement->value = value;
    return statement;
}

Stmt* Parser::parseRaise()
{
    Position start = startPosition();
    const Token raiseToken = advance();
    Expr* exception = nullptr;
    Expr* cause = nullptr;
    if (at(TokenKind::KwFrom))
        return syntaxErrorAt(rangeBetween(rangeOf(raiseToken), rangeOf(peek())), "did you forget an expression between 'raise' and 'from'?"_s);
    if (!endsSimpleStatement(kind())) {
        exception = parseExpression();
        if (!exception)
            return nullptr;
        if (at(TokenKind::KwFrom)) {
            const Token fromToken = advance();
            m_failedExpressionHasPrefix = false;
            cause = endsSimpleStatement(kind()) ? invalidSyntax() : parseExpression();
            if (!cause) {
                if (!m_errorIsGeneric || m_errorIsFatal || m_failedExpressionHasPrefix)
                    return nullptr;
                m_error = { };
                return syntaxErrorAt(fromToken, "did you forget an expression after 'from'?"_s);
            }
        }
    }
    auto* statement = make<Raise>(rangeFrom(start));
    statement->exc = exception;
    statement->cause = cause;
    return statement;
}

// 'del' ','.del_target+ [','] &(';' | NEWLINE)
Stmt* Parser::parseDelete()
{
    Position start = startPosition();
    advance();
    auto afterDel = checkpoint();
    ExprVector targets;
    // A target is a primary of a certain form.
    bool areTargets = true;
    for (;;) {
        Expr* target = parsePrimary();
        if (!target) {
            if (m_errorIsFatal || !m_errorIsGeneric)
                return nullptr;
            areTargets = false;
            break;
        }
        areTargets = !findInvalidTarget(target, TargetKind::Del);
        if (!areTargets)
            break;
        targets.append(target);
        if (!accept(TokenKind::Comma) || endsSimpleStatement(kind()))
            break;
    }
    if (!areTargets || !endsSimpleStatement(kind())) {
        // Read as expressions, they tell what is wrong.
        SetForScope diagnosing(m_diagnosing, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        rewind(afterDel);
        Expr* expressions = parseStarExpressions();
        if (expressions && findInvalidTarget(expressions, TargetKind::Del))
            return raiseInvalidTarget(expressions, TargetKind::Del);
        if (failed() && (m_errorIsFatal || !m_errorIsGeneric))
            return nullptr;
        m_error = { };
        return invalidSyntax();
    }
    for (Expr* target : targets)
        setContext(target, ExprContext::Del);
    auto* statement = make<Delete>(rangeFrom(start));
    statement->targets = m_arena.copy(targets);
    return statement;
}

Stmt* Parser::parseAssert()
{
    Position start = startPosition();
    advance();
    auto parseOperand = [&]() -> Expr* {
        Expr* operand = parseExpression();
        if (!operand)
            return nullptr;
        if (at(TokenKind::Equal) || at(TokenKind::ColonEqual)) {
            SetForScope diagnosing(m_diagnosing, true);
            bool isWalrus = at(TokenKind::ColonEqual);
            advance();
            Expr* other = parseExpression();
            if (!other)
                return nullptr;
            Range range = rangeBetween(operand->range, other->range);
            if (isWalrus)
                return syntaxErrorAt(range, "cannot use named expression without parentheses here"_s);
            return syntaxErrorAt(range, makeString("cannot assign to "_s, describe(operand), " here. Maybe you meant '==' instead of '='?"_s));
        }
        return operand;
    };
    Expr* test = parseOperand();
    if (!test)
        return nullptr;
    Expr* message = nullptr;
    if (accept(TokenKind::Comma)) {
        message = parseOperand();
        if (!message)
            return nullptr;
    }
    auto* statement = make<Assert>(rangeFrom(start));
    statement->test = test;
    statement->msg = message;
    return statement;
}

Stmt* Parser::parseGlobalOrNonlocal()
{
    Position start = startPosition();
    bool isGlobal = advance().kind == TokenKind::KwGlobal;
    Vector<Name, 4> names;
    do {
        Name name = expectName();
        if (!name)
            return nullptr;
        names.append(name);
    } while (accept(TokenKind::Comma));
    if (isGlobal) {
        auto* statement = make<Global>(rangeFrom(start));
        statement->names = m_arena.copy(names);
        return statement;
    }
    auto* statement = make<Nonlocal>(rangeFrom(start));
    statement->names = m_arena.copy(names);
    return statement;
}

// dotted_name, as one identifier with the dots in it.
Name Parser::parseDottedName()
{
    Name first = expectName();
    if (!first)
        return nullptr;
    if (!at(TokenKind::Dot))
        return first;
    StringBuilder builder;
    builder.append(first->string());
    while (accept(TokenKind::Dot)) {
        Name next = expectName();
        if (!next)
            return nullptr;
        builder.append('.', next->string());
    }
    String name = builder.toString();
    if (name.is8Bit())
        return &m_tree.identifiers.makeIdentifier(m_vm, name.span8());
    return &m_tree.identifiers.makeIdentifier(m_vm, name.span16());
}

// After `as` in an import. Anything but a name gets a message of its own.
static bool endsImportTarget(TokenKind kind)
{
    return kind == TokenKind::Comma || kind == TokenKind::RPar || kind == TokenKind::Semi || kind == TokenKind::Newline;
}

Stmt* Parser::parseImport(bool isLazy, Position start)
{
    const Token importToken = advance();
    if (at(TokenKind::Newline) && !isLazy)
        return syntaxErrorStartingFrom(peek().startPosition, "Expected one or more names after 'import'"_s);
    Vector<Alias*, 4> names;
    do {
        Position aliasStart = startPosition();
        Name name = parseDottedName();
        if (!name)
            return nullptr;
        Name asName = nullptr;
        if (accept(TokenKind::KwAs)) {
            if (!(at(TokenKind::Name) && endsImportTarget(peek(1).kind))) {
                SetForScope diagnosing(m_diagnosing, true);
                SetForScope acceptsPrefix(m_acceptsPrefix, true);
                Expr* target = parseExpression();
                if (!target)
                    return nullptr;
                return syntaxErrorAt(target->range, makeString("cannot use "_s, describe(target), " as import target"_s));
            }
            asName = advance().identifier;
        }
        auto* alias = make<Alias>();
        alias->name = name;
        alias->asname = asName;
        alias->range = rangeFrom(aliasStart);
        names.append(alias);
    } while (accept(TokenKind::Comma));
    if (at(TokenKind::KwFrom) && peek(1).kind == TokenKind::Name && !isLazy) {
        SetForScope diagnosing(m_diagnosing, true);
        advance();
        if (!parseDottedName())
            return nullptr;
        return syntaxErrorStartingFrom(importToken.startPosition, "Did you mean to use 'from ... import ...' instead?"_s);
    }
    auto* statement = make<Import>(rangeFrom(start));
    statement->names = m_arena.copy(names);
    statement->isLazy = isLazy;
    return statement;
}

Alias* Parser::parseImportFromAsName()
{
    Position start = startPosition();
    Name name = expectName();
    if (!name)
        return nullptr;
    Name asName = nullptr;
    if (accept(TokenKind::KwAs)) {
        if (!(at(TokenKind::Name) && endsImportTarget(peek(1).kind))) {
            SetForScope diagnosing(m_diagnosing, true);
            SetForScope acceptsPrefix(m_acceptsPrefix, true);
            Expr* target = parseExpression();
            if (!target)
                return nullptr;
            return syntaxErrorAt(target->range, makeString("cannot use "_s, describe(target), " as import target"_s));
        }
        asName = advance().identifier;
    }
    auto* alias = make<Alias>();
    alias->name = name;
    alias->asname = asName;
    alias->range = rangeFrom(start);
    return alias;
}

Stmt* Parser::parseImportFrom(const Token* lazyTokenPointer, Position start)
{
    std::optional<Token> lazyToken;
    if (lazyTokenPointer)
        lazyToken = *lazyTokenPointer;
    advance();

    int level = 0;
    Position lastDotEnd;
    for (;;) {
        if (at(TokenKind::Dot))
            level += 1;
        else if (at(TokenKind::Ellipsis))
            level += 3;
        else
            break;
        lastDotEnd = advance().endPosition;
    }

    Name module = nullptr;
    Range moduleRange;
    if (!level || at(TokenKind::Name)) {
        Position moduleStart = startPosition();
        module = parseDottedName();
        if (!module)
            return nullptr;
        moduleRange = rangeFrom(moduleStart);
    }

    if (module && atSoftKeyword(m_lazyKeyword) && peek(1).kind == TokenKind::KwImport)
        return syntaxErrorAt(peek(), "use 'lazy from ... ' instead of 'from ... lazy import'"_s);
    if (!expect(TokenKind::KwImport))
        return nullptr;

    Vector<Alias*, 4> names;
    if (at(TokenKind::Star)) {
        Position starStart = startPosition();
        advance();
        auto* alias = make<Alias>();
        alias->name = &m_tree.identifiers.makeIdentifier(m_vm, "*"_span8);
        alias->asname = nullptr;
        alias->range = rangeFrom(starStart);
        names.append(alias);
    } else if (accept(TokenKind::LPar)) {
        for (;;) {
            Alias* alias = parseImportFromAsName();
            if (!alias)
                return nullptr;
            names.append(alias);
            if (!accept(TokenKind::Comma) || at(TokenKind::RPar))
                break;
        }
        if (!expect(TokenKind::RPar))
            return nullptr;
    } else {
        if (at(TokenKind::Newline))
            return syntaxErrorStartingFrom(peek().startPosition, "Expected one or more names after 'import'"_s);
        for (;;) {
            Alias* alias = parseImportFromAsName();
            if (!alias)
                return nullptr;
            names.append(alias);
            if (!accept(TokenKind::Comma))
                break;
            if (at(TokenKind::Newline))
                return syntaxError("trailing comma not allowed without surrounding parentheses"_s);
        }
    }

    if (module && !level && module->string() == "__future__"_s) {
        if (lazyToken)
            return syntaxErrorAt(*lazyToken, "lazy from __future__ import is not allowed"_s);
        for (Alias* alias : names) {
            if (alias->name->string() == "barry_as_FLUFL"_s) {
                m_flags.add(ParserFlag::BarryAsBDFL);
                m_lexer.setBarryAsBDFL();
            }
        }
    } else if (module && level && !lazyToken && *module == m_lazyKeyword && lastDotEnd != moduleRange.start()) {
        // `from . lazy import x` reads like a misplaced `lazy`, and means `from .lazy import x`.
        StringBuilder dots;
        for (int i = 0; i < level; ++i)
            dots.append('.');
        String dotsString = dots.toString();
        if (!warn(makeString("'from "_s, dotsString, " lazy import' is the same as 'from "_s, dotsString, "lazy import'; did you mean 'lazy from "_s, dotsString, " import'?"_s), moduleRange))
            return nullptr;
    }

    auto* statement = make<ImportFrom>(rangeFrom(start));
    statement->module = module;
    statement->names = m_arena.copy(names);
    statement->level = level;
    statement->isLazy = !!lazyToken;
    return statement;
}

// "type" NAME [type_params] '=' expression
Stmt* Parser::parseTypeAlias()
{
    Position start = startPosition();
    advance();
    NameExpr* name = makeName(advance(), ExprContext::Store);
    Seq<TypeParam*> typeParams;
    // CPython's parser has by now tried `type` as a name, and has read that far.
    if (at(TokenKind::LSqb) && peek(1).kind == TokenKind::RSqb)
        peek(2);
    if (at(TokenKind::LSqb) && !parseTypeParams(typeParams))
        return nullptr;
    if (!expect(TokenKind::Equal))
        return nullptr;
    Expr* value = parseExpression();
    if (!value)
        return nullptr;
    if (!checkVersion(12, "Type statement is"_s))
        return nullptr;
    auto* statement = make<TypeAlias>(rangeFrom(start));
    statement->name = name;
    statement->typeParams = typeParams;
    statement->value = value;
    return statement;
}

// '[' ','.type_param+ [','] ']'
bool Parser::parseTypeParams(Seq<TypeParam*>& result)
{
    advance();
    if (at(TokenKind::RSqb)) {
        syntaxErrorStartingFrom(peek().startPosition, "Type parameter list cannot be empty"_s);
        return false;
    }
    Vector<TypeParam*, 4> params;
    for (;;) {
        Position start = startPosition();
        auto* param = make<TypeParam>();
        param->bound = nullptr;
        param->defaultValue = nullptr;
        if (accept(TokenKind::Star))
            param->kind = TypeParamKind::TypeVarTuple;
        else if (accept(TokenKind::DoubleStar))
            param->kind = TypeParamKind::ParamSpec;
        else
            param->kind = TypeParamKind::TypeVar;
        param->name = expectName();
        if (!param->name)
            return false;
        if (at(TokenKind::Colon)) {
            Position colon = startPosition();
            advance();
            Expr* bound = parseExpression();
            if (!bound)
                return false;
            if (param->kind != TypeParamKind::TypeVar) {
                syntaxErrorStartingFrom(colon, makeString("cannot use "_s, bound->is<Tuple>() ? "constraints"_s : "bound"_s, " with "_s, param->kind == TypeParamKind::TypeVarTuple ? "TypeVarTuple"_s : "ParamSpec"_s));
                return false;
            }
            param->bound = bound;
        }
        if (accept(TokenKind::Equal)) {
            param->defaultValue = param->kind == TypeParamKind::TypeVarTuple ? parseStarExpression() : parseExpression();
            if (!param->defaultValue || !checkVersion(13, "Type parameter defaults are"_s))
                return false;
        }
        param->range = rangeFrom(start);
        params.append(param);
        if (!accept(TokenKind::Comma) || at(TokenKind::RSqb))
            break;
    }
    if (!expect(TokenKind::RSqb) || !checkVersion(12, "Type parameter lists are"_s))
        return false;
    result = m_arena.copy(params);
    return true;
}

// block: NEWLINE INDENT statements DEDENT | simple_stmts
bool Parser::parseBlock(Seq<Stmt*>& result, ASCIILiteral what, const Token& opener)
{
    int openerLine = opener.startPosition.line;
    StmtVector body;
    if (accept(TokenKind::Newline)) {
        if (!accept(TokenKind::Indent)) {
            if (what.isNull())
                indentationError("expected an indented block"_s);
            else
                indentationError(makeString("expected an indented block after "_s, what, " on line "_s, openerLine));
            return false;
        }
        do {
            if (!parseStatement(body))
                return false;
        } while (!at(TokenKind::Dedent) && !at(TokenKind::EndMarker));
        if (!expect(TokenKind::Dedent))
            return false;
    } else if (!parseSimpleStatements(body))
        return false;
    result = m_arena.copy(body);
    return true;
}

// The ':' that ends the header of a compound statement. A header cut short by the end of the line gets a message for that.
#define EXPECT_COLON() \
    do { \
        if (!accept(TokenKind::Colon)) { \
            if (at(TokenKind::Newline)) \
                return syntaxError("expected ':'"_s); \
            return invalidSyntax(); \
        } \
    } while (false)

// else_block: 'else' &&':' block
bool Parser::parseElse(Seq<Stmt*>& result)
{
    const Token elseToken = advance();
    if (!expectForced(TokenKind::Colon, ":"_s, ForcedBy::RuleThatIsAlwaysTried))
        return false;
    if (!parseBlock(result, "'else' statement"_s, elseToken))
        return false;
    if (at(TokenKind::KwElif)) {
        syntaxError("'elif' block follows an 'else' block"_s);
        return false;
    }
    return true;
}

Stmt* Parser::parseIf()
{
    Position start = startPosition();
    const Token ifToken = advance();
    bool isElif = ifToken.kind == TokenKind::KwElif;
    Expr* test = parseNamedExpression();
    if (!test)
        return nullptr;
    EXPECT_COLON();
    Seq<Stmt*> body;
    if (!parseBlock(body, isElif ? "'elif' statement"_s : "'if' statement"_s, ifToken))
        return nullptr;
    *m_statementIsCompleteSoFar = true;
    Seq<Stmt*> orelse;
    if (at(TokenKind::KwElif)) {
        Stmt* nested = parseIf();
        if (!nested)
            return nullptr;
        orelse = m_arena.single(nested);
    } else if (at(TokenKind::KwElse) && !parseElse(orelse))
        return nullptr;
    auto* statement = make<If>(rangeFrom(start));
    statement->test = test;
    statement->body = body;
    statement->orelse = orelse;
    return statement;
}

Stmt* Parser::parseWhile()
{
    Position start = startPosition();
    const Token whileToken = advance();
    Expr* test = parseNamedExpression();
    if (!test)
        return nullptr;
    EXPECT_COLON();
    Seq<Stmt*> body;
    if (!parseBlock(body, "'while' statement"_s, whileToken))
        return nullptr;
    *m_statementIsCompleteSoFar = true;
    Seq<Stmt*> orelse;
    if (at(TokenKind::KwElse) && !parseElse(orelse))
        return nullptr;
    auto* statement = make<While>(rangeFrom(start));
    statement->test = test;
    statement->body = body;
    statement->orelse = orelse;
    return statement;
}

Stmt* Parser::parseFor(bool isAsync, Position start)
{
    const Token forToken = advance();
    Expr* target = parseForTargets(false);
    if (!target)
        return nullptr;
    advance();
    Expr* iter = parseStarExpressions();
    if (!iter)
        return nullptr;
    EXPECT_COLON();
    StringImpl* typeComment = acceptTypeComment();
    Seq<Stmt*> body;
    if (!parseBlock(body, "'for' statement"_s, forToken))
        return nullptr;
    *m_statementIsCompleteSoFar = true;
    Seq<Stmt*> orelse;
    if (at(TokenKind::KwElse) && !parseElse(orelse))
        return nullptr;
    if (isAsync && !checkVersion(5, "Async for loops are"_s))
        return nullptr;
    auto* statement = make<For>(isAsync, rangeFrom(start));
    statement->target = target;
    statement->iter = iter;
    statement->body = body;
    statement->orelse = orelse;
    statement->typeComment = typeComment;
    return statement;
}

// ','.with_item+, and in parentheses a trailing comma.
bool Parser::parseWithItems(Vector<WithItem*, 4>& items, bool parenthesized)
{
    for (;;) {
        Expr* context = parseExpression();
        if (!context)
            return false;
        Expr* target = nullptr;
        if (accept(TokenKind::KwAs)) {
            auto saved = checkpoint();
            target = parseStarTarget();
            bool ended = at(TokenKind::Comma) || at(TokenKind::RPar) || at(TokenKind::Colon);
            if (target && !parenthesized && at(TokenKind::Newline)) {
                syntaxError("expected ':'"_s);
                return false;
            }
            if (!target || !ended) {
                if (m_errorIsFatal)
                    return false;
                // Not a target. If it is an expression, say what is wrong with it.
                SetForScope diagnosing(m_diagnosing, true);
                rewind(saved);
                Expr* expression = parseExpression();
                if (expression && (at(TokenKind::Comma) || at(TokenKind::RPar) || at(TokenKind::Colon)))
                    raiseInvalidTarget(expression, TargetKind::Star);
                if (!failed())
                    invalidSyntax();
                return false;
            }
        }
        auto* item = make<WithItem>();
        item->contextExpr = context;
        item->optionalVars = target;
        items.append(item);
        if (!at(TokenKind::Comma))
            return true;
        const Token comma = advance();
        if (parenthesized && at(TokenKind::RPar))
            return true;
        if (!parenthesized && at(TokenKind::Colon)) {
            syntaxErrorAt(comma, "the last 'with' item has a trailing comma"_s);
            return false;
        }
    }
}

Stmt* Parser::parseWith(bool isAsync, Position start)
{
    const Token withToken = advance();
    Vector<WithItem*, 4> items;

    // `with (a as b, c):` has its items in parentheses; `with (a, b):` and `with (a) as b:` have an expression there.
    // The first reading is the one to prefer, and holds if a ':' follows the ')'.
    bool parsed = false;
    if (at(TokenKind::LPar)) {
        auto saved = checkpoint();
        advance();
        bool newlineInsteadOfColon = false;
        if (parseWithItems(items, true) && accept(TokenKind::RPar)) {
            parsed = at(TokenKind::Colon);
            newlineInsteadOfColon = at(TokenKind::Newline);
        }
        if (!parsed) {
            if (m_errorIsFatal || (failed() && !m_errorIsGeneric))
                return nullptr;
            rewind(saved);
            items.clear();
            if (!parseWithItems(items, false)) {
                if (newlineInsteadOfColon && m_errorIsGeneric && !m_errorIsFatal) {
                    m_error = { };
                    syntaxError("expected ':'"_s);
                }
                return nullptr;
            }
            parsed = true;
        }
    }
    if (!parsed && !parseWithItems(items, false))
        return nullptr;

    EXPECT_COLON();
    StringImpl* typeComment = acceptTypeComment();
    Seq<Stmt*> body;
    if (!parseBlock(body, "'with' statement"_s, withToken))
        return nullptr;
    if (isAsync && !checkVersion(5, "Async with statements are"_s))
        return nullptr;
    auto* statement = make<With>(isAsync, rangeFrom(start));
    statement->items = m_arena.copy(items);
    statement->body = body;
    statement->typeComment = typeComment;
    return statement;
}

Stmt* Parser::parseTry()
{
    Position start = startPosition();
    const Token tryToken = advance();
    if (!expectForced(TokenKind::Colon, ":"_s, ForcedBy::RuleThatIsAlwaysTried))
        return nullptr;
    Seq<Stmt*> body;
    if (!parseBlock(body, "'try' statement"_s, tryToken))
        return nullptr;

    Vector<ExceptHandler*, 4> handlers;
    bool isStar = false;
    while (at(TokenKind::KwExcept)) {
        Position handlerStart = startPosition();
        const Token exceptToken = advance();
        bool handlerIsStar = false;
        if (at(TokenKind::Star)) {
            const Token starToken = advance();
            handlerIsStar = true;
            if (!handlers.isEmpty() && !isStar) {
                SetForScope diagnosing(m_diagnosing, true);
                SetForScope acceptsPrefix(m_acceptsPrefix, true);
                if (parseExpression() && (!accept(TokenKind::KwAs) || accept(TokenKind::Name)) && at(TokenKind::Colon))
                    return syntaxErrorAt(rangeBetween(rangeOf(exceptToken), rangeOf(starToken)), "cannot have both 'except' and 'except*' on the same 'try'"_s);
                return failed() && !m_errorIsGeneric ? nullptr : invalidSyntaxAfterAttempt();
            }
            if (at(TokenKind::Newline) || at(TokenKind::Colon))
                return syntaxError("expected one or more exception types"_s);
        } else if (!handlers.isEmpty() && isStar) {
            SetForScope diagnosing(m_diagnosing, true);
            SetForScope acceptsPrefix(m_acceptsPrefix, true);
            if (at(TokenKind::Colon) || (parseExpression() && (!accept(TokenKind::KwAs) || accept(TokenKind::Name)) && at(TokenKind::Colon)))
                return syntaxErrorAt(exceptToken, "cannot have both 'except' and 'except*' on the same 'try'"_s);
            return failed() && !m_errorIsGeneric ? nullptr : invalidSyntaxAfterAttempt();
        }
        isStar = handlerIsStar;

        Expr* type = nullptr;
        Name name = nullptr;
        bool hasBareTuple = false;
        if (!at(TokenKind::Colon)) {
            if (at(TokenKind::Newline))
                return syntaxError("expected ':'"_s);
            Position typeStart = startPosition();
            type = parseExpression();
            if (!type)
                return nullptr;
            if (at(TokenKind::Comma)) {
                // Several types need no parentheses, unless the exception is named.
                ExprVector types;
                types.append(type);
                while (accept(TokenKind::Comma)) {
                    if (at(TokenKind::Colon) || at(TokenKind::KwAs))
                        break;
                    Expr* next = parseExpression();
                    if (!next)
                        return nullptr;
                    types.append(next);
                }
                if (at(TokenKind::KwAs) && peek(1).kind == TokenKind::Name && peek(2).kind == TokenKind::Colon) {
                    advance();
                    advance();
                    advance();
                    return syntaxErrorStartingFrom(typeStart, "multiple exception types must be parenthesized when using 'as'"_s);
                }
                type = makeTuple(types, rangeFrom(typeStart));
                hasBareTuple = true;
            } else if (accept(TokenKind::KwAs)) {
                if (at(TokenKind::Name) && (peek(1).kind == TokenKind::Colon || peek(1).kind == TokenKind::Newline))
                    name = advance().identifier;
                else {
                    SetForScope diagnosing(m_diagnosing, true);
                    Expr* target = parseExpression();
                    if (!target)
                        return nullptr;
                    if (!accept(TokenKind::Colon))
                        return invalidSyntax();
                    Seq<Stmt*> ignored;
                    if (!parseBlock(ignored, { }, exceptToken))
                        return nullptr;
                    return syntaxErrorAt(target->range, makeString("cannot use except"_s, handlerIsStar ? "*"_s : ""_s, " statement with "_s, describe(target)));
                }
            }
        }
        EXPECT_COLON();
        auto* handler = make<ExceptHandler>();
        if (!parseBlock(handler->body, handlerIsStar ? "'except*' statement"_s : "'except' statement"_s, exceptToken))
            return nullptr;
        if (hasBareTuple && !checkVersion(14, "except expressions without parentheses are"_s))
            return nullptr;
        handler->type = type;
        handler->name = name;
        handler->range = rangeFrom(handlerStart);
        handlers.append(handler);
        *m_statementIsCompleteSoFar = true;
    }

    Seq<Stmt*> orelse;
    if (!handlers.isEmpty() && at(TokenKind::KwElse) && !parseElse(orelse))
        return nullptr;

    Seq<Stmt*> finalbody;
    if (at(TokenKind::KwFinally)) {
        const Token finallyToken = advance();
        if (!expectForced(TokenKind::Colon, ":"_s, ForcedBy::RuleThatIsAlwaysTried))
            return nullptr;
        if (!parseBlock(finalbody, "'finally' statement"_s, finallyToken))
            return nullptr;
    } else if (handlers.isEmpty())
        return syntaxError("expected 'except' or 'finally' block"_s);

    if (isStar && !checkVersion(11, "Exception groups are"_s))
        return nullptr;
    auto* statement = make<Try>(isStar, rangeFrom(start));
    statement->body = body;
    statement->handlers = m_arena.copy(handlers);
    statement->orelse = orelse;
    statement->finalbody = finalbody;
    return statement;
}

// decorators: ('@' named_expression NEWLINE)+
Stmt* Parser::parseDecorated()
{
    ExprVector decorators;
    while (accept(TokenKind::At)) {
        Expr* decorator = parseNamedExpression();
        if (!decorator)
            return nullptr;
        decorators.append(decorator);
        if (!expect(TokenKind::Newline))
            return nullptr;
    }
    Position start = startPosition();
    switch (kind()) {
    case TokenKind::KwDef:
        return parseFunctionDef(false, start, m_arena.copy(decorators));
    case TokenKind::KwClass:
        return parseClassDef(m_arena.copy(decorators));
    case TokenKind::KwAsync:
        if (peek(1).kind == TokenKind::KwDef) {
            advance();
            return parseFunctionDef(true, start, m_arena.copy(decorators));
        }
        [[fallthrough]];
    default:
        return invalidSyntax();
    }
}

Stmt* Parser::parseFunctionDef(bool isAsync, Position start, Seq<Expr*> decorators)
{
    const Token defToken = advance();
    Name name = expectName();
    if (!name)
        return nullptr;
    // What is optional and does not parse is not there, and then the token that has to come next is missing.
    auto missingAfterOptionalPart = [&](const Token& startOfPart, ASCIILiteral message) -> std::nullptr_t {
        if (m_errorIsFatal || !m_errorIsGeneric)
            return nullptr;
        m_error = { };
        m_errorIsGeneric = false;
        m_errorIsFinal = false;
        return syntaxErrorAt(startOfPart, message);
    };
    Seq<TypeParam*> typeParams;
    if (at(TokenKind::LSqb)) {
        const Token open = peek();
        if (!parseTypeParams(typeParams))
            return missingAfterOptionalPart(open, "expected '('"_s);
    }
    if (!expectForced(TokenKind::LPar, "("_s, ForcedBy::RuleThatExplainsErrors))
        return nullptr;
    Arguments* arguments = parseParameters(false);
    if (!arguments)
        return nullptr;
    if (!expect(TokenKind::RPar))
        return nullptr;
    Expr* returns = nullptr;
    if (at(TokenKind::RArrow)) {
        const Token arrow = advance();
        auto afterArrow = checkpoint();
        returns = parseExpression();
        if (!returns && m_errorIsGeneric && !m_errorIsFatal) {
            // As much as is an expression is the annotation.
            SetForScope diagnosing(m_diagnosing, true);
            SetForScope acceptsPrefix(m_acceptsPrefix, true);
            rewind(afterArrow);
            returns = parseExpression();
        }
        if (!returns)
            return missingAfterOptionalPart(arrow, "expected ':'"_s);
    }
    if (!expectForced(TokenKind::Colon, ":"_s, ForcedBy::RuleThatExplainsErrors))
        return nullptr;

    // func_type_comment: on the line of the ':', or alone on the next one, before the indented block.
    StringImpl* typeComment = nullptr;
    if (at(TokenKind::TypeComment)) {
        if (peek(1).kind == TokenKind::Newline && peek(2).kind == TokenKind::TypeComment && peek(3).kind == TokenKind::Newline && peek(4).kind == TokenKind::Indent) {
            syntaxError("Cannot have two type comments on def"_s);
            return nullptr;
        }
        typeComment = acceptTypeComment();
    } else if (at(TokenKind::Newline) && peek(1).kind == TokenKind::TypeComment && peek(2).kind == TokenKind::Newline && peek(3).kind == TokenKind::Indent) {
        advance();
        typeComment = acceptTypeComment();
    }

    Seq<Stmt*> body;
    if (!parseBlock(body, "function definition"_s, defToken))
        return nullptr;
    if (isAsync && !checkVersion(5, "Async functions are"_s))
        return nullptr;
    auto* statement = make<FunctionDef>(isAsync, rangeFrom(start));
    statement->name = name;
    statement->args = arguments;
    statement->body = body;
    statement->decoratorList = decorators;
    statement->returns = returns;
    statement->typeComment = typeComment;
    statement->typeParams = typeParams;
    return statement;
}

Stmt* Parser::parseClassDef(Seq<Expr*> decorators)
{
    Position start = startPosition();
    const Token classToken = advance();
    Name name = expectName();
    if (!name)
        return nullptr;
    Seq<TypeParam*> typeParams;
    if (at(TokenKind::LSqb) && !parseTypeParams(typeParams))
        return nullptr;
    Seq<Expr*> bases;
    Seq<Keyword*> keywords;
    if (at(TokenKind::LPar)) {
        Position open = startPosition();
        advance();
        Expr* soleGenerator = nullptr;
        size_t firstIndex = m_index;
        if (!parseArguments(bases, keywords, open, soleGenerator, false))
            return nullptr;
        size_t forTokenIndex = firstIndex;
        while (soleGenerator && m_tokens[forTokenIndex].kind != TokenKind::KwFor && m_tokens[forTokenIndex].kind != TokenKind::KwAsync)
            ++forTokenIndex;
        // Not a call: a generator expression needs parentheses of its own.
        if (soleGenerator)
            return syntaxErrorAt(rangeOf(m_tokens[forTokenIndex]), "invalid syntax"_s);
    }
    EXPECT_COLON();
    Seq<Stmt*> body;
    if (!parseBlock(body, "class definition"_s, classToken))
        return nullptr;
    auto* statement = make<ClassDef>(rangeFrom(start));
    statement->name = name;
    statement->bases = bases;
    statement->keywords = keywords;
    statement->body = body;
    statement->decoratorList = decorators;
    statement->typeParams = typeParams;
    return statement;
}

Arguments* Parser::emptyArguments()
{
    auto* arguments = make<Arguments>();
    arguments->vararg = nullptr;
    arguments->kwarg = nullptr;
    return arguments;
}

// The parameters of a `def`, up to the ')', or of a `lambda`, up to the ':'.
Arguments* Parser::parseParameters(bool isLambda)
{
    TokenKind close = isLambda ? TokenKind::Colon : TokenKind::RPar;
    Arguments* arguments = emptyArguments();
    if (at(close))
        return arguments;

    Vector<Arg*, 8> positionalOnly;
    Vector<Arg*, 8> positional;
    ExprVector defaults;
    Vector<Arg*, 8> keywordOnly;
    ExprVector keywordDefaults;
    bool sawSlash = false;
    bool sawStar = false;

    auto parseParameter = [&](bool allowStarAnnotation) -> Arg* {
        Position start = startPosition();
        Name name = expectName();
        if (!name)
            return nullptr;
        auto* arg = make<Arg>();
        arg->arg = name;
        arg->annotation = nullptr;
        arg->typeComment = nullptr;
        if (!isLambda && accept(TokenKind::Colon)) {
            arg->annotation = allowStarAnnotation ? parseStarExpression() : parseExpression();
            if (!arg->annotation)
                return nullptr;
        }
        arg->range = rangeFrom(start);
        return arg;
    };

    // A type comment follows the comma, or the last parameter.
    auto finishParameter = [&](Arg* arg) -> bool {
        bool hadComma = accept(TokenKind::Comma);
        if (!isLambda && arg && at(TokenKind::TypeComment))
            arg->typeComment = acceptTypeComment();
        if (!hadComma && !at(close)) {
            invalidSyntax();
            return false;
        }
        return true;
    };

    auto parseDefault = [&]() -> Expr* {
        const Token equal = advance();
        if (at(TokenKind::RPar) || at(TokenKind::Comma))
            return syntaxErrorAt(equal, "expected default value expression"_s);
        return parseExpression();
    };

    while (!at(close)) {
        if (at(TokenKind::Slash)) {
            const Token slash = advance();
            if (sawSlash)
                return syntaxErrorAt(slash, "/ may appear only once"_s);
            if (sawStar)
                return syntaxErrorAt(slash, "/ must be ahead of *"_s);
            if (positional.isEmpty()) {
                SetForScope diagnosing(m_diagnosing, true);
                if (!at(TokenKind::Comma)) {
                    --m_index;
                    return invalidSyntax();
                }
                return syntaxErrorAt(slash, "at least one parameter must precede /"_s);
            }
            sawSlash = true;
            positionalOnly = std::exchange(positional, { });
            if (at(TokenKind::Star))
                return syntaxErrorAt(peek(), "expected comma between / and *"_s);
            if (!finishParameter(nullptr))
                return nullptr;
            continue;
        }

        if (at(TokenKind::Star)) {
            const Token star = advance();
            if (sawStar) {
                // Said of a star that would do as the first.
                SetForScope diagnosing(m_diagnosing, true);
                bool isWellFormed = at(TokenKind::Comma);
                if (at(TokenKind::Name)) {
                    TokenKind next = peek(1).kind;
                    isWellFormed = next == TokenKind::Comma || next == close || (!isLambda && next == TokenKind::Colon);
                }
                if (isWellFormed)
                    return syntaxErrorAt(star, "* may appear only once"_s);
                return invalidSyntax();
            }
            sawStar = true;
            if (at(TokenKind::Comma)) {
                // A bare star: only keyword parameters follow, and at least one has to.
                TokenKind afterComma = peek(1).kind;
                if (afterComma == close || afterComma == TokenKind::DoubleStar) {
                    if (isLambda)
                        return syntaxError("named parameters must follow bare *"_s);
                    return syntaxErrorAt(star, "named parameters must follow bare *"_s);
                }
                advance();
                if (at(TokenKind::TypeComment))
                    return syntaxError("bare * has associated type comment"_s);
                continue;
            }
            if (at(close)) {
                if (isLambda)
                    return syntaxError("named parameters must follow bare *"_s);
                return syntaxErrorAt(star, "named parameters must follow bare *"_s);
            }
            arguments->vararg = parseParameter(true);
            if (!arguments->vararg)
                return nullptr;
            if (at(TokenKind::Equal)) {
                // `*args: *Ts` is a form of its own, which the rule that explains this does not know.
                if (arguments->vararg->annotation && arguments->vararg->annotation->is<Starred>())
                    return invalidSyntax();
                return syntaxErrorAt(peek(), "var-positional parameter cannot have default value"_s);
            }
            if (!finishParameter(arguments->vararg))
                return nullptr;
            continue;
        }

        if (accept(TokenKind::DoubleStar)) {
            arguments->kwarg = parseParameter(false);
            if (!arguments->kwarg)
                return nullptr;
            if (at(TokenKind::Equal))
                return syntaxErrorAt(peek(), "var-keyword parameter cannot have default value"_s);
            if (at(TokenKind::Comma)) {
                switch (peek(1).kind) {
                case TokenKind::Name: {
                    SetForScope diagnosing(m_diagnosing, true);
                    advance();
                    Arg* extra = parseParameter(false);
                    if (!extra)
                        return nullptr;
                    return syntaxErrorAt(extra->range, "parameters cannot follow var-keyword parameter"_s);
                }
                case TokenKind::Star:
                case TokenKind::DoubleStar:
                case TokenKind::Slash:
                    return syntaxErrorAt(peek(1), "parameters cannot follow var-keyword parameter"_s);
                default:
                    break;
                }
            }
            if (!finishParameter(arguments->kwarg))
                return nullptr;
            if (!at(close))
                return invalidSyntax();
            break;
        }

        if (at(TokenKind::LPar)) {
            // def f((a, b)): is Python 2.
            SetForScope diagnosing(m_diagnosing, true);
            auto saved = checkpoint();
            const Token open = advance();
            bool onlyNames = at(TokenKind::Name);
            while (onlyNames && accept(TokenKind::Name)) {
                if (!accept(TokenKind::Comma))
                    break;
            }
            if (onlyNames && at(TokenKind::RPar) && !sawStar && !sawSlash && defaults.isEmpty())
                return syntaxErrorAt(rangeBetween(rangeOf(open), rangeOf(peek())), isLambda ? "Lambda expression parameters cannot be parenthesized"_s : "Function parameters cannot be parenthesized"_s);
            rewind(saved);
            return invalidSyntax();
        }

        Arg* arg = parseParameter(false);
        if (!arg)
            return nullptr;
        Expr* defaultValue = nullptr;
        if (at(TokenKind::Equal)) {
            defaultValue = parseDefault();
            if (!defaultValue)
                return nullptr;
        }
        if (sawStar) {
            keywordOnly.append(arg);
            keywordDefaults.append(defaultValue);
        } else {
            if (defaultValue)
                defaults.append(defaultValue);
            else if (!defaults.isEmpty()) {
                if (!at(TokenKind::Comma) && !at(close) && !at(TokenKind::TypeComment))
                    return invalidSyntax();
                return syntaxErrorAt(arg->range, "parameter without a default follows parameter with a default"_s);
            }
            positional.append(arg);
        }
        if (!finishParameter(arg))
            return nullptr;
    }

    if (!positionalOnly.isEmpty() && !checkVersion(8, "Positional-only parameters are"_s))
        return nullptr;
    arguments->posonlyargs = m_arena.copy(positionalOnly);
    arguments->args = m_arena.copy(positional);
    arguments->defaults = m_arena.copy(defaults);
    arguments->kwonlyargs = m_arena.copy(keywordOnly);
    arguments->kwDefaults = m_arena.copy(keywordDefaults);
    return arguments;
}

// "match" subject_expr ':' NEWLINE INDENT case_block+ DEDENT. Null, and it is not known to be a match statement: `match` may be a name here.
Stmt* Parser::parseMatch(std::optional<Token>& newlineInsteadOfColon, bool& isMatchStatement)
{
    Position start = startPosition();
    const Token matchToken = advance();

    // What cannot start an expression settles it at once.
    if (!atStartOfExpression() && !at(TokenKind::Star))
        return invalidSyntax();

    // subject_expr: star_named_expression ',' star_named_expressions? | named_expression
    Position subjectStart = startPosition();
    Expr* subject = parseStarNamedExpression();
    if (!subject)
        return nullptr;
    if (at(TokenKind::Comma)) {
        ExprVector elements;
        elements.append(subject);
        while (accept(TokenKind::Comma)) {
            if (at(TokenKind::Colon))
                break;
            Expr* element = parseStarNamedExpression();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        subject = makeTuple(elements, rangeFrom(subjectStart));
    } else if (subject->is<Starred>())
        return invalidSyntax();

    if (at(TokenKind::Newline))
        newlineInsteadOfColon = peek();
    if (!accept(TokenKind::Colon) || !accept(TokenKind::Newline))
        return invalidSyntax();
    if (!accept(TokenKind::Indent)) {
        indentationError(makeString("expected an indented block after 'match' statement on line "_s, matchToken.startPosition.line));
        isMatchStatement = true;
        return nullptr;
    }
    if (!atSoftKeyword(m_caseKeyword))
        return invalidSyntax();

    // It is a match statement: from here on errors are errors.
    isMatchStatement = true;
    auto fatal = [&]() -> Stmt* {
        if (!failed())
            invalidSyntax();
        return nullptr;
    };

    Vector<MatchCase*, 4> cases;
    while (atSoftKeyword(m_caseKeyword)) {
        const Token caseToken = advance();
        auto* matchCase = make<MatchCase>();
        matchCase->pattern = parsePatterns();
        if (!matchCase->pattern)
            return fatal();
        matchCase->guard = nullptr;
        if (accept(TokenKind::KwIf)) {
            matchCase->guard = parseNamedExpression();
            if (!matchCase->guard)
                return fatal();
        }
        if (!accept(TokenKind::Colon)) {
            if (at(TokenKind::Newline))
                syntaxError("expected ':'"_s);
            return fatal();
        }
        if (!parseBlock(matchCase->body, "'case' statement"_s, caseToken))
            return fatal();
        cases.append(matchCase);
    }
    if (!accept(TokenKind::Dedent))
        return fatal();

    if (!checkVersion(10, "Pattern matching is"_s))
        return fatal();
    auto* statement = make<Match>(rangeFrom(start));
    statement->subject = subject;
    statement->cases = m_arena.copy(cases);
    return statement;
}

#undef EXPECT_COLON

// ---- Targets ----

ASCIILiteral Parser::describe(Expr* expression)
{
    switch (expression->kind) {
    case ExprKind::Attribute:
        return "attribute"_s;
    case ExprKind::Subscript:
        return "subscript"_s;
    case ExprKind::Starred:
        return "starred"_s;
    case ExprKind::Name:
        return "name"_s;
    case ExprKind::List:
        return "list"_s;
    case ExprKind::Tuple:
        return "tuple"_s;
    case ExprKind::Lambda:
        return "lambda"_s;
    case ExprKind::Call:
        return "function call"_s;
    case ExprKind::BoolOp:
    case ExprKind::BinOp:
    case ExprKind::UnaryOp:
        return "expression"_s;
    case ExprKind::GeneratorExp:
        return "generator expression"_s;
    case ExprKind::Yield:
    case ExprKind::YieldFrom:
        return "yield expression"_s;
    case ExprKind::Await:
        return "await expression"_s;
    case ExprKind::ListComp:
        return "list comprehension"_s;
    case ExprKind::SetComp:
        return "set comprehension"_s;
    case ExprKind::DictComp:
        return "dict comprehension"_s;
    case ExprKind::Dict:
        return "dict literal"_s;
    case ExprKind::Set:
        return "set display"_s;
    case ExprKind::JoinedStr:
    case ExprKind::FormattedValue:
        return "f-string expression"_s;
    case ExprKind::TemplateStr:
    case ExprKind::Interpolation:
        return "t-string expression"_s;
    case ExprKind::Constant:
        switch (expression->as<Constant>()->value.kind) {
        case ConstantValue::Kind::None:
            return "None"_s;
        case ConstantValue::Kind::False:
            return "False"_s;
        case ConstantValue::Kind::True:
            return "True"_s;
        case ConstantValue::Kind::Ellipsis:
            return "ellipsis"_s;
        default:
            return "literal"_s;
        }
    case ExprKind::Compare:
        return "comparison"_s;
    case ExprKind::IfExp:
        return "conditional expression"_s;
    case ExprKind::NamedExpr:
        return "named expression"_s;
    case ExprKind::Slice:
        return "slice"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether the grammar's bitwise_or derives the expression as it was written.
// Of an expression at that level, which starts at `start`: its text starts with a list, a tuple, a generator expression,
// True, False or None. (invalid_named_expression: `!(list|tuple|genexp|'True'|'None'|'False') bitwise_or '='`)
bool Parser::startsLikeATarget(Expr* expression, Position start)
{
    for (;;) {
        if (expression->parenthesized) {
            // In parentheses of its own besides, it is a group.
            bool isGroup = expression->range.lineno != start.line || expression->range.colOffset != start.column;
            return !isGroup && (expression->is<Tuple>() || expression->is<GeneratorExp>());
        }
        switch (expression->kind) {
        case ExprKind::Attribute:
            expression = expression->as<Attribute>()->value;
            break;
        case ExprKind::Subscript:
            expression = expression->as<Subscript>()->value;
            break;
        case ExprKind::Call:
            expression = expression->as<Call>()->func;
            break;
        case ExprKind::BinOp:
            expression = expression->as<BinOp>()->left;
            break;
        case ExprKind::List:
            return true;
        case ExprKind::Constant: {
            auto kind = expression->as<Constant>()->value.kind;
            return kind == ConstantValue::Kind::True || kind == ConstantValue::Kind::False || kind == ConstantValue::Kind::None;
        }
        default:
            return false;
        }
    }
}

bool Parser::isAtBitwiseOrLevel(Expr* expression)
{
    if (expression->parenthesized)
        return true;
    switch (expression->kind) {
    case ExprKind::Compare:
    case ExprKind::BoolOp:
    case ExprKind::IfExp:
    case ExprKind::Lambda:
    case ExprKind::NamedExpr:
    case ExprKind::Yield:
    case ExprKind::YieldFrom:
    case ExprKind::Starred:
        return false;
    case ExprKind::UnaryOp:
        return expression->as<UnaryOp>()->op != UnaryKind::Not;
    case ExprKind::Tuple:
        return false;
    default:
        return true;
    }
}

// The part of an expression that cannot be assigned to or deleted, if any.
Expr* Parser::findInvalidTarget(Expr* expression, TargetKind kind)
{
    switch (expression->kind) {
    case ExprKind::List:
        for (Expr* element : expression->as<List>()->elts) {
            if (Expr* invalid = findInvalidTarget(element, kind))
                return invalid;
        }
        return nullptr;
    case ExprKind::Tuple:
        for (Expr* element : expression->as<Tuple>()->elts) {
            if (Expr* invalid = findInvalidTarget(element, kind))
                return invalid;
        }
        return nullptr;
    case ExprKind::Starred:
        if (kind == TargetKind::Del)
            return expression;
        return findInvalidTarget(expression->as<Starred>()->value, kind);
    case ExprKind::Compare:
        // `for a in b` read as an expression is a comparison.
        if (kind == TargetKind::For) {
            auto* compare = expression->as<Compare>();
            if (compare->ops.first() == CompareKind::In)
                return findInvalidTarget(compare->left, kind);
            return nullptr;
        }
        return expression;
    case ExprKind::Name:
    case ExprKind::Subscript:
    case ExprKind::Attribute:
        return nullptr;
    default:
        return expression;
    }
}

std::nullptr_t Parser::raiseInvalidTarget(Expr* expression, TargetKind kind)
{
    Expr* invalid = findInvalidTarget(expression, kind);
    if (!invalid)
        return invalidSyntax();
    return syntaxErrorAt(invalid->range, makeString(kind == TargetKind::Del ? "cannot delete "_s : "cannot assign to "_s, describe(invalid)));
}

void Parser::setContext(Expr* expression, ExprContext context)
{
    switch (expression->kind) {
    case ExprKind::Name:
        expression->as<NameExpr>()->ctx = context;
        break;
    case ExprKind::Attribute:
        expression->as<Attribute>()->ctx = context;
        break;
    case ExprKind::Subscript:
        expression->as<Subscript>()->ctx = context;
        break;
    case ExprKind::Starred:
        expression->as<Starred>()->ctx = context;
        setContext(expression->as<Starred>()->value, context);
        break;
    case ExprKind::List:
        expression->as<List>()->ctx = context;
        for (Expr* element : expression->as<List>()->elts)
            setContext(element, context);
        break;
    case ExprKind::Tuple:
        expression->as<Tuple>()->ctx = context;
        for (Expr* element : expression->as<Tuple>()->elts)
            setContext(element, context);
        break;
    default:
        break;
    }
}

NameExpr* Parser::makeName(const Token& token, ExprContext context)
{
    auto* name = make<NameExpr>(rangeOf(token));
    name->id = token.identifier;
    name->ctx = context;
    return name;
}

Tuple* Parser::makeTuple(const ExprVector& elements, const Range& range)
{
    auto* tuple = make<Tuple>(range);
    tuple->elts = m_arena.copy(elements);
    tuple->ctx = ExprContext::Load;
    return tuple;
}

bool parse(VM& vm, ParseTree& tree, ParserHost& host, std::span<const uint8_t> bytes, const String& filename, ParseMode mode, OptionSet<ParserFlag> flags, bool sourceIsUTF8, SyntaxErrorInfo& error, int featureVersion)
{
    OptionSet<SourceText::Flag> sourceFlags;
    if (mode == ParseMode::File)
        sourceFlags.add(SourceText::Flag::EndWithNewline);
    if (sourceIsUTF8)
        sourceFlags.add(SourceText::Flag::IgnoreCookie);
    if (!tree.source.initialize(bytes, sourceFlags, filename, host, error)) {
        // CPython finds these things out while it reads a file, which is while it parses; in a string, before.
        error.hasMetadata = !filename.isNull() && error.isSyntaxError();
        return false;
    }
    Parser parser(vm, tree, host, mode, flags, featureVersion);
    if (parser.parse())
        return true;
    error = parser.error();
    return false;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
