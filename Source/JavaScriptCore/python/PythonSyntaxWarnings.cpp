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
#include "PythonSyntaxWarnings.h"

#include "PythonSymbolTable.h"
#include "PythonText.h"

// What is warned of when a piece of source is compiled, besides what the lexer finds. CPython comes on these as it generates code. Here the code of a function is not generated until it is called, so
// they are looked for by themselves, in the whole of what was parsed, in the order that CPython's Python/codegen.c comes to things in.

namespace JSC { namespace Python {

namespace {

SyntaxWarning warningAt(const Node& node, String&& message)
{
    return { WTF::move(message), { }, node.line, node.column, node.endLine, node.endColumn };
}

// ---- 'return', 'break' and 'continue' in a 'finally' (PEP 765): Python/ast_preprocess.c

class ControlFlowChecker {
public:
    explicit ControlFlowChecker(Vector<SyntaxWarning>& warnings)
        : m_warnings(warnings)
    {
    }

    void visit(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements)
            visit(*statement);
    }

private:
    // What a statement is most closely in, of these.
    enum class Context : uint8_t { None, Finally, Function, Loop };

    void visitIn(Context context, Sequence<Statement*> statements)
    {
        SetForScope scope(m_context, context);
        visit(statements);
    }

    void warn(const Statement& statement, ASCIILiteral keyword)
    {
        m_warnings.append(warningAt(statement, concatenate('\'', keyword, "' in a 'finally' block"_s)));
    }

    void visit(Statement& statement)
    {
        switch (statement.kind) {
        case Statement::Kind::FunctionDef:
            return visitIn(Context::Function, statement.as<FunctionDef>().body);
        case Statement::Kind::ClassDef:
            return visit(statement.as<ClassDef>().body);
        case Statement::Kind::Return:
            if (m_context == Context::Finally)
                warn(statement, "return"_s);
            return;
        case Statement::Kind::Break:
            if (m_context == Context::Finally)
                warn(statement, "break"_s);
            return;
        case Statement::Kind::Continue:
            if (m_context == Context::Finally)
                warn(statement, "continue"_s);
            return;
        case Statement::Kind::For:
            visitIn(Context::Loop, statement.as<For>().body);
            return visit(statement.as<For>().orElse);
        case Statement::Kind::While:
            visitIn(Context::Loop, statement.as<While>().body);
            return visit(statement.as<While>().orElse);
        case Statement::Kind::If:
            visit(statement.as<If>().body);
            return visit(statement.as<If>().orElse);
        case Statement::Kind::With:
            return visit(statement.as<With>().body);
        case Statement::Kind::Try: {
            auto& node = statement.as<Try>();
            visit(node.body);
            for (ExceptHandler* handler : node.handlers)
                visit(handler->body);
            visit(node.orElse);
            return visitIn(Context::Finally, node.finalBody);
        }
        case Statement::Kind::Match:
            for (MatchCase* matchCase : statement.as<Match>().cases)
                visit(matchCase->body);
            return;
        default:
            return;
        }
    }

    Vector<SyntaxWarning>& m_warnings;
    Context m_context { Context::None };
};

// ---- What Python/codegen.c warns of

class CodeChecker {
public:
    CodeChecker(Vector<SyntaxWarning>& warnings, unsigned futureFeatures, unsigned optimizationLevel)
        : m_warnings(warnings)
        , m_hasFutureAnnotations(futureFeatures & FutureAnnotations)
        , m_optimizationLevel(optimizationLevel)
    {
    }

    // The body of a module or of a class. What its variables are annotated with is compiled when the rest has been.
    void visitBodyWithAnnotations(Sequence<Statement*> statements)
    {
        Vector<Expression*> outer = std::exchange(m_deferredAnnotations, { });
        SetForScope isInFunction(m_isInFunction, false);
        visit(statements);
        for (Expression* annotation : std::exchange(m_deferredAnnotations, WTF::move(outer)))
            visit(annotation);
    }

    void visit(Expression* expression)
    {
        if (expression)
            visit(*expression);
    }

private:
    template<typename... Arguments>
    void warn(const Node& node, Arguments&&... message)
    {
        m_warnings.append(warningAt(node, concatenate(std::forward<Arguments>(message)...)));
    }

    // __debug__ has been made a constant by now.
    static bool isDebug(Expression& expression)
    {
        return expression.is<Name>() && expression.as<Name>().context == ExpressionContext::Load && *expression.as<Name>().id == "__debug__"_s;
    }

    static bool isConstant(Expression& expression) { return expression.is<Constant>() || isDebug(expression); }

