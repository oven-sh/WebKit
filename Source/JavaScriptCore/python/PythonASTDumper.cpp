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
#include "PythonASTDumper.h"

#include "JSBigInt.h"
#include "JSCInlines.h"
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace Python {

namespace {

class Dumper {
public:
    explicit Dumper(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }

    String dump(Module& module)
    {
        switch (module.kind) {
        case Module::Kind::Module:
            open("Module"_s);
            field("body"_s, module.body);
            name("type_ignores"_s);
            m_out.append("[]"_s);
            break;
        case Module::Kind::Interactive:
            open("Interactive"_s);
            field("body"_s, module.body);
            break;
        case Module::Kind::Expression:
            open("Expression"_s);
            field("body"_s, module.expression);
            break;
        }
        m_out.append('}');
        return m_out.toString();
    }

private:
    // ---- JSON

    void open(ASCIILiteral type)
    {
        m_out.append("{\"_\":\""_s, type, '"');
    }

    void name(ASCIILiteral name)
    {
        m_out.append(",\""_s, name, "\":"_s);
    }

    void close(const Node& node)
    {
        m_out.append(",\"@\":["_s, node.line, ',', node.column, ',', node.endLine, ',', node.endColumn, "]}"_s);
    }

    void close()
    {
        m_out.append('}');
    }

    void string(StringView view)
    {
        m_out.append('"');
        for (char16_t c : view.codeUnits()) {
            switch (c) {
            case '"':
                m_out.append("\\\""_s);
                break;
            case '\\':
                m_out.append("\\\\"_s);
                break;
            case '\n':
                m_out.append("\\n"_s);
                break;
            case '\r':
                m_out.append("\\r"_s);
                break;
            case '\t':
                m_out.append("\\t"_s);
                break;
            case '\f':
                m_out.append("\\f"_s);
                break;
            case '\b':
                m_out.append("\\b"_s);
                break;
            default:
                if (c >= ' ' && c <= '~')
                    m_out.append(static_cast<Latin1Character>(c));
                else
                    m_out.append("\\u"_s, hex(static_cast<unsigned>(c), 4, Lowercase));
                break;
            }
        }
        m_out.append('"');
    }

    void value(const Identifier* identifier)
    {
        if (!identifier) {
            m_out.append("null"_s);
            return;
        }
        string(identifier->string());
    }

    void value(int number) { m_out.append(number); }
    void value(unsigned number) { m_out.append(number); }
    void value(bool flag) { m_out.append(flag ? '1' : '0'); }

    void empty(ASCIILiteral type)
    {
        open(type);
        close();
    }

    template<typename T>
    void value(Sequence<T> sequence)
    {
        m_out.append('[');
        bool isFirst = true;
        for (auto& item : sequence) {
            if (!isFirst)
                m_out.append(',');
            isFirst = false;
            value(item);
        }
        m_out.append(']');
    }

    template<typename T>
    void field(ASCIILiteral fieldName, const T& fieldValue)
    {
        name(fieldName);
        value(fieldValue);
    }

    void nullField(ASCIILiteral fieldName)
    {
        name(fieldName);
        m_out.append("null"_s);
    }

    // ---- Enumerations

    void value(ExpressionContext context)
    {
        static constexpr ASCIILiteral names[] = { "Load"_s, "Store"_s, "Del"_s };
        empty(names[static_cast<unsigned>(context)]);
    }

    void value(BooleanOperator op)
    {
        static constexpr ASCIILiteral names[] = { "And"_s, "Or"_s };
        empty(names[static_cast<unsigned>(op)]);
    }

    void value(BinaryOperator op)
    {
        static constexpr ASCIILiteral names[] = { "Add"_s, "Sub"_s, "Mult"_s, "MatMult"_s, "Div"_s, "Mod"_s, "Pow"_s, "LShift"_s, "RShift"_s, "BitOr"_s, "BitXor"_s, "BitAnd"_s, "FloorDiv"_s };
        empty(names[static_cast<unsigned>(op)]);
    }

    void value(UnaryOperator op)
    {
        static constexpr ASCIILiteral names[] = { "Invert"_s, "Not"_s, "UAdd"_s, "USub"_s };
        empty(names[static_cast<unsigned>(op)]);
    }

    void value(ComparisonOperator op)
    {
        static constexpr ASCIILiteral names[] = { "Eq"_s, "NotEq"_s, "Lt"_s, "LtE"_s, "Gt"_s, "GtE"_s, "Is"_s, "IsNot"_s, "In"_s, "NotIn"_s };
        empty(names[static_cast<unsigned>(op)]);
    }

    // ---- Constants

    void simpleConstant(ASCIILiteral type)
    {
        m_out.append("{\"c\":\""_s, type, "\"}"_s);
    }

    void constant(Constant::Type type)
    {
        switch (type) {
        case Constant::Type::None:
            return simpleConstant("None"_s);
        case Constant::Type::True:
            return simpleConstant("True"_s);
        case Constant::Type::False:
            return simpleConstant("False"_s);
        case Constant::Type::Ellipsis:
            return simpleConstant("Ellipsis"_s);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void constant(Constant& node)
    {
        switch (node.type) {
        case Constant::Type::Integer:
            m_out.append("{\"c\":\"int\",\"v\":\""_s, hex(node.integer, Lowercase), "\"}"_s);
            return;
        case Constant::Type::BigInteger: {
            VM& vm = m_globalObject->vm();
            JSValue bigInt = JSBigInt::parseInt(m_globalObject, vm, node.text->string(), node.radix, JSBigInt::ErrorParseMode::IgnoreExceptions);
            RELEASE_ASSERT(bigInt && bigInt.isHeapBigInt());
            m_out.append("{\"c\":\"int\",\"v\":\""_s, bigInt.asHeapBigInt()->toString(m_globalObject, 16), "\"}"_s);
            return;
        }
        case Constant::Type::Float:
            m_out.append("{\"c\":\"float\",\"v\":\""_s, std::bit_cast<uint64_t>(node.real), "\"}"_s);
            return;
        case Constant::Type::Imaginary:
            m_out.append("{\"c\":\"complex\",\"v\":\""_s, std::bit_cast<uint64_t>(node.real), "\"}"_s);
            return;
        case Constant::Type::String:
        case Constant::Type::Bytes:
            m_out.append("{\"c\":\""_s, node.type == Constant::Type::String ? "str"_s : "bytes"_s, "\",\"v\":"_s);
            string(node.text->string());
            m_out.append('}');
            return;
        default:
            constant(node.type);
            return;
        }
    }

    // ---- What is neither a statement nor an expression

    void value(Argument* node)
    {
        if (!node) {
            m_out.append("null"_s);
            return;
        }
        open("arg"_s);
        field("arg"_s, node->name);
        field("annotation"_s, node->annotation);
        nullField("type_comment"_s);
        close(*node);
    }

    void value(Arguments* node)
    {
        open("arguments"_s);
        field("posonlyargs"_s, node->positionalOnly);
        field("args"_s, node->positional);
        field("vararg"_s, node->variadic);
        field("kwonlyargs"_s, node->keywordOnly);
        field("kw_defaults"_s, node->keywordDefaults);
        field("kwarg"_s, node->keywordVariadic);
        field("defaults"_s, node->defaults);
        close();
    }

    void value(Keyword* node)
    {
        open("keyword"_s);
        field("arg"_s, node->name);
        field("value"_s, node->value);
        close(*node);
    }

    void value(Alias* node)
    {
        open("alias"_s);
        field("name"_s, node->name);
        field("asname"_s, node->asName);
        close(*node);
    }

    void value(WithItem* node)
    {
        open("withitem"_s);
        field("context_expr"_s, node->contextExpression);
        field("optional_vars"_s, node->optionalVariables);
        close();
    }

    void value(Comprehension* node)
    {
        open("comprehension"_s);
        field("target"_s, node->target);
        field("iter"_s, node->iterable);
        field("ifs"_s, node->conditions);
        field("is_async"_s, node->isAsync);
        close();
    }

    void value(ExceptHandler* node)
    {
        open("ExceptHandler"_s);
        field("type"_s, node->type);
        field("name"_s, node->name);
        field("body"_s, node->body);
        close(*node);
    }

    void value(MatchCase* node)
    {
        open("match_case"_s);
        field("pattern"_s, node->pattern);
        field("guard"_s, node->guard);
        field("body"_s, node->body);
        close();
    }

    void value(TypeParameter* node)
    {
        switch (node->kind) {
        case TypeParameter::Kind::TypeVar:
            open("TypeVar"_s);
            field("name"_s, node->name);
            field("bound"_s, node->bound);
            break;
        case TypeParameter::Kind::ParamSpec:
            open("ParamSpec"_s);
            field("name"_s, node->name);
            break;
        case TypeParameter::Kind::TypeVarTuple:
            open("TypeVarTuple"_s);
            field("name"_s, node->name);
            break;
        }
        field("default_value"_s, node->defaultValue);
        close(*node);
    }

    // ---- Expressions

    void value(Expression* expression)
    {
        if (!expression) {
            m_out.append("null"_s);
            return;
        }
        switch (expression->kind) {
        case Expression::Kind::BoolOp: {
            auto& node = expression->as<BoolOp>();
            open("BoolOp"_s);
            field("op"_s, node.op);
            field("values"_s, node.values);
            break;
        }
        case Expression::Kind::NamedExpr: {
            auto& node = expression->as<NamedExpr>();
            open("NamedExpr"_s);
            field("target"_s, node.target);
            field("value"_s, node.value);
            break;
        }
        case Expression::Kind::BinOp: {
            auto& node = expression->as<BinOp>();
            open("BinOp"_s);
            field("left"_s, node.left);
            field("op"_s, node.op);
            field("right"_s, node.right);
            break;
        }
        case Expression::Kind::UnaryOp: {
            auto& node = expression->as<UnaryOp>();
            open("UnaryOp"_s);
            field("op"_s, node.op);
            field("operand"_s, node.operand);
            break;
        }
        case Expression::Kind::Lambda: {
            auto& node = expression->as<Lambda>();
            open("Lambda"_s);
            field("args"_s, node.arguments);
            field("body"_s, node.body);
            break;
        }
        case Expression::Kind::IfExp: {
            auto& node = expression->as<IfExp>();
            open("IfExp"_s);
            field("test"_s, node.test);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            break;
        }
        case Expression::Kind::Dict: {
            auto& node = expression->as<Dict>();
            open("Dict"_s);
            field("keys"_s, node.keys);
            field("values"_s, node.values);
            break;
        }
        case Expression::Kind::Set: {
            auto& node = expression->as<Set>();
            open("Set"_s);
            field("elts"_s, node.elements);
            break;
        }
        case Expression::Kind::ListComp: {
            auto& node = expression->as<ListComp>();
            open("ListComp"_s);
            field("elt"_s, node.element);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::SetComp: {
            auto& node = expression->as<SetComp>();
            open("SetComp"_s);
            field("elt"_s, node.element);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::DictComp: {
            auto& node = expression->as<DictComp>();
            open("DictComp"_s);
            field("key"_s, node.key);
            field("value"_s, node.value);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::GeneratorExp: {
            auto& node = expression->as<GeneratorExp>();
            open("GeneratorExp"_s);
            field("elt"_s, node.element);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::Await:
            open("Await"_s);
            field("value"_s, expression->as<Await>().value);
            break;
        case Expression::Kind::Yield:
            open("Yield"_s);
            field("value"_s, expression->as<Yield>().value);
            break;
        case Expression::Kind::YieldFrom:
            open("YieldFrom"_s);
            field("value"_s, expression->as<YieldFrom>().value);
            break;
        case Expression::Kind::Compare: {
            auto& node = expression->as<Compare>();
            open("Compare"_s);
            field("left"_s, node.left);
            field("ops"_s, node.ops);
            field("comparators"_s, node.comparators);
            break;
        }
        case Expression::Kind::Call: {
            auto& node = expression->as<Call>();
            open("Call"_s);
            field("func"_s, node.function);
            field("args"_s, node.arguments);
            field("keywords"_s, node.keywords);
            break;
        }
        case Expression::Kind::FormattedValue: {
            auto& node = expression->as<FormattedValue>();
            open("FormattedValue"_s);
            field("value"_s, node.value);
            field("conversion"_s, node.conversion);
            field("format_spec"_s, node.formatSpecification);
            break;
        }
        case Expression::Kind::Interpolation: {
            auto& node = expression->as<Interpolation>();
            open("Interpolation"_s);
            field("value"_s, node.value);
            field("str"_s, node.source);
            field("conversion"_s, node.conversion);
            field("format_spec"_s, node.formatSpecification);
            break;
        }
        case Expression::Kind::JoinedStr:
            open("JoinedStr"_s);
            field("values"_s, expression->as<JoinedStr>().values);
            break;
        case Expression::Kind::TemplateStr:
            open("TemplateStr"_s);
            field("values"_s, expression->as<TemplateStr>().values);
            break;
        case Expression::Kind::Constant: {
            auto& node = expression->as<Constant>();
            open("Constant"_s);
            name("value"_s);
            constant(node);
            name("kind"_s);
            m_out.append(node.hasUnicodePrefix ? "\"u\""_s : "null"_s);
            break;
        }
        case Expression::Kind::Attribute: {
            auto& node = expression->as<Attribute>();
            open("Attribute"_s);
            field("value"_s, node.value);
            field("attr"_s, node.attribute);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Subscript: {
            auto& node = expression->as<Subscript>();
            open("Subscript"_s);
            field("value"_s, node.value);
            field("slice"_s, node.slice);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Starred: {
            auto& node = expression->as<Starred>();
            open("Starred"_s);
            field("value"_s, node.value);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Name: {
            auto& node = expression->as<Name>();
            open("Name"_s);
            field("id"_s, node.id);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::List: {
            auto& node = expression->as<List>();
            open("List"_s);
            field("elts"_s, node.elements);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Tuple: {
            auto& node = expression->as<Tuple>();
            open("Tuple"_s);
            field("elts"_s, node.elements);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            open("Slice"_s);
            field("lower"_s, node.lower);
            field("upper"_s, node.upper);
            field("step"_s, node.step);
            break;
        }
        }
        close(*expression);
    }

    // ---- Patterns

    void value(Pattern* pattern)
    {
        if (!pattern) {
            m_out.append("null"_s);
            return;
        }
        switch (pattern->kind) {
        case Pattern::Kind::MatchValue:
            open("MatchValue"_s);
            field("value"_s, pattern->as<MatchValue>().value);
            break;
        case Pattern::Kind::MatchSingleton:
            open("MatchSingleton"_s);
            name("value"_s);
            constant(pattern->as<MatchSingleton>().value);
            break;
        case Pattern::Kind::MatchSequence:
            open("MatchSequence"_s);
            field("patterns"_s, pattern->as<MatchSequence>().patterns);
            break;
        case Pattern::Kind::MatchMapping: {
            auto& node = pattern->as<MatchMapping>();
            open("MatchMapping"_s);
            field("keys"_s, node.keys);
            field("patterns"_s, node.patterns);
            field("rest"_s, node.rest);
            break;
        }
        case Pattern::Kind::MatchClass: {
            auto& node = pattern->as<MatchClass>();
            open("MatchClass"_s);
            field("cls"_s, node.cls);
            field("patterns"_s, node.patterns);
            field("kwd_attrs"_s, node.keywordAttributes);
            field("kwd_patterns"_s, node.keywordPatterns);
            break;
        }
        case Pattern::Kind::MatchStar:
            open("MatchStar"_s);
            field("name"_s, pattern->as<MatchStar>().name);
            break;
        case Pattern::Kind::MatchAs: {
            auto& node = pattern->as<MatchAs>();
            open("MatchAs"_s);
            field("pattern"_s, node.pattern);
            field("name"_s, node.name);
            break;
        }
        case Pattern::Kind::MatchOr:
            open("MatchOr"_s);
            field("patterns"_s, pattern->as<MatchOr>().patterns);
            break;
        }
        close(*pattern);
    }

    // ---- Statements

    void value(Statement* statement)
    {
        switch (statement->kind) {
        case Statement::Kind::FunctionDef: {
            auto& node = statement->as<FunctionDef>();
            open(node.isAsync ? "AsyncFunctionDef"_s : "FunctionDef"_s);
            field("name"_s, node.name);
            field("args"_s, node.arguments);
            field("body"_s, node.body);
            field("decorator_list"_s, node.decorators);
            field("returns"_s, node.returns);
            nullField("type_comment"_s);
            field("type_params"_s, node.typeParameters);
            break;
        }
        case Statement::Kind::ClassDef: {
            auto& node = statement->as<ClassDef>();
            open("ClassDef"_s);
            field("name"_s, node.name);
            field("bases"_s, node.bases);
            field("keywords"_s, node.keywords);
            field("body"_s, node.body);
            field("decorator_list"_s, node.decorators);
            field("type_params"_s, node.typeParameters);
            break;
        }
        case Statement::Kind::Return:
            open("Return"_s);
            field("value"_s, statement->as<Return>().value);
            break;
        case Statement::Kind::Delete:
            open("Delete"_s);
            field("targets"_s, statement->as<Delete>().targets);
            break;
        case Statement::Kind::Assign: {
            auto& node = statement->as<Assign>();
            open("Assign"_s);
            field("targets"_s, node.targets);
            field("value"_s, node.value);
            nullField("type_comment"_s);
            break;
        }
        case Statement::Kind::TypeAlias: {
            auto& node = statement->as<TypeAlias>();
            open("TypeAlias"_s);
            field("name"_s, node.name);
            field("type_params"_s, node.typeParameters);
            field("value"_s, node.value);
            break;
        }
        case Statement::Kind::AugAssign: {
            auto& node = statement->as<AugAssign>();
            open("AugAssign"_s);
            field("target"_s, node.target);
            field("op"_s, node.op);
            field("value"_s, node.value);
            break;
        }
        case Statement::Kind::AnnAssign: {
            auto& node = statement->as<AnnAssign>();
            open("AnnAssign"_s);
            field("target"_s, node.target);
            field("annotation"_s, node.annotation);
            field("value"_s, node.value);
            field("simple"_s, node.isSimple);
            break;
        }
        case Statement::Kind::For: {
            auto& node = statement->as<For>();
            open(node.isAsync ? "AsyncFor"_s : "For"_s);
            field("target"_s, node.target);
            field("iter"_s, node.iterable);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            nullField("type_comment"_s);
            break;
        }
        case Statement::Kind::While: {
            auto& node = statement->as<While>();
            open("While"_s);
            field("test"_s, node.test);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            break;
        }
        case Statement::Kind::If: {
            auto& node = statement->as<If>();
            open("If"_s);
            field("test"_s, node.test);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            break;
        }
        case Statement::Kind::With: {
            auto& node = statement->as<With>();
            open(node.isAsync ? "AsyncWith"_s : "With"_s);
            field("items"_s, node.items);
            field("body"_s, node.body);
            nullField("type_comment"_s);
            break;
        }
        case Statement::Kind::Match: {
            auto& node = statement->as<Match>();
            open("Match"_s);
            field("subject"_s, node.subject);
            field("cases"_s, node.cases);
            break;
        }
        case Statement::Kind::Raise: {
            auto& node = statement->as<Raise>();
            open("Raise"_s);
            field("exc"_s, node.exception);
            field("cause"_s, node.cause);
            break;
        }
        case Statement::Kind::Try: {
            auto& node = statement->as<Try>();
            open(node.isStar ? "TryStar"_s : "Try"_s);
            field("body"_s, node.body);
            field("handlers"_s, node.handlers);
            field("orelse"_s, node.orElse);
            field("finalbody"_s, node.finalBody);
            break;
        }
        case Statement::Kind::Assert: {
            auto& node = statement->as<Assert>();
            open("Assert"_s);
            field("test"_s, node.test);
            field("msg"_s, node.message);
            break;
        }
        case Statement::Kind::Import:
            open("Import"_s);
            field("names"_s, statement->as<Import>().names);
            break;
        case Statement::Kind::ImportFrom: {
            auto& node = statement->as<ImportFrom>();
            open("ImportFrom"_s);
            field("module"_s, node.module);
            field("names"_s, node.names);
            field("level"_s, node.level);
            break;
        }
        case Statement::Kind::Global:
            open("Global"_s);
            field("names"_s, statement->as<Global>().names);
            break;
        case Statement::Kind::Nonlocal:
            open("Nonlocal"_s);
            field("names"_s, statement->as<Nonlocal>().names);
            break;
        case Statement::Kind::Expr:
            open("Expr"_s);
            field("value"_s, statement->as<Expr>().value);
            break;
        case Statement::Kind::Pass:
            open("Pass"_s);
            break;
        case Statement::Kind::Break:
            open("Break"_s);
            break;
        case Statement::Kind::Continue:
            open("Continue"_s);
            break;
        }
        close(*statement);
    }

    JSGlobalObject* m_globalObject;
    StringBuilder m_out;
};

} // anonymous namespace

String dumpAST(JSGlobalObject* globalObject, Module& module)
{
    return Dumper(globalObject).dump(module);
}

} } // namespace JSC::Python
