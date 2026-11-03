/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyParser.h"

#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

// patterns: open_sequence_pattern | pattern. After `case`.
Pattern* Parser::parsePatterns()
{
    Position start = startPosition();
    Pattern* first = parseMaybeStarPattern();
    if (!first)
        return nullptr;
    if (!at(TokenKind::Comma)) {
        if (first->kind == PatternKind::MatchStar)
            return invalidSyntax();
        return first;
    }
    Vector<Pattern*, 8> patterns;
    if (!parseOpenSequence(patterns, first, TokenKind::Colon))
        return nullptr;
    auto* sequence = make<MatchSequence>(rangeFrom(start));
    sequence->patterns = m_arena.copy(patterns);
    return sequence;
}

// After a first pattern: (',' maybe_star_pattern)* [',']. `close` or `if` follows.
bool Parser::parseOpenSequence(Vector<Pattern*, 8>& patterns, Pattern* first, TokenKind close)
{
    patterns.append(first);
    while (accept(TokenKind::Comma)) {
        if (at(close) || at(TokenKind::KwIf))
            break;
        Pattern* pattern = parseMaybeStarPattern();
        if (!pattern)
            return false;
        patterns.append(pattern);
    }
    return true;
}

// maybe_star_pattern: '*' (capture target | "_") | pattern
Pattern* Parser::parseMaybeStarPattern()
{
    if (!at(TokenKind::Star))
        return parsePattern();
    Position start = startPosition();
    advance();
    Name name = expectName();
    if (!name)
        return nullptr;
    if (at(TokenKind::Dot) || at(TokenKind::LPar) || at(TokenKind::Equal))
        return invalidSyntax();
    auto* star = make<MatchStar>(rangeFrom(start));
    star->name = *name == m_underscore ? nullptr : name;
    return star;
}

// pattern: or_pattern ['as' capture target]
Pattern* Parser::parsePattern()
{
    if (!isSafeToRecurse()) [[unlikely]]
        return stackOverflow();
    Position start = startPosition();
    Pattern* pattern = parseOrPattern();
    if (!pattern || !at(TokenKind::KwAs))
        return pattern;
    advance();
    if (atSoftKeyword(m_underscore))
        return syntaxErrorAt(peek(), "cannot use '_' as a target"_s);
    TokenKind next = peek(1).kind;
    if (!at(TokenKind::Name) || next == TokenKind::Dot || next == TokenKind::LPar || next == TokenKind::Equal) {
        SetForScope diagnosing(m_diagnosing, true);
        Expr* target = parseExpression();
        if (!target)
            return nullptr;
        return syntaxErrorAt(target->range, makeString("cannot use "_s, describe(target), " as pattern target"_s));
    }
    Name name = advance().identifier;
    auto* as = make<MatchAs>(rangeFrom(start));
    as->pattern = pattern;
    as->name = name;
    return as;
}

// or_pattern: '|'.closed_pattern+
Pattern* Parser::parseOrPattern()
{
    Position start = startPosition();
    Pattern* first = parseClosedPattern();
    if (!first || !at(TokenKind::VBar))
        return first;
    Vector<Pattern*, 8> patterns;
    patterns.append(first);
    while (accept(TokenKind::VBar)) {
        Pattern* pattern = parseClosedPattern();
        if (!pattern)
            return nullptr;
        patterns.append(pattern);
    }
    auto* result = make<MatchOr>(rangeFrom(start));
    result->patterns = m_arena.copy(patterns);
    return result;
}

