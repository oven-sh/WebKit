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
#include "PythonUnparse.h"

#include "PythonBytes.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

String toDecimal(StringView digits, unsigned radix)
{
    constexpr uint32_t limbBase = 1000000000;
    Vector<uint32_t, 16> limbs; // The least significant first.
    for (char16_t c : digits.codeUnits()) {
        uint64_t carry = isASCIIDigit(c) ? c - '0' : toASCIILower(c) - 'a' + 10;
        for (auto& limb : limbs) {
            uint64_t value = static_cast<uint64_t>(limb) * radix + carry;
            limb = static_cast<uint32_t>(value % limbBase);
            carry = value / limbBase;
        }
        if (carry)
            limbs.append(static_cast<uint32_t>(carry));
    }
    if (limbs.isEmpty())
        return "0"_s;
    StringBuilder result;
    result.append(limbs.last());
    for (size_t i = limbs.size() - 1; i--;) {
        String limb = String::number(limbs[i]);
        for (unsigned padding = limb.length(); padding < 9; ++padding)
            result.append('0');
        result.append(limb);
    }
    return result.toString();
}

namespace {

enum Priority : int {
    Tuple_,
    Test, // if-else, lambda
    Or,
    And,
    Not,
    Comparison,
    Expression_,
    BitOr = Expression_,
    BitXor,
    BitAnd,
    Shift,
    Arithmetic,
    Term,
    Factor,
    Power,
    Await_,
    Atom,
};

class Unparser {
public:
    const SyntaxError& whyNot() const { return m_whyNot; }

    String run(Expression& expression, int level)
    {
        append(expression, level);
        return m_whyNot ? String() : m_out.toString();
    }

private:
    template<typename... Arguments> void write(Arguments... arguments) { m_out.append(arguments...); }

    void appendAll(Sequence<Expression*> expressions, int level)
    {
        bool isFirst = true;
        for (Expression* expression : expressions) {
            if (!isFirst)
                write(", "_s);
            isFirst = false;
            append(*expression, level);
        }
    }

    // What evaluates to infinity, which has no way of being written.
    static String withoutInfinity(String&& text) { return makeStringByReplacingAll(text, "inf"_s, "1e309"_s); }

    void append(Constant& node)
    {
        switch (node.type) {
        case Constant::Type::None:
            return write("None"_s);
        case Constant::Type::True:
            return write("True"_s);
        case Constant::Type::False:
            return write("False"_s);
        case Constant::Type::Ellipsis:
            return write("..."_s);
        case Constant::Type::Integer:
            return write(node.isNegative && node.integer ? "-"_s : ""_s, node.integer);
        case Constant::Type::BigInteger:
            return write(node.isNegative ? "-"_s : ""_s, toDecimal(node.text->string(), node.radix));
        case Constant::Type::Float:
            return write(withoutInfinity(reprOfDouble(node.real)));
        case Constant::Type::Imaginary: {
            String imaginary = reprOfDouble(node.real);
            if (imaginary.endsWith(".0"_s))
                imaginary = imaginary.left(imaginary.length() - 2);
            return write(withoutInfinity(WTF::move(imaginary)), 'j');
        }
        case Constant::Type::String:
            if (node.hasUnicodePrefix)
                write('u');
            return write(reprOfString(node.text->string()));
        case Constant::Type::Bytes: {
            Vector<uint8_t> bytes;
            for (char16_t c : StringView(node.text->string()).codeUnits())
                bytes.append(static_cast<uint8_t>(c));
            return write(reprOfBytes(bytes.span()));
        }
        case Constant::Type::Complex:
            return write(withoutInfinity(reprOfComplex(node.real, node.imaginary)));
        case Constant::Type::Tuple:
            write('(');
            for (size_t i = 0; i < node.elements.size(); ++i) {
                if (i)
                    write(", "_s);
                append(*node.elements[i]);
            }
            return write(node.elements.size() == 1 ? ",)"_s : ")"_s);
        case Constant::Type::FrozenSet:
            // As repr() has it.
            if (node.elements.empty())
                return write("frozenset()"_s);
            write("frozenset({"_s);
            for (size_t i = 0; i < node.elements.size(); ++i) {
                if (i)
                    write(", "_s);
                append(*node.elements[i]);
            }
            return write("})"_s);
        case Constant::Type::Invalid:
            return;
        }
    }

