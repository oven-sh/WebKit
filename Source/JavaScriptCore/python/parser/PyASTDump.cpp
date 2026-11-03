/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyASTDump.h"

#include "Identifier.h"
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

namespace {

// The format is that of tools/dump_ast_reference.py, which prints the same from CPython's parser, to compare.
class Dumper {
public:
    String dump(Mod* module)
    {
        switch (module->kind) {
        case ModKind::Module:
            open("Module"_s);
            field("body"_s, module->body);
            field("type_ignores"_s, module->typeIgnores);
            close();
            break;
        case ModKind::Interactive:
            open("Interactive"_s);
            field("body"_s, module->body);
            close();
            break;
        case ModKind::Expression:
            open("Expression"_s);
            field("body"_s, module->expression);
            close();
            break;
        case ModKind::FunctionType:
            open("FunctionType"_s);
            field("argtypes"_s, module->argtypes);
            field("returns"_s, module->expression);
            close();
            break;
        }
        return m_out.toString();
    }

private:
    void open(ASCIILiteral name)
    {
        m_out.append(name, '(');
        m_first = true;
    }
    void close()
    {
        m_out.append(')');
        m_first = false;
    }
    void close(const Range& range)
    {
        m_out.append(")@"_s, range.lineno, ',', range.colOffset, ',', range.endLineno, ',', range.endColOffset);
        m_first = false;
    }
    void name(ASCIILiteral fieldName)
    {
        if (!m_first)
            m_out.append(',');
        m_first = false;
        m_out.append(fieldName, '=');
    }

    template<typename T>
    void field(ASCIILiteral fieldName, T value)
    {
        name(fieldName);
        this->value(value);
    }

    template<typename T>
    void value(Seq<T> items)
    {
        m_out.append('[');
        bool first = true;
        for (T item : items) {
            if (!first)
                m_out.append(',');
            first = false;
            value(item);
        }
        m_out.append(']');
    }

    void hexByte(uint8_t byte)
    {
        static const char digits[] = "0123456789abcdef";
        m_out.append(digits[byte >> 4], digits[byte & 15]);
    }

