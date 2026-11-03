/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCompilerUnit.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyStr.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

// An expression as text, for `from __future__ import annotations`: not as it was written, but as the tree says, with the
// spacing, the quotes and no more parentheses than Python/ast_unparse.c in CPython gives it, because programs compare these.

namespace {

// How tightly an expression binds. What binds less than its place asks for goes in parentheses.
enum Precedence : int {
    TupleLevel,
    TestLevel, // if-else, lambda
    OrLevel,
    AndLevel,
    NotLevel,
    CompareLevel,
    BitOrLevel,
    BitXorLevel,
    BitAndLevel,
    ShiftLevel,
    ArithmeticLevel,
    TermLevel,
    FactorLevel,
    PowerLevel,
    AwaitLevel,
    AtomLevel,
};

class Unparser {
public:
    using ReprFunction = Function<String(const ConstantValue&)>;

    explicit Unparser(const ReprFunction& repr)
        : m_repr(repr)
    {
    }

    String unparse(Expr* node, int level)
    {
        Unparser nested(m_repr);
        nested.append(node, level);
        return nested.m_text.toString();
    }

private:
    template<typename Function>
    void parenthesizedIf(bool condition, const Function& function)
    {
        if (condition)
            m_text.append('(');
        function();
        if (condition)
            m_text.append(')');
    }

    void append(Name name) { m_text.append(name->string()); }

    void appendList(Seq<Expr*> items, int level = TestLevel)
    {
        bool first = true;
        for (Expr* item : items) {
            if (!std::exchange(first, false))
                m_text.append(", "_s);
            append(item, level);
        }
    }

    void appendRepr(const ConstantValue& value)
    {
        String text = m_repr(value);
        // There is no literal for infinity, but one that is too large for a float.
        if (value.kind == ConstantValue::Kind::Complex || (value.kind == ConstantValue::Kind::Float && std::isinf(value.number)))
            text = makeStringByReplacingAll(text, "inf"_s, "1e309"_s);
        m_text.append(text);
    }

    void appendConstant(const ConstantValue& value)
    {
        if (value.kind != ConstantValue::Kind::Tuple)
            return appendRepr(value);
        m_text.append('(');
        for (size_t i = 0; i < value.items.size; ++i) {
            if (i)
                m_text.append(", "_s);
            appendConstant(value.items.data[i]);
        }
        if (value.items.size == 1)
            m_text.append(',');
        m_text.append(')');
    }

    void append(Arg* parameter)
    {
        append(parameter->arg);
        if (parameter->annotation) {
            m_text.append(": "_s);
            append(parameter->annotation, TestLevel);
        }
    }

    void append(AST::Arguments* parameters)
    {
        bool first = true;
        auto separate = [&] {
            if (!std::exchange(first, false))
                m_text.append(", "_s);
        };
        size_t positionalOnly = parameters->posonlyargs.size();
        size_t positional = positionalOnly + parameters->args.size();
        for (size_t i = 0; i < positional; ++i) {
            separate();
            append(i < positionalOnly ? parameters->posonlyargs[i] : parameters->args[i - positionalOnly]);
            if (i + parameters->defaults.size() >= positional) {
                m_text.append('=');
                append(parameters->defaults[i + parameters->defaults.size() - positional], TestLevel);
            }
            if (i + 1 == positionalOnly)
                m_text.append(", /"_s);
        }
        if (parameters->vararg || parameters->kwonlyargs.size()) {
            separate();
            m_text.append('*');
            if (parameters->vararg)
                append(parameters->vararg);
        }
        for (size_t i = 0; i < parameters->kwonlyargs.size(); ++i) {
            separate();
            append(parameters->kwonlyargs[i]);
            if (Expr* value = parameters->kwDefaults[i]) {
                m_text.append('=');
                append(value, TestLevel);
            }
        }
        if (parameters->kwarg) {
            separate();
            m_text.append("**"_s);
            append(parameters->kwarg);
        }
    }