    void append(Argument& argument)
    {
        write(argument.name->string());
        if (argument.annotation) {
            write(": "_s);
            append(*argument.annotation, Test);
        }
    }

    void append(Arguments& arguments)
    {
        bool isFirst = true;
        auto separate = [&] {
            if (!isFirst)
                write(", "_s);
            isFirst = false;
        };
        size_t positionalOnlyCount = arguments.positionalOnly.size();
        size_t count = positionalOnlyCount + arguments.positional.size();
        for (size_t i = 0; i < count; ++i) {
            separate();
            append(i < positionalOnlyCount ? *arguments.positionalOnly[i] : *arguments.positional[i - positionalOnlyCount]);
            if (i + arguments.defaults.size() >= count) {
                write('=');
                append(*arguments.defaults[i + arguments.defaults.size() - count], Test);
            }
            if (positionalOnlyCount && i + 1 == positionalOnlyCount)
                write(", /"_s);
        }
        if (arguments.variadic || !arguments.keywordOnly.empty()) {
            separate();
            write('*');
            if (arguments.variadic)
                append(*arguments.variadic);
        }
        for (size_t i = 0; i < arguments.keywordOnly.size(); ++i) {
            separate();
            append(*arguments.keywordOnly[i]);
            if (arguments.keywordDefaults[i]) {
                write('=');
                append(*arguments.keywordDefaults[i], Test);
            }
        }
        if (arguments.keywordVariadic) {
            separate();
            write("**"_s);
            append(*arguments.keywordVariadic);
        }
    }

    void append(Sequence<Comprehension*> generators)
    {
        for (Comprehension* generator : generators) {
            write(generator->isAsync ? " async for "_s : " for "_s);
            append(*generator->target, Tuple_);
            write(" in "_s);
            append(*generator->iterable, Test + 1);
            for (Expression* condition : generator->conditions) {
                write(" if "_s);
                append(*condition, Test + 1);
            }
        }
    }

    void appendGeneratorExpression(GeneratorExp& node)
    {
        write('(');
        append(*node.element, Test);
        append(node.generators);
        write(')');
    }

    // ---- f"..." and t"..."

    // All that is between the quotes is put together first, so as to be written with one pair of them.
    String bodyOf(Sequence<Expression*> values, bool isFormatSpecification)
    {
        Unparser body;
        for (Expression* value : values)
            body.appendElement(*value, isFormatSpecification);
        fail(body.m_whyNot);
        return body.m_out.toString();
    }

    void appendElement(Expression& element, bool isFormatSpecification)
    {
        switch (element.kind) {
        case Expression::Kind::Constant:
            if (element.as<Constant>().type != Constant::Type::String)
                return fail(SyntaxError::Kind::TypeError, makeString("must be str, not "_s, typeNameOf(element.as<Constant>())));
            return write(makeStringByReplacingAll(makeStringByReplacingAll(element.as<Constant>().text->string(), '{', "{{"_s), '}', "}}"_s));
        case Expression::Kind::JoinedStr:
            return appendJoined(element.as<JoinedStr>(), isFormatSpecification);
        case Expression::Kind::TemplateStr:
            return write('t', reprOfString(bodyOf(element.as<TemplateStr>().values, false)));
        case Expression::Kind::FormattedValue:
            return appendFormatted(element.as<FormattedValue>());
        case Expression::Kind::Interpolation:
            return appendInterpolation(element.as<Interpolation>());
        default:
            return fail(SyntaxError::Kind::SystemError, "unknown expression kind inside f-string or t-string"_s);
        }
    }

