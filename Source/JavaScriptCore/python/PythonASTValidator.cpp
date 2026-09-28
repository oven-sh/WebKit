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
#include "PythonASTValidator.h"

#include "VM.h"
#include <wtf/text/MakeString.h>

// CPython's Python/ast.c, function for function.

namespace JSC { namespace Python {

namespace {

class Validator {
public:
    explicit Validator(VM& vm)
        : m_vm(vm)
    {
    }

    ASTError takeError() { return WTF::move(m_error); }

    // _PyAST_Validate()
    bool validate(Module& module)
    {
        switch (module.kind) {
        case Module::Kind::Module:
        case Module::Kind::Interactive:
            return validateStatements(module.body);
        case Module::Kind::Expression:
            return validateExpression(module.expression, ExpressionContext::Load);
        }
        return fail(ASTError::Kind::SystemError, "impossible module node"_s);
    }

    // validate_stmt()
    bool validateStatement(Statement* statement)
    {
        if (!validatePositions(*statement) || !enter())
            return false;
        using enum ExpressionContext;
        switch (statement->kind) {
        case Statement::Kind::FunctionDef: {
            auto& node = statement->as<FunctionDef>();
            return validateBody(node.body, node.isAsync ? "AsyncFunctionDef"_s : "FunctionDef"_s) && validateTypeParameters(node.typeParameters) && validateArguments(*node.arguments)
                && validateExpressions(node.decorators, Load, false) && (!node.returns || validateExpression(node.returns, Load));
        }
        case Statement::Kind::ClassDef: {
            auto& node = statement->as<ClassDef>();
            return validateBody(node.body, "ClassDef"_s) && validateTypeParameters(node.typeParameters) && validateExpressions(node.bases, Load, false) && validateKeywords(node.keywords)
                && validateExpressions(node.decorators, Load, false);
        }
        case Statement::Kind::Return:
            return !statement->as<Return>().value || validateExpression(statement->as<Return>().value, Load);
        case Statement::Kind::Delete:
            return validateAssignmentList(statement->as<Delete>().targets, Del);
        case Statement::Kind::Assign:
            return validateAssignmentList(statement->as<Assign>().targets, Store) && validateExpression(statement->as<Assign>().value, Load);
        case Statement::Kind::AugAssign:
            return validateExpression(statement->as<AugAssign>().target, Store) && validateExpression(statement->as<AugAssign>().value, Load);
        case Statement::Kind::AnnAssign: {
            auto& node = statement->as<AnnAssign>();
            if (!node.target->is<Name>() && node.isSimple)
                return fail(ASTError::Kind::TypeError, "AnnAssign with simple non-Name target"_s);
            return validateExpression(node.target, Store) && (!node.value || validateExpression(node.value, Load)) && validateExpression(node.annotation, Load);
        }
        case Statement::Kind::TypeAlias: {
            auto& node = statement->as<TypeAlias>();
            if (!node.name->is<Name>())
                return fail(ASTError::Kind::TypeError, "TypeAlias with non-Name name"_s);
            return validateExpression(node.name, Store) && validateTypeParameters(node.typeParameters) && validateExpression(node.value, Load);
        }
        case Statement::Kind::For: {
            auto& node = statement->as<For>();
            return validateExpression(node.target, Store) && validateExpression(node.iterable, Load) && validateBody(node.body, node.isAsync ? "AsyncFor"_s : "For"_s) && validateStatements(node.orElse);
        }
        case Statement::Kind::While: {
            auto& node = statement->as<While>();
            return validateExpression(node.test, Load) && validateBody(node.body, "While"_s) && validateStatements(node.orElse);
        }
        case Statement::Kind::If: {
            auto& node = statement->as<If>();
            return validateExpression(node.test, Load) && validateBody(node.body, "If"_s) && validateStatements(node.orElse);
        }
        case Statement::Kind::With: {
            auto& node = statement->as<With>();
            ASCIILiteral owner = node.isAsync ? "AsyncWith"_s : "With"_s;
            if (!validateNonEmpty(node.items, "items"_s, owner))
                return false;
            for (WithItem* item : node.items) {
                if (!validateExpression(item->contextExpression, Load) || (item->optionalVariables && !validateExpression(item->optionalVariables, Store)))
                    return false;
            }
            return validateBody(node.body, owner);
        }
        case Statement::Kind::Match: {
            auto& node = statement->as<Match>();
            if (!validateExpression(node.subject, Load) || !validateNonEmpty(node.cases, "cases"_s, "Match"_s))
                return false;
            for (MatchCase* matchCase : node.cases) {
                if (!validatePattern(matchCase->pattern, false) || (matchCase->guard && !validateExpression(matchCase->guard, Load)) || !validateBody(matchCase->body, "match_case"_s))
                    return false;
            }
            return true;
        }
        case Statement::Kind::Raise: {
            auto& node = statement->as<Raise>();
            if (node.exception)
                return validateExpression(node.exception, Load) && (!node.cause || validateExpression(node.cause, Load));
            if (node.cause)
                return fail("Raise with cause but no exception"_s);
            return true;
        }
        case Statement::Kind::Try: {
            auto& node = statement->as<Try>();
            ASCIILiteral owner = node.isStar ? "TryStar"_s : "Try"_s;
            if (!validateBody(node.body, owner))
                return false;
            if (node.handlers.empty() && node.finalBody.empty())
                return fail(makeString(owner, " has neither except handlers nor finalbody"_s));
            if (node.handlers.empty() && !node.orElse.empty())
                return fail(makeString(owner, " has orelse but no except handlers"_s));
            for (ExceptHandler* handler : node.handlers) {
                // CPython does not look, and falls over.
                if (!handler)
                    return fail("None disallowed in handler list"_s);
                if (!node.isStar && !validatePositions(*handler))
                    return false;
                if ((handler->type && !validateExpression(handler->type, Load)) || !validateBody(handler->body, "ExceptHandler"_s))
                    return false;
            }
            return validateStatements(node.finalBody) && validateStatements(node.orElse);
        }
        case Statement::Kind::Assert:
            return validateExpression(statement->as<Assert>().test, Load) && (!statement->as<Assert>().message || validateExpression(statement->as<Assert>().message, Load));
        case Statement::Kind::Import:
            return validateNonEmpty(statement->as<Import>().names, "names"_s, "Import"_s);
        case Statement::Kind::ImportFrom:
            if (static_cast<int>(statement->as<ImportFrom>().level) < 0)
                return fail("Negative ImportFrom level"_s);
            return validateNonEmpty(statement->as<ImportFrom>().names, "names"_s, "ImportFrom"_s);
        case Statement::Kind::Global:
            return validateNonEmpty(statement->as<Global>().names, "names"_s, "Global"_s) && validateIdentifiers(statement->as<Global>().names);
        case Statement::Kind::Nonlocal:
            return validateNonEmpty(statement->as<Nonlocal>().names, "names"_s, "Nonlocal"_s) && validateIdentifiers(statement->as<Nonlocal>().names);
        case Statement::Kind::Expr:
            return validateExpression(statement->as<Expr>().value, Load);
        case Statement::Kind::Pass:
        case Statement::Kind::Break:
        case Statement::Kind::Continue:
            return true;
        }
        return fail(ASTError::Kind::SystemError, "unexpected statement"_s);
    }