    // infer_type(): the name of the type that it is bound to be of. Null if there is no telling.
    static ASCIILiteral inferType(Expression& expression)
    {
        switch (expression.kind) {
        case Expression::Kind::Tuple:
            return "tuple"_s;
        case Expression::Kind::List:
        case Expression::Kind::ListComp:
            return "list"_s;
        case Expression::Kind::Dict:
        case Expression::Kind::DictComp:
            return "dict"_s;
        case Expression::Kind::Set:
        case Expression::Kind::SetComp:
            return "set"_s;
        case Expression::Kind::GeneratorExp:
            return "generator"_s;
        case Expression::Kind::Lambda:
            return "function"_s;
        case Expression::Kind::TemplateStr:
        case Expression::Kind::Interpolation:
            return "string.templatelib.Template"_s;
        case Expression::Kind::JoinedStr:
        case Expression::Kind::FormattedValue:
            return "str"_s;
        case Expression::Kind::Name:
            return isDebug(expression) ? "bool"_s : ASCIILiteral();
        case Expression::Kind::Constant:
            switch (expression.as<Constant>().type) {
            case Constant::Type::None:
                return "NoneType"_s;
            case Constant::Type::True:
            case Constant::Type::False:
                return "bool"_s;
            case Constant::Type::Ellipsis:
                return "ellipsis"_s;
            case Constant::Type::Integer:
            case Constant::Type::BigInteger:
                return "int"_s;
            case Constant::Type::Float:
                return "float"_s;
            case Constant::Type::Imaginary:
            case Constant::Type::Complex:
                return "complex"_s;
            case Constant::Type::String:
                return "str"_s;
            case Constant::Type::Bytes:
                return "bytes"_s;
            case Constant::Type::Tuple:
                return "tuple"_s;
            case Constant::Type::FrozenSet:
                return "frozenset"_s;
            case Constant::Type::Invalid:
                break;
            }
            RELEASE_ASSERT_NOT_REACHED();
        default:
            return { };
        }
    }

    // check_is_arg(): whether it makes sense to ask if something is it
    static bool canBeIdentical(Expression& expression)
    {
        if (auto* tuple = expression.tryAs<Tuple>())
            return !std::ranges::all_of(tuple->elements, [] (Expression* element) { return isConstant(*element); });
        if (isDebug(expression))
            return true;
        auto* constant = expression.tryAs<Constant>();
        if (!constant)
            return true;
        switch (constant->type) {
        case Constant::Type::None:
        case Constant::Type::True:
        case Constant::Type::False:
        case Constant::Type::Ellipsis:
            return true;
        default:
            return false;
        }
    }

    // codegen_check_compare()
    void checkCompare(Compare& node)
    {
        Expression* left = node.left;
        for (size_t i = 0; i < node.ops.size(); ++i) {
            Expression* right = node.comparators[i];
            ComparisonOperator op = node.ops[i];
            if ((op == ComparisonOperator::Is || op == ComparisonOperator::IsNot) && (!canBeIdentical(*left) || !canBeIdentical(*right))) {
                ASCIILiteral type = inferType(canBeIdentical(*left) ? *right : *left);
                if (op == ComparisonOperator::Is)
                    return warn(node, "\"is\" with '"_s, type, "' literal. Did you mean \"==\"?"_s);
                return warn(node, "\"is not\" with '"_s, type, "' literal. Did you mean \"!=\"?"_s);
            }
            left = right;
        }
    }

    // check_caller()
    void checkCaller(Expression& function)
    {
        switch (function.kind) {
        case Expression::Kind::Name:
            if (!isDebug(function))
                return;
            [[fallthrough]];
        case Expression::Kind::Constant:
        case Expression::Kind::Tuple:
        case Expression::Kind::List:
        case Expression::Kind::ListComp:
        case Expression::Kind::Dict:
        case Expression::Kind::DictComp:
        case Expression::Kind::Set:
        case Expression::Kind::SetComp:
        case Expression::Kind::GeneratorExp:
        case Expression::Kind::JoinedStr:
        case Expression::Kind::TemplateStr:
        case Expression::Kind::FormattedValue:
        case Expression::Kind::Interpolation:
            return warn(function, '\'', inferType(function), "' object is not callable; perhaps you missed a comma?"_s);
        default:
            return;
        }
    }