    void appendJoined(JoinedStr& node, bool isFormatSpecification)
    {
        String body = bodyOf(node.values, isFormatSpecification);
        if (isFormatSpecification)
            return write(body);
        write('f', reprOfString(body));
    }

    void appendReplacementField(const String& source, int conversion, Expression* formatSpecification)
    {
        // What begins with a brace is kept apart from the one that it is in.
        write(source.startsWith('{') ? "{ "_s : "{"_s, source);
        if (conversion >= 0 && conversion != 'a' && conversion != 'r' && conversion != 's')
            fail(SyntaxError::Kind::SystemError, "unknown f-value conversion kind"_s);
        else if (conversion >= 0)
            write('!', static_cast<char>(conversion));
        if (formatSpecification) {
            write(':');
            appendElement(*formatSpecification, true);
        }
        write('}');
    }

    void appendFormatted(FormattedValue& node)
    {
        // A lambda has a colon in it, and so has to be in parentheses.
        Unparser value;
        String source = value.run(*node.value, Test + 1);
        fail(value.m_whyNot);
        appendReplacementField(source, node.conversion, node.formatSpecification);
    }

    void appendInterpolation(Interpolation& node)
    {
        appendReplacementField(node.source->type == Constant::Type::String ? node.source->text->string() : emptyString(), node.conversion, node.formatSpecification);
    }

    // ---- Expressions