    // validate_expr()
    bool validateExpression(Expression* expression, ExpressionContext context)
    {
        if (!validatePositions(*expression) || !enter())
            return false;
        using enum ExpressionContext;

        // First whether it is looked at, assigned to or deleted, and says so.
        std::optional<ExpressionContext> actual;
        switch (expression->kind) {
        case Expression::Kind::Attribute:
            actual = expression->as<Attribute>().context;
            break;
        case Expression::Kind::Subscript:
            actual = expression->as<Subscript>().context;
            break;
        case Expression::Kind::Starred:
            actual = expression->as<Starred>().context;
            break;
        case Expression::Kind::Name:
            if (!validateName(*expression->as<Name>().id))
                return false;
            actual = expression->as<Name>().context;
            break;
        case Expression::Kind::List:
            actual = expression->as<List>().context;
            break;
        case Expression::Kind::Tuple:
            actual = expression->as<Tuple>().context;
            break;
        default:
            if (context != Load)
                return fail(makeString("expression which can't be assigned to in "_s, nameOf(context), " context"_s));
            break;
        }
        if (actual && *actual != context)
            return fail(makeString("expression must have "_s, nameOf(context), " context but has "_s, nameOf(*actual), " instead"_s));

        switch (expression->kind) {
        case Expression::Kind::BoolOp:
            if (expression->as<BoolOp>().values.size() < 2)
                return fail("BoolOp with less than 2 values"_s);
            return validateExpressions(expression->as<BoolOp>().values, Load, false);
        case Expression::Kind::BinOp:
            return validateExpression(expression->as<BinOp>().left, Load) && validateExpression(expression->as<BinOp>().right, Load);
        case Expression::Kind::UnaryOp:
            return validateExpression(expression->as<UnaryOp>().operand, Load);
        case Expression::Kind::Lambda:
            return validateArguments(*expression->as<Lambda>().arguments) && validateExpression(expression->as<Lambda>().body, Load);
        case Expression::Kind::IfExp: {
            auto& node = expression->as<IfExp>();
            return validateExpression(node.test, Load) && validateExpression(node.body, Load) && validateExpression(node.orElse, Load);
        }
        case Expression::Kind::Dict: {
            auto& node = expression->as<Dict>();
            if (node.keys.size() != node.values.size())
                return fail("Dict doesn't have the same number of keys as values"_s);
            // There is no key where it is {**value}.
            return validateExpressions(node.keys, Load, true) && validateExpressions(node.values, Load, false);
        }
        case Expression::Kind::Set:
            return validateExpressions(expression->as<Set>().elements, Load, false);
        case Expression::Kind::ListComp:
            return validateComprehension(expression->as<ListComp>().generators) && validateExpression(expression->as<ListComp>().element, Load);
        case Expression::Kind::SetComp:
            return validateComprehension(expression->as<SetComp>().generators) && validateExpression(expression->as<SetComp>().element, Load);
        case Expression::Kind::GeneratorExp:
            return validateComprehension(expression->as<GeneratorExp>().generators) && validateExpression(expression->as<GeneratorExp>().element, Load);
        case Expression::Kind::DictComp: {
            auto& node = expression->as<DictComp>();
            return validateComprehension(node.generators) && validateExpression(node.key, Load) && validateExpression(node.value, Load);
        }
        case Expression::Kind::Yield:
            return !expression->as<Yield>().value || validateExpression(expression->as<Yield>().value, Load);
        case Expression::Kind::YieldFrom:
            return validateExpression(expression->as<YieldFrom>().value, Load);
        case Expression::Kind::Await:
            return validateExpression(expression->as<Await>().value, Load);
        case Expression::Kind::Compare: {
            auto& node = expression->as<Compare>();
            if (node.comparators.empty())
                return fail("Compare with no comparators"_s);
            if (node.comparators.size() != node.ops.size())
                return fail("Compare has a different number of comparators and operands"_s);
            return validateExpressions(node.comparators, Load, false) && validateExpression(node.left, Load);
        }
        case Expression::Kind::Call: {
            auto& node = expression->as<Call>();
            return validateExpression(node.function, Load) && validateExpressions(node.arguments, Load, false) && validateKeywords(node.keywords);
        }
        case Expression::Kind::Constant:
            return validateConstant(expression->as<Constant>());
        case Expression::Kind::JoinedStr:
            return validateExpressions(expression->as<JoinedStr>().values, Load, false);
        case Expression::Kind::TemplateStr:
            return validateExpressions(expression->as<TemplateStr>().values, Load, false);
        case Expression::Kind::FormattedValue: {
            auto& node = expression->as<FormattedValue>();
            return validateExpression(node.value, Load) && (!node.formatSpecification || validateExpression(node.formatSpecification, Load));
        }
        case Expression::Kind::Interpolation: {
            auto& node = expression->as<Interpolation>();
            // CPython does not look at what it says its source is, and finds out when it comes to keep it that it cannot.
            return validateExpression(node.value, Load) && (!node.formatSpecification || validateExpression(node.formatSpecification, Load)) && validateConstant(*node.source);
        }
        case Expression::Kind::Attribute:
            return validateExpression(expression->as<Attribute>().value, Load);
        case Expression::Kind::Subscript:
            return validateExpression(expression->as<Subscript>().slice, Load) && validateExpression(expression->as<Subscript>().value, Load);
        case Expression::Kind::Starred:
            return validateExpression(expression->as<Starred>().value, context);
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            return (!node.lower || validateExpression(node.lower, Load)) && (!node.upper || validateExpression(node.upper, Load)) && (!node.step || validateExpression(node.step, Load));
        }
        case Expression::Kind::List:
            return validateExpressions(expression->as<List>().elements, context, false);
        case Expression::Kind::Tuple:
            return validateExpressions(expression->as<Tuple>().elements, context, false);
        case Expression::Kind::NamedExpr:
            if (!expression->as<NamedExpr>().target->is<Name>())
                return fail(ASTError::Kind::TypeError, "NamedExpr target must be a Name"_s);
            return validateExpression(expression->as<NamedExpr>().value, Load);
        case Expression::Kind::Name:
            return true;
        }
        return fail(ASTError::Kind::SystemError, "unexpected expression"_s);
    }

private:
    bool fail(ASTError::Kind kind, String&& message)
    {
        if (!m_error)
            m_error = { kind, WTF::move(message) };
        return false;
    }