    void append(Seq<Comprehension*> generators)
    {
        for (Comprehension* generator : generators) {
            m_text.append(generator->isAsync ? " async for "_s : " for "_s);
            append(generator->target, TupleLevel);
            m_text.append(" in "_s);
            append(generator->iter, TestLevel + 1);
            for (Expr* condition : generator->ifs) {
                m_text.append(" if "_s);
                append(condition, TestLevel + 1);
            }
        }
    }

    void appendComprehension(char open, Expr* element, Seq<Comprehension*> generators, char close)
    {
        m_text.append(open);
        append(element, TestLevel);
        append(generators);
        m_text.append(close);
    }

    // ---- f-strings and t-strings ----

    // What is between the quotes, before it is quoted.
    String bodyOf(Seq<Expr*> values, bool isFormatSpec)
    {
        Unparser nested(m_repr);
        for (Expr* value : values)
            nested.appendStringElement(value, isFormatSpec);
        return nested.m_text.toString();
    }

    void appendStringElement(Expr* node, bool isFormatSpec)
    {
        switch (node->kind) {
        case ExprKind::Constant: {
            String text(node->as<Constant>()->value.string);
            m_text.append(makeStringByReplacingAll(makeStringByReplacingAll(text, '{', "{{"_s), '}', "}}"_s));
            return;
        }
        case ExprKind::JoinedStr:
            return appendJoinedStr(node->as<JoinedStr>(), isFormatSpec);
        default:
            return append(node, TestLevel);
        }
    }

    void appendJoinedStr(JoinedStr* node, bool isFormatSpec)
    {
        String body = bodyOf(node->values, isFormatSpec);
        if (isFormatSpec)
            m_text.append(body);
        else
            m_text.append('f', strRepr(body));
    }

    void appendReplacementField(const String& expression, int conversion, Expr* formatSpec)
    {
        // "{{" would be a brace.
        m_text.append(expression.startsWith('{') ? "{ "_s : "{"_s, expression);
        if (conversion >= 0)
            m_text.append('!', static_cast<char>(conversion));
        if (formatSpec) {
            m_text.append(':');
            appendStringElement(formatSpec, true);
        }
        m_text.append('}');
    }

    // ---- Expressions ----