// signed_number !('+' | '-') | signed_real_number ('+' | '-') imaginary_number
Expr* Parser::parseSignedNumberOrComplex()
{
    Position start = startPosition();
    std::optional<UnaryOperator> sign;
    if (at(TokenKind::Minus) || at(TokenKind::Plus))
        sign = advance().kind == TokenKind::Minus ? UnaryOperator::USub : UnaryOperator::UAdd;
    if (!at(TokenKind::Number))
        return invalidSyntax();
    Expr* number = parseNumber();
    if (!number)
        return nullptr;
    Expr* real = number;
    if (sign) {
        auto* unary = make<UnaryOp>(rangeFrom(start));
        unary->op = *sign;
        unary->operand = number;
        real = unary;
    }
    if (!at(TokenKind::Plus) && !at(TokenKind::Minus))
        return real;

    if (number->as<Constant>()->value.kind == ConstantValue::Kind::Complex)
        return syntaxErrorAt(number->range, "real number required in complex literal"_s);
    Operator op = advance().kind == TokenKind::Plus ? Operator::Add : Operator::Sub;
    if (!at(TokenKind::Number))
        return invalidSyntax();
    Expr* imaginary = parseNumber();
    if (!imaginary)
        return nullptr;
    if (imaginary->as<Constant>()->value.kind != ConstantValue::Kind::Complex)
        return syntaxErrorAt(imaginary->range, "imaginary number required in complex literal"_s);
    auto* result = make<BinOp>(rangeFrom(start));
    result->left = real;
    result->op = op;
    result->right = imaginary;
    return result;
}

// name_or_attr: '.'.NAME+
Expr* Parser::parseNameOrAttribute()
{
    Position start = startPosition();
    Expr* result = makeName(advance());
    while (accept(TokenKind::Dot)) {
        Name name = expectName();
        if (!name)
            return nullptr;
        auto* attribute = make<Attribute>(rangeFrom(start));
        attribute->value = result;
        attribute->attr = name;
        attribute->ctx = ExprContext::Load;
        result = attribute;
    }
    return result;
}

Pattern* Parser::parseClosedPattern()
{
    Position start = startPosition();
    switch (kind()) {
    case TokenKind::Number:
    case TokenKind::Minus:
    case TokenKind::Plus: {
        Expr* value = parseSignedNumberOrComplex();
        if (!value)
            return nullptr;
        auto* pattern = make<MatchValue>(rangeFrom(start));
        pattern->value = value;
        return pattern;
    }
    case TokenKind::String:
    case TokenKind::FStringStart:
    case TokenKind::TStringStart: {
        Expr* value = parseStrings();
        if (!value)
            return nullptr;
        auto* pattern = make<MatchValue>(rangeFrom(start));
        pattern->value = value;
        return pattern;
    }
    case TokenKind::KwNone:
    case TokenKind::KwTrue:
    case TokenKind::KwFalse: {
        TokenKind kind = advance().kind;
        auto* pattern = make<MatchSingleton>(rangeFrom(start));
        pattern->value = ConstantValue(kind == TokenKind::KwNone ? ConstantValue::Kind::None : kind == TokenKind::KwTrue ? ConstantValue::Kind::True : ConstantValue::Kind::False);
        return pattern;
    }
    case TokenKind::Name: {
        TokenKind next = peek(1).kind;
        if (next != TokenKind::Dot && next != TokenKind::LPar) {
            if (next == TokenKind::Equal)
                return invalidSyntax();
            Name name = advance().identifier;
            auto* pattern = make<MatchAs>(rangeFrom(start));
            pattern->pattern = nullptr;
            pattern->name = *name == m_underscore ? nullptr : name;
            return pattern;
        }
        Expr* value = parseNameOrAttribute();
        if (!value)
            return nullptr;
        if (at(TokenKind::LPar))
            return parseClassPattern(value, start);
        if (at(TokenKind::Equal))
            return invalidSyntax();
        auto* pattern = make<MatchValue>(rangeFrom(start));
        pattern->value = value;
        return pattern;
    }
    case TokenKind::LPar:
        return parseSequencePattern(TokenKind::RPar);
    case TokenKind::LSqb:
        return parseSequencePattern(TokenKind::RSqb);
    case TokenKind::LBrace:
        return parseMappingPattern();
    default:
        return invalidSyntax();
    }
}

// '[' maybe_sequence_pattern? ']' | '(' open_sequence_pattern? ')' | '(' pattern ')'
Pattern* Parser::parseSequencePattern(TokenKind close)
{
    Position start = startPosition();
    advance();
    Vector<Pattern*, 8> patterns;
    if (!at(close)) {
        Pattern* first = parseMaybeStarPattern();
        if (!first)
            return nullptr;
        if (close == TokenKind::RPar && at(TokenKind::RPar)) {
            // Parentheses around one pattern only group.
            if (first->kind == PatternKind::MatchStar)
                return invalidSyntax();
            advance();
            return first;
        }
        if (!parseOpenSequence(patterns, first, close))
            return nullptr;
    }
    if (!expect(close))
        return nullptr;
    auto* sequence = make<MatchSequence>(rangeFrom(start));
    sequence->patterns = m_arena.copy(patterns);
    return sequence;
}