    bool fail(String&& message) { return fail(ASTError::Kind::ValueError, WTF::move(message)); }

    bool enter()
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return true;
        return fail(ASTError::Kind::RecursionError, "maximum recursion depth exceeded during compilation"_s);
    }

    static ASCIILiteral nameOf(ExpressionContext context)
    {
        static constexpr ASCIILiteral names[] = { "Load"_s, "Store"_s, "Del"_s };
        return names[static_cast<unsigned>(context)];
    }

    // VALIDATE_POSITIONS()
    bool validatePositions(const Node& node)
    {
        int line = node.line;
        int column = node.column;
        int endLine = node.endLine;
        int endColumn = node.endColumn;
        if (line > endLine)
            return fail(makeString("AST node line range ("_s, line, ", "_s, endLine, ") is not valid"_s));
        if ((line < 0 && endLine != line) || (column < 0 && column != endColumn))
            return fail(makeString("AST node column range ("_s, column, ", "_s, endColumn, ") for line range ("_s, line, ", "_s, endLine, ") is not valid"_s));
        if (line == endLine && column > endColumn)
            return fail(makeString("line "_s, line, ", column "_s, column, '-', endColumn, " is not a valid range"_s));
        return true;
    }

    bool validateName(const Identifier& name)
    {
        for (ASCIILiteral forbidden : { "None"_s, "True"_s, "False"_s }) {
            if (name == forbidden)
                return fail(makeString("identifier field can't represent '"_s, forbidden, "' constant"_s));
        }
        return true;
    }