    void append(Expression& expression, int level)
    {
        auto inParentheses = [&] (int priority, auto&& body) {
            if (level > priority)
                write('(');
            body();
            if (level > priority)
                write(')');
        };
        switch (expression.kind) {
        case Expression::Kind::BoolOp: {
            auto& node = expression.as<BoolOp>();
            bool isAnd = node.op == BooleanOperator::And;
            int priority = isAnd ? And : Or;
            return inParentheses(priority, [&] {
                bool isFirst = true;
                for (Expression* value : node.values) {
                    if (!isFirst)
                        write(isAnd ? " and "_s : " or "_s);
                    isFirst = false;
                    append(*value, priority + 1);
                }
            });
        }
        case Expression::Kind::BinOp: {
            auto& node = expression.as<BinOp>();
            ASCIILiteral op;
            int priority;
            bool isRightAssociative = false;
            switch (node.op) {
            case BinaryOperator::Add: op = " + "_s; priority = Arithmetic; break;
            case BinaryOperator::Sub: op = " - "_s; priority = Arithmetic; break;
            case BinaryOperator::Mult: op = " * "_s; priority = Term; break;
            case BinaryOperator::MatMult: op = " @ "_s; priority = Term; break;
            case BinaryOperator::Div: op = " / "_s; priority = Term; break;
            case BinaryOperator::Mod: op = " % "_s; priority = Term; break;
            case BinaryOperator::LShift: op = " << "_s; priority = Shift; break;
            case BinaryOperator::RShift: op = " >> "_s; priority = Shift; break;
            case BinaryOperator::BitOr: op = " | "_s; priority = BitOr; break;
            case BinaryOperator::BitXor: op = " ^ "_s; priority = BitXor; break;
            case BinaryOperator::BitAnd: op = " & "_s; priority = BitAnd; break;
            case BinaryOperator::FloorDiv: op = " // "_s; priority = Term; break;
            case BinaryOperator::Pow: op = " ** "_s; priority = Power; isRightAssociative = true; break;
            }
            return inParentheses(priority, [&] {
                append(*node.left, priority + isRightAssociative);
                write(op);
                append(*node.right, priority + !isRightAssociative);
            });
        }
        case Expression::Kind::UnaryOp: {
            auto& node = expression.as<UnaryOp>();
            ASCIILiteral op;
            int priority = Factor;
            switch (node.op) {
            case UnaryOperator::Invert: op = "~"_s; break;
            case UnaryOperator::Not: op = "not "_s; priority = Not; break;
            case UnaryOperator::UAdd: op = "+"_s; break;
            case UnaryOperator::USub: op = "-"_s; break;
            }
            return inParentheses(priority, [&] {
                write(op);
                append(*node.operand, priority);
            });
        }
        case Expression::Kind::Lambda: {
            auto& node = expression.as<Lambda>();
            return inParentheses(Test, [&] {
                write(node.arguments->positional.size() + node.arguments->positionalOnly.size() ? "lambda "_s : "lambda"_s);
                append(*node.arguments);
                write(": "_s);
                append(*node.body, Test);
            });
        }
        case Expression::Kind::IfExp: {
            auto& node = expression.as<IfExp>();
            return inParentheses(Test, [&] {
                append(*node.body, Test + 1);
                write(" if "_s);
                append(*node.test, Test + 1);
                write(" else "_s);
                append(*node.orElse, Test);
            });
        }
        case Expression::Kind::Dict: {
            auto& node = expression.as<Dict>();
            write('{');
            for (size_t i = 0; i < node.values.size(); ++i) {
                if (i)
                    write(", "_s);
                if (node.keys[i]) {
                    append(*node.keys[i], Test);
                    write(": "_s);
                    append(*node.values[i], Test);
                } else {
                    write("**"_s);
                    append(*node.values[i], Expression_);
                }
            }
            return write('}');
        }
        case Expression::Kind::Set:
            write('{');
            appendAll(expression.as<Set>().elements, Test);
            return write('}');
        case Expression::Kind::GeneratorExp:
            return appendGeneratorExpression(expression.as<GeneratorExp>());
        case Expression::Kind::ListComp:
            write('[');
            append(*expression.as<ListComp>().element, Test);
            append(expression.as<ListComp>().generators);
            return write(']');
        case Expression::Kind::SetComp:
            write('{');
            append(*expression.as<SetComp>().element, Test);
            append(expression.as<SetComp>().generators);
            return write('}');
        case Expression::Kind::DictComp:
            write('{');
            append(*expression.as<DictComp>().key, Test);
            write(": "_s);
            append(*expression.as<DictComp>().value, Test);
            append(expression.as<DictComp>().generators);
            return write('}');
        case Expression::Kind::Yield:
            if (!expression.as<Yield>().value)
                return write("(yield)"_s);
            write("(yield "_s);
            append(*expression.as<Yield>().value, Test);
            return write(')');
        case Expression::Kind::YieldFrom:
            write("(yield from "_s);
            append(*expression.as<YieldFrom>().value, Test);
            return write(')');
        case Expression::Kind::Await:
            return inParentheses(Await_, [&] {
                write("await "_s);
                append(*expression.as<Await>().value, Atom);
            });
        case Expression::Kind::Compare: {
            auto& node = expression.as<Compare>();
            return inParentheses(Comparison, [&] {
                append(*node.left, Comparison + 1);
                for (size_t i = 0; i < node.comparators.size(); ++i) {
                    switch (node.ops[i]) {
                    case ComparisonOperator::Eq: write(" == "_s); break;
                    case ComparisonOperator::NotEq: write(" != "_s); break;
                    case ComparisonOperator::Lt: write(" < "_s); break;
                    case ComparisonOperator::LtE: write(" <= "_s); break;
                    case ComparisonOperator::Gt: write(" > "_s); break;
                    case ComparisonOperator::GtE: write(" >= "_s); break;
                    case ComparisonOperator::Is: write(" is "_s); break;
                    case ComparisonOperator::IsNot: write(" is not "_s); break;
                    case ComparisonOperator::In: write(" in "_s); break;
                    case ComparisonOperator::NotIn: write(" not in "_s); break;
                    }
                    append(*node.comparators[i], Comparison + 1);
                }
            });
        }
        case Expression::Kind::Call: {
            auto& node = expression.as<Call>();
            append(*node.function, Atom);
            // f(x for x in y)
            if (node.arguments.size() == 1 && node.keywords.empty() && node.arguments[0]->is<GeneratorExp>())
                return appendGeneratorExpression(node.arguments[0]->as<GeneratorExp>());
            write('(');
            appendAll(node.arguments, Test);
            bool isFirst = node.arguments.empty();
            for (Keyword* keyword : node.keywords) {
                if (!isFirst)
                    write(", "_s);
                isFirst = false;
                if (keyword->name)
                    write(keyword->name->string(), '=');
                else
                    write("**"_s);
                append(*keyword->value, Test);
            }
            return write(')');
        }
        case Expression::Kind::Constant:
            return append(expression.as<Constant>());
        case Expression::Kind::JoinedStr:
            return appendJoined(expression.as<JoinedStr>(), false);
        case Expression::Kind::TemplateStr:
            return write('t', reprOfString(bodyOf(expression.as<TemplateStr>().values, false)));
        case Expression::Kind::FormattedValue:
            return appendFormatted(expression.as<FormattedValue>());
        case Expression::Kind::Interpolation:
            return appendInterpolation(expression.as<Interpolation>());
        case Expression::Kind::Attribute: {
            auto& node = expression.as<Attribute>();
            append(*node.value, Atom);
            // 1 .real, since 1.real is something else.
            auto* constant = node.value->tryAs<Constant>();
            bool isInteger = constant && (constant->type == Constant::Type::Integer || constant->type == Constant::Type::BigInteger);
            return write(isInteger ? " ."_s : "."_s, node.attribute->string());
        }
        case Expression::Kind::Subscript:
            append(*expression.as<Subscript>().value, Atom);
            write('[');
            append(*expression.as<Subscript>().slice, Tuple_);
            return write(']');
        case Expression::Kind::Starred:
            write('*');
            return append(*expression.as<Starred>().value, Expression_);
        case Expression::Kind::Slice: {
            auto& node = expression.as<Slice>();
            if (node.lower)
                append(*node.lower, Test);
            write(':');
            if (node.upper)
                append(*node.upper, Test);
            if (node.step) {
                write(':');
                append(*node.step, Test);
            }
            return;
        }
        case Expression::Kind::Name:
            return write(expression.as<Name>().id->string());
        case Expression::Kind::List:
            write('[');
            appendAll(expression.as<List>().elements, Test);
            return write(']');
        case Expression::Kind::Tuple: {
            auto& node = expression.as<Tuple>();
            if (node.elements.empty())
                return write("()"_s);
            return inParentheses(Tuple_, [&] {
                appendAll(node.elements, Test);
                if (node.elements.size() == 1)
                    write(',');
            });
        }
        case Expression::Kind::NamedExpr:
            return inParentheses(Tuple_, [&] {
                append(*expression.as<NamedExpr>().target, Atom);
                write(" := "_s);
                append(*expression.as<NamedExpr>().value, Atom);
            });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Only a tree that a program made can have what could not have been written.
    void fail(SyntaxError::Kind kind, String&& message)
    {
        if (m_whyNot)
            return;
        m_whyNot.kind = kind;
        m_whyNot.message = WTF::move(message);
    }

    // What went wrong with a part of it went wrong with it.
    void fail(const SyntaxError& whyNot)
    {
        if (whyNot && !m_whyNot)
            m_whyNot = whyNot;
    }

    static ASCIILiteral typeNameOf(const Constant& constant)
    {
        switch (constant.type) {
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
    }

    StringBuilder m_out;
    SyntaxError m_whyNot;
};

} // namespace

String unparse(Expression& expression, SyntaxError& whyNot)
{
    Unparser unparser;
    String result = unparser.run(expression, Test);
    whyNot = unparser.whyNot();
    return result;
}

} } // namespace JSC::Python
