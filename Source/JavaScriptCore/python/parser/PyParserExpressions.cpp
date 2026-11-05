/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyParser.h"

#include <wtf/dtoa.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

#define CHECK_RECURSION() \
    do { \
        if (!isSafeToRecurse()) [[unlikely]] \
            return stackOverflow(); \
    } while (false)

// What follows stands for one of the rules of the grammar that are only there to explain errors. Where nothing is to be diagnosed
// they are not tried, so nothing is read on their account either.
#define DIAGNOSE() \
    if (m_quiet) \
        return invalidSyntax(); \
    SetForScope diagnosing(m_diagnosing, true)
#define DIAGNOSE_OR_RETURN(result) \
    if (m_quiet) { \
        invalidSyntax(); \
        return result; \
    } \
    SetForScope diagnosing(m_diagnosing, true)

bool Parser::atStartOfExpression(unsigned ahead)
{
    switch (peek(ahead).kind) {
    case TokenKind::Name:
    case TokenKind::Number:
    case TokenKind::String:
    case TokenKind::FStringStart:
    case TokenKind::TStringStart:
    case TokenKind::LPar:
    case TokenKind::LSqb:
    case TokenKind::LBrace:
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Tilde:
    case TokenKind::Ellipsis:
    case TokenKind::KwNot:
    case TokenKind::KwLambda:
    case TokenKind::KwAwait:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwNone:
        return true;
    default:
        return false;
    }
}

// star_expressions: ','.star_expression+ [',']. More than one, or a trailing comma, makes a tuple.
Expr* Parser::parseStarExpressions()
{
    Position start = startPosition();
    Expr* first = parseStarExpression();
    if (!first || !at(TokenKind::Comma))
        return first;
    ExprVector elements;
    elements.append(first);
    while (accept(TokenKind::Comma)) {
        if (!atStartOfExpression() && !at(TokenKind::Star))
            break;
        auto afterComma = checkpoint();
        Expr* element = parseStarExpression();
        if (!element) {
            if (endsBeforeOperator(afterComma))
                break;
            return nullptr;
        }
        elements.append(element);
    }
    return makeTuple(elements, rangeFrom(start));
}

// star_expression: '*' bitwise_or | expression
Expr* Parser::parseStarExpression()
{
    if (!at(TokenKind::Star))
        return parseExpression();
    Position start = startPosition();
    advance();
    Expr* value = parseBitwiseOr();
    if (!value)
        return nullptr;
    auto* starred = make<Starred>(rangeFrom(start));
    starred->value = value;
    starred->ctx = ExprContext::Load;
    return starred;
}

// expressions: ','.expression+ [',']
Expr* Parser::parseExpressions()
{
    Position start = startPosition();
    Expr* first = parseExpression();
    if (!first || !at(TokenKind::Comma))
        return first;
    ExprVector elements;
    elements.append(first);
    while (accept(TokenKind::Comma)) {
        if (!atStartOfExpression())
            break;
        Expr* element = parseExpression();
        if (!element)
            return nullptr;
        elements.append(element);
    }
    return makeTuple(elements, rangeFrom(start));
}

Expr* Parser::parseAnnotatedRHS()
{
    if (at(TokenKind::KwYield))
        return parseYield();
    return parseStarExpressions();
}

Expr* Parser::parseYield()
{
    Position start = startPosition();
    advance();
    auto afterYield = checkpoint();
    if (accept(TokenKind::KwFrom)) {
        if (Expr* value = parseExpression()) {
            auto* yield = make<YieldFrom>(rangeFrom(start));
            yield->value = value;
            return yield;
        }
        if (!endsBeforeOperator(afterYield))
            return nullptr;
    }
    Expr* value = nullptr;
    if (atStartOfExpression() || at(TokenKind::Star)) {
        value = parseStarExpressions();
        if (!value && !endsBeforeOperator(afterYield))
            return nullptr;
    }
    auto* yield = make<Yield>(rangeFrom(start));
    yield->value = value;
    return yield;
}