    bool validateComprehension(Sequence<Comprehension*> generators)
    {
        if (generators.empty())
            return fail("comprehension with no generators"_s);
        for (Comprehension* generator : generators) {
            if (!validateExpression(generator->target, ExpressionContext::Store) || !validateExpression(generator->iterable, ExpressionContext::Load) || !validateExpressions(generator->conditions, ExpressionContext::Load, false))
                return false;
        }
        return true;
    }

    bool validateKeywords(Sequence<Keyword*> keywords)
    {
        for (Keyword* keyword : keywords) {
            if (!validateExpression(keyword->value, ExpressionContext::Load))
                return false;
        }
        return true;
    }

    // validate_args()
    bool validateParameters(Sequence<Argument*> parameters)
    {
        for (Argument* parameter : parameters) {
            if (!validatePositions(*parameter))
                return false;
            if (parameter->annotation && !validateExpression(parameter->annotation, ExpressionContext::Load))
                return false;
        }
        return true;
    }

    bool validateArguments(Arguments& arguments)
    {
        using enum ExpressionContext;
        if (!validateParameters(arguments.positionalOnly) || !validateParameters(arguments.positional))
            return false;
        if (arguments.variadic && arguments.variadic->annotation && !validateExpression(arguments.variadic->annotation, Load))
            return false;
        if (!validateParameters(arguments.keywordOnly))
            return false;
        if (arguments.keywordVariadic && arguments.keywordVariadic->annotation && !validateExpression(arguments.keywordVariadic->annotation, Load))
            return false;
        if (arguments.defaults.size() > arguments.positionalOnly.size() + arguments.positional.size())
            return fail("more positional defaults than args on arguments"_s);
        if (arguments.keywordDefaults.size() != arguments.keywordOnly.size())
            return fail("length of kwonlyargs is not the same as kw_defaults on arguments"_s);
        return validateExpressions(arguments.defaults, Load, false) && validateExpressions(arguments.keywordDefaults, Load, true);
    }

    bool validateConstant(Constant& constant)
    {
        switch (constant.type) {
        case Constant::Type::Tuple:
        case Constant::Type::FrozenSet:
            if (!enter())
                return false;
            for (Constant* element : constant.elements) {
                if (!validateConstant(*element))
                    return false;
            }
            return true;
        case Constant::Type::Invalid:
            return fail(ASTError::Kind::TypeError, makeString("got an invalid type in Constant: "_s, constant.text->string()));
        default:
            return true;
        }
    }

