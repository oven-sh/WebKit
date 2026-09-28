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

#include "config.h"
#include "PythonParser.h"

#include "PythonLexer.h"
#include "VM.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

namespace {

// Recursive descent over the tokens. A function here is a rule of Grammar/python.gram in CPython, or a few of them, and gives null
// if what is there is not what it is for. It goes back and tries something else only where the grammar cannot be told apart by
// looking a token or two ahead: `with (`, and the words that are keywords only sometimes.
class Parser {
public:
    // `error` is the scanner's, if it has one.
    Parser(VM& vm, Arena& arena, const Vector<Token>& tokens, SyntaxError& error)
        : m_vm(vm)
        , m_arena(arena)
        , m_tokens(tokens)
        , m_error(error)
        , m_scannerError(std::exchange(error, { }))
    {
    }

    Module* parseModule(Module::Kind kind)
    {
        auto* module = m_arena.create<Module>();
        module->kind = kind;
        bool ok = false;
        switch (kind) {
        case Module::Kind::Module: {
            Vector<Statement*, 16> body;
            ok = parseStatementsUntil(TokenKind::EndMarker, body);
            module->body = m_arena.copy(body);
            break;
        }
        case Module::Kind::Interactive: {
            Vector<Statement*, 16> body;
            ok = at(TokenKind::EndMarker) || consume(TokenKind::Newline) || parseStatement(body);
            module->body = m_arena.copy(body);
            break;
        }
        case Module::Kind::Expression:
            module->expression = parseExpressions();
            if (module->expression) {
                while (consume(TokenKind::Newline)) { }
                ok = at(TokenKind::EndMarker);
            }
            break;
        }
        if (ok && !m_error)
            return module;
        // What the scanner stumbled on comes first if the parser got that far.
        if (m_tokens[m_furthest].kind == TokenKind::Error)
            m_error = m_scannerError;
        else if (!m_error) {
            failGenerically();
            // With nothing better to say, what is wrong further on may be the reason. A bracket that was never closed is, if it was opened before this.
            if (m_scannerError && m_error.kind == SyntaxError::Kind::SyntaxError && (!m_scannerError.isUnclosedBracket || m_error.line > m_scannerError.line))
                m_error = m_scannerError;
        }
        return nullptr;
    }

    Statement* parseDefinitionAlone() { return at(SoftKeyword::Type) ? parseTypeAlias() : parseDefinition(); }
    Expression* parseExpressionAlone() { return parseExpression(); }

private:
    // ---- Tokens

    const Token& peek(unsigned ahead = 0)
    {
        unsigned index = std::min<unsigned>(m_index + ahead, m_tokens.size() - 1);
        m_furthest = std::max(m_furthest, index);
        return m_tokens[index];
    }

    bool at(TokenKind kind) { return peek().kind == kind; }
    bool at(SoftKeyword keyword) { return peek().kind == TokenKind::Name && peek().softKeyword == keyword; }
    bool atAhead(unsigned ahead, TokenKind kind) { return peek(ahead).kind == kind; }

    const Token& next()
    {
        const Token& token = peek();
        if (m_index + 1 < m_tokens.size())
            ++m_index;
        return token;
    }

    bool consume(TokenKind kind)
    {
        if (!at(kind))
            return false;
        next();
        return true;
    }

    // The last one taken that is something one can see.
    const Token& previous()
    {
        unsigned index = m_index;
        while (index) {
            const Token& token = m_tokens[--index];
            switch (token.kind) {
            case TokenKind::Newline:
            case TokenKind::Indent:
            case TokenKind::Dedent:
            case TokenKind::EndMarker:
            case TokenKind::Error:
                continue;
            default:
                return token;
            }
        }
        return m_tokens[0];
    }

    // ---- Errors

    // While something is only being tried, being wrong is not an error.
    bool isSpeculating() const { return m_speculationDepth; }

    std::nullptr_t fail(String&& message, unsigned line, unsigned column, unsigned endLine, unsigned endColumn, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        if (!m_error && !isSpeculating())
            m_error = { kind, false, WTF::move(message), line, column, endLine, endColumn };
        return nullptr;
    }

    std::nullptr_t fail(String&& message, const Token& token, SyntaxError::Kind kind = SyntaxError::Kind::SyntaxError)
    {
        return fail(WTF::move(message), token.line, token.column, token.endLine, token.endColumn, kind);
    }

    std::nullptr_t fail(String&& message, const Node& node)
    {
        return fail(WTF::move(message), node.line, node.column, node.endLine, node.endColumn);
    }

    std::nullptr_t fail(String&& message, const Node& first, const Node& last)
    {
        return fail(WTF::move(message), first.line, first.column, last.endLine, last.endColumn);
    }

    std::nullptr_t fail(String&& message)
    {
        return fail(WTF::move(message), peek());
    }

    // Nothing better to say than where it stopped making sense.
    void failGenerically()
    {
        const Token& token = m_tokens[m_furthest];
        if (token.kind == TokenKind::Indent) {
            fail("unexpected indent"_s, token, SyntaxError::Kind::IndentationError);
            return;
        }
        if (token.kind == TokenKind::Dedent) {
            fail("unexpected unindent"_s, token, SyntaxError::Kind::IndentationError);
            return;
        }
        fail("invalid syntax"_s, token);
    }

    bool expect(TokenKind kind)
    {
        return consume(kind);
    }

    // Where the grammar insists on a colon, its absence is what is wrong. Elsewhere that is only said if the line ends there.
    enum class ColonIs : uint8_t { Insisted, Expected };
    bool expectColon(ColonIs colonIs = ColonIs::Expected)
    {
        if (consume(TokenKind::Colon))
            return true;
        if (colonIs == ColonIs::Insisted || at(TokenKind::Newline))
            fail("expected ':'"_s);
        return false;
    }

    bool isSafeToRecurse()
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return true;
        fail("too many nested parentheses"_s);
        return false;
    }

    template<typename Function>
    auto speculate(const Function& function)
    {
        unsigned index = m_index;
        ++m_speculationDepth;
        auto result = function();
        --m_speculationDepth;
        if (!result)
            m_index = index;
        return result;
    }

    // ---- Nodes

    struct Mark {
        unsigned line;
        unsigned column;
        unsigned start;
    };

    Mark mark()
    {
        const Token& token = peek();
        return { token.line, token.column, token.start };
    }

    static Mark markOf(const Token& token) { return { token.line, token.column, token.start }; }
    static Mark markOf(const Node& node) { return { node.line, node.column, node.start }; }

    // A node that began at the mark and ends with the last token taken.
    template<typename T>
    T* make(Mark mark)
    {
        T* node = m_arena.create<T>();
        node->line = mark.line;
        node->column = mark.column;
        node->start = mark.start;
        const Token& last = previous();
        node->endLine = last.endLine;
        node->endColumn = last.endColumn;
        node->end = last.end;
        return node;
    }

    template<typename T>
    T* make(const Token& token)
    {
        T* node = m_arena.create<T>();
        setRange(*node, token, token);
        return node;
    }

    template<typename First, typename Last>
    static void setRange(Node& node, const First& first, const Last& last)
    {
        node.line = first.line;
        node.column = first.column;
        node.start = first.start;
        node.endLine = last.endLine;
        node.endColumn = last.endColumn;
        node.end = last.end;
    }

    const Identifier* makeIdentifier(const String& string)
    {
        if (string.is8Bit())
            return &m_arena.identifiers().makeIdentifier(m_vm, string.span8());
        return &m_arena.identifiers().makeIdentifier(m_vm, string.span16());
    }

    Name* makeName(const Token& token, ExpressionContext context = ExpressionContext::Load)
    {
        auto* name = make<Name>(token);
        name->id = token.text;
        name->context = context;
        return name;
    }

    // ---- Targets

    static ASCIILiteral describe(Expression& expression)
    {
        switch (expression.kind) {
        case Expression::Kind::Attribute:
            return "attribute"_s;
        case Expression::Kind::Subscript:
            return "subscript"_s;
        case Expression::Kind::Starred:
            return "starred"_s;
        case Expression::Kind::Name:
            return "name"_s;
        case Expression::Kind::List:
            return "list"_s;
        case Expression::Kind::Tuple:
            return "tuple"_s;
        case Expression::Kind::Lambda:
            return "lambda"_s;
        case Expression::Kind::Call:
            return "function call"_s;
        case Expression::Kind::BoolOp:
        case Expression::Kind::BinOp:
        case Expression::Kind::UnaryOp:
            return "expression"_s;
        case Expression::Kind::GeneratorExp:
            return "generator expression"_s;
        case Expression::Kind::Yield:
        case Expression::Kind::YieldFrom:
            return "yield expression"_s;
        case Expression::Kind::Await:
            return "await expression"_s;
        case Expression::Kind::ListComp:
            return "list comprehension"_s;
        case Expression::Kind::SetComp:
            return "set comprehension"_s;
        case Expression::Kind::DictComp:
            return "dict comprehension"_s;
        case Expression::Kind::Dict:
            return "dict literal"_s;
        case Expression::Kind::Set:
            return "set display"_s;
        case Expression::Kind::JoinedStr:
        case Expression::Kind::FormattedValue:
            return "f-string expression"_s;
        case Expression::Kind::TemplateStr:
        case Expression::Kind::Interpolation:
            return "t-string expression"_s;
        case Expression::Kind::Constant:
            switch (expression.as<Constant>().type) {
            case Constant::Type::None:
                return "None"_s;
            case Constant::Type::True:
                return "True"_s;
            case Constant::Type::False:
                return "False"_s;
            case Constant::Type::Ellipsis:
                return "ellipsis"_s;
            default:
                return "literal"_s;
            }
        case Expression::Kind::Compare:
            return "comparison"_s;
        case Expression::Kind::IfExp:
            return "conditional expression"_s;
        case Expression::Kind::NamedExpr:
            return "named expression"_s;
        case Expression::Kind::Slice:
            return "slice"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // What was parsed as an expression turns out to be assigned to, or deleted. Gives the part of it that cannot be, if any.
    Expression* setContext(Expression& expression, ExpressionContext context)
    {
        switch (expression.kind) {
        case Expression::Kind::Name:
            expression.as<Name>().context = context;
            return nullptr;
        case Expression::Kind::Attribute:
            expression.as<Attribute>().context = context;
            return nullptr;
        case Expression::Kind::Subscript:
            expression.as<Subscript>().context = context;
            return nullptr;
        case Expression::Kind::Starred:
            if (context == ExpressionContext::Del)
                return &expression;
            expression.as<Starred>().context = context;
            return setContext(*expression.as<Starred>().value, context);
        case Expression::Kind::List:
            expression.as<List>().context = context;
            for (Expression* element : expression.as<List>().elements) {
                if (Expression* invalid = setContext(*element, context))
                    return invalid;
            }
            return nullptr;
        case Expression::Kind::Tuple:
            expression.as<Tuple>().context = context;
            for (Expression* element : expression.as<Tuple>().elements) {
                if (Expression* invalid = setContext(*element, context))
                    return invalid;
            }
            return nullptr;
        default:
            return &expression;
        }
    }

    bool makeTarget(Expression& expression, ExpressionContext context)
    {
        Expression* invalid = setContext(expression, context);
        if (!invalid)
            return true;
        fail(makeString(context == ExpressionContext::Del ? "cannot delete "_s : "cannot assign to "_s, describe(*invalid)), *invalid);
        return false;
    }

    // The left of `=` in a statement.
    bool makeAssignmentTarget(Expression& expression)
    {
        Expression* invalid = setContext(expression, ExpressionContext::Store);
        if (!invalid)
            return true;
        if (invalid->is<Yield>() || invalid->is<YieldFrom>()) {
            fail("assignment to yield expression not possible"_s, *invalid);
            return false;
        }
        bool isWhole = invalid == &expression;
        bool mayHaveMeantEquality = isWhole && !invalid->is<GeneratorExp>();
        if (auto* constant = invalid->tryAs<Constant>())
            mayHaveMeantEquality &= constant->type != Constant::Type::None && constant->type != Constant::Type::True && constant->type != Constant::Type::False;
        if (mayHaveMeantEquality)
            fail(makeString("cannot assign to "_s, describe(*invalid), " here. Maybe you meant '==' instead of '='?"_s), *invalid);
        else
            fail(makeString("cannot assign to "_s, describe(*invalid)), *invalid);
        return false;
    }

    // star_target
    Expression* parseStarTarget()
    {
        if (at(TokenKind::Star)) {
            Mark start = mark();
            next();
            if (at(TokenKind::Star))
                return nullptr;
            Expression* value = parseStarTarget();
            if (!value)
                return nullptr;
            auto* starred = make<Starred>(start);
            starred->value = value;
            starred->context = ExpressionContext::Store;
            return starred;
        }
        Expression* target = parsePrimary();
        if (!target || !makeTarget(*target, ExpressionContext::Store))
            return nullptr;
        return target;
    }

    // star_targets, which are followed by `in`.
    Expression* parseStarTargets()
    {
        Mark start = mark();
        Expression* first = parseStarTarget();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma))
            return first;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (at(TokenKind::KeywordIn))
                break;
            Expression* element = parseStarTarget();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        tuple->context = ExpressionContext::Store;
        return tuple;
    }

