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
#include "PythonASTOptimizer.h"

#include "PythonArena.h"
#include "PythonSymbolTable.h"
#include "PythonUnparse.h"
#include "VM.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

// CPython's Python/ast_preprocess.c. There a node is written over with another. Here they are not all of a size, so it is whatever points to the node that is changed.

namespace JSC { namespace Python {

namespace {

class Optimizer {
public:
    Optimizer(VM& vm, Arena& arena, unsigned optimizationLevel, unsigned futureFeatures, bool onlyLookedOver)
        : m_vm(vm)
        , m_arena(arena)
        , m_optimizationLevel(optimizationLevel)
        , m_annotationsAreStrings(futureFeatures & FutureAnnotations)
        , m_onlyLookedOver(onlyLookedOver)
    {
    }

    bool run(Module& module)
    {
        switch (module.kind) {
        case Module::Kind::Module:
            foldBody(module.body);
            break;
        case Module::Kind::Interactive:
            fold(module.body);
            break;
        case Module::Kind::Expression:
            fold(module.expression);
            break;
        case Module::Kind::FunctionType:
            break;
        }
        return !m_isTooDeep;
    }

private:
    bool enter()
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            m_isTooDeep = true;
        return !m_isTooDeep;
    }

    template<typename T>
    T* make(const Node& where)
    {
        T* node = m_arena.create<T>();
        static_cast<Node&>(*node) = where;
        return node;
    }

    const Identifier* makeText(StringView text)
    {
        if (text.is8Bit())
            return &m_arena.identifiers().makeIdentifier(m_vm, text.span8());
        return &m_arena.identifiers().makeIdentifier(m_vm, text.span16());
    }

    // It is nowhere, as what CPython makes is.
    Constant* makeString(StringView text)
    {
        auto* constant = m_arena.create<Constant>();
        constant->type = Constant::Type::String;
        constant->text = makeText(text);
        constant->line = constant->column = constant->endLine = constant->endColumn = static_cast<unsigned>(-1);
        return constant;
    }

    // _PyAST_GetDocString()
    static bool hasDocstring(Sequence<Statement*> body)
    {
        if (body.empty() || !body[0]->is<Expr>())
            return false;
        auto* constant = body[0]->as<Expr>().value->tryAs<Constant>();
        return constant && constant->type == Constant::Type::String;
    }

    // astfold_body()
    void foldBody(Sequence<Statement*>& body)
    {
        bool hadDocstring = hasDocstring(body);
        if (hadDocstring && m_optimizationLevel >= 2) {
            if (body.size() == 1) {
                // There has to be something.
                Node where = *body[0];
                where.endLine = where.line;
                where.endColumn = where.column + 4;
                body[0] = make<Pass>(where);
            } else
                body = body.subspan(1);
            hadDocstring = false;
        }
        fold(body);
        // What has come to be a string is not to be taken for a docstring.
        if (!hadDocstring && hasDocstring(body)) {
            auto& statement = body[0]->as<Expr>();
            auto* joined = make<JoinedStr>(statement);
            Vector<Expression*, 1> values { statement.value };
            joined->values = m_arena.copy(values);
            statement.value = joined;
        }
    }

    void fold(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements) {
            if (statement)
                fold(*statement);
        }
    }

    void fold(Sequence<Expression*> expressions)
    {
        for (Expression*& expression : expressions)
            fold(expression);
    }

    void foldAnnotation(Expression*& annotation)
    {
        if (!m_annotationsAreStrings)
            fold(annotation);
    }

    void fold(Sequence<Argument*> parameters)
    {
        for (Argument* parameter : parameters)
            foldAnnotation(parameter->annotation);
    }

    void fold(Arguments& arguments)
    {
        fold(arguments.positionalOnly);
        fold(arguments.positional);
        if (arguments.variadic)
            foldAnnotation(arguments.variadic->annotation);
        fold(arguments.keywordOnly);
        fold(arguments.keywordDefaults);
        if (arguments.keywordVariadic)
            foldAnnotation(arguments.keywordVariadic->annotation);
        fold(arguments.defaults);
    }