    // ---- Patterns

    static bool isLiteralNumber(Expression& expression, bool allowsReal, bool allowsImaginary)
    {
        switch (expression.as<Constant>().type) {
        case Constant::Type::Integer:
        case Constant::Type::BigInteger:
        case Constant::Type::Float:
            return allowsReal;
        case Constant::Type::Imaginary:
        case Constant::Type::Complex:
            return allowsImaginary;
        default:
            return false;
        }
    }

    static bool isLiteralNegative(Expression& expression, bool allowsReal, bool allowsImaginary)
    {
        auto& node = expression.as<UnaryOp>();
        return node.op == UnaryOperator::USub && node.operand->is<Constant>() && isLiteralNumber(*node.operand, allowsReal, allowsImaginary);
    }

    static bool isLiteralComplex(Expression& expression)
    {
        auto& node = expression.as<BinOp>();
        if (node.op != BinaryOperator::Add && node.op != BinaryOperator::Sub)
            return false;
        if (node.left->is<Constant>() ? !isLiteralNumber(*node.left, true, false) : !node.left->is<UnaryOp>() || !isLiteralNegative(*node.left, true, false))
            return false;
        return node.right->is<Constant>() && isLiteralNumber(*node.right, false, true);
    }

    // validate_pattern_match_value()
    bool validatePatternValue(Expression* expression)
    {
        if (!validateExpression(expression, ExpressionContext::Load))
            return false;
        switch (expression->kind) {
        case Expression::Kind::Constant:
            // Not ..., nor a tuple. And for True, False and None there is MatchSingleton.
            switch (expression->as<Constant>().type) {
            case Constant::Type::Integer:
            case Constant::Type::BigInteger:
            case Constant::Type::Float:
            case Constant::Type::Imaginary:
            case Constant::Type::Complex:
            case Constant::Type::String:
            case Constant::Type::Bytes:
                return true;
            default:
                return fail("unexpected constant inside of a literal pattern"_s);
            }
        case Expression::Kind::Attribute:
            return true;
        case Expression::Kind::UnaryOp:
            if (isLiteralNegative(*expression, true, true))
                return true;
            break;
        case Expression::Kind::BinOp:
            if (isLiteralComplex(*expression))
                return true;
            break;
        case Expression::Kind::JoinedStr:
        case Expression::Kind::TemplateStr:
            // What generates code will have none of it, and says so as it would of source.
            return true;
        default:
            break;
        }
        return fail("patterns may only match literals and attribute lookups"_s);
    }

    bool validateCapture(const Identifier& name)
    {
        if (name == "_"_s)
            return fail("can't capture name '_' in patterns"_s);
        return validateName(name);
    }

    bool validatePatterns(Sequence<Pattern*> patterns, bool allowsStar)
    {
        for (Pattern* pattern : patterns) {
            // Nor here.
            if (!pattern)
                return fail("None disallowed in pattern list"_s);
            if (!validatePattern(pattern, allowsStar))
                return false;
        }
        return true;
    }