    // check_subscripter()
    void checkSubscripter(Expression& value)
    {
        switch (value.kind) {
        case Expression::Kind::Name:
            if (!isDebug(value))
                return;
            break;
        case Expression::Kind::Constant:
            switch (value.as<Constant>().type) {
            case Constant::Type::String:
            case Constant::Type::Bytes:
            case Constant::Type::Tuple:
                return;
            default:
                break;
            }
            break;
        case Expression::Kind::Set:
        case Expression::Kind::SetComp:
        case Expression::Kind::GeneratorExp:
        case Expression::Kind::TemplateStr:
        case Expression::Kind::Interpolation:
        case Expression::Kind::Lambda:
            break;
        default:
            return;
        }
        warn(value, '\'', inferType(value), "' object is not subscriptable; perhaps you missed a comma?"_s);
    }

    // check_index()
    void checkIndex(Expression& value, Expression& index)
    {
        ASCIILiteral indexType = inferType(index);
        // A bool is an int. There is no telling what a slice is, since it is no constant.
        if (indexType.isNull() || indexType == "int"_s || indexType == "bool"_s)
            return;
        switch (value.kind) {
        case Expression::Kind::Constant:
            if (value.as<Constant>().type != Constant::Type::String && value.as<Constant>().type != Constant::Type::Bytes)
                return;
            break;
        case Expression::Kind::Tuple:
        case Expression::Kind::List:
        case Expression::Kind::ListComp:
        case Expression::Kind::JoinedStr:
        case Expression::Kind::FormattedValue:
            break;
        default:
            return;
        }
        warn(value, inferType(value), " indices must be integers or slices, not "_s, indexType, "; perhaps you missed a comma?"_s);
    }

    void visit(Sequence<Expression*> expressions)
    {
        for (Expression* expression : expressions)
            visit(expression);
    }

    void visit(Sequence<Keyword*> keywords)
    {
        for (Keyword* keyword : keywords)
            visit(keyword->value);
    }

    void visit(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements)
            visit(*statement);
    }

    // codegen_default_arguments()
    void visitDefaults(const Arguments& arguments)
    {
        visit(arguments.defaults);
        visit(arguments.keywordDefaults);
    }

    // codegen_function_annotations()
    void visitAnnotations(const Arguments& arguments, Expression* returns)
    {
        if (m_hasFutureAnnotations)
            return;
        auto visitEach = [&] (Sequence<Argument*> sequence) {
            for (Argument* argument : sequence)
                visit(argument->annotation);
        };
        visitEach(arguments.positionalOnly);
        visitEach(arguments.positional);
        if (arguments.variadic)
            visit(arguments.variadic->annotation);
        visitEach(arguments.keywordOnly);
        if (arguments.keywordVariadic)
            visit(arguments.keywordVariadic->annotation);
        visit(returns);
    }

    // codegen_type_params()
    void visit(Sequence<TypeParameter*> parameters)
    {
        for (TypeParameter* parameter : parameters) {
            visit(parameter->bound);
            visit(parameter->defaultValue);
        }
    }

    // codegen_comprehension(): what the first `for` goes through comes first, and what is made of it all comes last.
    template<typename VisitElement>
    void visitComprehension(Sequence<Comprehension*> generators, const VisitElement& visitElement)
    {
        for (Comprehension* generator : generators) {
            visit(generator->iterable);
            visit(generator->target);
            visit(generator->conditions);
        }
        visitElement();
    }

    void visit(Pattern& pattern)
    {
        switch (pattern.kind) {
        case Pattern::Kind::MatchValue:
            return visit(pattern.as<MatchValue>().value);
        case Pattern::Kind::MatchSequence:
            for (Pattern* item : pattern.as<MatchSequence>().patterns)
                visit(*item);
            return;
        case Pattern::Kind::MatchMapping: {
            auto& node = pattern.as<MatchMapping>();
            visit(node.keys);
            for (Pattern* item : node.patterns)
                visit(*item);
            return;
        }
        case Pattern::Kind::MatchClass: {
            auto& node = pattern.as<MatchClass>();
            visit(node.cls);
            for (Pattern* item : node.patterns)
                visit(*item);
            for (Pattern* item : node.keywordPatterns)
                visit(*item);
            return;
        }
        case Pattern::Kind::MatchAs:
            if (Pattern* inner = pattern.as<MatchAs>().pattern)
                visit(*inner);
            return;
        case Pattern::Kind::MatchOr:
            for (Pattern* item : pattern.as<MatchOr>().patterns)
                visit(*item);
            return;
        case Pattern::Kind::MatchSingleton:
        case Pattern::Kind::MatchStar:
            return;
        }
    }