    void fold(Sequence<Keyword*> keywords)
    {
        for (Keyword* keyword : keywords)
            fold(keyword->value);
    }

    void fold(Sequence<Comprehension*> generators)
    {
        for (Comprehension* generator : generators) {
            fold(generator->target);
            fold(generator->iterable);
            fold(generator->conditions);
        }
    }

    void fold(Sequence<TypeParameter*> parameters)
    {
        for (TypeParameter* parameter : parameters) {
            fold(parameter->bound);
            fold(parameter->defaultValue);
        }
    }

    // astfold_stmt()
    void fold(Statement& statement)
    {
        if (!enter())
            return;
        switch (statement.kind) {
        case Statement::Kind::FunctionDef: {
            auto& node = statement.as<FunctionDef>();
            fold(node.typeParameters);
            fold(*node.arguments);
            foldBody(node.body);
            fold(node.decorators);
            foldAnnotation(node.returns);
            return;
        }
        case Statement::Kind::ClassDef: {
            auto& node = statement.as<ClassDef>();
            fold(node.typeParameters);
            fold(node.bases);
            fold(node.keywords);
            foldBody(node.body);
            fold(node.decorators);
            return;
        }
        case Statement::Kind::Return:
            return fold(statement.as<Return>().value);
        case Statement::Kind::Delete:
            return fold(statement.as<Delete>().targets);
        case Statement::Kind::Assign:
            fold(statement.as<Assign>().targets);
            return fold(statement.as<Assign>().value);
        case Statement::Kind::TypeAlias:
            fold(statement.as<TypeAlias>().name);
            fold(statement.as<TypeAlias>().typeParameters);
            return fold(statement.as<TypeAlias>().value);
        case Statement::Kind::AugAssign:
            fold(statement.as<AugAssign>().target);
            return fold(statement.as<AugAssign>().value);
        case Statement::Kind::AnnAssign:
            fold(statement.as<AnnAssign>().target);
            foldAnnotation(statement.as<AnnAssign>().annotation);
            return fold(statement.as<AnnAssign>().value);
        case Statement::Kind::For: {
            auto& node = statement.as<For>();
            fold(node.target);
            fold(node.iterable);
            fold(node.body);
            return fold(node.orElse);
        }
        case Statement::Kind::While:
            fold(statement.as<While>().test);
            fold(statement.as<While>().body);
            return fold(statement.as<While>().orElse);
        case Statement::Kind::If:
            fold(statement.as<If>().test);
            fold(statement.as<If>().body);
            return fold(statement.as<If>().orElse);
        case Statement::Kind::With:
            for (WithItem* item : statement.as<With>().items) {
                fold(item->contextExpression);
                fold(item->optionalVariables);
            }
            return fold(statement.as<With>().body);
        case Statement::Kind::Match:
            fold(statement.as<Match>().subject);
            for (MatchCase* matchCase : statement.as<Match>().cases) {
                fold(*matchCase->pattern);
                fold(matchCase->guard);
                fold(matchCase->body);
            }
            return;
        case Statement::Kind::Raise:
            fold(statement.as<Raise>().exception);
            return fold(statement.as<Raise>().cause);
        case Statement::Kind::Try: {
            auto& node = statement.as<Try>();
            fold(node.body);
            for (ExceptHandler* handler : node.handlers) {
                fold(handler->type);
                fold(handler->body);
            }
            fold(node.orElse);
            return fold(node.finalBody);
        }
        case Statement::Kind::Assert:
            fold(statement.as<Assert>().test);
            return fold(statement.as<Assert>().message);
        case Statement::Kind::Expr:
            return fold(statement.as<Expr>().value);
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

    // astfold_expr(). There may be none.
    void fold(Expression*& expression)
    {
        if (!expression || !enter())
            return;
        switch (expression->kind) {
        case Expression::Kind::BoolOp:
            return fold(expression->as<BoolOp>().values);
        case Expression::Kind::BinOp:
            fold(expression->as<BinOp>().left);
            fold(expression->as<BinOp>().right);
            return foldFormat(expression);
        case Expression::Kind::UnaryOp:
            return fold(expression->as<UnaryOp>().operand);
        case Expression::Kind::Lambda:
            fold(*expression->as<Lambda>().arguments);
            return fold(expression->as<Lambda>().body);
        case Expression::Kind::IfExp:
            fold(expression->as<IfExp>().test);
            fold(expression->as<IfExp>().body);
            return fold(expression->as<IfExp>().orElse);
        case Expression::Kind::Dict:
            fold(expression->as<Dict>().keys);
            return fold(expression->as<Dict>().values);
        case Expression::Kind::Set:
            return fold(expression->as<Set>().elements);
        case Expression::Kind::ListComp:
            fold(expression->as<ListComp>().element);
            return fold(expression->as<ListComp>().generators);
        case Expression::Kind::SetComp:
            fold(expression->as<SetComp>().element);
            return fold(expression->as<SetComp>().generators);
        case Expression::Kind::DictComp:
            fold(expression->as<DictComp>().key);
            fold(expression->as<DictComp>().value);
            return fold(expression->as<DictComp>().generators);
        case Expression::Kind::GeneratorExp:
            fold(expression->as<GeneratorExp>().element);
            return fold(expression->as<GeneratorExp>().generators);
        case Expression::Kind::Await:
            return fold(expression->as<Await>().value);
        case Expression::Kind::Yield:
            return fold(expression->as<Yield>().value);
        case Expression::Kind::YieldFrom:
            return fold(expression->as<YieldFrom>().value);
        case Expression::Kind::Compare:
            fold(expression->as<Compare>().left);
            return fold(expression->as<Compare>().comparators);
        case Expression::Kind::Call:
            fold(expression->as<Call>().function);
            fold(expression->as<Call>().arguments);
            return fold(expression->as<Call>().keywords);
        case Expression::Kind::FormattedValue:
            fold(expression->as<FormattedValue>().value);
            return fold(expression->as<FormattedValue>().formatSpecification);
        case Expression::Kind::Interpolation:
            fold(expression->as<Interpolation>().value);
            return fold(expression->as<Interpolation>().formatSpecification);
        case Expression::Kind::JoinedStr:
            return fold(expression->as<JoinedStr>().values);
        case Expression::Kind::TemplateStr:
            return fold(expression->as<TemplateStr>().values);
        case Expression::Kind::Attribute:
            return fold(expression->as<Attribute>().value);
        case Expression::Kind::Subscript:
            fold(expression->as<Subscript>().value);
            return fold(expression->as<Subscript>().slice);
        case Expression::Kind::Starred:
            return fold(expression->as<Starred>().value);
        case Expression::Kind::Slice:
            fold(expression->as<Slice>().lower);
            fold(expression->as<Slice>().upper);
            return fold(expression->as<Slice>().step);
        case Expression::Kind::List:
            return fold(expression->as<List>().elements);
        case Expression::Kind::Tuple:
            return fold(expression->as<Tuple>().elements);
        case Expression::Kind::Name: {
            auto& node = expression->as<Name>();
            if (m_onlyLookedOver || node.context != ExpressionContext::Load || *node.id != "__debug__"_s)
                return;
            auto* constant = make<Constant>(node);
            constant->type = m_optimizationLevel ? Constant::Type::False : Constant::Type::True;
            expression = constant;
            return;
        }
        case Expression::Kind::NamedExpr:
            return fold(expression->as<NamedExpr>().value);
        case Expression::Kind::Constant:
            return;
        }
    }

    // ---- 'format' % (a, b)

    // fold_binop() and optimize_format(): only where there is no more to it than %s, %r and %a, with how wide and how much of it, and there are as many of them as there are values.
    void foldFormat(Expression*& expression)
    {
        if (m_onlyLookedOver)
            return;
        auto& node = expression->as<BinOp>();
        auto* format = node.left->tryAs<Constant>();
        auto* values = node.right->tryAs<Tuple>();
        if (node.op != BinaryOperator::Mod || !format || format->type != Constant::Type::String || !values)
            return;
        if (std::ranges::any_of(values->elements, [] (Expression* element) { return element->is<Starred>(); }))
            return;

        StringView text = format->text->string();
        unsigned position = 0;
        size_t used = 0;
        Vector<Expression*, 8> pieces;
        while (true) {
            // parse_literal()
            StringBuilder literal;
            while (position < text.length()) {
                if (text[position] != '%')
                    literal.append(text[position++]);
                else if (position + 1 < text.length() && text[position + 1] == '%') {
                    literal.append('%');
                    position += 2;
                } else
                    break;
            }
            if (!literal.isEmpty())
                pieces.append(makeString(literal.toString()));
            if (position >= text.length())
                break;
            if (used >= values->elements.size())
                return;
            ++position;
            Expression* piece = parseFormat(text, position, *values->elements[used++]);
            if (!piece)
                return;
            pieces.append(piece);
        }
        if (used < values->elements.size())
            return;
        auto* joined = make<JoinedStr>(node);
        joined->values = m_arena.copy(pieces);
        expression = joined;
    }

    // simple_format_arg_parse() and parse_format(). Null if there is more to it than can be dealt with.
    Expression* parseFormat(StringView text, unsigned& position, Expression& value)
    {
        static constexpr unsigned maximumDigits = 3;
        char16_t c = 0;
        auto advance = [&] {
            if (position >= text.length())
                return false;
            c = text[position++];
            return true;
        };
        bool isToTheLeft = false;
        while (true) {
            if (!advance())
                return nullptr;
            if (c == '-')
                isToTheLeft = true;
            else if (c != '+' && c != ' ' && c != '#' && c != '0')
                break;
        }
        auto number = [&] (int& result) {
            result = 0;
            unsigned digits = 0;
            while (isASCIIDigit(c)) {
                result = result * 10 + (c - '0');
                if (!advance() || ++digits >= maximumDigits)
                    return false;
            }
            return true;
        };
        int width = -1;
        int precision = -1;
        if (isASCIIDigit(c) && !number(width))
            return nullptr;
        if (c == '.') {
            if (!advance() || !number(precision))
                return nullptr;
        }
        if (c != 's' && c != 'r' && c != 'a')
            return nullptr;

        StringBuilder specification;
        if (!isToTheLeft && width > 0)
            specification.append('>');
        if (width >= 0)
            specification.append(width);
        if (precision >= 0)
            specification.append('.', precision);
        auto* formatted = make<FormattedValue>(value);
        formatted->value = &value;
        formatted->conversion = c;
        if (!specification.isEmpty())
            formatted->formatSpecification = makeString(specification.toString());
        return formatted;
    }

    // ---- Patterns

    // astfold_pattern()
    void fold(Pattern& pattern)
    {
        if (!enter())
            return;
        switch (pattern.kind) {
        case Pattern::Kind::MatchValue:
            return foldNumber(pattern.as<MatchValue>().value);
        case Pattern::Kind::MatchSequence:
            return fold(pattern.as<MatchSequence>().patterns);
        case Pattern::Kind::MatchMapping:
            for (Expression*& key : pattern.as<MatchMapping>().keys)
                foldNumber(key);
            return fold(pattern.as<MatchMapping>().patterns);
        case Pattern::Kind::MatchClass:
            fold(pattern.as<MatchClass>().cls);
            fold(pattern.as<MatchClass>().patterns);
            return fold(pattern.as<MatchClass>().keywordPatterns);
        case Pattern::Kind::MatchAs:
            if (pattern.as<MatchAs>().pattern)
                fold(*pattern.as<MatchAs>().pattern);
            return;
        case Pattern::Kind::MatchOr:
            return fold(pattern.as<MatchOr>().patterns);
        case Pattern::Kind::MatchSingleton:
        case Pattern::Kind::MatchStar:
            return;
        }
    }

    void fold(Sequence<Pattern*> patterns)
    {
        for (Pattern* pattern : patterns)
            fold(*pattern);
    }

    static bool isReal(const Constant& constant) { return constant.type == Constant::Type::Integer || constant.type == Constant::Type::BigInteger || constant.type == Constant::Type::Float; }
    static bool isComplex(const Constant& constant) { return constant.type == Constant::Type::Imaginary || constant.type == Constant::Type::Complex; }

    // Nothing if it is too large an int to be a float.
    static std::optional<double> toDouble(const Constant& constant)
    {
        double result;
        switch (constant.type) {
        case Constant::Type::Integer:
            result = static_cast<double>(constant.integer);
            break;
        case Constant::Type::BigInteger:
            result = toDecimal(constant.text->string(), constant.radix).toDouble();
            if (!std::isfinite(result))
                return std::nullopt;
            break;
        default:
            return constant.real;
        }
        return constant.isNegative ? -result : result;
    }

    // fold_const_match_patterns(): -1, and 1+2j
    void foldNumber(Expression*& expression)
    {
        if (m_onlyLookedOver)
            return;
        if (auto* unary = expression->tryAs<UnaryOp>()) {
            auto* operand = unary->operand->tryAs<Constant>();
            if (unary->op != UnaryOperator::USub || !operand || (!isReal(*operand) && !isComplex(*operand)))
                return;
            auto* result = make<Constant>(*unary);
            result->type = operand->type;
            result->radix = operand->radix;
            result->text = operand->text;
            switch (operand->type) {
            case Constant::Type::Integer:
                result->integer = operand->integer;
                result->isNegative = !operand->isNegative && operand->integer;
                break;
            case Constant::Type::BigInteger:
                result->isNegative = !operand->isNegative;
                break;
            case Constant::Type::Float:
                result->real = -operand->real;
                break;
            case Constant::Type::Imaginary:
                // It has a real part, which is nought, and that is negated too.
                result->type = Constant::Type::Complex;
                result->real = -0.0;
                result->imaginary = -operand->real;
                break;
            default:
                result->real = -operand->real;
                result->imaginary = -operand->imaginary;
                break;
            }
            expression = result;
            return;
        }
        auto* binary = expression->tryAs<BinOp>();
        if (!binary || (binary->op != BinaryOperator::Add && binary->op != BinaryOperator::Sub) || !binary->right->is<Constant>())
            return;
        foldNumber(binary->left);
        auto* left = binary->left->tryAs<Constant>();
        auto& right = binary->right->as<Constant>();
        if (!left || !isReal(*left) || !isComplex(right))
            return;
        auto real = toDouble(*left);
        if (!real)
            return;
        double rightReal = right.type == Constant::Type::Complex ? right.real : 0;
        double rightImaginary = right.type == Constant::Type::Complex ? right.imaginary : right.real;
        auto* result = make<Constant>(*binary);
        result->type = Constant::Type::Complex;
        result->real = binary->op == BinaryOperator::Add ? *real + rightReal : *real - rightReal;
        result->imaginary = binary->op == BinaryOperator::Add ? rightImaginary : -rightImaginary;
        expression = result;
    }

    VM& m_vm;
    Arena& m_arena;
    unsigned m_optimizationLevel;
    bool m_annotationsAreStrings;
    bool m_onlyLookedOver;
    bool m_isTooDeep { false };
};

} // anonymous namespace

bool optimize(VM& vm, Arena& arena, Module& module, unsigned optimizationLevel, unsigned futureFeatures, bool onlyLookedOver)
{
    return Optimizer(vm, arena, optimizationLevel, futureFeatures, onlyLookedOver).run(module);
}

} } // namespace JSC::Python