// '{' [','.key_value_pattern+] [',' '**' capture target] [','] '}'
Pattern* Parser::parseMappingPattern()
{
    Position start = startPosition();
    advance();
    ExprVector keys;
    Vector<Pattern*, 8> patterns;
    Name rest = nullptr;
    Range restRange;
    bool restIsLast = true;
    while (!at(TokenKind::RBrace)) {
        if (at(TokenKind::DoubleStar)) {
            if (rest)
                return invalidSyntax();
            advance();
            if (!at(TokenKind::Name) || atSoftKeyword(m_underscore))
                return invalidSyntax();
            restRange = rangeOf(peek());
            rest = advance().identifier;
            if (at(TokenKind::Dot) || at(TokenKind::LPar) || at(TokenKind::Equal))
                return invalidSyntax();
        } else {
            Expr* key;
            switch (kind()) {
            case TokenKind::Number:
            case TokenKind::Minus:
            case TokenKind::Plus:
                key = parseSignedNumberOrComplex();
                break;
            case TokenKind::String:
            case TokenKind::FStringStart:
            case TokenKind::TStringStart:
                key = parseStrings();
                break;
            case TokenKind::KwNone:
            case TokenKind::KwTrue:
            case TokenKind::KwFalse:
                key = parseAtom();
                break;
            case TokenKind::Name:
                // Only a dotted name is a value.
                if (peek(1).kind != TokenKind::Dot)
                    return invalidSyntax();
                key = parseNameOrAttribute();
                break;
            default:
                return invalidSyntax();
            }
            if (!key || !expect(TokenKind::Colon))
                return nullptr;
            Pattern* pattern = parsePattern();
            if (!pattern)
                return nullptr;
            keys.append(key);
            patterns.append(pattern);
            if (rest)
                restIsLast = false;
        }
        if (!accept(TokenKind::Comma))
            break;
    }
    if (!expect(TokenKind::RBrace))
        return nullptr;
    if (!restIsLast)
        return syntaxErrorAt(restRange, "double star pattern must be the last (right-most) subpattern in the mapping pattern"_s);
    auto* mapping = make<MatchMapping>(rangeFrom(start));
    mapping->keys = m_arena.copy(keys);
    mapping->patterns = m_arena.copy(patterns);
    mapping->rest = rest;
    return mapping;
}

// name_or_attr '(' [','.pattern+] [',' ','.(NAME '=' pattern)+] [','] ')'
Pattern* Parser::parseClassPattern(Expr* cls, Position start)
{
    advance();
    Vector<Pattern*, 8> patterns;
    Vector<Name, 8> keywordNames;
    Vector<Pattern*, 8> keywordPatterns;
    Vector<Pattern*, 8> misplaced;
    while (!at(TokenKind::RPar)) {
        if (at(TokenKind::Name) && peek(1).kind == TokenKind::Equal) {
            if (!misplaced.isEmpty())
                break;
            Name name = advance().identifier;
            advance();
            Pattern* pattern = parsePattern();
            if (!pattern)
                return nullptr;
            keywordNames.append(name);
            keywordPatterns.append(pattern);
        } else {
            Pattern* pattern = parsePattern();
            if (!pattern)
                return nullptr;
            (keywordNames.isEmpty() ? patterns : misplaced).append(pattern);
        }
        if (!accept(TokenKind::Comma))
            break;
    }
    if (!misplaced.isEmpty())
        return syntaxErrorAt(rangeBetween(misplaced.first()->range, misplaced.last()->range), "positional patterns follow keyword patterns"_s);
    if (!expect(TokenKind::RPar))
        return nullptr;
    auto* result = make<MatchClass>(rangeFrom(start));
    result->cls = cls;
    result->patterns = m_arena.copy(patterns);
    result->kwdAttrs = m_arena.copy(keywordNames);
    result->kwdPatterns = m_arena.copy(keywordPatterns);
    return result;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