Expr* Parser::parseExpression()
{
    CHECK_RECURSION();
    if (wasParsedQuietly(MemoizedRule::Expression)) [[unlikely]] {
        SetForScope quiet(m_quiet, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        Expr* expression = parseExpressionOnce();
        m_failedExpressionHasPrefix = false;
        return expression;
    }
    if (m_acceptsPrefix || m_quiet) {
        Expr* expression = parseExpressionOnce();
        m_failedExpressionHasPrefix = false;
        return expression;
    }
    auto saved = checkpoint();
    Expr* expression = parseExpressionOnce();
    if (expression || !m_errorIsGeneric || m_errorIsFatal) [[likely]]
        return expression;

    // To a parser that backtracks, an expression ends where it can no longer go on, and what it makes of the rest may explain
    // the error: in `f(a b.)`, `a` and `b` are two expressions that lack a comma.
    SyntaxErrorInfo unexplained = m_error;
    rewind(saved);
    Expr* prefix;
    {
        SetForScope diagnosing(m_diagnosing, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        prefix = parseExpressionOnce();
    }
    if (failed() && !m_errorIsGeneric)
        return nullptr;
    m_failedExpressionHasPrefix = !!prefix;
    m_error = unexplained;
    m_errorIsGeneric = true;
    return nullptr;
}

// expression: disjunction 'if' disjunction 'else' expression | disjunction | lambdef
Expr* Parser::parseExpressionOnce()
{
    if (at(TokenKind::KwLambda))
        return parseLambda();
    if ((at(TokenKind::KwPass) || at(TokenKind::KwBreak) || at(TokenKind::KwContinue)) && !m_quiet) [[unlikely]] {
        bool isBeforeIf;
        {
            SetForScope reading(m_diagnosing, true);
            isBeforeIf = peek(1).kind == TokenKind::KwIf;
        }
        return isBeforeIf ? diagnoseStatementBeforeIf() : invalidSyntax();
    }

    Position start = startPosition();
    size_t startIndex = m_index;
    Expr* body = parseDisjunction();
    if (!body)
        return nullptr;

    if (at(TokenKind::KwIf)) {
        auto beforeIf = checkpoint();
        advance();
        Expr* test = parseDisjunction();
        if (!test)
            return endsBeforeOperator(beforeIf) ? body : nullptr;
        if (!at(TokenKind::KwElse)) {
            if (m_quiet || at(TokenKind::Colon)) {
                invalidSyntax();
                return endsBeforeOperator(beforeIf) ? body : nullptr;
            }
            return syntaxErrorAt(rangeBetween(body->range, test->range), "expected 'else' after 'if' expression"_s);
        }
        advance();
        if (at(TokenKind::Star))
            return syntaxErrorAt(peek(), "cannot unpack only part of a conditional expression"_s);
        if (at(TokenKind::DoubleStar))
            return syntaxErrorAt(peek(), "cannot use dict unpacking on only part of a conditional expression"_s);
        auto afterElse = checkpoint();
        Expr* orelse = parseExpression();
        if (!orelse) {
            if (!m_errorIsGeneric || m_errorIsFatal || m_failedExpressionHasPrefix)
                return nullptr;
            rewind(afterElse);
            return syntaxErrorOnNextToken("expected expression after 'else', but statement is given"_s);
        }
        auto* conditional = make<IfExp>(rangeFrom(start));
        conditional->test = test;
        conditional->body = body;
        conditional->orelse = orelse;
        return conditional;
    }

    // Nothing valid has two expressions side by side. (Nor these statements here, which are looked at as the start of `pass if a else b`.)
    if ((atStartOfExpression() || at(TokenKind::KwPass) || at(TokenKind::KwBreak) || at(TokenKind::KwContinue)) && !m_quiet) [[unlikely]] {
        auto afterBody = checkpoint();
        diagnoseJuxtaposition(body, startIndex);
        return endsBeforeOperator(afterBody) ? body : nullptr;
    }
    return body;
}

bool Parser::isSoftKeyword(const Token& token)
{
    if (token.kind != TokenKind::Name)
        return false;
    auto& name = *token.identifier;
    return name == m_matchKeyword || name == m_caseKeyword || name == m_typeKeyword || name == m_lazyKeyword || name == m_underscore;
}

static bool isLegacyStatement(Expr* expression)
{
    auto* name = expression->tryAs<NameExpr>();
    return name && (name->id->string() == "print"_s || name->id->string() == "exec"_s);
}

// An expression follows `first`, which started at the token `startIndex`. Says what was probably meant.
std::nullptr_t Parser::diagnoseJuxtaposition(Expr* first, size_t startIndex)
{
    SetForScope diagnosing(m_diagnosing, true);
    SetForScope acceptsPrefix(m_acceptsPrefix, true);
    auto afterFirst = checkpoint();
    const Token firstToken = m_tokens[startIndex];
    TokenKind secondKind = m_tokens[startIndex + 1].kind;

    // "a" b "c". What is between the strings is looked at afresh: in `"a" + b "c"` it is `+ b`.
    if (firstToken.kind == TokenKind::String) {
        m_index = startIndex + 1;
        m_lastEnd = firstToken.endPosition;
        Expr* firstBetween = nullptr;
        Expr* lastBetween = nullptr;
        while (!at(TokenKind::String) && atStartOfExpression()) {
            lastBetween = parseExpressionQuietly();
            if (!lastBetween)
                break;
            if (!firstBetween)
                firstBetween = lastBetween;
        }
        if (m_errorIsFatal)
            return nullptr;
        if (lastBetween && at(TokenKind::String))
            return syntaxErrorAt(rangeBetween(firstBetween->range, lastBetween->range), "invalid syntax. Is this intended to be part of the string?"_s);
        rewind(afterFirst);
    }

    if (!(firstToken.kind == TokenKind::Name && secondKind == TokenKind::String) && !isSoftKeyword(firstToken)) {
        Expr* second = parseExpressionQuietly();
        if (m_errorIsFatal)
            return nullptr;
        if (second && !isLegacyStatement(first) && m_tokens[m_index - 1].level) {
            if (first->range.endLineno > first->range.lineno)
                return raise(SyntaxErrorInfo::Type::SyntaxError, first->range.endLineno, first->range.colOffset, first->range.endLineno, first->range.endColOffset, "invalid syntax. Perhaps you forgot a comma?"_s);
            return syntaxErrorAt(rangeBetween(first->range, second->range), "invalid syntax. Perhaps you forgot a comma?"_s);
        }
        rewind(afterFirst);
    }

    // print "hello". Whatever the name, what follows it is looked at, and what is wrong with that is reported.
    if (firstToken.kind == TokenKind::Name && secondKind != TokenKind::LPar) {
        m_index = startIndex + 1;
        m_lastEnd = firstToken.endPosition;
        SetForScope notQuiet(m_quiet, false);
        Expr* rest = parseStarExpressions();
        if (m_errorIsFatal)
            return nullptr;
        auto& name = firstToken.identifier->string();
        if (rest && (name == "print"_s || name == "exec"_s))
            return syntaxErrorAt(rangeBetween(rangeOf(firstToken), rest->range), makeString("Missing parentheses in call to '"_s, name, "'. Did you mean "_s, name, "(...)?"_s));
        if (failed() && !m_errorIsGeneric)
            return nullptr;
        rewind(afterFirst);
    }
    return invalidSyntax();
}

// An expression, or null without an error (unless a fatal one).
Expr* Parser::parseExpressionQuietly()
{
    CHECK_RECURSION();
    SetForScope quiet(m_quiet, true);
    SetForScope acceptsPrefix(m_acceptsPrefix, true);
    auto saved = checkpoint();
    // This is a rule of its own (expression_without_invalid), so what comes of it is not remembered for `expression`.
    Expr* expression = parseExpressionOnce();
    if (!expression)
        rewind(saved);
    return expression;
}

// named_expression: NAME ':=' expression | expression !':='
Expr* Parser::parseNamedExpression()
{
    if (at(TokenKind::Name) && peek(1).kind == TokenKind::ColonEqual) {
        Position start = startPosition();
        NameExpr* target = makeName(advance(), ExprContext::Store);
        advance();
        Expr* value = parseExpression();
        if (!value)
            return nullptr;
        if (!checkVersion(8, "Assignment expressions are"_s))
            return nullptr;
        auto* named = make<NamedExpr>(rangeFrom(start));
        named->target = target;
        named->value = value;
        return named;
    }

    Position start = startPosition();
    Expr* expression = parseExpression();
    if (!expression)
        return nullptr;

    if (at(TokenKind::ColonEqual)) {
        DIAGNOSE();
        advance();
        if (!parseExpression())
            return nullptr;
        return syntaxErrorAt(expression->range, makeString("cannot use assignment expressions with "_s, describe(expression)));
    }

    if (at(TokenKind::Equal) && isAtBitwiseOrLevel(expression) && !m_quiet) {
        // `if x = 3:`
        SetForScope diagnosing(m_diagnosing, true);
        bool isName = expression->is<NameExpr>() && !expression->parenthesized;
        if (isName || !startsLikeATarget(expression, start)) {
            auto saved = checkpoint();
            advance();
            Expr* value = parseBitwiseOr();
            if (m_errorIsFatal)
                return nullptr;
            if (value && !at(TokenKind::Equal) && !at(TokenKind::ColonEqual)) {
                if (isName)
                    return syntaxErrorAt(rangeBetween(expression->range, value->range), "invalid syntax. Maybe you meant '==' or ':=' instead of '='?"_s);
                return syntaxErrorAt(expression->range, makeString("cannot assign to "_s, describe(expression), " here. Maybe you meant '==' instead of '='?"_s));
            }
            rewind(saved);
        }
        return m_acceptsPrefix ? expression : invalidSyntax();
    }
    return expression;
}

// star_named_expression: '*' bitwise_or | named_expression
Expr* Parser::parseStarNamedExpression()
{
    if (at(TokenKind::Star))
        return parseStarExpression();
    return parseNamedExpression();
}

// An element of a list, a set or a tuple in parentheses, which may turn out to be the element of a comprehension.
Expr* Parser::parseSequenceElement()
{
    if (at(TokenKind::DoubleStar)) {
        DIAGNOSE();
        const Token doubleStar = advance();
        if (!parseBitwiseOr())
            return nullptr;
        return syntaxErrorAt(doubleStar, "cannot use dict unpacking here"_s);
    }
    if (!at(TokenKind::Star))
        return parseNamedExpression();

    const Token star = peek();
    size_t starIndex = m_index;
    Expr* starred = parseStarExpression();
    if (!starred || m_quiet)
        return starred;
    if (atStartOfExpression()) [[unlikely]] {
        // Nothing valid. What follows the star was looked at as an expression, to see whether it is `*a if b else c` or `*a = b`.
        SetForScope diagnosing(m_diagnosing, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        auto afterStarred = checkpoint();
        m_index = starIndex + 1;
        m_lastEnd = star.endPosition;
        parseExpression();
        if (failed() && (m_errorIsFatal || !m_errorIsGeneric))
            return nullptr;
        rewind(afterStarred);
        return starred;
    }
    if (at(TokenKind::KwIf)) {
        // *a if b else c
        SetForScope diagnosing(m_diagnosing, true);
        advance();
        if (parseDisjunction() && accept(TokenKind::KwElse) && parseExpression())
            return syntaxErrorAt(rangeFrom(star.startPosition), "invalid starred expression. Did you forget to wrap the conditional expression in parentheses?"_s);
        return failed() ? nullptr : invalidSyntax();
    }
    if (at(TokenKind::Equal)) {
        SetForScope diagnosing(m_diagnosing, true);
        advance();
        if (!parseExpression())
            return nullptr;
        return syntaxErrorAt(rangeFrom(star.startPosition), "cannot assign to iterable argument unpacking"_s);
    }
    return starred;
}

Expr* Parser::parseLambda()
{
    Position start = startPosition();
    const Token lambdaToken = advance();
    Arguments* arguments = parseParameters(true);
    if (!arguments)
        return nullptr;
    auto* colon = expect(TokenKind::Colon);
    if (!colon)
        return nullptr;
    // In a replacement field the ':' starts the format spec.
    if (at(TokenKind::FStringMiddle) || at(TokenKind::TStringMiddle)) {
        Range range = rangeBetween(rangeOf(lambdaToken), { 0, 0, m_lastEnd.line, m_lastEnd.column });
        return syntaxErrorAt(range, makeString(at(TokenKind::FStringMiddle) ? 'f' : 't', "-string: lambda expressions are not allowed without parentheses"_s));
    }
    Expr* body = parseExpression();
    if (!body)
        return nullptr;
    auto* lambda = make<Lambda>(rangeFrom(start));
    lambda->args = arguments;
    lambda->body = body;
    return lambda;
}

Expr* Parser::parseDisjunction()
{
    if (wasParsedQuietly(MemoizedRule::Disjunction)) [[unlikely]] {
        SetForScope quiet(m_quiet, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        return parseDisjunction();
    }
    Position start = startPosition();
    Expr* first = parseConjunction();
    if (!first || !at(TokenKind::KwOr))
        return first;
    ExprVector values;
    values.append(first);
    while (at(TokenKind::KwOr)) {
        auto beforeOperator = checkpoint();
        advance();
        Expr* value = parseConjunction();
        if (!value) {
            if (endsBeforeOperator(beforeOperator))
                break;
            return nullptr;
        }
        values.append(value);
    }
    if (values.size() == 1)
        return first;
    auto* result = make<BoolOp>(rangeFrom(start));
    result->op = BoolOperator::Or;
    result->values = m_arena.copy(values);
    return result;
}

Expr* Parser::parseConjunction()
{
    Position start = startPosition();
    Expr* first = parseInversion();
    if (!first || !at(TokenKind::KwAnd))
        return first;
    ExprVector values;
    values.append(first);
    while (at(TokenKind::KwAnd)) {
        auto beforeOperator = checkpoint();
        advance();
        Expr* value = parseInversion();
        if (!value) {
            if (endsBeforeOperator(beforeOperator))
                break;
            return nullptr;
        }
        values.append(value);
    }
    if (values.size() == 1)
        return first;
    auto* result = make<BoolOp>(rangeFrom(start));
    result->op = BoolOperator::And;
    result->values = m_arena.copy(values);
    return result;
}

Expr* Parser::parseInversion()
{
    if (!at(TokenKind::KwNot))
        return parseComparison();
    CHECK_RECURSION();
    Position start = startPosition();
    advance();
    Expr* operand = parseInversion();
    if (!operand)
        return nullptr;
    auto* result = make<UnaryOp>(rangeFrom(start));
    result->op = UnaryKind::Not;
    result->operand = operand;
    return result;
}

Expr* Parser::parseComparison()
{
    Position start = startPosition();
    Expr* left = parseBitwiseOr();
    if (!left)
        return nullptr;

    Vector<CompareKind, 4> operators;
    ExprVector comparators;
    for (;;) {
        CompareKind op;
        auto beforeOperator = checkpoint();
        switch (kind()) {
        case TokenKind::EqEqual:
            op = CompareKind::Eq;
            break;
        case TokenKind::NotEqual:
            // Only under `from __future__ import barry_as_FLUFL` does the lexer make <> a token, and then it is the only spelling.
            if (m_flags.contains(ParserFlag::BarryAsBDFL) && textOf(peek())[0] == '!')
                return syntaxError("with Barry as BDFL, use '<>' instead of '!='"_s);
            op = CompareKind::NotEq;
            break;
        case TokenKind::LessEqual:
            op = CompareKind::LtE;
            break;
        case TokenKind::Less:
            if (peek(1).kind == TokenKind::Greater && peek().end == peek(1).start)
                return syntaxErrorAt(rangeBetween(rangeOf(peek()), rangeOf(peek(1))), "invalid syntax.  Maybe you meant '!=' instead of '<>'?"_s);
            op = CompareKind::Lt;
            break;
        case TokenKind::GreaterEqual:
            op = CompareKind::GtE;
            break;
        case TokenKind::Greater:
            op = CompareKind::Gt;
            break;
        case TokenKind::KwIn:
            op = CompareKind::In;
            break;
        case TokenKind::KwNot:
            if (peek(1).kind != TokenKind::KwIn)
                goto done;
            advance();
            op = CompareKind::NotIn;
            break;
        case TokenKind::KwIs:
            if (peek(1).kind == TokenKind::KwNot) {
                advance();
                op = CompareKind::IsNot;
            } else
                op = CompareKind::Is;
            break;
        default:
            goto done;
        }
        advance();
        Expr* right = parseBitwiseOr();
        if (!right) {
            if (endsBeforeOperator(beforeOperator))
                break;
            return nullptr;
        }
        operators.append(op);
        comparators.append(right);
    }
done:
    if (operators.isEmpty())
        return left;
    auto* compare = make<Compare>(rangeFrom(start));
    compare->left = left;
    compare->ops = m_arena.copy(operators);
    compare->comparators = m_arena.copy(comparators);
    return compare;
}

struct BinaryOperatorInfo {
    int precedence; // 0: not a binary operator
    Operator op;
};

static BinaryOperatorInfo binaryOperator(TokenKind kind)
{
    switch (kind) {
    case TokenKind::VBar: return { 1, Operator::BitOr };
    case TokenKind::Circumflex: return { 2, Operator::BitXor };
    case TokenKind::Amper: return { 3, Operator::BitAnd };
    case TokenKind::LeftShift: return { 4, Operator::LShift };
    case TokenKind::RightShift: return { 4, Operator::RShift };
    case TokenKind::Plus: return { 5, Operator::Add };
    case TokenKind::Minus: return { 5, Operator::Sub };
    case TokenKind::Star: return { 6, Operator::Mult };
    case TokenKind::Slash: return { 6, Operator::Div };
    case TokenKind::DoubleSlash: return { 6, Operator::FloorDiv };
    case TokenKind::Percent: return { 6, Operator::Mod };
    case TokenKind::At: return { 6, Operator::MatMult };
    default: return { 0, Operator::Add };
    }
}

// bitwise_or down to term: the binary operators that associate to the left.
Expr* Parser::parseBinary(int minimumPrecedence)
{
    if (wasParsedQuietly(MemoizedRule::Binary)) [[unlikely]] {
        SetForScope quiet(m_quiet, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        return parseBinary(minimumPrecedence);
    }
    Position start = startPosition();
    Expr* left = parseFactor();
    if (!left)
        return nullptr;
    for (;;) {
        auto [precedence, op] = binaryOperator(kind());
        if (!precedence || precedence < minimumPrecedence)
            return left;
        auto beforeOperator = checkpoint();
        advance();
        if (at(TokenKind::KwNot) && precedence >= 5 && !m_quiet) {
            SetForScope diagnosing(m_diagnosing, true);
            Position notStart = startPosition();
            if (!parseInversion())
                return nullptr;
            return syntaxErrorAt(rangeFrom(notStart), "'not' after an operator must be parenthesized"_s);
        }
        Expr* right = parseBinary(precedence + 1);
        if (!right)
            return endsBeforeOperator(beforeOperator) ? left : nullptr;
        if (op == Operator::MatMult && !checkVersion(5, "The '@' operator is"_s))
            return nullptr;
        auto* result = make<BinOp>(rangeFrom(start));
        result->left = left;
        result->op = op;
        result->right = right;
        left = result;
    }
}

Expr* Parser::parseFactor()
{
    UnaryKind op;
    switch (kind()) {
    case TokenKind::Plus:
        op = UnaryKind::UAdd;
        break;
    case TokenKind::Minus:
        op = UnaryKind::USub;
        break;
    case TokenKind::Tilde:
        op = UnaryKind::Invert;
        break;
    default:
        return parsePower();
    }
    CHECK_RECURSION();
    Position start = startPosition();
    advance();
    if (at(TokenKind::KwNot)) {
        DIAGNOSE();
        Position notStart = startPosition();
        advance();
        if (!parseFactor())
            return nullptr;
        return syntaxErrorAt(rangeFrom(notStart), "'not' after an operator must be parenthesized"_s);
    }
    Expr* operand = parseFactor();
    if (!operand)
        return nullptr;
    auto* result = make<UnaryOp>(rangeFrom(start));
    result->op = op;
    result->operand = operand;
    return result;
}

// power: await_primary '**' factor | await_primary
Expr* Parser::parsePower()
{
    Position start = startPosition();
    Expr* base = parseAwaitPrimary();
    if (!base || !at(TokenKind::DoubleStar))
        return base;
    CHECK_RECURSION();
    auto beforeOperator = checkpoint();
    advance();
    Expr* exponent = parseFactor();
    if (!exponent)
        return endsBeforeOperator(beforeOperator) ? base : nullptr;
    auto* result = make<BinOp>(rangeFrom(start));
    result->left = base;
    result->op = Operator::Pow;
    result->right = exponent;
    return result;
}

Expr* Parser::parseAwaitPrimary()
{
    if (!at(TokenKind::KwAwait))
        return parsePrimary();
    Position start = startPosition();
    advance();
    Expr* value = parsePrimary();
    if (!value)
        return nullptr;
    if (!checkVersion(5, "Await expressions are"_s))
        return nullptr;
    auto* result = make<Await>(rangeFrom(start));
    result->value = value;
    return result;
}

Expr* Parser::parsePrimary()
{
    if (wasParsedQuietly(MemoizedRule::Primary)) [[unlikely]] {
        SetForScope quiet(m_quiet, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        return parsePrimary();
    }
    Position start = startPosition();
    Expr* atom = parseAtom();
    if (!atom)
        return nullptr;
    return parseTrailers(atom, start);
}

Expr* Parser::parseTrailers(Expr* expression, Position start)
{
    for (;;) {
        switch (kind()) {
        case TokenKind::Dot: {
            auto beforeOperator = checkpoint();
            advance();
            Name name = expectName();
            if (!name)
                return endsBeforeOperator(beforeOperator) ? expression : nullptr;
            auto* attribute = make<Attribute>(rangeFrom(start));
            attribute->value = expression;
            attribute->attr = name;
            attribute->ctx = ExprContext::Load;
            expression = attribute;
            break;
        }
        case TokenKind::LPar: {
            auto beforeOperator = checkpoint();
            Position open = startPosition();
            advance();
            Seq<Expr*> arguments;
            Seq<Keyword*> keywords;
            Expr* soleGenerator = nullptr;
            if (!parseArguments(arguments, keywords, open, soleGenerator))
                return endsBeforeOperator(beforeOperator) ? expression : nullptr;
            auto* call = make<Call>(rangeFrom(start));
            call->func = expression;
            call->args = soleGenerator ? m_arena.single(soleGenerator) : arguments;
            call->keywords = keywords;
            expression = call;
            break;
        }
        case TokenKind::LSqb: {
            auto beforeOperator = checkpoint();
            advance();
            Expr* slice = parseSlices();
            if (!slice || !expect(TokenKind::RSqb))
                return endsBeforeOperator(beforeOperator) ? expression : nullptr;
            auto* subscript = make<Subscript>(rangeFrom(start));
            subscript->value = expression;
            subscript->slice = slice;
            subscript->ctx = ExprContext::Load;
            expression = subscript;
            break;
        }
        case TokenKind::LBrace: {
            // Nothing valid. What follows a primary is tried as a generator expression, and among the mistakes looked for in
            // those is a comprehension in braces whose target lacks parentheses: so the elements are looked at.
            if (m_quiet)
                return expression;
            SetForScope diagnosing(m_diagnosing, true);
            SetForScope acceptsPrefix(m_acceptsPrefix, true);
            auto beforeBrace = checkpoint();
            advance();
            ExprVector elements;
            Token lastComma;
            bool endsWithComma = false;
            while (atStartOfExpression() || at(TokenKind::Star)) {
                Expr* element = parseStarNamedExpression();
                if (!element)
                    break;
                elements.append(element);
                endsWithComma = at(TokenKind::Comma);
                if (!endsWithComma)
                    break;
                lastComma = advance();
            }
            if (!failed() && (elements.size() > 1 || endsWithComma) && atComprehension())
                diagnoseComprehensionTarget(elements, lastComma, endsWithComma);
            if (failed() && (m_errorIsFatal || !m_errorIsGeneric))
                return nullptr;
            rewind(beforeBrace);
            return expression;
        }
        default:
            return expression;
        }
    }
}

// slices: slice !',' | ','.(slice | starred_expression)+ [',']
Expr* Parser::parseSlices()
{
    Position start = startPosition();
    auto parseItem = [&]() -> Expr* {
        if (!at(TokenKind::Star))
            return parseSlice();
        Position starStart = startPosition();
        advance();
        Expr* value = parseExpression();
        if (!value) {
            if (m_errorIsGeneric && !m_errorIsFatal && !m_failedExpressionHasPrefix) {
                m_error = { };
                syntaxError("Invalid star expression"_s);
            }
            return nullptr;
        }
        auto* starred = make<Starred>(rangeFrom(starStart));
        starred->value = value;
        starred->ctx = ExprContext::Load;
        return starred;
    };
    Expr* first = parseItem();
    if (!first)
        return nullptr;
    if (!at(TokenKind::Comma) && !first->is<Starred>())
        return first;
    ExprVector elements;
    elements.append(first);
    while (accept(TokenKind::Comma)) {
        if (at(TokenKind::RSqb))
            break;
        Expr* element = parseItem();
        if (!element)
            return nullptr;
        elements.append(element);
    }
    return makeTuple(elements, rangeFrom(start));
}

// slice: [expression] ':' [expression] [':' [expression]] | named_expression
Expr* Parser::parseSlice()
{
    Position start = startPosition();
    Expr* lower = nullptr;
    if (!at(TokenKind::Colon)) {
        lower = parseNamedExpression();
        if (!lower || !at(TokenKind::Colon))
            return lower;
        if (lower->is<NamedExpr>() && !lower->parenthesized)
            return invalidSyntax();
    }
    advance();
    auto atEndOfPart = [&] { return at(TokenKind::Colon) || at(TokenKind::Comma) || at(TokenKind::RSqb); };
    Expr* upper = nullptr;
    if (!atEndOfPart()) {
        upper = parseExpression();
        if (!upper)
            return nullptr;
    }
    Expr* step = nullptr;
    if (accept(TokenKind::Colon) && !atEndOfPart()) {
        step = parseExpression();
        if (!step)
            return nullptr;
    }
    auto* slice = make<Slice>(rangeFrom(start));
    slice->lower = lower;
    slice->upper = upper;
    slice->step = step;
    return slice;
}

static Expr* lastItemOf(Comprehension* comprehension)
{
    return comprehension->ifs.isEmpty() ? comprehension->iter : comprehension->ifs.last();
}

// After the '(' of a call or of the bases of a class, through the ')'.
// `f(x for x in y)`: the generator expression, which takes the parentheses of the call for its own, is `soleGenerator`.
bool Parser::parseArguments(Seq<Expr*>& resultArguments, Seq<Keyword*>& resultKeywords, Position openParenthesis, Expr*& soleGenerator, bool isCall)
{
    ExprVector arguments;
    Vector<Keyword*, 8> keywords;
    bool sawKeyword = false;
    bool sawDoubleStar = false;
    ASCIILiteral orderError;
    Position lastComma;

    auto fail = [&]() {
        // Arguments out of order are reported where parsing stopped, whatever stopped it.
        if (!orderError.isNull() && !m_errorIsFatal && (!failed() || m_errorIsGeneric)) {
            m_error = { };
            syntaxError(orderError);
        }
        return false;
    };

    auto makeStarred = [&](Position start, Expr* value) {
        auto* starred = make<Starred>(rangeFrom(start));
        starred->value = value;
        starred->ctx = ExprContext::Load;
        return starred;
    };

    // `for`, or `async for`. Whether a `for` follows the `async` is of interest only after what can be the element of a generator expression.
    auto atGenerator = [&](bool canBeElement) {
        if (!at(TokenKind::KwAsync))
            return at(TokenKind::KwFor);
        if (!canBeElement && m_quiet)
            return false;
        SetForScope reading(m_diagnosing, m_diagnosing || !canBeElement);
        return peek(1).kind == TokenKind::KwFor;
    };

    // An argument followed by `for`.
    auto parseGenerator = [&](Expr* element) -> bool {
        // Only the first argument can be the element of a generator expression. After another one, this is about what was meant.
        bool isFirst = arguments.isEmpty() && keywords.isEmpty();
        if (!isFirst && m_quiet) {
            invalidSyntax();
            return false;
        }
        SetForScope diagnosing(m_diagnosing, m_diagnosing || !isFirst);
        // What lacks parentheses is `x for ...`, or the last of several positional arguments and what follows it.
        bool isTakenForGenerator = isFirst ? !element->is<Starred>() && !element->is<NamedExpr>() : !element->is<Starred>() || !arguments.isEmpty();
        Range forToken = rangeOf(peek());
        if (!isFirst && !isTakenForGenerator) {
            invalidSyntax();
            return false;
        }
        Seq<Comprehension*> generators;
        if (!parseComprehensionClauses(generators))
            return false;
        if (arguments.isEmpty() && keywords.isEmpty() && at(TokenKind::RPar)) {
            advance();
            auto* generator = make<GeneratorExp>(rangeFrom(openParenthesis));
            generator->elt = element;
            generator->generators = generators;
            generator->parenthesized = true;
            soleGenerator = generator;
            return true;
        }
        // Alone, it has to be followed by a comma to be taken for an argument that lacks parentheses.
        if (arguments.isEmpty() && keywords.isEmpty() && !at(TokenKind::Comma)) {
            invalidSyntax();
            return false;
        }
        if (!isTakenForGenerator) {
            // In a call, it was a generator expression up to the comma.
            if (isCall)
                invalidSyntax();
            else
                syntaxErrorAt(forToken, "invalid syntax"_s);
            return false;
        }
        // CPython's parser has looked at what follows the comma by now: if the lexer cannot read that, it says so first.
        if (at(TokenKind::Comma)) {
            peek(1);
            if (failed())
                return false;
        }
        syntaxErrorAt(rangeBetween(element->range, lastItemOf(generators.last())->range), "Generator expression must be parenthesized"_s);
        return false;
    };

    while (!at(TokenKind::RPar)) {
        Position start = startPosition();
        switch (kind()) {
        case TokenKind::Star: {
            advance();
            if (!atStartOfExpression()) {
                syntaxError("Invalid star expression"_s);
                return fail();
            }
            Expr* value = parseExpression();
            if (!value) {
                if (m_errorIsGeneric && !m_errorIsFatal && !m_failedExpressionHasPrefix) {
                    m_error = { };
                    syntaxError("Invalid star expression"_s);
                }
                return fail();
            }
            if (at(TokenKind::Equal)) {
                DIAGNOSE_OR_RETURN(fail());
                advance();
                if (parseExpression())
                    syntaxErrorAt(rangeFrom(start), "cannot assign to iterable argument unpacking"_s);
                return fail();
            }
            Expr* starred = makeStarred(start, value);
            if (atGenerator(arguments.isEmpty() && keywords.isEmpty()))
                return parseGenerator(starred) || fail();
            if (sawDoubleStar) {
                // As many of them as follow are part of the mistake.
                DIAGNOSE_OR_RETURN(fail());
                while (at(TokenKind::Comma) && peek(1).kind == TokenKind::Star) {
                    advance();
                    advance();
                    if (!parseExpression())
                        return fail();
                }
                peek();
                syntaxErrorStartingFrom(lastComma, "iterable argument unpacking follows keyword argument unpacking"_s);
                return false;
            }
            arguments.append(starred);
            break;
        }
        case TokenKind::DoubleStar: {
            advance();
            Expr* value = parseExpression();
            if (!value)
                return fail();
            if (at(TokenKind::Equal)) {
                DIAGNOSE_OR_RETURN(fail());
                advance();
                if (parseExpression())
                    syntaxErrorAt(rangeFrom(start), "cannot assign to keyword argument unpacking"_s);
                return fail();
            }
            if (atComprehension() && arguments.isEmpty() && keywords.isEmpty()) {
                DIAGNOSE_OR_RETURN(fail());
                Range range = rangeFrom(start);
                Seq<Comprehension*> generators;
                if (parseComprehensionClauses(generators))
                    syntaxErrorAt(range, "cannot use dict unpacking in generator expression"_s);
                return fail();
            }
            auto* keyword = make<Keyword>();
            keyword->arg = nullptr;
            keyword->value = value;
            keyword->range = rangeFrom(start);
            keywords.append(keyword);
            sawKeyword = sawDoubleStar = true;
            break;
        }
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
        case TokenKind::KwNone:
            if (peek(1).kind == TokenKind::Equal) {
                syntaxErrorAt(rangeBetween(rangeOf(peek()), rangeOf(peek(1))), makeString("cannot assign to "_s, peek().identifier->string()));
                return fail();
            }
            [[fallthrough]];
        default: {
            if (at(TokenKind::Name) && peek(1).kind == TokenKind::Equal) {
                const Token name = advance();
                const Token equal = advance();
                Range nameAndEqual = rangeBetween(rangeOf(name), rangeOf(equal));
                if (at(TokenKind::Comma) || at(TokenKind::RPar)) {
                    syntaxErrorAt(nameAndEqual, "expected argument value expression"_s);
                    return fail();
                }
                Expr* value = parseExpression();
                if (!value)
                    return fail();
                if (atGenerator(false)) {
                    DIAGNOSE_OR_RETURN(fail());
                    Seq<Comprehension*> generators;
                    if (parseComprehensionClauses(generators))
                        syntaxErrorAt(nameAndEqual, "invalid syntax. Maybe you meant '==' or ':=' instead of '='?"_s);
                    return fail();
                }
                auto* keyword = make<Keyword>();
                keyword->arg = name.identifier;
                keyword->value = value;
                keyword->range = rangeFrom(start);
                keywords.append(keyword);
                sawKeyword = true;
                break;
            }

            Expr* value;
            if (sawKeyword && m_quiet) {
                invalidSyntax();
                return fail();
            }
            SetForScope diagnosing(m_diagnosing, m_diagnosing || sawKeyword);
            if (at(TokenKind::Name) && peek(1).kind == TokenKind::ColonEqual)
                value = parseNamedExpression();
            else {
                value = parseExpression();
                if (value && at(TokenKind::ColonEqual)) {
                    invalidSyntax();
                    return fail();
                }
            }
            if (!value)
                return fail();
            if (at(TokenKind::Equal)) {
                syntaxErrorAt(rangeBetween(value->range, rangeOf(peek())), "expression cannot contain assignment, perhaps you meant \"==\"?"_s);
                return fail();
            }
            if (atGenerator(arguments.isEmpty() && keywords.isEmpty()))
                return parseGenerator(value) || fail();
            if (sawKeyword) {
                if (!orderError.isNull())
                    return fail();
                orderError = sawDoubleStar ? "positional argument follows keyword argument unpacking"_s : "positional argument follows keyword argument"_s;
                sawKeyword = sawDoubleStar = false;
            }
            arguments.append(value);
            break;
        }
        }

        if (!at(TokenKind::Comma))
            break;
        lastComma = startPosition();
        advance();
    }

    if (!orderError.isNull()) {
        peek();
        return fail();
    }
    if (!expect(TokenKind::RPar))
        return false;
    resultArguments = m_arena.copy(arguments);
    resultKeywords = m_arena.copy(keywords);
    return true;
}

// The targets of a `for`, up to the `in`, which is left to read.
Expr* Parser::parseForTargets(bool inComprehension)
{
    auto saved = checkpoint();
    Expr* target = parseStarTargets();
    if (target && at(TokenKind::KwIn))
        return target;
    if (m_errorIsFatal)
        return nullptr;

    // Read as expressions, they tell what is wrong. What parseStarTargets() had to say comes last of all that can be said here.
    m_errorWasFoundDiagnosing = false;
    rewind(saved);
    DIAGNOSE();
    SetForScope acceptsPrefix(m_acceptsPrefix, true);
    if (inComprehension) {
        // At least one variable, or it is not known to be a loop at all.
        bool parsed = atStartOfExpression();
        do {
            if (!atStartOfExpression())
                break;
            parsed = !!parseBitwiseOr();
        } while (parsed && accept(TokenKind::Comma));
        if (m_errorIsFatal)
            return nullptr;
        if (parsed && !at(TokenKind::KwIn))
            return syntaxError("'in' expected after for-loop variables"_s);
        rewind(saved);
    }
    Expr* expressions = parseStarExpressions();
    if (expressions && findInvalidTarget(expressions, TargetKind::For))
        return raiseInvalidTarget(expressions, TargetKind::For);
    return failed() ? nullptr : invalidSyntax();
}

// for_if_clauses: (['async'] 'for' star_targets 'in' disjunction ('if' disjunction)*)+
bool Parser::parseComprehensionClauses(Seq<Comprehension*>& result)
{
    Vector<Comprehension*, 2> clauses;
    while (atComprehension()) {
        auto* clause = make<Comprehension>();
        clause->isAsync = accept(TokenKind::KwAsync);
        advance();
        clause->target = parseForTargets(true);
        if (!clause->target)
            return false;
        advance();
        clause->iter = parseDisjunction();
        if (!clause->iter)
            return false;
        ExprVector conditions;
        while (accept(TokenKind::KwIf)) {
            Expr* condition = parseDisjunction();
            if (!condition)
                return false;
            conditions.append(condition);
        }
        if (clause->isAsync && !checkVersion(6, "Async comprehensions are"_s))
            return false;
        clause->ifs = m_arena.copy(conditions);
        clauses.append(clause);
    }
    result = m_arena.copy(clauses);
    return true;
}

// star_targets: star_target !',' | ','.star_target+ [',']
Expr* Parser::parseStarTargets()
{
    Position start = startPosition();
    Expr* first = parseStarTarget();
    if (!first || !at(TokenKind::Comma))
        return first;
    ExprVector elements;
    elements.append(first);
    while (accept(TokenKind::Comma)) {
        if (!atStartOfExpression() && !at(TokenKind::Star))
            break;
        Expr* element = parseStarTarget();
        if (!element)
            return nullptr;
        elements.append(element);
    }
    Tuple* tuple = makeTuple(elements, rangeFrom(start));
    tuple->ctx = ExprContext::Store;
    return tuple;
}

// star_target: '*' (!'*' star_target) | target_with_star_atom
Expr* Parser::parseStarTarget()
{
    if (at(TokenKind::Star)) {
        Position start = startPosition();
        advance();
        if (at(TokenKind::Star))
            return invalidSyntax();
        Expr* value = parseStarTarget();
        if (!value)
            return nullptr;
        auto* starred = make<Starred>(rangeFrom(start));
        starred->value = value;
        starred->ctx = ExprContext::Store;
        return starred;
    }
    // A target is a primary of a certain form.
    Expr* target = parsePrimary();
    if (!target)
        return nullptr;
    if (findInvalidTarget(target, TargetKind::Star))
        return raiseInvalidTarget(target, TargetKind::Star);
    setContext(target, ExprContext::Store);
    return target;
}

Expr* Parser::parseAtom()
{
    switch (kind()) {
    case TokenKind::Name:
        return makeName(advance());
    case TokenKind::KwTrue:
    case TokenKind::KwFalse:
    case TokenKind::KwNone: {
        const Token& token = advance();
        auto* constant = make<Constant>(rangeOf(token));
        constant->value = ConstantValue(token.kind == TokenKind::KwTrue ? ConstantValue::Kind::True : token.kind == TokenKind::KwFalse ? ConstantValue::Kind::False : ConstantValue::Kind::None);
        return constant;
    }
    case TokenKind::Ellipsis: {
        auto* constant = make<Constant>(rangeOf(advance()));
        constant->value = ConstantValue(ConstantValue::Kind::Ellipsis);
        return constant;
    }
    case TokenKind::String:
    case TokenKind::FStringStart:
    case TokenKind::TStringStart:
        return parseStrings();
    case TokenKind::Number:
        return parseNumber();
    case TokenKind::LPar:
        CHECK_RECURSION();
        return parseParenthesized();
    case TokenKind::LSqb:
        CHECK_RECURSION();
        return parseListDisplay();
    case TokenKind::LBrace:
        CHECK_RECURSION();
        return parseBraceDisplay();
    default:
        return invalidSyntax();
    }
}

// '(' ... ')': a generator expression, a tuple, or an expression in parentheses.
Expr* Parser::parseParenthesized()
{
    Position start = startPosition();
    advance();

    if (accept(TokenKind::RPar)) {
        Tuple* tuple = makeTuple({ }, rangeFrom(start));
        tuple->parenthesized = true;
        return tuple;
    }

    if (at(TokenKind::KwYield)) {
        Expr* yield = parseYield();
        if (!yield || !expect(TokenKind::RPar))
            return nullptr;
        yield->parenthesized = true;
        return yield;
    }

    if (at(TokenKind::DoubleStar)) {
        DIAGNOSE();
        const Token doubleStar = advance();
        Expr* value = parseExpression();
        if (!value)
            return nullptr;
        if (atComprehension()) {
            Seq<Comprehension*> generators;
            if (!parseComprehensionClauses(generators))
                return nullptr;
            return syntaxErrorAt(rangeBetween(rangeOf(doubleStar), value->range), "cannot use dict unpacking in generator expression"_s);
        }
        return syntaxErrorAt(doubleStar, "cannot use dict unpacking here"_s);
    }

    // The element of a generator expression may be '*' expression; of a tuple, '*' bitwise_or.
    Expr* first;
    if (at(TokenKind::Star)) {
        Position starStart = startPosition();
        auto saved = checkpoint();
        advance();
        m_failedExpressionHasPrefix = false;
        Expr* value = atStartOfExpression() ? parseExpression() : invalidSyntax();
        if (value && atComprehension()) {
            auto* starred = make<Starred>(rangeFrom(starStart));
            starred->value = value;
            starred->ctx = ExprContext::Load;
            first = starred;
        } else {
            if (m_errorIsFatal)
                return nullptr;
            if (!value && m_errorIsGeneric && !m_failedExpressionHasPrefix && !m_quiet) {
                m_error = { };
                return syntaxError("Invalid star expression"_s);
            }
            rewind(saved);
            first = parseSequenceElement();
        }
    } else
        first = parseSequenceElement();
    if (!first)
        return nullptr;

    if (atComprehension()) {
        Seq<Comprehension*> generators;
        if (!parseComprehensionClauses(generators) || !expect(TokenKind::RPar))
            return nullptr;
        auto* generator = make<GeneratorExp>(rangeFrom(start));
        generator->elt = first;
        generator->generators = generators;
        generator->parenthesized = true;
        return generator;
    }

    if (at(TokenKind::Comma)) {
        ExprVector elements;
        elements.append(first);
        while (accept(TokenKind::Comma)) {
            if (at(TokenKind::RPar))
                break;
            Expr* element = parseSequenceElement();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        if (!expect(TokenKind::RPar))
            return nullptr;
        Tuple* tuple = makeTuple(elements, rangeFrom(start));
        tuple->parenthesized = true;
        return tuple;
    }

    if (first->is<Starred>() && at(TokenKind::RPar))
        return syntaxErrorAt(first->range, "cannot use starred expression here"_s);
    if (!expect(TokenKind::RPar))
        return nullptr;
    first->parenthesized = true;
    return first;
}

// After the elements of a list or a set: `[a, b for a, b in c]`
std::nullptr_t Parser::diagnoseComprehensionTarget(const ExprVector& elements, const Token& lastComma, bool endsWithComma)
{
    DIAGNOSE();
    Seq<Comprehension*> generators;
    if (!parseComprehensionClauses(generators))
        return nullptr;
    Range end = endsWithComma ? rangeOf(lastComma) : elements.last()->range;
    return syntaxErrorAt(rangeBetween(elements.first()->range, end), "did you forget parentheses around the comprehension target?"_s);
}

Expr* Parser::parseListDisplay()
{
    Position start = startPosition();
    advance();

    ExprVector elements;
    if (!at(TokenKind::RSqb)) {
        if (at(TokenKind::DoubleStar)) {
            DIAGNOSE();
            auto saved = checkpoint();
            const Token doubleStar = advance();
            Expr* value = parseExpression();
            if (value && atComprehension()) {
                Seq<Comprehension*> generators;
                if (!parseComprehensionClauses(generators))
                    return nullptr;
                return syntaxErrorAt(rangeBetween(rangeOf(doubleStar), value->range), "cannot use dict unpacking in list comprehension"_s);
            }
            if (m_errorIsFatal)
                return nullptr;
            rewind(saved);
        }
        Expr* first = parseSequenceElement();
        if (!first)
            return nullptr;
        if (atComprehension()) {
            Seq<Comprehension*> generators;
            if (!parseComprehensionClauses(generators) || !expect(TokenKind::RSqb))
                return nullptr;
            auto* comprehension = make<ListComp>(rangeFrom(start));
            comprehension->elt = first;
            comprehension->generators = generators;
            return comprehension;
        }
        elements.append(first);
        Token lastComma;
        bool endsWithComma = false;
        while (at(TokenKind::Comma)) {
            lastComma = advance();
            endsWithComma = true;
            if (at(TokenKind::RSqb) || atMisplacedComprehension())
                break;
            Expr* element = parseSequenceElement();
            if (!element)
                return nullptr;
            elements.append(element);
            endsWithComma = false;
        }
        if (atMisplacedComprehension())
            return diagnoseComprehensionTarget(elements, lastComma, endsWithComma);
    }
    if (!expect(TokenKind::RSqb))
        return nullptr;
    auto* list = make<List>(rangeFrom(start));
    list->elts = m_arena.copy(elements);
    list->ctx = ExprContext::Load;
    return list;
}

// '{' ... '}': a dict, a set, or a comprehension of either.
Expr* Parser::parseBraceDisplay()
{
    Position start = startPosition();
    advance();

    ExprVector keys;
    ExprVector values;

    // The rules that say what is wrong with the first item of a dict are, unlike all the others of their kind, always tried
    // (in the grammar, neither `dict` nor invalid_double_starred_kvpairs has them as an alternative of their own).
    auto dictError = [&](auto raiseIt) -> std::nullptr_t {
        if (failed())
            return nullptr;
        if (!keys.isEmpty()) {
            raiseIt();
            return nullptr;
        }
        SetForScope notQuiet(m_quiet, false);
        raiseIt();
        errorIsOfARuleThatIsAlwaysTried();
        return nullptr;
    };
#define DIAGNOSE_UNLESS_FIRST_ITEM() \
    if (m_quiet && !keys.isEmpty()) \
        return invalidSyntax(); \
    SetForScope diagnosing(m_diagnosing, m_diagnosing || !keys.isEmpty())

    auto finishDict = [&]() -> Expr* {
        if (!expect(TokenKind::RBrace))
            return nullptr;
        auto* dict = make<Dict>(rangeFrom(start));
        dict->keys = m_arena.copy(keys);
        dict->values = m_arena.copy(values);
        return dict;
    };

    // A group's parentheses are not part of the operand.
    auto rangeUpTo = [](Position start, Expr* operand) -> Range {
        return { start.line, start.column, operand->range.endLineno, operand->range.endColOffset };
    };

    // After the ':' of a pair.
    auto parseValue = [&](const Token& colon) -> Expr* {
        if (at(TokenKind::RBrace) || at(TokenKind::Comma))
            return dictError([&] { syntaxErrorAt(colon, "expression expected after dictionary key and ':'"_s); });
        if (at(TokenKind::Star) || at(TokenKind::DoubleStar)) {
            DIAGNOSE_UNLESS_FIRST_ITEM();
            bool isDouble = at(TokenKind::DoubleStar);
            Position starStart = startPosition();
            advance();
            Expr* operand = parseBitwiseOr();
            if (!operand)
                return nullptr;
            auto message = isDouble ? "cannot use dict unpacking in a dictionary value"_s : "cannot use a starred expression in a dictionary value"_s;
            if (keys.isEmpty())
                return dictError([&] { syntaxErrorAt(rangeUpTo(starStart, operand), message); });
            peek();
            return dictError([&] { syntaxErrorStartingFrom(starStart, message); });
        }
        return parseExpression();
    };

    // '**' bitwise_or, after the stars.
    auto parseUnpacked = [&](const Token& doubleStar) -> Expr* {
        Expr* value = parseBitwiseOr();
        if (!value)
            return nullptr;
        if (at(TokenKind::KwIf)) {
            DIAGNOSE_UNLESS_FIRST_ITEM();
            advance();
            if (parseDisjunction() && accept(TokenKind::KwElse) && parseExpression())
                return dictError([&] { syntaxErrorAt(rangeFrom(doubleStar.startPosition), "invalid double starred expression. Did you forget to wrap the conditional expression in parentheses?"_s); });
            return failed() ? nullptr : invalidSyntax();
        }
        if (at(TokenKind::Colon)) {
            DIAGNOSE_UNLESS_FIRST_ITEM();
            Range range = rangeUpTo(doubleStar.startPosition, value);
            advance();
            if (!parseExpression())
                return nullptr;
            return dictError([&] { syntaxErrorAt(range, "cannot use dict unpacking in a dictionary key"_s); });
        }
        return value;
    };

    auto parseRestOfDict = [&]() -> Expr* {
        while (accept(TokenKind::Comma)) {
            if (at(TokenKind::RBrace))
                break;
            if (at(TokenKind::DoubleStar)) {
                const Token doubleStar = advance();
                Expr* value = parseUnpacked(doubleStar);
                if (!value)
                    return nullptr;
                keys.append(nullptr);
                values.append(value);
                continue;
            }
            if (at(TokenKind::Star)) {
                DIAGNOSE_UNLESS_FIRST_ITEM();
                Position starStart = startPosition();
                advance();
                Expr* operand = parseBitwiseOr();
                if (!operand)
                    return nullptr;
                Range range = rangeUpTo(starStart, operand);
                if (accept(TokenKind::Colon) && parseExpression())
                    return dictError([&] { syntaxErrorAt(range, "cannot use a starred expression in a dictionary key"_s); });
                return failed() ? nullptr : invalidSyntax();
            }
            Expr* key = parseExpression();
            if (!key)
                return nullptr;
            if (!at(TokenKind::Colon))
                return dictError([&] { raise(SyntaxErrorInfo::Type::SyntaxError, key->range.lineno, key->range.endColOffset - 1, key->range.endLineno, -1, "':' expected after dictionary key"_s); });
            const Token colon = advance();
            Expr* value = parseValue(colon);
            if (!value)
                return nullptr;
            keys.append(key);
            values.append(value);
        }
        return finishDict();
    };

    if (at(TokenKind::RBrace))
        return finishDict();

    if (at(TokenKind::DoubleStar)) {
        // {**a for ...} takes an expression, {**a, ...} a bitwise_or.
        auto saved = checkpoint();
        const Token doubleStar = advance();
        Expr* value = parseExpression();
        bool isComprehension = value && atComprehension();
        if (isComprehension) {
            Seq<Comprehension*> generators;
            if (parseComprehensionClauses(generators) && expect(TokenKind::RBrace)) {
                auto* comprehension = make<DictComp>(rangeFrom(start));
                comprehension->key = value;
                comprehension->value = nullptr;
                comprehension->generators = generators;
                return comprehension;
            }
        }
        if (m_errorIsFatal)
            return nullptr;
        if (!isComprehension && (!failed() || m_errorIsGeneric)) {
            rewind(saved);
            advance();
            value = parseUnpacked(doubleStar);
            if (value) {
                keys.append(nullptr);
                values.append(value);
                if (Expr* dict = parseRestOfDict())
                    return dict;
            }
        }
        if (!m_errorIsGeneric || m_errorIsFatal || m_quiet)
            return nullptr;
        // No dict. Next it is tried as a set, where this cannot be.
        SyntaxErrorInfo unexplained = m_error;
        SetForScope reading(m_diagnosing, true);
        SetForScope acceptsPrefix(m_acceptsPrefix, true);
        rewind(saved);
        advance();
        if (parseBitwiseOr())
            return syntaxErrorAt(doubleStar, "cannot use dict unpacking here"_s);
        if (!m_errorIsFatal) {
            m_error = unexplained;
            m_errorIsGeneric = true;
        }
        return nullptr;
    }

    Expr* first = parseSequenceElement();
    if (!first)
        return nullptr;

    if (at(TokenKind::Colon)) {
        if (first->is<Starred>()) {
            advance();
            if (!parseExpression())
                return nullptr;
            return dictError([&] { syntaxErrorAt(rangeBetween(first->range, first->as<Starred>()->value->range), "cannot use a starred expression in a dictionary key"_s); });
        }
        if (first->is<NamedExpr>() && !first->parenthesized)
            return invalidSyntax();
        const Token colon = advance();
        Expr* value = parseValue(colon);
        if (!value)
            return nullptr;
        if (atComprehension()) {
            Seq<Comprehension*> generators;
            if (!parseComprehensionClauses(generators) || !expect(TokenKind::RBrace))
                return nullptr;
            auto* comprehension = make<DictComp>(rangeFrom(start));
            comprehension->key = first;
            comprehension->value = value;
            comprehension->generators = generators;
            return comprehension;
        }
        keys.append(first);
        values.append(value);
        return parseRestOfDict();
    }

    if (atComprehension()) {
        Seq<Comprehension*> generators;
        if (!parseComprehensionClauses(generators) || !expect(TokenKind::RBrace))
            return nullptr;
        auto* comprehension = make<SetComp>(rangeFrom(start));
        comprehension->elt = first;
        comprehension->generators = generators;
        return comprehension;
    }

    ExprVector elements;
    elements.append(first);
    Token lastComma;
    bool endsWithComma = false;
    while (at(TokenKind::Comma)) {
        lastComma = advance();
        endsWithComma = true;
        if (at(TokenKind::RBrace) || atMisplacedComprehension())
            break;
        Expr* element = parseSequenceElement();
        if (!element)
            return nullptr;
        elements.append(element);
        endsWithComma = false;
    }
    if (atMisplacedComprehension())
        return diagnoseComprehensionTarget(elements, lastComma, endsWithComma);
    if (!expect(TokenKind::RBrace))
        return nullptr;
    auto* set = make<Set>(rangeFrom(start));
    set->elts = m_arena.copy(elements);
    return set;
}

#undef DIAGNOSE_UNLESS_FIRST_ITEM

// The lexer has checked the form of the literal.
Expr* Parser::parseNumber()
{
    const Token token = advance();
    auto text = textOf(token);
    if (m_featureVersion < 6 && std::ranges::find(text, '_') != text.end())
        return syntaxError("Underscores in numeric literals are only supported in Python 3.6 and greater"_s);
    auto* constant = make<Constant>(rangeOf(token));

    Vector<Latin1Character, 32> digits;
    digits.reserveInitialCapacity(text.size());
    for (uint8_t c : text) {
        if (c != '_')
            digits.append(c);
    }

    unsigned radix = 10;
    size_t prefixLength = 0;
    if (digits.size() > 1 && digits[0] == '0') {
        switch (toASCIILower(digits[1])) {
        case 'x':
            radix = 16;
            break;
        case 'o':
            radix = 8;
            break;
        case 'b':
            radix = 2;
            break;
        }
        if (radix != 10)
            prefixLength = 2;
    }

    bool isImaginary = radix == 10 && toASCIILower(digits.last()) == 'j';
    if (isImaginary)
        digits.removeLast();

    bool isFloat = false;
    if (radix == 10) {
        for (auto c : digits) {
            if (c == '.' || c == 'e' || c == 'E') {
                isFloat = true;
                break;
            }
        }
    }

    if (isFloat || isImaginary) {
        size_t parsedLength = 0;
        double value = WTF::parseDouble(digits.span(), parsedLength);
        ASSERT(parsedLength == digits.size());
        if (isImaginary) {
            constant->value.kind = ConstantValue::Kind::Complex;
            constant->value.complex = { 0, value };
        } else {
            constant->value.kind = ConstantValue::Kind::Float;
            constant->value.number = value;
        }
        return constant;
    }

    auto significant = digits.span().subspan(prefixLength);
    while (significant.size() > 1 && significant[0] == '0')
        significant = significant.subspan(1);

    uint64_t value = 0;
    bool fits = true;
    for (auto c : significant) {
        unsigned digit = toASCIIHexValue(c);
        if (value > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - digit) / radix) {
            fits = false;
            break;
        }
        value = value * radix + digit;
    }
    if (fits) {
        constant->value.kind = ConstantValue::Kind::Int;
        constant->value.integer = static_cast<int64_t>(value);
        return constant;
    }

    int maxDigits = m_host.maxStrDigits();
    if (radix == 10 && maxDigits > 0 && significant.size() > static_cast<size_t>(maxDigits)) {
        return syntaxErrorAt(token, makeString("Exceeds the limit ("_s, maxDigits, " digits) for integer string conversion: value has "_s, significant.size(),
            " digits; use sys.set_int_max_str_digits() to increase the limit - Consider hexadecimal for huge integer literals to avoid decimal conversion limits."_s));
    }
    auto copy = m_arena.copy(std::span<const Latin1Character> { significant });
    constant->value.kind = ConstantValue::Kind::BigInt;
    constant->value.radix = static_cast<uint8_t>(radix);
    constant->value.digits = { byteCast<char>(copy.begin()), copy.size() };
    return constant;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