    // ---- Expressions

    template<typename ParseElement>
    Expression* parseTupleOrSingle(const ParseElement& parseElement, bool (Parser::*canStartElement)())
    {
        Mark start = mark();
        Expression* first = parseElement();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma))
            return first;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (!(this->*canStartElement)())
                break;
            Expression* element = parseElement();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        return tuple;
    }

    // Whether an expression could begin here. After a comma, that says whether the comma was the last thing.
    bool canStartExpression()
    {
        switch (peek().kind) {
        case TokenKind::Name:
        case TokenKind::Number:
        case TokenKind::String:
        case TokenKind::FStringStart:
        case TokenKind::TStringStart:
        case TokenKind::LeftParenthesis:
        case TokenKind::LeftBracket:
        case TokenKind::LeftBrace:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Tilde:
        case TokenKind::Ellipsis:
        case TokenKind::KeywordNone:
        case TokenKind::KeywordTrue:
        case TokenKind::KeywordFalse:
        case TokenKind::KeywordNot:
        case TokenKind::KeywordLambda:
        case TokenKind::KeywordAwait:
            return true;
        default:
            return false;
        }
    }

    bool canStartStarExpression() { return at(TokenKind::Star) || canStartExpression(); }

    // expressions
    Expression* parseExpressions()
    {
        return parseTupleOrSingle([&] { return parseExpression(); }, &Parser::canStartExpression);
    }

    // star_expressions
    Expression* parseStarExpressions()
    {
        return parseTupleOrSingle([&] { return parseStarExpression(); }, &Parser::canStartStarExpression);
    }

    Expression* parseStarred()
    {
        Mark start = mark();
        next();
        Expression* value = parseBitwiseOr();
        if (!value)
            return nullptr;
        auto* starred = make<Starred>(start);
        starred->value = value;
        return starred;
    }

    // star_expression
    Expression* parseStarExpression()
    {
        if (at(TokenKind::Star))
            return parseStarred();
        return parseExpression();
    }

    // star_named_expression
    Expression* parseStarNamedExpression()
    {
        if (at(TokenKind::Star))
            return parseStarred();
        return parseNamedExpression();
    }

    // annotated_rhs
    Expression* parseYieldOrStarExpressions()
    {
        if (at(TokenKind::KeywordYield))
            return parseYield();
        return parseStarExpressions();
    }

    // yield_expr
    Expression* parseYield()
    {
        Mark start = mark();
        next();
        if (consume(TokenKind::KeywordFrom)) {
            Expression* value = parseExpression();
            if (!value)
                return nullptr;
            auto* yield = make<YieldFrom>(start);
            yield->value = value;
            return yield;
        }
        Expression* value = nullptr;
        if (canStartStarExpression()) {
            value = parseStarExpressions();
            if (!value)
                return nullptr;
        }
        auto* yield = make<Yield>(start);
        yield->value = value;
        return yield;
    }

    // named_expression
    Expression* parseNamedExpression()
    {
        if (at(TokenKind::Name) && atAhead(1, TokenKind::ColonEqual)) {
            Mark start = mark();
            Name* target = makeName(next(), ExpressionContext::Store);
            next();
            Expression* value = parseExpression();
            if (!value)
                return nullptr;
            auto* named = make<NamedExpr>(start);
            named->target = target;
            named->value = value;
            return named;
        }
        Expression* expression = parseExpression();
        if (!expression)
            return nullptr;
        if (at(TokenKind::ColonEqual))
            return fail(makeString("cannot use assignment expressions with "_s, describe(*expression)), *expression);
        return expression;
    }

    // expression
    Expression* parseExpression()
    {
        if (!isSafeToRecurse())
            return nullptr;
        if (at(TokenKind::KeywordLambda))
            return parseLambda();
        Mark start = mark();
        Expression* body = parseDisjunction();
        if (!body || !at(TokenKind::KeywordIf))
            return body;
        next();
        Expression* test = parseDisjunction();
        if (!test)
            return nullptr;
        if (!consume(TokenKind::KeywordElse))
            return fail("expected 'else' after 'if' expression"_s, *body, *test);
        Expression* orElse = parseExpression();
        if (!orElse)
            return nullptr;
        auto* conditional = make<IfExp>(start);
        conditional->test = test;
        conditional->body = body;
        conditional->orElse = orElse;
        return conditional;
    }

    // lambdef
    Expression* parseLambda()
    {
        Mark start = mark();
        next();
        Arguments* arguments = parseParameters(TokenKind::Colon, false);
        if (!arguments || !expectColon(ColonIs::Insisted))
            return nullptr;
        Expression* body = parseExpression();
        if (!body)
            return nullptr;
        auto* lambda = make<Lambda>(start);
        lambda->arguments = arguments;
        lambda->body = body;
        return lambda;
    }

    template<typename ParseOperand>
    Expression* parseBooleanOperation(TokenKind keyword, BooleanOperator op, const ParseOperand& parseOperand)
    {
        Mark start = mark();
        Expression* first = parseOperand();
        if (!first || !at(keyword))
            return first;
        Vector<Expression*, 8> values;
        values.append(first);
        while (consume(keyword)) {
            Expression* value = parseOperand();
            if (!value)
                return nullptr;
            values.append(value);
        }
        auto* operation = make<BoolOp>(start);
        operation->op = op;
        operation->values = m_arena.copy(values);
        return operation;
    }

    Expression* parseDisjunction()
    {
        return parseBooleanOperation(TokenKind::KeywordOr, BooleanOperator::Or, [&] { return parseConjunction(); });
    }

    Expression* parseConjunction()
    {
        return parseBooleanOperation(TokenKind::KeywordAnd, BooleanOperator::And, [&] { return parseInversion(); });
    }

    Expression* parseInversion()
    {
        if (!at(TokenKind::KeywordNot))
            return parseComparison();
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        Expression* operand = parseInversion();
        if (!operand)
            return nullptr;
        auto* operation = make<UnaryOp>(start);
        operation->op = UnaryOperator::Not;
        operation->operand = operand;
        return operation;
    }

    std::optional<ComparisonOperator> consumeComparisonOperator()
    {
        switch (peek().kind) {
        case TokenKind::EqualEqual:
            next();
            return ComparisonOperator::Eq;
        case TokenKind::NotEqual:
            // from __future__ import barry_as_FLUFL
            if (peek().isLessGreater != m_usesLessGreater)
                return std::nullopt;
            next();
            return ComparisonOperator::NotEq;
        case TokenKind::Less:
            next();
            return ComparisonOperator::Lt;
        case TokenKind::LessEqual:
            next();
            return ComparisonOperator::LtE;
        case TokenKind::Greater:
            next();
            return ComparisonOperator::Gt;
        case TokenKind::GreaterEqual:
            next();
            return ComparisonOperator::GtE;
        case TokenKind::KeywordIn:
            next();
            return ComparisonOperator::In;
        case TokenKind::KeywordNot:
            if (!atAhead(1, TokenKind::KeywordIn))
                return std::nullopt;
            next();
            next();
            return ComparisonOperator::NotIn;
        case TokenKind::KeywordIs:
            next();
            if (consume(TokenKind::KeywordNot))
                return ComparisonOperator::IsNot;
            return ComparisonOperator::Is;
        default:
            return std::nullopt;
        }
    }

    Expression* parseComparison()
    {
        Mark start = mark();
        Expression* left = parseBitwiseOr();
        if (!left)
            return nullptr;
        auto op = consumeComparisonOperator();
        if (!op)
            return left;
        Vector<ComparisonOperator, 4> ops;
        Vector<Expression*, 4> comparators;
        do {
            Expression* comparator = parseBitwiseOr();
            if (!comparator)
                return nullptr;
            ops.append(*op);
            comparators.append(comparator);
            op = consumeComparisonOperator();
        } while (op);
        auto* compare = make<Compare>(start);
        compare->left = left;
        compare->ops = m_arena.copy(ops);
        compare->comparators = m_arena.copy(comparators);
        return compare;
    }

    struct BinaryOperatorInfo {
        BinaryOperator op;
        unsigned precedence;
    };

    // From bitwise_or, which binds loosest, down to term. All of them group to the left.
    static std::optional<BinaryOperatorInfo> binaryOperator(TokenKind kind)
    {
        switch (kind) {
        case TokenKind::VerticalBar:
            return BinaryOperatorInfo { BinaryOperator::BitOr, 1 };
        case TokenKind::Circumflex:
            return BinaryOperatorInfo { BinaryOperator::BitXor, 2 };
        case TokenKind::Ampersand:
            return BinaryOperatorInfo { BinaryOperator::BitAnd, 3 };
        case TokenKind::LeftShift:
            return BinaryOperatorInfo { BinaryOperator::LShift, 4 };
        case TokenKind::RightShift:
            return BinaryOperatorInfo { BinaryOperator::RShift, 4 };
        case TokenKind::Plus:
            return BinaryOperatorInfo { BinaryOperator::Add, 5 };
        case TokenKind::Minus:
            return BinaryOperatorInfo { BinaryOperator::Sub, 5 };
        case TokenKind::Star:
            return BinaryOperatorInfo { BinaryOperator::Mult, 6 };
        case TokenKind::Slash:
            return BinaryOperatorInfo { BinaryOperator::Div, 6 };
        case TokenKind::DoubleSlash:
            return BinaryOperatorInfo { BinaryOperator::FloorDiv, 6 };
        case TokenKind::Percent:
            return BinaryOperatorInfo { BinaryOperator::Mod, 6 };
        case TokenKind::At:
            return BinaryOperatorInfo { BinaryOperator::MatMult, 6 };
        default:
            return std::nullopt;
        }
    }

    Expression* parseBitwiseOr() { return parseBinaryOperation(1); }

    Expression* parseBinaryOperation(unsigned minimumPrecedence)
    {
        Mark start = mark();
        Expression* left = parseFactor();
        if (!left)
            return nullptr;
        while (true) {
            auto info = binaryOperator(peek().kind);
            if (!info || info->precedence < minimumPrecedence)
                return left;
            next();
            Expression* right = parseBinaryOperation(info->precedence + 1);
            if (!right)
                return nullptr;
            auto* operation = make<BinOp>(start);
            operation->left = left;
            operation->op = info->op;
            operation->right = right;
            left = operation;
        }
    }

    // factor
    Expression* parseFactor()
    {
        UnaryOperator op;
        switch (peek().kind) {
        case TokenKind::Plus:
            op = UnaryOperator::UAdd;
            break;
        case TokenKind::Minus:
            op = UnaryOperator::USub;
            break;
        case TokenKind::Tilde:
            op = UnaryOperator::Invert;
            break;
        default:
            return parsePower();
        }
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        Expression* operand = parseFactor();
        if (!operand)
            return nullptr;
        auto* operation = make<UnaryOp>(start);
        operation->op = op;
        operation->operand = operand;
        return operation;
    }

    // power
    Expression* parsePower()
    {
        Mark start = mark();
        Expression* left = parseAwaitPrimary();
        if (!left || !consume(TokenKind::DoubleStar))
            return left;
        Expression* right = parseFactor();
        if (!right)
            return nullptr;
        auto* operation = make<BinOp>(start);
        operation->left = left;
        operation->op = BinaryOperator::Pow;
        operation->right = right;
        return operation;
    }

    // await_primary
    Expression* parseAwaitPrimary()
    {
        if (!at(TokenKind::KeywordAwait))
            return parsePrimary();
        Mark start = mark();
        next();
        Expression* value = parsePrimary();
        if (!value)
            return nullptr;
        auto* await = make<Await>(start);
        await->value = value;
        return await;
    }

    // primary
    Expression* parsePrimary()
    {
        Mark start = mark();
        Expression* value = parseAtom();
        while (value) {
            switch (peek().kind) {
            case TokenKind::Dot: {
                next();
                if (!at(TokenKind::Name))
                    return nullptr;
                const Identifier* name = next().text;
                auto* attribute = make<Attribute>(start);
                attribute->value = value;
                attribute->attribute = name;
                value = attribute;
                continue;
            }
            case TokenKind::LeftParenthesis: {
                Vector<Expression*, 8> arguments;
                Vector<Keyword*, 8> keywords;
                if (!parseArguments(arguments, keywords))
                    return nullptr;
                auto* call = make<Call>(start);
                call->function = value;
                call->arguments = m_arena.copy(arguments);
                call->keywords = m_arena.copy(keywords);
                value = call;
                continue;
            }
            case TokenKind::LeftBracket: {
                next();
                Expression* slice = parseSlices();
                if (!slice || !expect(TokenKind::RightBracket))
                    return nullptr;
                auto* subscript = make<Subscript>(start);
                subscript->value = value;
                subscript->slice = slice;
                value = subscript;
                continue;
            }
            default:
                return value;
            }
        }
        return nullptr;
    }

    bool atComprehension()
    {
        return at(TokenKind::KeywordFor) || (at(TokenKind::KeywordAsync) && atAhead(1, TokenKind::KeywordFor));
    }

    // '(' [arguments] ')'
    bool parseArguments(Vector<Expression*, 8>& arguments, Vector<Keyword*, 8>& keywords)
    {
        Mark open = mark();
        next();
        bool sawKeywordUnpacking = false;
        while (!at(TokenKind::RightParenthesis)) {
            Mark start = mark();
            if (at(TokenKind::Star)) {
                next();
                Expression* value = parseExpression();
                if (!value)
                    return false;
                if (sawKeywordUnpacking) {
                    fail("iterable argument unpacking follows keyword argument unpacking"_s, *value);
                    return false;
                }
                auto* starred = make<Starred>(start);
                starred->value = value;
                arguments.append(starred);
            } else if (at(TokenKind::DoubleStar)) {
                next();
                Expression* value = parseExpression();
                if (!value)
                    return false;
                auto* keyword = make<Keyword>(start);
                keyword->value = value;
                keywords.append(keyword);
                sawKeywordUnpacking = true;
            } else if (at(TokenKind::Name) && atAhead(1, TokenKind::Equal)) {
                const Identifier* name = next().text;
                next();
                if (at(TokenKind::Comma) || at(TokenKind::RightParenthesis)) {
                    fail("expected argument value expression"_s, previous());
                    return false;
                }
                Expression* value = parseExpression();
                if (!value)
                    return false;
                auto* keyword = make<Keyword>(start);
                keyword->name = name;
                keyword->value = value;
                keywords.append(keyword);
            } else {
                Expression* value = parseNamedExpression();
                if (!value)
                    return false;
                if (atComprehension()) {
                    Vector<Comprehension*, 2> generators;
                    if (!parseComprehensionClauses(generators))
                        return false;
                    // f(x for x in y): the parentheses of the call are those of the generator expression.
                    bool isAlone = arguments.isEmpty() && keywords.isEmpty() && at(TokenKind::RightParenthesis);
                    if (!isAlone) {
                        fail("Generator expression must be parenthesized"_s, value->line, value->column, previous().endLine, previous().endColumn);
                        return false;
                    }
                    next();
                    auto* generator = make<GeneratorExp>(open);
                    generator->element = value;
                    generator->generators = m_arena.copy(generators);
                    arguments.append(generator);
                    return true;
                }
                if (at(TokenKind::Equal)) {
                    fail("expression cannot contain assignment, perhaps you meant \"==\"?"_s, value->line, value->column, peek().endLine, peek().endColumn);
                    return false;
                }
                if (!keywords.isEmpty()) {
                    fail(sawKeywordUnpacking ? "positional argument follows keyword argument unpacking"_s : "positional argument follows keyword argument"_s, *value);
                    return false;
                }
                arguments.append(value);
            }
            if (!consume(TokenKind::Comma))
                break;
        }
        return expect(TokenKind::RightParenthesis);
    }

    // slices
    Expression* parseSlices()
    {
        Mark start = mark();
        Expression* first = parseSliceOrStarred();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma) && !first->is<Starred>())
            return first;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (at(TokenKind::RightBracket))
                break;
            Expression* element = parseSliceOrStarred();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        return tuple;
    }

    Expression* parseSliceOrStarred()
    {
        Mark start = mark();
        if (at(TokenKind::Star)) {
            next();
            Expression* value = parseExpression();
            if (!value)
                return nullptr;
            auto* starred = make<Starred>(start);
            starred->value = value;
            return starred;
        }

        Expression* lower = nullptr;
        if (!at(TokenKind::Colon)) {
            lower = parseNamedExpression();
            if (!lower || !at(TokenKind::Colon))
                return lower;
        }
        next();
        auto atEndOfPart = [&] {
            return at(TokenKind::Colon) || at(TokenKind::Comma) || at(TokenKind::RightBracket);
        };
        Expression* upper = nullptr;
        if (!atEndOfPart()) {
            upper = parseExpression();
            if (!upper)
                return nullptr;
        }
        Expression* step = nullptr;
        if (consume(TokenKind::Colon) && !atEndOfPart()) {
            step = parseExpression();
            if (!step)
                return nullptr;
        }
        auto* slice = make<Slice>(start);
        slice->lower = lower;
        slice->upper = upper;
        slice->step = step;
        return slice;
    }

    Constant* makeConstant(const Token& token, Constant::Type type)
    {
        auto* constant = make<Constant>(token);
        constant->type = type;
        return constant;
    }

    Constant* makeNumber(const Token& token)
    {
        auto* constant = make<Constant>(token);
        switch (token.numberKind) {
        case NumberKind::Integer:
            constant->type = Constant::Type::Integer;
            constant->integer = token.integer;
            break;
        case NumberKind::BigInteger:
            constant->type = Constant::Type::BigInteger;
            constant->text = token.text;
            constant->radix = token.radix;
            break;
        case NumberKind::Float:
            constant->type = Constant::Type::Float;
            constant->real = token.real;
            break;
        case NumberKind::Imaginary:
            constant->type = Constant::Type::Imaginary;
            constant->real = token.real;
            break;
        }
        return constant;
    }

    // atom
    Expression* parseAtom()
    {
        switch (peek().kind) {
        case TokenKind::Name:
            return makeName(next());
        case TokenKind::KeywordTrue:
            return makeConstant(next(), Constant::Type::True);
        case TokenKind::KeywordFalse:
            return makeConstant(next(), Constant::Type::False);
        case TokenKind::KeywordNone:
            return makeConstant(next(), Constant::Type::None);
        case TokenKind::Ellipsis:
            return makeConstant(next(), Constant::Type::Ellipsis);
        case TokenKind::Number:
            return makeNumber(next());
        case TokenKind::String:
        case TokenKind::FStringStart:
        case TokenKind::TStringStart:
            return parseStrings();
        case TokenKind::LeftParenthesis:
            return parseParenthesized();
        case TokenKind::LeftBracket:
            return parseListDisplay();
        case TokenKind::LeftBrace:
            return parseBraceDisplay();
        default:
            return nullptr;
        }
    }

    // tuple | group | genexp
    Expression* parseParenthesized()
    {
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        if (consume(TokenKind::RightParenthesis))
            return make<Tuple>(start);

        if (at(TokenKind::KeywordYield)) {
            Expression* yield = parseYield();
            if (!yield || !expect(TokenKind::RightParenthesis))
                return nullptr;
            yield->isParenthesized = true;
            return yield;
        }

        Expression* first = parseStarNamedExpression();
        if (!first)
            return nullptr;

        if (atComprehension()) {
            if (first->is<Starred>())
                return fail("iterable unpacking cannot be used in comprehension"_s, *first);
            Vector<Comprehension*, 2> generators;
            if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightParenthesis))
                return nullptr;
            auto* generator = make<GeneratorExp>(start);
            generator->element = first;
            generator->generators = m_arena.copy(generators);
            return generator;
        }

        if (consume(TokenKind::RightParenthesis)) {
            if (first->is<Starred>())
                return fail("cannot use starred expression here"_s, *first);
            first->isParenthesized = true;
            return first;
        }

        if (!at(TokenKind::Comma))
            return nullptr;
        Vector<Expression*, 8> elements;
        elements.append(first);
        while (consume(TokenKind::Comma)) {
            if (at(TokenKind::RightParenthesis))
                break;
            Expression* element = parseStarNamedExpression();
            if (!element)
                return nullptr;
            elements.append(element);
        }
        if (!expect(TokenKind::RightParenthesis))
            return nullptr;
        auto* tuple = make<Tuple>(start);
        tuple->elements = m_arena.copy(elements);
        tuple->isParenthesized = true;
        return tuple;
    }

    // list | listcomp
    Expression* parseListDisplay()
    {
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        Vector<Expression*, 8> elements;
        while (!at(TokenKind::RightBracket)) {
            Expression* element = parseStarNamedExpression();
            if (!element)
                return nullptr;
            if (elements.isEmpty() && atComprehension()) {
                if (element->is<Starred>())
                    return fail("iterable unpacking cannot be used in comprehension"_s, *element);
                Vector<Comprehension*, 2> generators;
                if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightBracket))
                    return nullptr;
                auto* comprehension = make<ListComp>(start);
                comprehension->element = element;
                comprehension->generators = m_arena.copy(generators);
                return comprehension;
            }
            elements.append(element);
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightBracket))
            return nullptr;
        auto* list = make<List>(start);
        list->elements = m_arena.copy(elements);
        return list;
    }

    // dict | set | dictcomp | setcomp
    Expression* parseBraceDisplay()
    {
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        next();
        if (consume(TokenKind::RightBrace))
            return make<Dict>(start);

        Expression* first = nullptr;
        if (!at(TokenKind::DoubleStar)) {
            first = parseStarNamedExpression();
            if (!first)
                return nullptr;
        }

        if (first && !at(TokenKind::Colon)) {
            if (atComprehension()) {
                if (first->is<Starred>())
                    return fail("iterable unpacking cannot be used in comprehension"_s, *first);
                Vector<Comprehension*, 2> generators;
                if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightBrace))
                    return nullptr;
                auto* comprehension = make<SetComp>(start);
                comprehension->element = first;
                comprehension->generators = m_arena.copy(generators);
                return comprehension;
            }
            Vector<Expression*, 8> elements;
            elements.append(first);
            while (consume(TokenKind::Comma)) {
                if (at(TokenKind::RightBrace))
                    break;
                Expression* element = parseStarNamedExpression();
                if (!element)
                    return nullptr;
                elements.append(element);
            }
            if (!expect(TokenKind::RightBrace))
                return nullptr;
            auto* set = make<Set>(start);
            set->elements = m_arena.copy(elements);
            return set;
        }

        Vector<Expression*, 8> keys;
        Vector<Expression*, 8> values;
        Expression* key = first;
        while (true) {
            if (!key && at(TokenKind::DoubleStar)) {
                const Token& stars = next();
                Expression* value = parseBitwiseOr();
                if (!value)
                    return nullptr;
                if (keys.isEmpty() && atComprehension())
                    return fail("dict unpacking cannot be used in dict comprehension"_s, stars.line, stars.column, value->endLine, value->endColumn);
                keys.append(nullptr);
                values.append(value);
            } else {
                if (!key) {
                    key = parseExpression();
                    if (!key)
                        return nullptr;
                }
                if (key->is<Starred>() || (key->is<NamedExpr>() && !key->isParenthesized))
                    return nullptr;
                if (!consume(TokenKind::Colon))
                    return fail("':' expected after dictionary key"_s, *key);
                if (at(TokenKind::Star))
                    return fail("cannot use a starred expression in a dictionary value"_s);
                if (at(TokenKind::Comma) || at(TokenKind::RightBrace))
                    return fail("expression expected after dictionary key and ':'"_s, previous());
                Expression* value = parseExpression();
                if (!value)
                    return nullptr;
                if (keys.isEmpty() && atComprehension()) {
                    Vector<Comprehension*, 2> generators;
                    if (!parseComprehensionClauses(generators) || !expect(TokenKind::RightBrace))
                        return nullptr;
                    auto* comprehension = make<DictComp>(start);
                    comprehension->key = key;
                    comprehension->value = value;
                    comprehension->generators = m_arena.copy(generators);
                    return comprehension;
                }
                keys.append(key);
                values.append(value);
                key = nullptr;
            }
            if (!consume(TokenKind::Comma) || at(TokenKind::RightBrace))
                break;
        }
        if (!expect(TokenKind::RightBrace))
            return nullptr;
        auto* dict = make<Dict>(start);
        dict->keys = m_arena.copy(keys);
        dict->values = m_arena.copy(values);
        return dict;
    }

    // for_if_clauses
    bool parseComprehensionClauses(Vector<Comprehension*, 2>& generators)
    {
        while (atComprehension()) {
            auto* comprehension = m_arena.create<Comprehension>();
            comprehension->isAsync = consume(TokenKind::KeywordAsync);
            next();
            comprehension->target = parseStarTargets();
            if (!comprehension->target)
                return false;
            if (!consume(TokenKind::KeywordIn)) {
                fail("'in' expected after for-loop variables"_s);
                return false;
            }
            comprehension->iterable = parseDisjunction();
            if (!comprehension->iterable)
                return false;
            Vector<Expression*, 2> conditions;
            while (consume(TokenKind::KeywordIf)) {
                Expression* condition = parseDisjunction();
                if (!condition)
                    return false;
                conditions.append(condition);
            }
            comprehension->conditions = m_arena.copy(conditions);
            generators.append(comprehension);
        }
        return true;
    }

    // ---- Strings

    static bool isEmptyString(Expression& expression)
    {
        auto* constant = expression.tryAs<Constant>();
        return constant && constant->type == Constant::Type::String && constant->text->isEmpty();
    }

    Constant* makeStringConstant(const Token& token)
    {
        auto* constant = make<Constant>(token);
        constant->type = token.isBytes ? Constant::Type::Bytes : Constant::Type::String;
        constant->text = token.text;
        constant->hasUnicodePrefix = token.hasUnicodePrefix;
        return constant;
    }

    // Constants that stand next to each other are one constant.
    Constant* joinConstants(std::span<Expression*> constants, const Node& first, const Node& last)
    {
        StringBuilder builder;
        for (Expression* constant : constants)
            builder.append(constant->as<Constant>().text->string());
        auto* result = m_arena.create<Constant>();
        setRange(*result, first, last);
        result->type = constants[0]->as<Constant>().type;
        result->hasUnicodePrefix = constants[0]->as<Constant>().hasUnicodePrefix;
        result->text = makeIdentifier(builder.toString());
        return result;
    }

    // The pieces of strings that stand next to each other, at least one of which has expressions in it.
    Sequence<Expression*> joinPieces(const Vector<Expression*, 8>& strings)
    {
        Vector<Expression*, 16> flattened;
        for (Expression* string : strings) {
            if (auto* joined = string->tryAs<JoinedStr>())
                flattened.append(joined->values);
            else if (auto* templateString = string->tryAs<TemplateStr>())
                flattened.append(templateString->values);
            else
                flattened.append(string);
        }

        Vector<Expression*, 16> values;
        for (unsigned i = 0; i < flattened.size(); ++i) {
            Expression* piece = flattened[i];
            if (piece->is<Constant>()) {
                unsigned end = i + 1;
                while (end < flattened.size() && flattened[end]->is<Constant>())
                    ++end;
                if (end - i > 1) {
                    piece = joinConstants(flattened.mutableSpan().subspan(i, end - i), *flattened[i], *flattened[end - 1]);
                    i = end - 1;
                }
                if (isEmptyString(*piece))
                    continue;
            }
            values.append(piece);
        }
        return m_arena.copy(values);
    }

    // What _PyPegen_concatenate_strings does, of pieces that span from `first` to `last`.
    Expression* concatenateStrings(const Vector<Expression*, 8>& strings, const Node& first, const Node& last)
    {
        bool hasExpressions = false;
        bool hasText = false;
        bool hasBytes = false;
        for (Expression* string : strings) {
            if (auto* constant = string->tryAs<Constant>())
                (constant->type == Constant::Type::Bytes ? hasBytes : hasText) = true;
            else
                hasExpressions = true;
        }
        if ((hasText || hasExpressions) && hasBytes)
            return fail("cannot mix bytes and nonbytes literals"_s, first, last);

        if (!hasExpressions) {
            if (strings.size() == 1)
                return strings[0];
            Vector<Expression*, 8> copy = strings;
            return joinConstants(copy.mutableSpan(), first, last);
        }
        auto* joined = m_arena.create<JoinedStr>();
        setRange(*joined, first, last);
        joined->values = joinPieces(strings);
        return joined;
    }

    // strings
    Expression* parseStrings()
    {
        Vector<Expression*, 8> strings;
        bool isTemplate = at(TokenKind::TStringStart);
        while (true) {
            Expression* string = nullptr;
            if (at(TokenKind::String) && !isTemplate)
                string = makeStringConstant(next());
            else if (at(TokenKind::FStringStart) && !isTemplate)
                string = parseStringWithExpressions(false);
            else if (at(TokenKind::TStringStart) && isTemplate)
                string = parseStringWithExpressions(true);
            else if (at(TokenKind::String) || at(TokenKind::FStringStart) || at(TokenKind::TStringStart))
                return fail("cannot mix t-string literals with string or bytes literals"_s, strings[0]->line, strings[0]->column, peek().endLine, peek().endColumn);
            else
                break;
            if (!string)
                return nullptr;
            strings.append(string);
        }

        if (!isTemplate)
            return concatenateStrings(strings, *strings.first(), *strings.last());
        auto* result = m_arena.create<TemplateStr>();
        setRange(*result, *strings.first(), *strings.last());
        result->values = joinPieces(strings);
        return result;
    }

    // fstring | tstring
    Expression* parseStringWithExpressions(bool isTemplate)
    {
        const Token& open = next();
        TokenKind middle = isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle;
        TokenKind close = isTemplate ? TokenKind::TStringEnd : TokenKind::FStringEnd;

        Vector<Expression*, 8> values;
        while (!at(close)) {
            if (at(middle)) {
                Constant* text = makeStringConstant(next());
                if (!isEmptyString(*text))
                    values.append(text);
                continue;
            }
            if (!at(TokenKind::LeftBrace))
                return nullptr;
            if (!parseReplacementField(isTemplate, isTemplate, values))
                return nullptr;
        }
        const Token& end = next();

        if (isTemplate) {
            auto* result = m_arena.create<TemplateStr>();
            setRange(*result, open, end);
            result->values = m_arena.copy(values);
            return result;
        }
        auto* result = m_arena.create<JoinedStr>();
        setRange(*result, open, end);
        result->values = m_arena.copy(values);
        return result;
    }

    String prefixed(bool isTemplate, ASCIILiteral message)
    {
        return makeString(isTemplate ? 't' : 'f', "-string: "_s, message);
    }

    // fstring_replacement_field and its like. `isTemplate` is what kind of string this is in, for what to say when it is wrong, and
    // `makesInterpolation` is whether it is at the top of a t-string, and not in a format specification.
    bool parseReplacementField(bool isTemplate, bool makesInterpolation, Vector<Expression*, 8>& values)
    {
        if (!isSafeToRecurse())
            return false;
        Mark start = mark();
        next();

        switch (peek().kind) {
        case TokenKind::Equal:
            fail(prefixed(isTemplate, "valid expression required before '='"_s));
            return false;
        case TokenKind::Exclamation:
            fail(prefixed(isTemplate, "valid expression required before '!'"_s));
            return false;
        case TokenKind::Colon:
            fail(prefixed(isTemplate, "valid expression required before ':'"_s));
            return false;
        case TokenKind::RightBrace:
            fail(prefixed(isTemplate, "valid expression required before '}'"_s));
            return false;
        default:
            break;
        }

        Expression* value = parseYieldOrStarExpressions();
        if (!value) {
            fail(prefixed(isTemplate, "expecting a valid expression after '{'"_s), previous());
            return false;
        }

        bool isDebug = consume(TokenKind::Equal);

        // Whichever token comes right after the expression has its source.
        const Identifier* source = nullptr;
        unsigned debugEndLine = 0;
        unsigned debugEndColumn = 0;

        int conversion = -1;
        if (at(TokenKind::Exclamation)) {
            const Token& exclamation = next();
            if (!at(TokenKind::Name)) {
                fail(prefixed(isTemplate, "missing conversion character"_s));
                return false;
            }
            const Token& name = next();
            if (name.start != exclamation.end) {
                fail(prefixed(isTemplate, "conversion type must come right after the exclamation mark"_s), name);
                return false;
            }
            StringView text = name.text->string();
            if (text.length() != 1 || (text[0] != 's' && text[0] != 'r' && text[0] != 'a')) {
                fail(makeString(isTemplate ? 't' : 'f', "-string: invalid conversion character '"_s, text, "': expected 's', 'r', or 'a'"_s), name);
                return false;
            }
            conversion = text[0];
            source = exclamation.expressionSource;
            debugEndLine = name.line;
            debugEndColumn = name.column;
        }

        Expression* formatSpecification = nullptr;
        if (at(TokenKind::Colon)) {
            const Token& colon = next();
            formatSpecification = parseFormatSpecification(isTemplate, colon);
            if (!formatSpecification)
                return false;
            if (!source) {
                source = colon.expressionSource;
                debugEndLine = formatSpecification->line;
                debugEndColumn = formatSpecification->column + 1;
            }
        } else if (isDebug && conversion == -1)
            conversion = 'r';

        if (!at(TokenKind::RightBrace)) {
            fail(prefixed(isTemplate, formatSpecification ? "expecting '}', or format specs"_s : conversion != -1 && !isDebug ? "expecting ':' or '}'"_s : isDebug ? "expecting '!', or ':', or '}'"_s : "expecting '=', or '!', or ':', or '}'"_s));
            return false;
        }
        const Token& close = next();
        if (!source) {
            source = close.expressionSource;
            debugEndLine = close.endLine;
            debugEndColumn = close.endColumn;
        }

        Expression* result;
        if (makesInterpolation) {
            auto* interpolation = make<Interpolation>(start);
            interpolation->value = value;
            interpolation->conversion = conversion;
            interpolation->formatSpecification = formatSpecification;
            // Without what is after it: white space, and the = of t"{x=}".
            StringView text = source->string();
            unsigned length = text.length();
            while (length && (text[length - 1] == '=' || isPythonWhitespace(text[length - 1])))
                --length;
            interpolation->source = makeIdentifier(text.left(length).toString());
            result = interpolation;
        } else {
            auto* formatted = make<FormattedValue>(start);
            formatted->value = value;
            formatted->conversion = conversion;
            formatted->formatSpecification = formatSpecification;
            result = formatted;
        }

        if (isDebug) {
            // f"{x=}" is "x=" and then x.
            auto* text = m_arena.create<Constant>();
            text->type = Constant::Type::String;
            text->text = source;
            text->line = start.line;
            text->column = start.column + 1;
            text->start = start.start + 1;
            text->endLine = debugEndLine;
            text->endColumn = debugEndColumn - 1;
            values.append(text);
        }
        values.append(result);
        return true;
    }

    static bool isPythonWhitespace(char16_t c)
    {
        return c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1C && c <= 0x1F) || c == 0x85 || c == 0xA0 || (c >= 0x80 && u_isUWhiteSpace(c));
    }

    // fstring_full_format_spec, after its colon.
    Expression* parseFormatSpecification(bool isTemplate, const Token& colon)
    {
        TokenKind middle = isTemplate ? TokenKind::TStringMiddle : TokenKind::FStringMiddle;
        Vector<Expression*, 8> pieces;
        while (true) {
            if (at(middle)) {
                Constant* text = makeStringConstant(next());
                if (!isEmptyString(*text))
                    pieces.append(text);
                continue;
            }
            if (!at(TokenKind::LeftBrace))
                break;
            // What f"{x=}" adds is kept together with it here, and taken apart when the pieces are joined.
            Vector<Expression*, 8> field;
            if (!parseReplacementField(isTemplate, false, field))
                return nullptr;
            if (field.size() == 1) {
                pieces.append(field[0]);
                continue;
            }
            auto* pair = m_arena.create<JoinedStr>();
            setRange(*pair, *field[1], *field[1]);
            pair->values = m_arena.copy(field);
            pieces.append(pair);
        }

        Node range;
        setRange(range, colon, previous());
        if (pieces.isEmpty() || (pieces.size() == 1 && pieces[0]->is<Constant>())) {
            auto* joined = m_arena.create<JoinedStr>();
            setRange(*joined, range, range);
            joined->values = m_arena.copy(pieces);
            return joined;
        }
        return concatenateStrings(pieces, range, range);
    }

    // ---- Parameters

    // param, and lambda_param
    Argument* parseParameter(bool allowsAnnotation, bool allowsStarAnnotation = false)
    {
        if (!at(TokenKind::Name))
            return nullptr;
        Mark start = mark();
        const Identifier* name = next().text;
        Expression* annotation = nullptr;
        if (allowsAnnotation && consume(TokenKind::Colon)) {
            annotation = allowsStarAnnotation ? parseStarExpression() : parseExpression();
            if (!annotation)
                return nullptr;
        }
        auto* argument = make<Argument>(start);
        argument->name = name;
        argument->annotation = annotation;
        return argument;
    }

    // parameters and lambda_parameters, which may be none at all. `terminator` is what comes after them.
    Arguments* parseParameters(TokenKind terminator, bool allowsAnnotations)
    {
        Vector<Argument*, 8> positionalOnly;
        Vector<Argument*, 8> positional;
        Vector<Argument*, 8> keywordOnly;
        Vector<Expression*, 8> defaults;
        Vector<Expression*, 8> keywordDefaults;
        auto* arguments = m_arena.create<Arguments>();
        bool sawSlash = false;
        bool sawStar = false;

        while (!at(terminator)) {
            if (at(TokenKind::Slash)) {
                if (sawSlash)
                    return fail("/ may appear only once"_s);
                if (sawStar)
                    return fail("/ must be ahead of *"_s);
                if (positional.isEmpty())
                    return fail("at least one argument must precede /"_s);
                next();
                sawSlash = true;
                positionalOnly = std::exchange(positional, { });
                if (at(TokenKind::Star))
                    return fail("expected comma between / and *"_s, previous().line, previous().column, peek().endLine, peek().endColumn);
            } else if (at(TokenKind::Star)) {
                if (sawStar)
                    return fail("* argument may appear only once"_s);
                const Token& star = next();
                sawStar = true;
                if (at(TokenKind::Comma)) {
                    if (atAhead(1, terminator) || atAhead(1, TokenKind::DoubleStar))
                        return fail("named arguments must follow bare *"_s, star);
                } else {
                    if (at(terminator))
                        return fail("named arguments must follow bare *"_s, star);
                    arguments->variadic = parseParameter(allowsAnnotations, true);
                    if (!arguments->variadic)
                        return nullptr;
                    if (at(TokenKind::Equal))
                        return fail("var-positional argument cannot have default value"_s);
                }
            } else if (at(TokenKind::DoubleStar)) {
                next();
                arguments->keywordVariadic = parseParameter(allowsAnnotations);
                if (!arguments->keywordVariadic)
                    return nullptr;
                if (at(TokenKind::Equal))
                    return fail("var-keyword argument cannot have default value"_s);
                consume(TokenKind::Comma);
                if (!at(terminator))
                    return fail("arguments cannot follow var-keyword argument"_s);
                break;
            } else {
                if (at(TokenKind::LeftParenthesis))
                    return fail(allowsAnnotations ? "Function parameters cannot be parenthesized"_s : "Lambda expression parameters cannot be parenthesized"_s);
                Argument* argument = parseParameter(allowsAnnotations);
                if (!argument)
                    return nullptr;
                Expression* defaultValue = nullptr;
                if (consume(TokenKind::Equal)) {
                    if (at(TokenKind::Comma) || at(terminator))
                        return fail("expected default value expression"_s, previous());
                    defaultValue = parseExpression();
                    if (!defaultValue)
                        return nullptr;
                }
                if (sawStar) {
                    keywordOnly.append(argument);
                    keywordDefaults.append(defaultValue);
                } else {
                    if (defaultValue)
                        defaults.append(defaultValue);
                    else if (!defaults.isEmpty())
                        return fail("parameter without a default follows parameter with a default"_s, *argument);
                    positional.append(argument);
                }
            }
            if (!consume(TokenKind::Comma))
                break;
        }

        arguments->positionalOnly = m_arena.copy(positionalOnly);
        arguments->positional = m_arena.copy(positional);
        arguments->keywordOnly = m_arena.copy(keywordOnly);
        arguments->defaults = m_arena.copy(defaults);
        arguments->keywordDefaults = m_arena.copy(keywordDefaults);
        return arguments;
    }

    // type_params, if there are any.
    bool parseTypeParameters(Sequence<TypeParameter*>& result)
    {
        if (!at(TokenKind::LeftBracket))
            return true;
        const Token& open = next();
        if (at(TokenKind::RightBracket)) {
            fail("Type parameter list cannot be empty"_s, open.line, open.column, peek().endLine, peek().endColumn);
            return false;
        }
        Vector<TypeParameter*, 4> parameters;
        while (!at(TokenKind::RightBracket)) {
            Mark start = mark();
            auto kind = TypeParameter::Kind::TypeVar;
            if (consume(TokenKind::Star))
                kind = TypeParameter::Kind::TypeVarTuple;
            else if (consume(TokenKind::DoubleStar))
                kind = TypeParameter::Kind::ParamSpec;
            if (!at(TokenKind::Name))
                return false;
            const Identifier* name = next().text;
            Expression* bound = nullptr;
            if (at(TokenKind::Colon)) {
                const Token& colon = next();
                bound = parseExpression();
                if (!bound)
                    return false;
                if (kind != TypeParameter::Kind::TypeVar) {
                    ASCIILiteral what = bound->is<Tuple>() ? "constraints"_s : "bound"_s;
                    fail(makeString("cannot use "_s, what, " with "_s, kind == TypeParameter::Kind::TypeVarTuple ? "TypeVarTuple"_s : "ParamSpec"_s), colon.line, colon.column, bound->endLine, bound->endColumn);
                    return false;
                }
            }
            Expression* defaultValue = nullptr;
            if (consume(TokenKind::Equal)) {
                defaultValue = kind == TypeParameter::Kind::TypeVarTuple ? parseStarExpression() : parseExpression();
                if (!defaultValue)
                    return false;
            }
            auto* parameter = make<TypeParameter>(start);
            parameter->kind = kind;
            parameter->name = name;
            parameter->bound = bound;
            parameter->defaultValue = defaultValue;
            parameters.append(parameter);
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightBracket))
            return false;
        result = m_arena.copy(parameters);
        return true;
    }

    // ---- Patterns

    // patterns
    Pattern* parsePatterns()
    {
        Mark start = mark();
        Pattern* first = parseMaybeStarPattern();
        if (!first)
            return nullptr;
        if (!at(TokenKind::Comma)) {
            if (first->is<MatchStar>())
                return nullptr;
            return first;
        }
        Vector<Pattern*, 8> patterns;
        patterns.append(first);
        if (!parseRestOfSequencePattern(patterns, TokenKind::Colon))
            return nullptr;
        auto* sequence = make<MatchSequence>(start);
        sequence->patterns = m_arena.copy(patterns);
        return sequence;
    }

    // After the first of them: (',' maybe_star_pattern)* [',']
    bool parseRestOfSequencePattern(Vector<Pattern*, 8>& patterns, TokenKind terminator)
    {
        while (consume(TokenKind::Comma)) {
            if (at(terminator) || at(TokenKind::KeywordIf))
                break;
            Pattern* pattern = parseMaybeStarPattern();
            if (!pattern)
                return false;
            patterns.append(pattern);
        }
        return true;
    }

    // maybe_star_pattern
    Pattern* parseMaybeStarPattern()
    {
        if (!at(TokenKind::Star))
            return parsePattern();
        Mark start = mark();
        next();
        if (!at(TokenKind::Name))
            return nullptr;
        const Token& name = next();
        auto* star = make<MatchStar>(start);
        if (name.softKeyword != SoftKeyword::Underscore)
            star->name = name.text;
        return star;
    }

    // pattern
    Pattern* parsePattern()
    {
        if (!isSafeToRecurse())
            return nullptr;
        Mark start = mark();
        Pattern* pattern = parseOrPattern();
        if (!pattern || !consume(TokenKind::KeywordAs))
            return pattern;
        if (!at(TokenKind::Name)) {
            Expression* target = speculate([&] { return parseExpression(); });
            if (target)
                return fail(makeString("cannot use "_s, describe(*target), " as pattern target"_s), *target);
            return nullptr;
        }
        if (at(SoftKeyword::Underscore))
            return fail("cannot use '_' as a target"_s);
        const Token& name = next();
        if (at(TokenKind::Dot) || at(TokenKind::LeftParenthesis) || at(TokenKind::Equal))
            return nullptr;
        auto* as = make<MatchAs>(start);
        as->pattern = pattern;
        as->name = name.text;
        return as;
    }

    // or_pattern
    Pattern* parseOrPattern()
    {
        Mark start = mark();
        Pattern* first = parseClosedPattern();
        if (!first || !at(TokenKind::VerticalBar))
            return first;
        Vector<Pattern*, 8> patterns;
        patterns.append(first);
        while (consume(TokenKind::VerticalBar)) {
            Pattern* pattern = parseClosedPattern();
            if (!pattern)
                return nullptr;
            patterns.append(pattern);
        }
        auto* orPattern = make<MatchOr>(start);
        orPattern->patterns = m_arena.copy(patterns);
        return orPattern;
    }

    // signed_number, and complex_number if what follows makes it one.
    Expression* parseNumberInPattern()
    {
        Mark start = mark();
        bool isNegative = consume(TokenKind::Minus);
        if (!at(TokenKind::Number))
            return nullptr;
        Expression* number = makeNumber(next());
        if (isNegative) {
            auto* negation = make<UnaryOp>(start);
            negation->op = UnaryOperator::USub;
            negation->operand = number;
            number = negation;
        }
        if (!at(TokenKind::Plus) && !at(TokenKind::Minus))
            return number;

        Constant& real = (isNegative ? number->as<UnaryOp>().operand : number)->as<Constant>();
        if (real.type == Constant::Type::Imaginary)
            return fail("real number required in complex literal"_s, real);
        BinaryOperator op = next().kind == TokenKind::Plus ? BinaryOperator::Add : BinaryOperator::Sub;
        if (!at(TokenKind::Number))
            return nullptr;
        Constant* imaginary = makeNumber(next());
        if (imaginary->type != Constant::Type::Imaginary)
            return fail("imaginary number required in complex literal"_s, *imaginary);
        auto* complex = make<BinOp>(start);
        complex->left = number;
        complex->op = op;
        complex->right = imaginary;
        return complex;
    }

    // name_or_attr
    Expression* parseNameOrAttribute()
    {
        Mark start = mark();
        Expression* value = makeName(next());
        while (consume(TokenKind::Dot)) {
            if (!at(TokenKind::Name))
                return nullptr;
            const Identifier* name = next().text;
            auto* attribute = make<Attribute>(start);
            attribute->value = value;
            attribute->attribute = name;
            value = attribute;
        }
        return value;
    }

    template<typename T>
    Pattern* makeValuePattern(Mark start, T* value)
    {
        if (!value)
            return nullptr;
        auto* pattern = make<MatchValue>(start);
        pattern->value = value;
        return pattern;
    }

    Pattern* makeSingletonPattern(Constant::Type value)
    {
        auto* pattern = make<MatchSingleton>(next());
        pattern->value = value;
        return pattern;
    }

    // closed_pattern
    Pattern* parseClosedPattern()
    {
        Mark start = mark();
        switch (peek().kind) {
        case TokenKind::Number:
        case TokenKind::Minus:
            return makeValuePattern(start, parseNumberInPattern());
        case TokenKind::String:
        case TokenKind::FStringStart:
        case TokenKind::TStringStart:
            return makeValuePattern(start, parseStrings());
        case TokenKind::KeywordNone:
            return makeSingletonPattern(Constant::Type::None);
        case TokenKind::KeywordTrue:
            return makeSingletonPattern(Constant::Type::True);
        case TokenKind::KeywordFalse:
            return makeSingletonPattern(Constant::Type::False);
        case TokenKind::Name:
            return parseNamePattern();
        case TokenKind::LeftParenthesis:
        case TokenKind::LeftBracket:
            return parseGroupOrSequencePattern();
        case TokenKind::LeftBrace:
            return parseMappingPattern();
        default:
            return nullptr;
        }
    }

    // capture_pattern | wildcard_pattern | value_pattern | class_pattern
    Pattern* parseNamePattern()
    {
        Mark start = mark();
        if (!atAhead(1, TokenKind::Dot) && !atAhead(1, TokenKind::LeftParenthesis)) {
            if (atAhead(1, TokenKind::Equal))
                return nullptr;
            const Token& name = next();
            auto* capture = make<MatchAs>(start);
            if (name.softKeyword != SoftKeyword::Underscore)
                capture->name = name.text;
            return capture;
        }

        Expression* value = parseNameOrAttribute();
        if (!value)
            return nullptr;
        if (!at(TokenKind::LeftParenthesis)) {
            if (at(TokenKind::Equal))
                return nullptr;
            return makeValuePattern(start, value);
        }

        next();
        Vector<Pattern*, 8> patterns;
        Vector<const Identifier*, 8> keywordAttributes;
        Vector<Pattern*, 8> keywordPatterns;
        while (!at(TokenKind::RightParenthesis)) {
            if (at(TokenKind::Name) && atAhead(1, TokenKind::Equal)) {
                keywordAttributes.append(next().text);
                next();
                Pattern* pattern = parsePattern();
                if (!pattern)
                    return nullptr;
                keywordPatterns.append(pattern);
            } else {
                Pattern* pattern = parsePattern();
                if (!pattern)
                    return nullptr;
                if (!keywordPatterns.isEmpty())
                    return fail("positional patterns follow keyword patterns"_s, *pattern);
                patterns.append(pattern);
            }
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightParenthesis))
            return nullptr;
        auto* pattern = make<MatchClass>(start);
        pattern->cls = value;
        pattern->patterns = m_arena.copy(patterns);
        pattern->keywordAttributes = m_arena.copy(keywordAttributes);
        pattern->keywordPatterns = m_arena.copy(keywordPatterns);
        return pattern;
    }

    // group_pattern | sequence_pattern
    Pattern* parseGroupOrSequencePattern()
    {
        Mark start = mark();
        bool isParenthesis = next().kind == TokenKind::LeftParenthesis;
        TokenKind close = isParenthesis ? TokenKind::RightParenthesis : TokenKind::RightBracket;
        Vector<Pattern*, 8> patterns;
        if (!at(close)) {
            Pattern* first = parseMaybeStarPattern();
            if (!first)
                return nullptr;
            // (pattern) is the pattern. (pattern,) and [pattern] are sequences of one.
            if (isParenthesis && at(close) && !first->is<MatchStar>()) {
                next();
                return first;
            }
            if (isParenthesis && !at(TokenKind::Comma))
                return nullptr;
            patterns.append(first);
            if (!parseRestOfSequencePattern(patterns, close))
                return nullptr;
        }
        if (!expect(close))
            return nullptr;
        auto* sequence = make<MatchSequence>(start);
        sequence->patterns = m_arena.copy(patterns);
        return sequence;
    }

    // mapping_pattern
    Pattern* parseMappingPattern()
    {
        Mark start = mark();
        next();
        Vector<Expression*, 8> keys;
        Vector<Pattern*, 8> patterns;
        const Identifier* rest = nullptr;
        while (!at(TokenKind::RightBrace)) {
            if (consume(TokenKind::DoubleStar)) {
                if (!at(TokenKind::Name) || at(SoftKeyword::Underscore))
                    return nullptr;
                rest = next().text;
                consume(TokenKind::Comma);
                break;
            }
            Expression* key = nullptr;
            switch (peek().kind) {
            case TokenKind::Number:
            case TokenKind::Minus:
                key = parseNumberInPattern();
                break;
            case TokenKind::String:
            case TokenKind::FStringStart:
            case TokenKind::TStringStart:
                key = parseStrings();
                break;
            case TokenKind::KeywordNone:
                key = makeConstant(next(), Constant::Type::None);
                break;
            case TokenKind::KeywordTrue:
                key = makeConstant(next(), Constant::Type::True);
                break;
            case TokenKind::KeywordFalse:
                key = makeConstant(next(), Constant::Type::False);
                break;
            case TokenKind::Name:
                if (atAhead(1, TokenKind::Dot))
                    key = parseNameOrAttribute();
                break;
            default:
                break;
            }
            if (!key || !expect(TokenKind::Colon))
                return nullptr;
            Pattern* pattern = parsePattern();
            if (!pattern)
                return nullptr;
            keys.append(key);
            patterns.append(pattern);
            if (!consume(TokenKind::Comma))
                break;
        }
        if (!expect(TokenKind::RightBrace))
            return nullptr;
        auto* mapping = make<MatchMapping>(start);
        mapping->keys = m_arena.copy(keys);
        mapping->patterns = m_arena.copy(patterns);
        mapping->rest = rest;
        return mapping;
    }

    // ---- Statements

    bool parseStatementsUntil(TokenKind terminator, Vector<Statement*, 16>& body)
    {
        while (!at(terminator)) {
            if (!parseStatement(body))
                return false;
        }
        return true;
    }

    // block. `after` and `line` are for saying what it should have been the block of.
    bool parseBlock(Sequence<Statement*>& result, ASCIILiteral after, unsigned line)
    {
        Vector<Statement*, 16> body;
        if (consume(TokenKind::Newline)) {
            if (!consume(TokenKind::Indent)) {
                fail(makeString("expected an indented block after "_s, after, " on line "_s, line), peek(), SyntaxError::Kind::IndentationError);
                return false;
            }
            if (!parseStatementsUntil(TokenKind::Dedent, body))
                return false;
            next();
        } else if (!parseSimpleStatements(body))
            return false;
        result = m_arena.copy(body);
        return true;
    }

    // statement
    bool parseStatement(Vector<Statement*, 16>& body)
    {
        if (!isSafeToRecurse())
            return false;
        Statement* statement = nullptr;
        switch (peek().kind) {
        case TokenKind::KeywordDef:
        case TokenKind::KeywordClass:
        case TokenKind::At:
            statement = parseDefinition();
            break;
        case TokenKind::KeywordAsync:
            if (atAhead(1, TokenKind::KeywordDef))
                statement = parseDefinition();
            else if (atAhead(1, TokenKind::KeywordWith))
                statement = parseWith();
            else if (atAhead(1, TokenKind::KeywordFor))
                statement = parseFor();
            break;
        case TokenKind::KeywordIf:
            statement = parseIf();
            break;
        case TokenKind::KeywordWith:
            statement = parseWith();
            break;
        case TokenKind::KeywordFor:
            statement = parseFor();
            break;
        case TokenKind::KeywordTry:
            statement = parseTry();
            break;
        case TokenKind::KeywordWhile:
            statement = parseWhile();
            break;
        case TokenKind::Name:
            if (at(SoftKeyword::Match)) {
                bool isMatch = false;
                statement = parseMatch(isMatch);
                if (isMatch)
                    break;
            }
            return parseSimpleStatements(body);
        default:
            return parseSimpleStatements(body);
        }
        if (!statement)
            return false;
        body.append(statement);
        return true;
    }

    // simple_stmts
    bool parseSimpleStatements(Vector<Statement*, 16>& body)
    {
        while (true) {
            Statement* statement = parseSimpleStatement();
            if (!statement)
                return false;
            body.append(statement);
            if (!consume(TokenKind::Semicolon))
                break;
            if (at(TokenKind::Newline))
                break;
        }
        return expect(TokenKind::Newline);
    }

    template<typename T>
    Statement* makeKeywordStatement()
    {
        return make<T>(next());
    }

    // simple_stmt
    Statement* parseSimpleStatement()
    {
        Mark start = mark();
        switch (peek().kind) {
        case TokenKind::KeywordPass:
            return makeKeywordStatement<Pass>();
        case TokenKind::KeywordBreak:
            return makeKeywordStatement<Break>();
        case TokenKind::KeywordContinue:
            return makeKeywordStatement<Continue>();
        case TokenKind::KeywordReturn: {
            next();
            Expression* value = nullptr;
            if (canStartStarExpression()) {
                value = parseStarExpressions();
                if (!value)
                    return nullptr;
            }
            auto* statement = make<Return>(start);
            statement->value = value;
            return statement;
        }
        case TokenKind::KeywordRaise: {
            next();
            Expression* exception = nullptr;
            Expression* cause = nullptr;
            if (canStartExpression()) {
                exception = parseExpression();
                if (!exception)
                    return nullptr;
                if (consume(TokenKind::KeywordFrom)) {
                    cause = parseExpression();
                    if (!cause)
                        return nullptr;
                }
            }
            auto* statement = make<Raise>(start);
            statement->exception = exception;
            statement->cause = cause;
            return statement;
        }
        case TokenKind::KeywordGlobal:
        case TokenKind::KeywordNonlocal: {
            bool isGlobal = next().kind == TokenKind::KeywordGlobal;
            Vector<const Identifier*, 8> names;
            do {
                if (!at(TokenKind::Name))
                    return nullptr;
                names.append(next().text);
            } while (consume(TokenKind::Comma));
            if (isGlobal) {
                auto* statement = make<Global>(start);
                statement->names = m_arena.copy(names);
                return statement;
            }
            auto* statement = make<Nonlocal>(start);
            statement->names = m_arena.copy(names);
            return statement;
        }
        case TokenKind::KeywordAssert: {
            next();
            Expression* test = parseExpression();
            if (!test)
                return nullptr;
            Expression* message = nullptr;
            if (consume(TokenKind::Comma)) {
                message = parseExpression();
                if (!message)
                    return nullptr;
            }
            auto* statement = make<Assert>(start);
            statement->test = test;
            statement->message = message;
            return statement;
        }
        case TokenKind::KeywordDel:
            return parseDelete();
        case TokenKind::KeywordImport:
            return parseImport();
        case TokenKind::KeywordFrom:
            return parseImportFrom();
        case TokenKind::KeywordYield: {
            Expression* value = parseYield();
            if (!value)
                return nullptr;
            auto* statement = make<Expr>(start);
            statement->value = value;
            return statement;
        }
        case TokenKind::Name:
            if (at(SoftKeyword::Type) && atAhead(1, TokenKind::Name) && (atAhead(2, TokenKind::Equal) || atAhead(2, TokenKind::LeftBracket))) {
                // Two names side by side begin nothing else, so what is wrong with it is what is wrong with a type statement.
                return parseTypeAlias();
            }
            return parseAssignmentOrExpression();
        default:
            return parseAssignmentOrExpression();
        }
    }

    // type_alias
    Statement* parseTypeAlias()
    {
        Mark start = mark();
        next();
        Name* name = makeName(next(), ExpressionContext::Store);
        Sequence<TypeParameter*> typeParameters;
        if (!parseTypeParameters(typeParameters) || !expect(TokenKind::Equal))
            return nullptr;
        Expression* value = parseExpression();
        if (!value)
            return nullptr;
        // `type` is a name like any other unless the whole statement is this.
        if (!at(TokenKind::Newline) && !at(TokenKind::Semicolon))
            return nullptr;
        auto* alias = make<TypeAlias>(start);
        alias->name = name;
        alias->typeParameters = typeParameters;
        alias->value = value;
        return alias;
    }

    static std::optional<BinaryOperator> augmentedOperator(TokenKind kind)
    {
        switch (kind) {
        case TokenKind::PlusEqual:
            return BinaryOperator::Add;
        case TokenKind::MinusEqual:
            return BinaryOperator::Sub;
        case TokenKind::StarEqual:
            return BinaryOperator::Mult;
        case TokenKind::AtEqual:
            return BinaryOperator::MatMult;
        case TokenKind::SlashEqual:
            return BinaryOperator::Div;
        case TokenKind::PercentEqual:
            return BinaryOperator::Mod;
        case TokenKind::AmpersandEqual:
            return BinaryOperator::BitAnd;
        case TokenKind::VerticalBarEqual:
            return BinaryOperator::BitOr;
        case TokenKind::CircumflexEqual:
            return BinaryOperator::BitXor;
        case TokenKind::LeftShiftEqual:
            return BinaryOperator::LShift;
        case TokenKind::RightShiftEqual:
            return BinaryOperator::RShift;
        case TokenKind::DoubleStarEqual:
            return BinaryOperator::Pow;
        case TokenKind::DoubleSlashEqual:
            return BinaryOperator::FloorDiv;
        default:
            return std::nullopt;
        }
    }

    // assignment | star_expressions
    Statement* parseAssignmentOrExpression()
    {
        Mark start = mark();
        Expression* first = parseStarExpressions();
        if (!first)
            return nullptr;

        if (at(TokenKind::Colon)) {
            next();
            Expression* annotation = parseExpression();
            if (!annotation)
                return nullptr;
            if (first->is<List>() || first->is<Tuple>())
                return fail(makeString("only single target (not "_s, describe(*first), ") can be annotated"_s), *first);
            if (!first->is<Name>() && !first->is<Attribute>() && !first->is<Subscript>())
                return fail("illegal target for annotation"_s, *first);
            setContext(*first, ExpressionContext::Store);
            Expression* value = nullptr;
            if (consume(TokenKind::Equal)) {
                value = parseYieldOrStarExpressions();
                if (!value)
                    return nullptr;
            }
            auto* statement = make<AnnAssign>(start);
            statement->target = first;
            statement->annotation = annotation;
            statement->value = value;
            statement->isSimple = first->is<Name>() && !first->isParenthesized;
            return statement;
        }

        if (at(TokenKind::Equal)) {
            Vector<Expression*, 4> targets;
            Expression* value = first;
            while (consume(TokenKind::Equal)) {
                if (!makeAssignmentTarget(*value))
                    return nullptr;
                targets.append(value);
                value = parseYieldOrStarExpressions();
                if (!value)
                    return nullptr;
            }
            auto* statement = make<Assign>(start);
            statement->targets = m_arena.copy(targets);
            statement->value = value;
            return statement;
        }

        if (auto op = augmentedOperator(peek().kind)) {
            if (!first->is<Name>() && !first->is<Attribute>() && !first->is<Subscript>())
                return fail(makeString('\'', describe(*first), "' is an illegal expression for augmented assignment"_s), *first);
            setContext(*first, ExpressionContext::Store);
            next();
            Expression* value = parseYieldOrStarExpressions();
            if (!value)
                return nullptr;
            auto* statement = make<AugAssign>(start);
            statement->target = first;
            statement->op = *op;
            statement->value = value;
            return statement;
        }

        auto* statement = make<Expr>(start);
        statement->value = first;
        return statement;
    }

    // del_stmt
    Statement* parseDelete()
    {
        Mark start = mark();
        next();
        Vector<Expression*, 8> targets;
        do {
            if (at(TokenKind::Newline) || at(TokenKind::Semicolon))
                break;
            Expression* target = parsePrimary();
            if (!target)
                return nullptr;
            if (!at(TokenKind::Comma) && !at(TokenKind::Newline) && !at(TokenKind::Semicolon)) {
                // It goes on, so it is not something that can be deleted. Find out what it is, to say so.
                m_index = indexOf(*target);
                Expression* whole = parseStarExpressions();
                if (!whole)
                    return nullptr;
                makeTarget(*whole, ExpressionContext::Del);
                return nullptr;
            }
            if (!makeTarget(*target, ExpressionContext::Del))
                return nullptr;
            targets.append(target);
        } while (consume(TokenKind::Comma));
        if (targets.isEmpty())
            return nullptr;
        auto* statement = make<Delete>(start);
        statement->targets = m_arena.copy(targets);
        return statement;
    }

    // The token that a node that has just been parsed began with.
    unsigned indexOf(const Node& node)
    {
        unsigned index = m_index;
        while (index && m_tokens[index].start > node.start)
            --index;
        // (x) begins before x does.
        while (index && m_tokens[index - 1].kind == TokenKind::LeftParenthesis && m_tokens[index].start == node.start && m_tokens[index].kind != TokenKind::LeftParenthesis)
            --index;
        return index;
    }

    // dotted_name
    const Identifier* parseDottedName()
    {
        if (!at(TokenKind::Name))
            return nullptr;
        const Identifier* first = next().text;
        if (!at(TokenKind::Dot))
            return first;
        StringBuilder builder;
        builder.append(first->string());
        while (consume(TokenKind::Dot)) {
            if (!at(TokenKind::Name))
                return nullptr;
            builder.append('.', next().text->string());
        }
        return makeIdentifier(builder.toString());
    }

    // ['as' NAME]
    bool parseAsName(const Identifier*& result)
    {
        if (!consume(TokenKind::KeywordAs))
            return true;
        if (!at(TokenKind::Name)) {
            Expression* target = speculate([&] { return parseExpression(); });
            if (target)
                fail(makeString("cannot use "_s, describe(*target), " as import target"_s), *target);
            return false;
        }
        result = next().text;
        return true;
    }

    // import_name
    Statement* parseImport()
    {
        Mark start = mark();
        next();
        if (at(TokenKind::Newline))
            return fail("Expected one or more names after 'import'"_s, previous());
        Vector<Alias*, 4> names;
        do {
            Mark aliasStart = mark();
            const Identifier* name = parseDottedName();
            const Identifier* asName = nullptr;
            if (!name || !parseAsName(asName))
                return nullptr;
            auto* alias = make<Alias>(aliasStart);
            alias->name = name;
            alias->asName = asName;
            names.append(alias);
        } while (consume(TokenKind::Comma));
        auto* statement = make<Import>(start);
        statement->names = m_arena.copy(names);
        return statement;
    }

    // import_from
    Statement* parseImportFrom()
    {
        Mark start = mark();
        next();
        unsigned level = 0;
        while (true) {
            if (consume(TokenKind::Dot))
                ++level;
            else if (consume(TokenKind::Ellipsis))
                level += 3;
            else
                break;
        }
        const Identifier* module = nullptr;
        if (at(TokenKind::Name)) {
            module = parseDottedName();
            if (!module)
                return nullptr;
        } else if (!level)
            return nullptr;
        if (!expect(TokenKind::KeywordImport))
            return nullptr;

        Vector<Alias*, 8> names;
        if (at(TokenKind::Star)) {
            auto* alias = make<Alias>(next());
            alias->name = &m_arena.identifiers().makeIdentifier(m_vm, "*"_span8);
            names.append(alias);
        } else {
            if (at(TokenKind::Newline))
                return fail("Expected one or more names after 'import'"_s, previous());
            bool isParenthesized = consume(TokenKind::LeftParenthesis);
            while (true) {
                if (!at(TokenKind::Name))
                    return nullptr;
                Mark aliasStart = mark();
                const Identifier* name = next().text;
                const Identifier* asName = nullptr;
                if (!parseAsName(asName))
                    return nullptr;
                auto* alias = make<Alias>(aliasStart);
                alias->name = name;
                alias->asName = asName;
                names.append(alias);
                if (!consume(TokenKind::Comma))
                    break;
                if (isParenthesized && at(TokenKind::RightParenthesis))
                    break;
                if (!isParenthesized && at(TokenKind::Newline))
                    return fail("trailing comma not allowed without surrounding parentheses"_s, previous());
            }
            if (isParenthesized && !expect(TokenKind::RightParenthesis))
                return nullptr;
        }

        if (!level && module && *module == "__future__"_s) {
            for (Alias* alias : names) {
                static constexpr ASCIILiteral features[] = { "nested_scopes"_s, "generators"_s, "division"_s, "absolute_import"_s, "with_statement"_s, "print_function"_s, "unicode_literals"_s, "barry_as_FLUFL"_s, "generator_stop"_s, "annotations"_s };
                bool isFeature = false;
                for (ASCIILiteral feature : features)
                    isFeature |= *alias->name == feature;
                if (*alias->name == "barry_as_FLUFL"_s)
                    m_usesLessGreater = true;
                else if (*alias->name == "braces"_s)
                    return fail("not a chance"_s, *alias);
                else if (!isFeature)
                    return fail(makeString("future feature "_s, alias->name->string(), " is not defined"_s), *alias);
            }
        }

        auto* statement = make<ImportFrom>(start);
        statement->module = module;
        statement->names = m_arena.copy(names);
        statement->level = level;
        return statement;
    }

    // ['else' ':' block]
    bool parseElse(Sequence<Statement*>& result)
    {
        if (!at(TokenKind::KeywordElse))
            return true;
        unsigned line = next().line;
        return expectColon(ColonIs::Insisted) && parseBlock(result, "'else' statement"_s, line);
    }

    // if_stmt, and elif_stmt
    Statement* parseIf()
    {
        Mark start = mark();
        bool isElif = next().kind == TokenKind::KeywordElif;
        Expression* test = parseNamedExpression();
        if (!test || !expectColon())
            return nullptr;
        Sequence<Statement*> body;
        if (!parseBlock(body, isElif ? "'elif' statement"_s : "'if' statement"_s, start.line))
            return nullptr;
        Sequence<Statement*> orElse;
        if (at(TokenKind::KeywordElif)) {
            if (!isSafeToRecurse())
                return nullptr;
            Statement* elif = parseIf();
            if (!elif)
                return nullptr;
            Vector<Statement*, 1> single { elif };
            orElse = m_arena.copy(single);
        } else {
            if (!parseElse(orElse))
                return nullptr;
            if (at(TokenKind::KeywordElif))
                return fail("'elif' block follows an 'else' block"_s);
        }
        auto* statement = make<If>(start);
        statement->test = test;
        statement->body = body;
        statement->orElse = orElse;
        return statement;
    }

    // while_stmt
    Statement* parseWhile()
    {
        Mark start = mark();
        next();
        Expression* test = parseNamedExpression();
        if (!test || !expectColon())
            return nullptr;
        Sequence<Statement*> body;
        Sequence<Statement*> orElse;
        if (!parseBlock(body, "'while' statement"_s, start.line) || !parseElse(orElse))
            return nullptr;
        auto* statement = make<While>(start);
        statement->test = test;
        statement->body = body;
        statement->orElse = orElse;
        return statement;
    }

    // for_stmt
    Statement* parseFor()
    {
        Mark start = mark();
        bool isAsync = consume(TokenKind::KeywordAsync);
        next();
        Expression* target = parseStarTargets();
        if (!target)
            return nullptr;
        if (!consume(TokenKind::KeywordIn))
            return fail("'in' expected after for-loop variables"_s);
        Expression* iterable = parseStarExpressions();
        if (!iterable || !expectColon())
            return nullptr;
        Sequence<Statement*> body;
        Sequence<Statement*> orElse;
        if (!parseBlock(body, "'for' statement"_s, start.line) || !parseElse(orElse))
            return nullptr;
        auto* statement = make<For>(start);
        statement->isAsync = isAsync;
        statement->target = target;
        statement->iterable = iterable;
        statement->body = body;
        statement->orElse = orElse;
        return statement;
    }

    // with_item
    WithItem* parseWithItem()
    {
        auto* item = m_arena.create<WithItem>();
        item->contextExpression = parseExpression();
        if (!item->contextExpression)
            return nullptr;
        if (consume(TokenKind::KeywordAs)) {
            item->optionalVariables = parseStarTarget();
            if (!item->optionalVariables)
                return nullptr;
            if (!at(TokenKind::Comma) && !at(TokenKind::RightParenthesis) && !at(TokenKind::Colon))
                return nullptr;
        }
        return item;
    }

    // with_stmt
    Statement* parseWith()
    {
        Mark start = mark();
        bool isAsync = consume(TokenKind::KeywordAsync);
        next();

        Vector<WithItem*, 4> items;
        // with (a as b, c as d): or with (a, b): where the parentheses are those of a tuple, or with (a).b: and so on.
        bool isParenthesized = at(TokenKind::LeftParenthesis) && speculate([&] {
            next();
            do {
                if (at(TokenKind::RightParenthesis))
                    break;
                WithItem* item = parseWithItem();
                if (!item) {
                    items.clear();
                    return false;
                }
                items.append(item);
            } while (consume(TokenKind::Comma));
            if (!items.isEmpty() && consume(TokenKind::RightParenthesis) && at(TokenKind::Colon))
                return true;
            items.clear();
            return false;
        });
        if (!isParenthesized) {
            do {
                WithItem* item = parseWithItem();
                if (!item)
                    return nullptr;
                items.append(item);
            } while (consume(TokenKind::Comma));
        }
        if (!expectColon())
            return nullptr;
        Sequence<Statement*> body;
        if (!parseBlock(body, "'with' statement"_s, start.line))
            return nullptr;
        auto* statement = make<With>(start);
        statement->isAsync = isAsync;
        statement->items = m_arena.copy(items);
        statement->body = body;
        return statement;
    }

    // try_stmt
    Statement* parseTry()
    {
        Mark start = mark();
        next();
        if (!expectColon(ColonIs::Insisted))
            return nullptr;
        Sequence<Statement*> body;
        if (!parseBlock(body, "'try' statement"_s, start.line))
            return nullptr;

        Vector<ExceptHandler*, 4> handlers;
        bool isStar = false;
        while (at(TokenKind::KeywordExcept)) {
            Mark handlerStart = mark();
            next();
            bool handlerIsStar = consume(TokenKind::Star);
            if (!handlers.isEmpty() && handlerIsStar != isStar)
                return fail("cannot have both 'except' and 'except*' on the same 'try'"_s, previous());
            isStar = handlerIsStar;

            Expression* type = nullptr;
            const Identifier* name = nullptr;
            if (!at(TokenKind::Colon)) {
                type = parseExpressions();
                if (!type)
                    return nullptr;
                if (at(TokenKind::KeywordAs)) {
                    if (type->is<Tuple>() && !type->isParenthesized)
                        return fail("multiple exception types must be parenthesized when using 'as'"_s, *type);
                    next();
                    if (!at(TokenKind::Name)) {
                        Expression* target = speculate([&] { return parseExpression(); });
                        if (target)
                            return fail(makeString("cannot use except"_s, isStar ? "*"_s : ""_s, " statement with "_s, describe(*target)), *target);
                        return nullptr;
                    }
                    name = next().text;
                }
            } else if (isStar)
                return fail("expected one or more exception types"_s);
            if (!expectColon())
                return nullptr;
            Sequence<Statement*> handlerBody;
            if (!parseBlock(handlerBody, isStar ? "'except*' statement"_s : "'except' statement"_s, handlerStart.line))
                return nullptr;
            auto* handler = make<ExceptHandler>(handlerStart);
            handler->type = type;
            handler->name = name;
            handler->body = handlerBody;
            handlers.append(handler);
        }

        Sequence<Statement*> orElse;
        if (!handlers.isEmpty() && !parseElse(orElse))
            return nullptr;

        Sequence<Statement*> finalBody;
        if (at(TokenKind::KeywordFinally)) {
            unsigned line = next().line;
            if (!expectColon(ColonIs::Insisted) || !parseBlock(finalBody, "'finally' statement"_s, line))
                return nullptr;
        } else if (handlers.isEmpty())
            return fail("expected 'except' or 'finally' block"_s);

        auto* statement = make<Try>(start);
        statement->isStar = isStar;
        statement->body = body;
        statement->handlers = m_arena.copy(handlers);
        statement->orElse = orElse;
        statement->finalBody = finalBody;
        return statement;
    }

    // match_stmt, if that is what this is: `match` is a name like any other unless what follows says otherwise.
    Statement* parseMatch(bool& isMatch)
    {
        Mark start = mark();
        Expression* subject = speculate([&] () -> Expression* {
            next();
            Mark subjectStart = mark();
            Expression* first = parseStarNamedExpression();
            if (!first)
                return nullptr;
            if (at(TokenKind::Comma)) {
                Vector<Expression*, 8> elements;
                elements.append(first);
                while (consume(TokenKind::Comma)) {
                    if (at(TokenKind::Colon))
                        break;
                    Expression* element = parseStarNamedExpression();
                    if (!element)
                        return nullptr;
                    elements.append(element);
                }
                auto* tuple = make<Tuple>(subjectStart);
                tuple->elements = m_arena.copy(elements);
                first = tuple;
            } else if (first->is<Starred>())
                return nullptr;
            if (!at(TokenKind::Colon) || !atAhead(1, TokenKind::Newline))
                return nullptr;
            return first;
        });
        if (!subject)
            return nullptr;

        isMatch = true;
        next();
        next();
        if (!consume(TokenKind::Indent))
            return fail(makeString("expected an indented block after 'match' statement on line "_s, start.line), peek(), SyntaxError::Kind::IndentationError);

        Vector<MatchCase*, 8> cases;
        while (at(SoftKeyword::Case)) {
            unsigned line = next().line;
            auto* matchCase = m_arena.create<MatchCase>();
            matchCase->pattern = parsePatterns();
            if (!matchCase->pattern)
                return nullptr;
            if (consume(TokenKind::KeywordIf)) {
                matchCase->guard = parseNamedExpression();
                if (!matchCase->guard)
                    return nullptr;
            }
            if (!expectColon() || !parseBlock(matchCase->body, "'case' statement"_s, line))
                return nullptr;
            cases.append(matchCase);
        }
        if (cases.isEmpty() || !expect(TokenKind::Dedent))
            return nullptr;
        auto* statement = make<Match>(start);
        statement->subject = subject;
        statement->cases = m_arena.copy(cases);
        return statement;
    }

    // function_def | class_def
    Statement* parseDefinition()
    {
        Vector<Expression*, 4> decorators;
        while (consume(TokenKind::At)) {
            Expression* decorator = parseNamedExpression();
            if (!decorator || !expect(TokenKind::Newline))
                return nullptr;
            decorators.append(decorator);
        }

        Mark start = mark();
        if (consume(TokenKind::KeywordClass)) {
            if (!at(TokenKind::Name))
                return nullptr;
            const Identifier* name = next().text;
            Sequence<TypeParameter*> typeParameters;
            if (!parseTypeParameters(typeParameters))
                return nullptr;
            Vector<Expression*, 8> bases;
            Vector<Keyword*, 8> keywords;
            if (at(TokenKind::LeftParenthesis) && !parseArguments(bases, keywords))
                return nullptr;
            if (!expectColon())
                return nullptr;
            Sequence<Statement*> body;
            if (!parseBlock(body, "class definition"_s, start.line))
                return nullptr;
            auto* statement = make<ClassDef>(start);
            statement->name = name;
            statement->bases = m_arena.copy(bases);
            statement->keywords = m_arena.copy(keywords);
            statement->body = body;
            statement->decorators = m_arena.copy(decorators);
            statement->typeParameters = typeParameters;
            return statement;
        }

        bool isAsync = consume(TokenKind::KeywordAsync);
        if (!expect(TokenKind::KeywordDef) || !at(TokenKind::Name))
            return nullptr;
        const Identifier* name = next().text;
        Sequence<TypeParameter*> typeParameters;
        if (!parseTypeParameters(typeParameters))
            return nullptr;
        if (!consume(TokenKind::LeftParenthesis))
            return fail("expected '('"_s);
        Arguments* arguments = parseParameters(TokenKind::RightParenthesis, true);
        if (!arguments || !expect(TokenKind::RightParenthesis))
            return nullptr;
        Expression* returns = nullptr;
        if (consume(TokenKind::Arrow)) {
            returns = parseExpression();
            if (!returns)
                return nullptr;
        }
        if (!expect(TokenKind::Colon))
            return nullptr;
        Sequence<Statement*> body;
        if (!parseBlock(body, "function definition"_s, start.line))
            return nullptr;
        auto* statement = make<FunctionDef>(start);
        statement->isAsync = isAsync;
        statement->name = name;
        statement->arguments = arguments;
        statement->body = body;
        statement->decorators = m_arena.copy(decorators);
        statement->returns = returns;
        statement->typeParameters = typeParameters;
        return statement;
    }

    VM& m_vm;
    Arena& m_arena;
    const Vector<Token>& m_tokens;
    SyntaxError& m_error;
    SyntaxError m_scannerError;
    unsigned m_index { 0 };
    unsigned m_furthest { 0 };
    unsigned m_speculationDepth { 0 };
    bool m_usesLessGreater { false };
};

} // anonymous namespace