    // UTF-8, with lone surrogates encoded like any other character.
    void hexString(StringView string)
    {
        m_out.append("s:"_s);
        for (char32_t c : string.codePoints()) {
            if (c < 0x80)
                hexByte(c);
            else if (c < 0x800) {
                hexByte(0xC0 | (c >> 6));
                hexByte(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                hexByte(0xE0 | (c >> 12));
                hexByte(0x80 | ((c >> 6) & 0x3F));
                hexByte(0x80 | (c & 0x3F));
            } else {
                hexByte(0xF0 | (c >> 18));
                hexByte(0x80 | ((c >> 12) & 0x3F));
                hexByte(0x80 | ((c >> 6) & 0x3F));
                hexByte(0x80 | (c & 0x3F));
            }
        }
    }

    void hexDouble(double number)
    {
        uint64_t bits = std::bit_cast<uint64_t>(number);
        for (int shift = 56; shift >= 0; shift -= 8)
            hexByte(static_cast<uint8_t>(bits >> shift));
    }

    void value(Name identifier)
    {
        if (!identifier)
            m_out.append('~');
        else
            hexString(identifier->string());
    }
    void value(StringImpl* string)
    {
        if (!string)
            m_out.append('~');
        else
            hexString(*string);
    }
    void value(int number) { m_out.append(number); }
    void value(bool flag) { m_out.append(flag ? '1' : '0'); }

    void value(const ConstantValue& constant)
    {
        switch (constant.kind) {
        case ConstantValue::Kind::None:
            m_out.append('N');
            break;
        case ConstantValue::Kind::True:
            m_out.append('T');
            break;
        case ConstantValue::Kind::False:
            m_out.append('F');
            break;
        case ConstantValue::Kind::Ellipsis:
            m_out.append('E');
            break;
        case ConstantValue::Kind::Str:
            hexString(*constant.string);
            break;
        case ConstantValue::Kind::Bytes:
            m_out.append("b:"_s);
            for (size_t i = 0; i < constant.bytes.size; ++i)
                hexByte(constant.bytes.data[i]);
            break;
        case ConstantValue::Kind::Int:
            m_out.append("i:"_s, hex(static_cast<uint64_t>(constant.integer), Lowercase));
            break;
        case ConstantValue::Kind::BigInt: {
            // Limbs of 32 bits, least significant first.
            Vector<uint32_t> limbs;
            for (size_t i = 0; i < constant.digits.size; ++i) {
                uint64_t carry = toASCIIHexValue(constant.digits.data[i]);
                for (auto& limb : limbs) {
                    carry += static_cast<uint64_t>(limb) * constant.radix;
                    limb = static_cast<uint32_t>(carry);
                    carry >>= 32;
                }
                if (carry)
                    limbs.append(static_cast<uint32_t>(carry));
            }
            m_out.append("i:"_s, hex(limbs.last(), Lowercase));
            for (size_t i = limbs.size() - 1; i--;)
                m_out.append(hex(limbs[i], 8, Lowercase));
            break;
        }
        case ConstantValue::Kind::Float:
            m_out.append("f:"_s);
            hexDouble(constant.number);
            break;
        case ConstantValue::Kind::Complex:
            m_out.append("c:"_s);
            hexDouble(constant.complex.real);
            m_out.append(',');
            hexDouble(constant.complex.imaginary);
            break;
        }
    }

    void value(ExprContext context)
    {
        static constexpr ASCIILiteral names[] = { "Load"_s, "Store"_s, "Del"_s };
        m_out.append(names[static_cast<unsigned>(context)]);
    }
    void value(BoolOperator op) { m_out.append(op == BoolOperator::And ? "And"_s : "Or"_s); }
    void value(Operator op)
    {
        static constexpr ASCIILiteral names[] = { "Add"_s, "Sub"_s, "Mult"_s, "MatMult"_s, "Div"_s, "Mod"_s, "Pow"_s, "LShift"_s, "RShift"_s, "BitOr"_s, "BitXor"_s, "BitAnd"_s, "FloorDiv"_s };
        m_out.append(names[static_cast<unsigned>(op)]);
    }
    void value(UnaryOperator op)
    {
        static constexpr ASCIILiteral names[] = { "Invert"_s, "Not"_s, "UAdd"_s, "USub"_s };
        m_out.append(names[static_cast<unsigned>(op)]);
    }
    void value(CompareOperator op)
    {
        static constexpr ASCIILiteral names[] = { "Eq"_s, "NotEq"_s, "Lt"_s, "LtE"_s, "Gt"_s, "GtE"_s, "Is"_s, "IsNot"_s, "In"_s, "NotIn"_s };
        m_out.append(names[static_cast<unsigned>(op)]);
    }

    void value(Comprehension* comprehension)
    {
        open("comprehension"_s);
        field("target"_s, comprehension->target);
        field("iter"_s, comprehension->iter);
        field("ifs"_s, comprehension->ifs);
        field("is_async"_s, comprehension->isAsync);
        close();
    }

    void value(Keyword* keyword)
    {
        open("keyword"_s);
        field("arg"_s, keyword->arg);
        field("value"_s, keyword->value);
        close(keyword->range);
    }

    void value(Arg* arg)
    {
        if (!arg) {
            m_out.append('~');
            return;
        }
        open("arg"_s);
        field("arg"_s, arg->arg);
        field("annotation"_s, arg->annotation);
        field("type_comment"_s, arg->typeComment);
        close(arg->range);
    }

    void value(Arguments* arguments)
    {
        open("arguments"_s);
        field("posonlyargs"_s, arguments->posonlyargs);
        field("args"_s, arguments->args);
        field("vararg"_s, arguments->vararg);
        field("kwonlyargs"_s, arguments->kwonlyargs);
        field("kw_defaults"_s, arguments->kwDefaults);
        field("kwarg"_s, arguments->kwarg);
        field("defaults"_s, arguments->defaults);
        close();
    }

    void value(TypeParam* param)
    {
        switch (param->kind) {
        case TypeParamKind::TypeVar:
            open("TypeVar"_s);
            field("name"_s, param->name);
            field("bound"_s, param->bound);
            break;
        case TypeParamKind::ParamSpec:
            open("ParamSpec"_s);
            field("name"_s, param->name);
            break;
        case TypeParamKind::TypeVarTuple:
            open("TypeVarTuple"_s);
            field("name"_s, param->name);
            break;
        }
        field("default_value"_s, param->defaultValue);
        close(param->range);
    }

    void value(Alias* alias)
    {
        open("alias"_s);
        field("name"_s, alias->name);
        field("asname"_s, alias->asname);
        close(alias->range);
    }

    void value(WithItem* item)
    {
        open("withitem"_s);
        field("context_expr"_s, item->contextExpr);
        field("optional_vars"_s, item->optionalVars);
        close();
    }

    void value(ExceptHandler* handler)
    {
        open("ExceptHandler"_s);
        field("type"_s, handler->type);
        field("name"_s, handler->name);
        field("body"_s, handler->body);
        close(handler->range);
    }

    void value(MatchCase* matchCase)
    {
        open("match_case"_s);
        field("pattern"_s, matchCase->pattern);
        field("guard"_s, matchCase->guard);
        field("body"_s, matchCase->body);
        close();
    }

    void value(TypeIgnore* ignore)
    {
        open("TypeIgnore"_s);
        field("lineno"_s, ignore->lineno);
        field("tag"_s, ignore->tag);
        close();
    }

    void value(Expr* expression)
    {
        if (!expression) {
            m_out.append('~');
            return;
        }
        switch (expression->kind) {
        case ExprKind::BoolOp: {
            auto* node = expression->as<BoolOp>();
            open("BoolOp"_s);
            field("op"_s, node->op);
            field("values"_s, node->values);
            break;
        }
        case ExprKind::NamedExpr: {
            auto* node = expression->as<NamedExpr>();
            open("NamedExpr"_s);
            field("target"_s, node->target);
            field("value"_s, node->value);
            break;
        }
        case ExprKind::BinOp: {
            auto* node = expression->as<BinOp>();
            open("BinOp"_s);
            field("left"_s, node->left);
            field("op"_s, node->op);
            field("right"_s, node->right);
            break;
        }
        case ExprKind::UnaryOp: {
            auto* node = expression->as<UnaryOp>();
            open("UnaryOp"_s);
            field("op"_s, node->op);
            field("operand"_s, node->operand);
            break;
        }
        case ExprKind::Lambda: {
            auto* node = expression->as<Lambda>();
            open("Lambda"_s);
            field("args"_s, node->args);
            field("body"_s, node->body);
            break;
        }
        case ExprKind::IfExp: {
            auto* node = expression->as<IfExp>();
            open("IfExp"_s);
            field("test"_s, node->test);
            field("body"_s, node->body);
            field("orelse"_s, node->orelse);
            break;
        }
        case ExprKind::Dict: {
            auto* node = expression->as<Dict>();
            open("Dict"_s);
            field("keys"_s, node->keys);
            field("values"_s, node->values);
            break;
        }
        case ExprKind::Set: {
            auto* node = expression->as<Set>();
            open("Set"_s);
            field("elts"_s, node->elts);
            break;
        }
        case ExprKind::ListComp: {
            auto* node = expression->as<ListComp>();
            open("ListComp"_s);
            field("elt"_s, node->elt);
            field("generators"_s, node->generators);
            break;
        }
        case ExprKind::SetComp: {
            auto* node = expression->as<SetComp>();
            open("SetComp"_s);
            field("elt"_s, node->elt);
            field("generators"_s, node->generators);
            break;
        }
        case ExprKind::DictComp: {
            auto* node = expression->as<DictComp>();
            open("DictComp"_s);
            field("key"_s, node->key);
            field("value"_s, node->value);
            field("generators"_s, node->generators);
            break;
        }
        case ExprKind::GeneratorExp: {
            auto* node = expression->as<GeneratorExp>();
            open("GeneratorExp"_s);
            field("elt"_s, node->elt);
            field("generators"_s, node->generators);
            break;
        }
        case ExprKind::Await: {
            open("Await"_s);
            field("value"_s, expression->as<Await>()->value);
            break;
        }
        case ExprKind::Yield: {
            open("Yield"_s);
            field("value"_s, expression->as<Yield>()->value);
            break;
        }
        case ExprKind::YieldFrom: {
            open("YieldFrom"_s);
            field("value"_s, expression->as<YieldFrom>()->value);
            break;
        }
        case ExprKind::Compare: {
            auto* node = expression->as<Compare>();
            open("Compare"_s);
            field("left"_s, node->left);
            field("ops"_s, node->ops);
            field("comparators"_s, node->comparators);
            break;
        }
        case ExprKind::Call: {
            auto* node = expression->as<Call>();
            open("Call"_s);
            field("func"_s, node->func);
            field("args"_s, node->args);
            field("keywords"_s, node->keywords);
            break;
        }
        case ExprKind::FormattedValue: {
            auto* node = expression->as<FormattedValue>();
            open("FormattedValue"_s);
            field("value"_s, node->value);
            field("conversion"_s, node->conversion);
            field("format_spec"_s, node->formatSpec);
            break;
        }
        case ExprKind::Interpolation: {
            auto* node = expression->as<Interpolation>();
            open("Interpolation"_s);
            field("value"_s, node->value);
            field<const ConstantValue&>("str"_s, node->str);
            field("conversion"_s, node->conversion);
            field("format_spec"_s, node->formatSpec);
            break;
        }
        case ExprKind::JoinedStr: {
            open("JoinedStr"_s);
            field("values"_s, expression->as<JoinedStr>()->values);
            break;
        }
        case ExprKind::TemplateStr: {
            open("TemplateStr"_s);
            field("values"_s, expression->as<TemplateStr>()->values);
            break;
        }
        case ExprKind::Constant: {
            auto* node = expression->as<Constant>();
            open("Constant"_s);
            field<const ConstantValue&>("value"_s, node->value);
            name("kind"_s);
            m_out.append(node->kindIsU ? "s:75"_s : "~"_s);
            break;
        }
        case ExprKind::Attribute: {
            auto* node = expression->as<Attribute>();
            open("Attribute"_s);
            field("value"_s, node->value);
            field("attr"_s, node->attr);
            field("ctx"_s, node->ctx);
            break;
        }
        case ExprKind::Subscript: {
            auto* node = expression->as<Subscript>();
            open("Subscript"_s);
            field("value"_s, node->value);
            field("slice"_s, node->slice);
            field("ctx"_s, node->ctx);
            break;
        }
        case ExprKind::Starred: {
            auto* node = expression->as<Starred>();
            open("Starred"_s);
            field("value"_s, node->value);
            field("ctx"_s, node->ctx);
            break;
        }
        case ExprKind::Name: {
            auto* node = expression->as<NameExpr>();
            open("Name"_s);
            field("id"_s, node->id);
            field("ctx"_s, node->ctx);
            break;
        }
        case ExprKind::List: {
            auto* node = expression->as<List>();
            open("List"_s);
            field("elts"_s, node->elts);
            field("ctx"_s, node->ctx);
            break;
        }
        case ExprKind::Tuple: {
            auto* node = expression->as<Tuple>();
            open("Tuple"_s);
            field("elts"_s, node->elts);
            field("ctx"_s, node->ctx);
            break;
        }
        case ExprKind::Slice: {
            auto* node = expression->as<Slice>();
            open("Slice"_s);
            field("lower"_s, node->lower);
            field("upper"_s, node->upper);
            field("step"_s, node->step);
            break;
        }
        }
        close(expression->range);
    }

    void value(Pattern* pattern)
    {
        if (!pattern) {
            m_out.append('~');
            return;
        }
        switch (pattern->kind) {
        case PatternKind::MatchValue:
            open("MatchValue"_s);
            field("value"_s, pattern->as<MatchValue>()->value);
            break;
        case PatternKind::MatchSingleton:
            open("MatchSingleton"_s);
            field<const ConstantValue&>("value"_s, pattern->as<MatchSingleton>()->value);
            break;
        case PatternKind::MatchSequence:
            open("MatchSequence"_s);
            field("patterns"_s, pattern->as<MatchSequence>()->patterns);
            break;
        case PatternKind::MatchMapping: {
            auto* node = pattern->as<MatchMapping>();
            open("MatchMapping"_s);
            field("keys"_s, node->keys);
            field("patterns"_s, node->patterns);
            field("rest"_s, node->rest);
            break;
        }
        case PatternKind::MatchClass: {
            auto* node = pattern->as<MatchClass>();
            open("MatchClass"_s);
            field("cls"_s, node->cls);
            field("patterns"_s, node->patterns);
            field("kwd_attrs"_s, node->kwdAttrs);
            field("kwd_patterns"_s, node->kwdPatterns);
            break;
        }
        case PatternKind::MatchStar:
            open("MatchStar"_s);
            field("name"_s, pattern->as<MatchStar>()->name);
            break;
        case PatternKind::MatchAs: {
            auto* node = pattern->as<MatchAs>();
            open("MatchAs"_s);
            field("pattern"_s, node->pattern);
            field("name"_s, node->name);
            break;
        }
        case PatternKind::MatchOr:
            open("MatchOr"_s);
            field("patterns"_s, pattern->as<MatchOr>()->patterns);
            break;
        }
        close(pattern->range);
    }

    void value(Stmt* statement)
    {
        switch (statement->kind) {
        case StmtKind::FunctionDef:
        case StmtKind::AsyncFunctionDef: {
            auto* node = statement->as<FunctionDef>();
            open(node->isAsync() ? "AsyncFunctionDef"_s : "FunctionDef"_s);
            field("name"_s, node->name);
            field("args"_s, node->args);
            field("body"_s, node->body);
            field("decorator_list"_s, node->decoratorList);
            field("returns"_s, node->returns);
            field("type_comment"_s, node->typeComment);
            field("type_params"_s, node->typeParams);
            break;
        }
        case StmtKind::ClassDef: {
            auto* node = statement->as<ClassDef>();
            open("ClassDef"_s);
            field("name"_s, node->name);
            field("bases"_s, node->bases);
            field("keywords"_s, node->keywords);
            field("body"_s, node->body);
            field("decorator_list"_s, node->decoratorList);
            field("type_params"_s, node->typeParams);
            break;
        }
        case StmtKind::Return:
            open("Return"_s);
            field("value"_s, statement->as<Return>()->value);
            break;
        case StmtKind::Delete:
            open("Delete"_s);
            field("targets"_s, statement->as<Delete>()->targets);
            break;
        case StmtKind::Assign: {
            auto* node = statement->as<Assign>();
            open("Assign"_s);
            field("targets"_s, node->targets);
            field("value"_s, node->value);
            field("type_comment"_s, node->typeComment);
            break;
        }
        case StmtKind::TypeAlias: {
            auto* node = statement->as<TypeAlias>();
            open("TypeAlias"_s);
            field("name"_s, node->name);
            field("type_params"_s, node->typeParams);
            field("value"_s, node->value);
            break;
        }
        case StmtKind::AugAssign: {
            auto* node = statement->as<AugAssign>();
            open("AugAssign"_s);
            field("target"_s, node->target);
            field("op"_s, node->op);
            field("value"_s, node->value);
            break;
        }
        case StmtKind::AnnAssign: {
            auto* node = statement->as<AnnAssign>();
            open("AnnAssign"_s);
            field("target"_s, node->target);
            field("annotation"_s, node->annotation);
            field("value"_s, node->value);
            field("simple"_s, node->simple);
            break;
        }
        case StmtKind::For:
        case StmtKind::AsyncFor: {
            auto* node = statement->as<For>();
            open(node->isAsync() ? "AsyncFor"_s : "For"_s);
            field("target"_s, node->target);
            field("iter"_s, node->iter);
            field("body"_s, node->body);
            field("orelse"_s, node->orelse);
            field("type_comment"_s, node->typeComment);
            break;
        }
        case StmtKind::While: {
            auto* node = statement->as<While>();
            open("While"_s);
            field("test"_s, node->test);
            field("body"_s, node->body);
            field("orelse"_s, node->orelse);
            break;
        }
        case StmtKind::If: {
            auto* node = statement->as<If>();
            open("If"_s);
            field("test"_s, node->test);
            field("body"_s, node->body);
            field("orelse"_s, node->orelse);
            break;
        }
        case StmtKind::With:
        case StmtKind::AsyncWith: {
            auto* node = statement->as<With>();
            open(node->isAsync() ? "AsyncWith"_s : "With"_s);
            field("items"_s, node->items);
            field("body"_s, node->body);
            field("type_comment"_s, node->typeComment);
            break;
        }
        case StmtKind::Match: {
            auto* node = statement->as<Match>();
            open("Match"_s);
            field("subject"_s, node->subject);
            field("cases"_s, node->cases);
            break;
        }
        case StmtKind::Raise: {
            auto* node = statement->as<Raise>();
            open("Raise"_s);
            field("exc"_s, node->exc);
            field("cause"_s, node->cause);
            break;
        }
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* node = statement->as<Try>();
            open(node->isStar() ? "TryStar"_s : "Try"_s);
            field("body"_s, node->body);
            field("handlers"_s, node->handlers);
            field("orelse"_s, node->orelse);
            field("finalbody"_s, node->finalbody);
            break;
        }
        case StmtKind::Assert: {
            auto* node = statement->as<Assert>();
            open("Assert"_s);
            field("test"_s, node->test);
            field("msg"_s, node->msg);
            break;
        }
        case StmtKind::Import: {
            auto* node = statement->as<Import>();
            open("Import"_s);
            field("names"_s, node->names);
            field("is_lazy"_s, node->isLazy);
            break;
        }
        case StmtKind::ImportFrom: {
            auto* node = statement->as<ImportFrom>();
            open("ImportFrom"_s);
            field("module"_s, node->module);
            field("names"_s, node->names);
            field("level"_s, node->level);
            field("is_lazy"_s, node->isLazy);
            break;
        }
        case StmtKind::Global:
            open("Global"_s);
            field("names"_s, statement->as<Global>()->names);
            break;
        case StmtKind::Nonlocal:
            open("Nonlocal"_s);
            field("names"_s, statement->as<Nonlocal>()->names);
            break;
        case StmtKind::Expr:
            open("Expr"_s);
            field("value"_s, statement->as<ExprStmt>()->value);
            break;
        case StmtKind::Pass:
            open("Pass"_s);
            break;
        case StmtKind::Break:
            open("Break"_s);
            break;
        case StmtKind::Continue:
            open("Continue"_s);
            break;
        }
        close(statement->range);
    }