    void visit(Expression& expression)
    {
        switch (expression.kind) {
        case Expression::Kind::BoolOp:
            return visit(expression.as<BoolOp>().values);
        case Expression::Kind::NamedExpr:
            visit(expression.as<NamedExpr>().value);
            return visit(expression.as<NamedExpr>().target);
        case Expression::Kind::BinOp:
            visit(expression.as<BinOp>().left);
            return visit(expression.as<BinOp>().right);
        case Expression::Kind::UnaryOp:
            return visit(expression.as<UnaryOp>().operand);
        case Expression::Kind::Lambda: {
            auto& node = expression.as<Lambda>();
            visitDefaults(*node.arguments);
            SetForScope isInFunction(m_isInFunction, true);
            return visit(node.body);
        }
        case Expression::Kind::IfExp:
            visit(expression.as<IfExp>().test);
            visit(expression.as<IfExp>().body);
            return visit(expression.as<IfExp>().orElse);
        case Expression::Kind::Dict: {
            auto& node = expression.as<Dict>();
            for (size_t i = 0; i < node.values.size(); ++i) {
                visit(node.keys[i]);
                visit(node.values[i]);
            }
            return;
        }
        case Expression::Kind::Set:
            return visit(expression.as<Set>().elements);
        case Expression::Kind::ListComp:
            return visitComprehension(expression.as<ListComp>().generators, [&] { visit(expression.as<ListComp>().element); });
        case Expression::Kind::SetComp:
            return visitComprehension(expression.as<SetComp>().generators, [&] { visit(expression.as<SetComp>().element); });
        case Expression::Kind::GeneratorExp:
            return visitComprehension(expression.as<GeneratorExp>().generators, [&] { visit(expression.as<GeneratorExp>().element); });
        case Expression::Kind::DictComp:
            return visitComprehension(expression.as<DictComp>().generators, [&] {
                visit(expression.as<DictComp>().key);
                visit(expression.as<DictComp>().value);
            });
        case Expression::Kind::Await:
            return visit(expression.as<Await>().value);
        case Expression::Kind::Yield:
            return visit(expression.as<Yield>().value);
        case Expression::Kind::YieldFrom:
            return visit(expression.as<YieldFrom>().value);
        case Expression::Kind::Compare: {
            auto& node = expression.as<Compare>();
            checkCompare(node);
            visit(node.left);
            return visit(node.comparators);
        }
        case Expression::Kind::Call: {
            auto& node = expression.as<Call>();
            checkCaller(*node.function);
            visit(node.function);
            visit(node.arguments);
            return visit(node.keywords);
        }
        case Expression::Kind::FormattedValue:
            visit(expression.as<FormattedValue>().value);
            return visit(expression.as<FormattedValue>().formatSpecification);
        case Expression::Kind::Interpolation:
            visit(expression.as<Interpolation>().value);
            return visit(expression.as<Interpolation>().formatSpecification);
        case Expression::Kind::JoinedStr:
            return visit(expression.as<JoinedStr>().values);
        case Expression::Kind::TemplateStr:
            return visit(expression.as<TemplateStr>().values);
        case Expression::Kind::Attribute:
            return visit(expression.as<Attribute>().value);
        case Expression::Kind::Subscript: {
            auto& node = expression.as<Subscript>();
            if (node.context == ExpressionContext::Load) {
                checkSubscripter(*node.value);
                checkIndex(*node.value, *node.slice);
            }
            visit(node.value);
            return visit(node.slice);
        }
        case Expression::Kind::Starred:
            return visit(expression.as<Starred>().value);
        case Expression::Kind::List:
            return visit(expression.as<List>().elements);
        case Expression::Kind::Tuple:
            return visit(expression.as<Tuple>().elements);
        case Expression::Kind::Slice:
            visit(expression.as<Slice>().lower);
            visit(expression.as<Slice>().upper);
            return visit(expression.as<Slice>().step);
        case Expression::Kind::Constant:
        case Expression::Kind::Name:
            return;
        }
    }