    bool validatePattern(Pattern* pattern, bool allowsStar)
    {
        if (!validatePositions(*pattern) || !enter())
            return false;
        switch (pattern->kind) {
        case Pattern::Kind::MatchValue:
            return validatePatternValue(pattern->as<MatchValue>().value);
        case Pattern::Kind::MatchSingleton:
            switch (pattern->as<MatchSingleton>().value) {
            case Constant::Type::None:
            case Constant::Type::True:
            case Constant::Type::False:
                return true;
            default:
                return fail("MatchSingleton can only contain True, False and None"_s);
            }
        case Pattern::Kind::MatchSequence:
            return validatePatterns(pattern->as<MatchSequence>().patterns, true);
        case Pattern::Kind::MatchMapping: {
            auto& node = pattern->as<MatchMapping>();
            if (node.keys.size() != node.patterns.size())
                return fail("MatchMapping doesn't have the same number of keys as patterns"_s);
            if (node.rest && !validateCapture(*node.rest))
                return false;
            for (Expression* key : node.keys) {
                // These can be written for a key, though not for a pattern by itself.
                if (auto* constant = key->tryAs<Constant>(); constant && (constant->type == Constant::Type::None || constant->type == Constant::Type::True || constant->type == Constant::Type::False))
                    continue;
                if (!validatePatternValue(key))
                    return false;
            }
            return validatePatterns(node.patterns, false);
        }
        case Pattern::Kind::MatchClass: {
            auto& node = pattern->as<MatchClass>();
            if (node.keywordAttributes.size() != node.keywordPatterns.size())
                return fail("MatchClass doesn't have the same number of keyword attributes as patterns"_s);
            if (!validateExpression(node.cls, ExpressionContext::Load))
                return false;
            for (Expression* cls = node.cls; !cls->is<Name>(); cls = cls->as<Attribute>().value) {
                if (!cls->is<Attribute>())
                    return fail("MatchClass cls field can only contain Name or Attribute nodes."_s);
            }
            if (!validateIdentifiers(node.keywordAttributes))
                return false;
            for (const Identifier* name : node.keywordAttributes) {
                if (!validateName(*name))
                    return false;
            }
            return validatePatterns(node.patterns, false) && validatePatterns(node.keywordPatterns, false);
        }
        case Pattern::Kind::MatchStar:
            if (!allowsStar)
                return fail("can't use MatchStar here"_s);
            return !pattern->as<MatchStar>().name || validateCapture(*pattern->as<MatchStar>().name);
        case Pattern::Kind::MatchAs: {
            auto& node = pattern->as<MatchAs>();
            if (node.name && !validateCapture(*node.name))
                return false;
            if (!node.pattern)
                return true;
            if (!node.name)
                return fail("MatchAs must specify a target name if a pattern is given"_s);
            return validatePattern(node.pattern, false);
        }
        case Pattern::Kind::MatchOr:
            if (pattern->as<MatchOr>().patterns.size() < 2)
                return fail("MatchOr requires at least 2 patterns"_s);
            return validatePatterns(pattern->as<MatchOr>().patterns, false);
        }
        return fail(ASTError::Kind::SystemError, "unexpected pattern"_s);
    }

    // ---- Lists

    template<typename T>
    bool validateNonEmpty(Sequence<T> sequence, ASCIILiteral what, ASCIILiteral owner)
    {
        if (!sequence.empty())
            return true;
        return fail(makeString("empty "_s, what, " on "_s, owner));
    }

    // CPython does not look, and falls over.
    bool validateIdentifiers(Sequence<const Identifier*> names)
    {
        for (const Identifier* name : names) {
            if (!name)
                return fail("None disallowed in identifier list"_s);
        }
        return true;
    }

    bool validateAssignmentList(Sequence<Expression*> targets, ExpressionContext context)
    {
        return validateNonEmpty(targets, "targets"_s, context == ExpressionContext::Del ? "Delete"_s : "Assign"_s) && validateExpressions(targets, context, false);
    }

    bool validateBody(Sequence<Statement*> body, ASCIILiteral owner)
    {
        return validateNonEmpty(body, "body"_s, owner) && validateStatements(body);
    }

    bool validateStatements(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements) {
            if (!statement)
                return fail("None disallowed in statement list"_s);
            if (!validateStatement(statement))
                return false;
        }
        return true;
    }

    bool validateExpressions(Sequence<Expression*> expressions, ExpressionContext context, bool allowsNull)
    {
        for (Expression* expression : expressions) {
            if (expression) {
                if (!validateExpression(expression, context))
                    return false;
            } else if (!allowsNull)
                return fail("None disallowed in expression list"_s);
        }
        return true;
    }

    bool validateTypeParameters(Sequence<TypeParameter*> parameters)
    {
        for (TypeParameter* parameter : parameters) {
            // This it passes over, and falls over later.
            if (!parameter)
                return fail("None disallowed in type parameter list"_s);
            if (!validatePositions(*parameter) || !validateName(*parameter->name))
                return false;
            if (parameter->bound && !validateExpression(parameter->bound, ExpressionContext::Load))
                return false;
            if (parameter->defaultValue && !validateExpression(parameter->defaultValue, ExpressionContext::Load))
                return false;
        }
        return true;
    }

    VM& m_vm;
    ASTError m_error;
};

} // anonymous namespace

ASTError validate(VM& vm, Module& module)
{
    Validator validator(vm);
    validator.validate(module);
    return validator.takeError();
}

ASTError validate(VM& vm, Statement& statement)
{
    Validator validator(vm);
    validator.validateStatement(&statement);
    return validator.takeError();
}

ASTError validate(VM& vm, Expression& expression)
{
    Validator validator(vm);
    validator.validateExpression(&expression, ExpressionContext::Load);
    return validator.takeError();
}

} } // namespace JSC::Python