    StringBuilder m_out;
    bool m_first { true };
};

} // anonymous namespace

String dumpAST(Mod* module)
{
    return Dumper().dump(module);
}

String dumpSyntaxError(const SyntaxErrorInfo& error)
{
    ASCIILiteral type;
    switch (error.type) {
    case SyntaxErrorInfo::Type::None:
        type = "None"_s;
        break;
    case SyntaxErrorInfo::Type::SyntaxError:
        type = "SyntaxError"_s;
        break;
    case SyntaxErrorInfo::Type::IndentationError:
        type = "IndentationError"_s;
        break;
    case SyntaxErrorInfo::Type::TabError:
        type = "TabError"_s;
        break;
    case SyntaxErrorInfo::Type::IncompleteInputError:
        type = "_IncompleteInputError"_s;
        break;
    case SyntaxErrorInfo::Type::ValueError:
        type = "ValueError"_s;
        break;
    case SyntaxErrorInfo::Type::OverflowError:
        type = "OverflowError"_s;
        break;
    case SyntaxErrorInfo::Type::MemoryError:
        type = "MemoryError"_s;
        break;
    case SyntaxErrorInfo::Type::RecursionError:
        type = "RecursionError"_s;
        break;
    case SyntaxErrorInfo::Type::SystemError:
        type = "SystemError"_s;
        break;
    case SyntaxErrorInfo::Type::HostException:
        type = "HostException"_s;
        break;
    }
    if (!error.isSyntaxError())
        return makeString(type, ": "_s, error.message);
    StringBuilder out;
    out.append(type, ": "_s, error.message, " ("_s);
    if (error.hasLocation)
        out.append(error.lineno, ", "_s, error.offset, ", "_s);
    else
        out.append("None, None, "_s);
    if (error.hasEnd)
        out.append(error.endLineno, ", "_s, error.endOffset, ')');
    else
        out.append("None, None)"_s);
    return out.toString();
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