    void visit(Statement& statement)
    {
        switch (statement.kind) {
        case Statement::Kind::FunctionDef: {
            auto& node = statement.as<FunctionDef>();
            visit(node.decorators);
            visitDefaults(*node.arguments);
            visit(node.typeParameters);
            visitAnnotations(*node.arguments, node.returns);
            SetForScope isInFunction(m_isInFunction, true);
            return visit(node.body);
        }
        case Statement::Kind::ClassDef: {
            auto& node = statement.as<ClassDef>();
            visit(node.decorators);
            visit(node.typeParameters);
            visitBodyWithAnnotations(node.body);
            visit(node.bases);
            return visit(node.keywords);
        }
        case Statement::Kind::Return:
            return visit(statement.as<Return>().value);
        case Statement::Kind::Delete:
            return visit(statement.as<Delete>().targets);
        case Statement::Kind::Assign:
            visit(statement.as<Assign>().value);
            return visit(statement.as<Assign>().targets);
        case Statement::Kind::TypeAlias:
            visit(statement.as<TypeAlias>().typeParameters);
            return visit(statement.as<TypeAlias>().value);
        case Statement::Kind::AugAssign:
            visit(statement.as<AugAssign>().target);
            return visit(statement.as<AugAssign>().value);
        case Statement::Kind::AnnAssign: {
            // codegen_annassign()
            auto& node = statement.as<AnnAssign>();
            if (node.value) {
                visit(node.value);
                visit(node.target);
            }
            if (node.target->is<Name>()) {
                // Only that of a variable of a module or of a class is ever worked out.
                if (node.isSimple && !m_isInFunction && !m_hasFutureAnnotations)
                    m_deferredAnnotations.append(node.annotation);
                return;
            }
            // What is not a name is worked out, to see that it can be, if nothing is being assigned to it.
            if (!node.value) {
                if (auto* attribute = node.target->tryAs<Attribute>())
                    visit(attribute->value);
                else if (auto* subscript = node.target->tryAs<Subscript>()) {
                    visit(subscript->value);
                    visit(subscript->slice);
                }
            }
            if (!m_hasFutureAnnotations && !m_isInFunction)
                visit(node.annotation);
            return;
        }
        case Statement::Kind::For: {
            auto& node = statement.as<For>();
            visit(node.iterable);
            visit(node.target);
            visit(node.body);
            return visit(node.orElse);
        }
        case Statement::Kind::While:
            visit(statement.as<While>().test);
            visit(statement.as<While>().body);
            return visit(statement.as<While>().orElse);
        case Statement::Kind::If:
            visit(statement.as<If>().test);
            visit(statement.as<If>().body);
            return visit(statement.as<If>().orElse);
        case Statement::Kind::With: {
            auto& node = statement.as<With>();
            for (WithItem* item : node.items) {
                visit(item->contextExpression);
                visit(item->optionalVariables);
            }
            return visit(node.body);
        }
        case Statement::Kind::Match: {
            auto& node = statement.as<Match>();
            visit(node.subject);
            for (MatchCase* matchCase : node.cases) {
                visit(*matchCase->pattern);
                visit(matchCase->guard);
                visit(matchCase->body);
            }
            return;
        }
        case Statement::Kind::Raise:
            visit(statement.as<Raise>().exception);
            return visit(statement.as<Raise>().cause);
        case Statement::Kind::Try: {
            // codegen_try_except(), in codegen_try_finally(). CPython generates the code of a `finally` again for each `return`, `break` and `continue` that goes by way of it, and warns of what is in
            // it each time. Here it is warned of once.
            auto& node = statement.as<Try>();
            visit(node.body);
            visit(node.orElse);
            for (ExceptHandler* handler : node.handlers) {
                visit(handler->type);
                visit(handler->body);
            }
            return visit(node.finalBody);
        }
        case Statement::Kind::Assert: {
            auto& node = statement.as<Assert>();
            if (node.test->is<Tuple>() && !node.test->as<Tuple>().elements.empty())
                warn(node, "assertion is always true, perhaps remove parentheses?"_s);
            if (m_optimizationLevel)
                return;
            visit(node.test);
            return visit(node.message);
        }
        case Statement::Kind::Expr:
            return visit(statement.as<Expr>().value);
        case Statement::Kind::Import:
        case Statement::Kind::ImportFrom:
        case Statement::Kind::Global:
        case Statement::Kind::Nonlocal:
        case Statement::Kind::Pass:
        case Statement::Kind::Break:
        case Statement::Kind::Continue:
            return;
        }
    }

    Vector<SyntaxWarning>& m_warnings;
    Vector<Expression*> m_deferredAnnotations;
    bool m_hasFutureAnnotations;
    bool m_isInFunction { false };
    unsigned m_optimizationLevel;
};

} // anonymous namespace

void collectControlFlowWarnings(const Module& module, Vector<SyntaxWarning>& warnings)
{
    ControlFlowChecker(warnings).visit(module.body);
}

void collectCodeWarnings(const Module& module, unsigned futureFeatures, unsigned optimizationLevel, Vector<SyntaxWarning>& warnings)
{
    CodeChecker checker(warnings, futureFeatures, optimizationLevel);
    checker.visitBodyWithAnnotations(module.body);
    checker.visit(module.expression);
}

} } // namespace JSC::Python