Module* parse(VM& vm, Arena& arena, StringView source, Module::Kind kind, Vector<SyntaxWarning>& warnings, SyntaxError& error)
{
    Vector<Token> tokens;
    tokenize(vm, arena, source, { }, tokens, warnings, error);
    return Parser(vm, arena, tokens, error).parseModule(kind);
}

static ScanRange rangeFor(StringView source, unsigned start, unsigned end, unsigned line, bool isInsideBrackets)
{
    unsigned lineStart = start;
    while (lineStart && source[lineStart - 1] != '\n' && source[lineStart - 1] != '\r')
        --lineStart;
    return { start, end, line, lineStart, isInsideBrackets };
}

Statement* parseDefinition(VM& vm, Arena& arena, StringView source, unsigned start, unsigned end, unsigned line)
{
    Vector<Token> tokens;
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    tokenize(vm, arena, source, rangeFor(source, start, end, line, false), tokens, warnings, error);
    return Parser(vm, arena, tokens, error).parseDefinitionAlone();
}

Expression* parseExpression(VM& vm, Arena& arena, StringView source, unsigned start, unsigned end, unsigned line)
{
    Vector<Token> tokens;
    Vector<SyntaxWarning> warnings;
    SyntaxError error;
    // If it goes over more than one line it is inside brackets, and if it does not it makes no difference.
    tokenize(vm, arena, source, rangeFor(source, start, end, line, true), tokens, warnings, error);
    return Parser(vm, arena, tokens, error).parseExpressionAlone();
}

} } // namespace JSC::Python