    void append(Expr* node, int level)
    {
        switch (node->kind) {
        case ExprKind::BoolOp: {
            auto* operation = node->as<BoolOp>();
            bool isAnd = operation->op == BoolOperator::And;
            int own = isAnd ? AndLevel : OrLevel;
            return parenthesizedIf(level > own, [&] {
                bool first = true;
                for (Expr* value : operation->values) {
                    if (!std::exchange(first, false))
                        m_text.append(isAnd ? " and "_s : " or "_s);
                    append(value, own + 1);
                }
            });
        }
        case ExprKind::NamedExpr:
            return parenthesizedIf(level > TupleLevel, [&] {
                append(node->as<NamedExpr>()->target, AtomLevel);
                m_text.append(" := "_s);
                append(node->as<NamedExpr>()->value, AtomLevel);
            });
        case ExprKind::BinOp: {
            auto* operation = node->as<BinOp>();
            static constexpr struct {
                ASCIILiteral text;
                int level;
            } operators[] = {
                { " + "_s, ArithmeticLevel }, { " - "_s, ArithmeticLevel }, { " * "_s, TermLevel }, { " @ "_s, TermLevel }, { " / "_s, TermLevel },
                { " % "_s, TermLevel }, { " ** "_s, PowerLevel }, { " << "_s, ShiftLevel }, { " >> "_s, ShiftLevel }, { " | "_s, BitOrLevel },
                { " ^ "_s, BitXorLevel }, { " & "_s, BitAndLevel }, { " // "_s, TermLevel },
            };
            auto& entry = operators[static_cast<unsigned>(operation->op)];
            bool isRightAssociative = operation->op == Operator::Pow;
            return parenthesizedIf(level > entry.level, [&] {
                append(operation->left, entry.level + isRightAssociative);
                m_text.append(entry.text);
                append(operation->right, entry.level + !isRightAssociative);
            });
        }
        case ExprKind::UnaryOp: {
            auto* operation = node->as<UnaryOp>();
            static constexpr ASCIILiteral operators[] = { "~"_s, "not "_s, "+"_s, "-"_s };
            int own = operation->op == UnaryKind::Not ? NotLevel : FactorLevel;
            return parenthesizedIf(level > own, [&] {
                m_text.append(operators[static_cast<unsigned>(operation->op)]);
                append(operation->operand, own);
            });
        }
        case ExprKind::Lambda: {
            auto* lambda = node->as<Lambda>();
            return parenthesizedIf(level > TestLevel, [&] {
                m_text.append(lambda->args->args.size() + lambda->args->posonlyargs.size() ? "lambda "_s : "lambda"_s);
                append(lambda->args);
                m_text.append(": "_s);
                append(lambda->body, TestLevel);
            });
        }
        case ExprKind::IfExp: {
            auto* conditional = node->as<IfExp>();
            return parenthesizedIf(level > TestLevel, [&] {
                append(conditional->body, TestLevel + 1);
                m_text.append(" if "_s);
                append(conditional->test, TestLevel + 1);
                m_text.append(" else "_s);
                append(conditional->orelse, TestLevel);
            });
        }
        case ExprKind::Dict: {
            auto* dict = node->as<Dict>();
            m_text.append('{');
            for (size_t i = 0; i < dict->values.size(); ++i) {
                if (i)
                    m_text.append(", "_s);
                if (Expr* key = dict->keys[i]) {
                    append(key, TestLevel);
                    m_text.append(": "_s);
                    append(dict->values[i], TestLevel);
                } else {
                    m_text.append("**"_s);
                    append(dict->values[i], BitOrLevel);
                }
            }
            m_text.append('}');
            return;
        }
        case ExprKind::Set:
            m_text.append('{');
            appendList(node->as<Set>()->elts);
            m_text.append('}');
            return;
        case ExprKind::List:
            m_text.append('[');
            appendList(node->as<List>()->elts);
            m_text.append(']');
            return;
        case ExprKind::Tuple: {
            auto elements = node->as<Tuple>()->elts;
            if (!elements.size()) {
                m_text.append("()"_s);
                return;
            }
            return parenthesizedIf(level > TupleLevel, [&] {
                appendList(elements);
                if (elements.size() == 1)
                    m_text.append(',');
            });
        }
        case ExprKind::ListComp:
            return appendComprehension('[', node->as<ListComp>()->elt, node->as<ListComp>()->generators, ']');
        case ExprKind::SetComp:
            return appendComprehension('{', node->as<SetComp>()->elt, node->as<SetComp>()->generators, '}');
        case ExprKind::GeneratorExp:
            return appendComprehension('(', node->as<GeneratorExp>()->elt, node->as<GeneratorExp>()->generators, ')');
        case ExprKind::DictComp: {
            auto* comprehension = node->as<DictComp>();
            m_text.append('{');
            if (comprehension->value) {
                append(comprehension->key, TestLevel);
                m_text.append(": "_s);
                append(comprehension->value, TestLevel);
            } else {
                m_text.append("**"_s);
                append(comprehension->key, TestLevel);
            }
            append(comprehension->generators);
            m_text.append('}');
            return;
        }
        case ExprKind::Await:
            return parenthesizedIf(level > AwaitLevel, [&] {
                m_text.append("await "_s);
                append(node->as<Await>()->value, AtomLevel);
            });
        case ExprKind::Yield:
            if (Expr* value = node->as<Yield>()->value) {
                m_text.append("(yield "_s);
                append(value, TestLevel);
                m_text.append(')');
            } else
                m_text.append("(yield)"_s);
            return;
        case ExprKind::YieldFrom:
            m_text.append("(yield from "_s);
            append(node->as<YieldFrom>()->value, TestLevel);
            m_text.append(')');
            return;
        case ExprKind::Compare: {
            auto* comparison = node->as<Compare>();
            static constexpr ASCIILiteral operators[] = { " == "_s, " != "_s, " < "_s, " <= "_s, " > "_s, " >= "_s, " is "_s, " is not "_s, " in "_s, " not in "_s };
            return parenthesizedIf(level > CompareLevel, [&] {
                append(comparison->left, CompareLevel + 1);
                for (size_t i = 0; i < comparison->ops.size(); ++i) {
                    m_text.append(operators[static_cast<unsigned>(comparison->ops[i])]);
                    append(comparison->comparators[i], CompareLevel + 1);
                }
            });
        }
        case ExprKind::Call: {
            auto* call = node->as<Call>();
            append(call->func, AtomLevel);
            // A lone generator expression needs no parentheses of its own.
            if (call->args.size() == 1 && !call->keywords.size() && call->args[0]->is<GeneratorExp>())
                return append(call->args[0], TestLevel);
            m_text.append('(');
            appendList(call->args);
            bool first = !call->args.size();
            for (Keyword* keyword : call->keywords) {
                if (!std::exchange(first, false))
                    m_text.append(", "_s);
                if (keyword->arg) {
                    append(keyword->arg);
                    m_text.append('=');
                } else
                    m_text.append("**"_s);
                append(keyword->value, TestLevel);
            }
            m_text.append(')');
            return;
        }
        case ExprKind::FormattedValue: {
            auto* field = node->as<FormattedValue>();
            // More than the grammar asks for, so that the ':' of a lambda is not taken for that of a format.
            return appendReplacementField(unparse(field->value, TestLevel + 1), field->conversion, field->formatSpec);
        }
        case ExprKind::Interpolation: {
            auto* field = node->as<Interpolation>();
            return appendReplacementField(String(field->str.string), field->conversion, field->formatSpec);
        }
        case ExprKind::JoinedStr:
            return appendJoinedStr(node->as<JoinedStr>(), false);
        case ExprKind::TemplateStr:
            m_text.append('t', strRepr(bodyOf(node->as<TemplateStr>()->values, false)));
            return;
        case ExprKind::Constant: {
            auto* constant = node->as<Constant>();
            if (constant->value.kind == ConstantValue::Kind::Ellipsis) {
                m_text.append("..."_s);
                return;
            }
            if (constant->kindIsU)
                m_text.append('u');
            return appendConstant(constant->value);
        }
        case ExprKind::Attribute: {
            auto* attribute = node->as<Attribute>();
            append(attribute->value, AtomLevel);
            // "1.real" would be a float followed by a name.
            auto* constant = attribute->value->tryAs<Constant>();
            bool isInt = constant && (constant->value.kind == ConstantValue::Kind::Int || constant->value.kind == ConstantValue::Kind::BigInt);
            m_text.append(isInt ? " ."_s : "."_s);
            append(attribute->attr);
            return;
        }
        case ExprKind::Subscript:
            append(node->as<Subscript>()->value, AtomLevel);
            m_text.append('[');
            append(node->as<Subscript>()->slice, TupleLevel);
            m_text.append(']');
            return;
        case ExprKind::Starred:
            m_text.append('*');
            append(node->as<Starred>()->value, BitOrLevel);
            return;
        case ExprKind::Name:
            return append(node->as<NameExpr>()->id);
        case ExprKind::Slice: {
            auto* slice = node->as<Slice>();
            if (slice->lower)
                append(slice->lower, TestLevel);
            m_text.append(':');
            if (slice->upper)
                append(slice->upper, TestLevel);
            if (slice->step) {
                m_text.append(':');
                append(slice->step, TestLevel);
            }
            return;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const ReprFunction& m_repr;
    StringBuilder m_text;
};

}

String Unit::annotationText(Expr* annotation)
{
    Unparser::ReprFunction repr = [&](const ConstantValue& value) {
        return reprForMessage(globalObject(), constantValue(value));
    };
    return Unparser(repr).unparse(annotation, TestLevel);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
