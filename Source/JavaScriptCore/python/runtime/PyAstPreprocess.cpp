/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAstModule.h"

#include "Identifier.h"
#include "JSCInlines.h"
#include "PyNumbers.h"
#include "PySymbolTable.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// What is done to a tree that compile() hands out as objects. (Python/ast_preprocess.c in CPython is the reference.
// Its warnings about `return` in a `finally` block are not given for such a tree; the compiler gives them.)

namespace AstPreprocess {

using namespace AST;

class Preprocessor {
public:
    Preprocessor(VM& vm, ParseTree& tree, int optimize, bool futureAnnotations, bool syntaxCheckOnly, SyntaxErrorInfo& error)
        : m_vm(vm)
        , m_arena(tree.arena)
        , m_optimize(optimize)
        , m_futureAnnotations(futureAnnotations)
        , m_syntaxCheckOnly(syntaxCheckOnly)
        , m_error(error)
    {
    }

    bool run(Mod* module)
    {
        switch (module->kind) {
        case ModKind::Module:
            return body(module->body);
        case ModKind::Interactive:
            return stmts(module->body);
        case ModKind::Expression:
            return expr(module->expression);
        case ModKind::FunctionType:
            return true;
        }
        return true;
    }

private:
    static constexpr Range noRange { -1, -1, -1, -1 };

    bool enter()
    {
        if (m_vm.isSafeToRecurseSoft()) [[likely]]
            return true;
        m_error.type = SyntaxErrorInfo::Type::RecursionError;
        m_error.message = "maximum recursion depth exceeded during compilation"_s;
        return false;
    }

    static bool startsWithDocstring(Seq<Stmt*> statements)
    {
        if (statements.isEmpty() || statements[0]->kind != StmtKind::Expr)
            return false;
        auto* constant = statements[0]->as<ExprStmt>()->value->tryAs<Constant>();
        return constant && constant->value.isString();
    }

    bool body(Seq<Stmt*>& statements)
    {
        bool hasDocstring = startsWithDocstring(statements);
        if (hasDocstring && m_optimize >= 2) {
            if (statements.size() == 1) {
                // A body cannot be empty.
                const Range& range = statements[0]->range;
                statements[0] = m_arena.create<Pass>(Range { range.lineno, range.colOffset, range.lineno, range.colOffset + 4 });
            } else
                statements = Seq<Stmt*>(statements.begin() + 1, statements.size() - 1);
            hasDocstring = false;
        }
        if (!stmts(statements))
            return false;
        if (!hasDocstring && startsWithDocstring(statements)) {
            // What folding has made a string was not written as a docstring.
            auto* statement = statements[0]->as<ExprStmt>();
            auto* joined = m_arena.create<JoinedStr>(statement->range);
            joined->values = m_arena.single(statement->value);
            statement->value = joined;
        }
        return true;
    }

    bool stmts(Seq<Stmt*> items)
    {
        for (Stmt* item : items) {
            if (!stmt(item))
                return false;
        }
        return true;
    }

    bool exprs(Seq<Expr*> items)
    {
        for (Expr*& item : items) {
            if (!expr(item))
                return false;
        }
        return true;
    }

    bool keywords(Seq<Keyword*> items)
    {
        for (Keyword* item : items) {
            if (!expr(item->value))
                return false;
        }
        return true;
    }

    bool comprehensions(Seq<Comprehension*> items)
    {
        for (Comprehension* item : items) {
            if (!expr(item->target) || !expr(item->iter) || !exprs(item->ifs))
                return false;
        }
        return true;
    }

    bool arg(Arg* item) { return !item || m_futureAnnotations || expr(item->annotation); }

    bool args(Seq<Arg*> items)
    {
        for (Arg* item : items) {
            if (!arg(item))
                return false;
        }
        return true;
    }

    bool arguments(Arguments* item)
    {
        return args(item->posonlyargs) && args(item->args) && arg(item->vararg) && args(item->kwonlyargs) && exprs(item->kwDefaults) && arg(item->kwarg) && exprs(item->defaults);
    }

    bool typeParams(Seq<TypeParam*> items)
    {
        for (TypeParam* item : items) {
            if (!expr(item->bound) || !expr(item->defaultValue))
                return false;
        }
        return true;
    }

    bool patterns(Seq<Pattern*> items)
    {
        for (Pattern* item : items) {
            if (!pattern(item))
                return false;
        }
        return true;
    }

    Constant* makeConstant(const Range& range, const ConstantValue& value)
    {
        auto* constant = m_arena.create<Constant>(range);
        constant->value = value;
        return constant;
    }

    Constant* makeStringConstant(const String& string)
    {
        ConstantValue value(ConstantValue::Kind::Str);
        value.string = m_arena.retain(string);
        return makeConstant(noRange, value);
    }

    // ---- 'format' % (a, b) as an f-string ----

    // The text up to the next conversion. Null: there is none.
    Expr* parseLiteral(StringView format, unsigned& position)
    {
        unsigned start = position;
        bool hasPercents = false;
        while (position < format.length()) {
            if (format[position] != '%')
                ++position;
            else if (position + 1 < format.length() && format[position + 1] == '%') {
                hasPercents = true;
                position += 2;
            } else
                break;
        }
        if (position == start)
            return nullptr;
        String text = format.substring(start, position - start).toString();
        if (hasPercents)
            text = makeStringByReplacingAll(text, "%%"_s, "%"_s);
        return makeStringConstant(text);
    }

    // One conversion, after the '%'. Null: not one this can do.
    Expr* parseFormat(StringView format, unsigned& position, Expr* argument)
    {
        static constexpr int maxDigits = 3;
        unsigned at = position;
        char16_t c = 0;
        auto next = [&] {
            if (at >= format.length())
                return false;
            c = format[at++];
            return true;
        };
        auto number = [&](int& result) {
            result = 0;
            int digits = 0;
            while (c >= '0' && c <= '9') {
                result = result * 10 + (c - '0');
                if (!next() || ++digits >= maxDigits)
                    return false;
            }
            return true;
        };

        bool leftJustified = false;
        while (true) {
            if (!next())
                return nullptr;
            if (c == '-')
                leftJustified = true;
            else if (c != '+' && c != ' ' && c != '#' && c != '0')
                break;
        }
        int width = -1;
        int precision = -1;
        if (c >= '0' && c <= '9' && !number(width))
            return nullptr;
        if (c == '.') {
            if (!next() || !number(precision))
                return nullptr;
        }
        position = at;
        if (c != 's' && c != 'r' && c != 'a')
            return nullptr;

        StringBuilder spec;
        if (!leftJustified && width > 0)
            spec.append('>');
        if (width >= 0)
            spec.append(width);
        if (precision >= 0)
            spec.append('.', precision);
        auto* result = m_arena.create<FormattedValue>(argument->range);
        result->value = argument;
        result->conversion = c;
        result->formatSpec = spec.isEmpty() ? nullptr : makeStringConstant(spec.toString());
        return result;
    }

    void foldFormat(Expr*& node, StringView format, Seq<Expr*> items)
    {
        Vector<Expr*, 16> parts;
        unsigned position = 0;
        unsigned count = 0;
        while (true) {
            if (Expr* literal = parseLiteral(format, position))
                parts.append(literal);
            if (position >= format.length())
                break;
            if (count >= items.size())
                return;
            ++position;
            Expr* part = parseFormat(format, position, items[count++]);
            if (!part)
                return;
            parts.append(part);
        }
        if (count < items.size())
            return;
        auto* joined = m_arena.create<JoinedStr>(node->range);
        joined->values = m_arena.copy(parts);
        node = joined;
    }

    void foldBinOp(Expr*& node)
    {
        if (m_syntaxCheckOnly)
            return;
        auto* binary = node->as<BinOp>();
        auto* left = binary->left->tryAs<Constant>();
        auto* right = binary->right->tryAs<Tuple>();
        if (!left || !right || binary->op != Operator::Mod || !left->value.isString())
            return;
        for (Expr* item : right->elts) {
            if (item->is<Starred>())
                return;
        }
        foldFormat(node, StringView(left->value.string), right->elts);
    }

    // ---- Numbers in patterns: -1, 1 + 2j ----

    static bool negate(ConstantValue& value)
    {
        switch (value.kind) {
        case ConstantValue::Kind::True:
        case ConstantValue::Kind::False:
            value.integer = value.kind == ConstantValue::Kind::True ? -1 : 0;
            value.kind = ConstantValue::Kind::Int;
            return true;
        case ConstantValue::Kind::Int:
            if (value.integer == std::numeric_limits<int64_t>::min())
                return false;
            value.integer = -value.integer;
            return true;
        case ConstantValue::Kind::BigInt:
            value.isNegative = !value.isNegative;
            return true;
        case ConstantValue::Kind::Float:
            value.number = -value.number;
            return true;
        case ConstantValue::Kind::Complex:
            value.complex.real = -value.complex.real;
            value.complex.imaginary = -value.complex.imaginary;
            return true;
        default:
            return false;
        }
    }

    static bool isNumber(const ConstantValue& value)
    {
        switch (value.kind) {
        case ConstantValue::Kind::True:
        case ConstantValue::Kind::False:
        case ConstantValue::Kind::Int:
        case ConstantValue::Kind::BigInt:
        case ConstantValue::Kind::Float:
        case ConstantValue::Kind::Complex:
            return true;
        default:
            return false;
        }
    }

    static std::optional<double> realValue(const ConstantValue& value)
    {
        switch (value.kind) {
        case ConstantValue::Kind::True:
            return 1;
        case ConstantValue::Kind::False:
            return 0;
        case ConstantValue::Kind::Int:
            return static_cast<double>(value.integer);
        case ConstantValue::Kind::Float:
            return value.number;
        case ConstantValue::Kind::BigInt: {
            std::optional<double> parsed;
            if (value.radix == 10)
                parsed = parseDouble(byteCast<Latin1Character>(std::span<const char> { value.digits.data, value.digits.size }));
            else {
                // The first 64 bits, the last of which also stands for all that follow: enough to round correctly.
                unsigned bitsPerDigit = value.radix == 16 ? 4 : value.radix == 8 ? 3 : 1;
                uint64_t bits = 0;
                int exponent = 0;
                for (size_t i = 0; i < value.digits.size; ++i) {
                    unsigned digit = toASCIIHexValue(value.digits.data[i]);
                    for (unsigned shift = bitsPerDigit; shift--;) {
                        uint64_t bit = (digit >> shift) & 1;
                        if (bits >> 63) {
                            ++exponent;
                            bits |= bit;
                        } else
                            bits = bits << 1 | bit;
                    }
                }
                parsed = std::ldexp(static_cast<double>(bits), exponent);
            }
            if (!parsed || std::isinf(*parsed))
                return std::nullopt;
            return value.isNegative ? -*parsed : *parsed;
        }
        default:
            return std::nullopt;
        }
    }

    void foldPatternValue(Expr*& node)
    {
        if (m_syntaxCheckOnly || !node)
            return;
        if (auto* unary = node->tryAs<UnaryOp>()) {
            auto* operand = unary->operand->tryAs<Constant>();
            if (!operand || (unary->op != UnaryKind::USub && unary->op != UnaryKind::UAdd))
                return;
            ConstantValue value = operand->value;
            if (unary->op == UnaryKind::USub) {
                if (!negate(value))
                    return;
            } else {
                if (!isNumber(value))
                    return;
                if (value.kind == ConstantValue::Kind::True || value.kind == ConstantValue::Kind::False) {
                    value.integer = value.kind == ConstantValue::Kind::True;
                    value.kind = ConstantValue::Kind::Int;
                }
            }
            node = makeConstant(node->range, value);
            return;
        }
        auto* binary = node->tryAs<BinOp>();
        if (!binary || (binary->op != Operator::Add && binary->op != Operator::Sub))
            return;
        auto* right = binary->right->tryAs<Constant>();
        if (!right)
            return;
        foldPatternValue(binary->left);
        auto* left = binary->left->tryAs<Constant>();
        // Nothing else passes for the value of a pattern.
        if (!left || right->value.kind != ConstantValue::Kind::Complex)
            return;
        auto real = realValue(left->value);
        if (!real)
            return;
        double sign = binary->op == Operator::Add ? 1 : -1;
        ConstantValue value(ConstantValue::Kind::Complex);
        value.complex.real = *real + sign * right->value.complex.real;
        value.complex.imaginary = sign * right->value.complex.imaginary;
        node = makeConstant(node->range, value);
    }

    // ---- The walk ----

    bool expr(Expr*& node)
    {
        if (!node)
            return true;
        if (!enter())
            return false;
        switch (node->kind) {
        case ExprKind::BoolOp:
            return exprs(node->as<BoolOp>()->values);
        case ExprKind::BinOp: {
            // A long chain of operators leans to the left, however long: that side is not worth a call for each.
            Vector<Expr**, 16> chain;
            Expr** slot = &node;
            while (auto* binary = (*slot)->tryAs<BinOp>()) {
                chain.append(slot);
                slot = &binary->left;
            }
            if (!expr(*slot))
                return false;
            for (size_t i = chain.size(); i--;) {
                if (!expr((*chain[i])->as<BinOp>()->right))
                    return false;
                foldBinOp(*chain[i]);
            }
            return true;
        }
        case ExprKind::UnaryOp:
            return expr(node->as<UnaryOp>()->operand);
        case ExprKind::Lambda: {
            auto* e = node->as<Lambda>();
            return arguments(e->args) && expr(e->body);
        }
        case ExprKind::IfExp: {
            auto* e = node->as<IfExp>();
            return expr(e->test) && expr(e->body) && expr(e->orelse);
        }
        case ExprKind::Dict: {
            auto* e = node->as<Dict>();
            return exprs(e->keys) && exprs(e->values);
        }
        case ExprKind::Set:
            return exprs(node->as<Set>()->elts);
        case ExprKind::ListComp: {
            auto* e = node->as<ListComp>();
            return expr(e->elt) && comprehensions(e->generators);
        }
        case ExprKind::SetComp: {
            auto* e = node->as<SetComp>();
            return expr(e->elt) && comprehensions(e->generators);
        }
        case ExprKind::DictComp: {
            auto* e = node->as<DictComp>();
            return expr(e->key) && expr(e->value) && comprehensions(e->generators);
        }
        case ExprKind::GeneratorExp: {
            auto* e = node->as<GeneratorExp>();
            return expr(e->elt) && comprehensions(e->generators);
        }
        case ExprKind::Await:
            return expr(node->as<Await>()->value);
        case ExprKind::Yield:
            return expr(node->as<Yield>()->value);
        case ExprKind::YieldFrom:
            return expr(node->as<YieldFrom>()->value);
        case ExprKind::Compare: {
            auto* e = node->as<Compare>();
            return expr(e->left) && exprs(e->comparators);
        }
        case ExprKind::Call: {
            auto* e = node->as<Call>();
            return expr(e->func) && exprs(e->args) && keywords(e->keywords);
        }
        case ExprKind::FormattedValue: {
            auto* e = node->as<FormattedValue>();
            return expr(e->value) && expr(e->formatSpec);
        }
        case ExprKind::Interpolation: {
            auto* e = node->as<Interpolation>();
            return expr(e->value) && expr(e->formatSpec);
        }
        case ExprKind::JoinedStr:
            return exprs(node->as<JoinedStr>()->values);
        case ExprKind::TemplateStr:
            return exprs(node->as<TemplateStr>()->values);
        case ExprKind::Attribute:
            return expr(node->as<Attribute>()->value);
        case ExprKind::Subscript: {
            auto* e = node->as<Subscript>();
            return expr(e->value) && expr(e->slice);
        }
        case ExprKind::Starred:
            return expr(node->as<Starred>()->value);
        case ExprKind::Slice: {
            auto* e = node->as<Slice>();
            return expr(e->lower) && expr(e->upper) && expr(e->step);
        }
        case ExprKind::List:
            return exprs(node->as<List>()->elts);
        case ExprKind::Tuple:
            return exprs(node->as<Tuple>()->elts);
        case ExprKind::Name: {
            auto* e = node->as<NameExpr>();
            if (!m_syntaxCheckOnly && e->ctx == ExprContext::Load && e->id->string() == "__debug__"_s)
                node = makeConstant(node->range, ConstantValue(m_optimize ? ConstantValue::Kind::False : ConstantValue::Kind::True));
            return true;
        }
        case ExprKind::NamedExpr:
            return expr(node->as<NamedExpr>()->value);
        case ExprKind::Constant:
            return true;
        }
        return true;
    }

    bool stmt(Stmt* node)
    {
        if (!enter())
            return false;
        switch (node->kind) {
        case StmtKind::FunctionDef:
        case StmtKind::AsyncFunctionDef: {
            auto* s = node->as<FunctionDef>();
            return typeParams(s->typeParams) && arguments(s->args) && body(s->body) && exprs(s->decoratorList) && (m_futureAnnotations || expr(s->returns));
        }
        case StmtKind::ClassDef: {
            auto* s = node->as<ClassDef>();
            return typeParams(s->typeParams) && exprs(s->bases) && keywords(s->keywords) && body(s->body) && exprs(s->decoratorList);
        }
        case StmtKind::Return:
            return expr(node->as<Return>()->value);
        case StmtKind::Delete:
            return exprs(node->as<Delete>()->targets);
        case StmtKind::Assign: {
            auto* s = node->as<Assign>();
            return exprs(s->targets) && expr(s->value);
        }
        case StmtKind::AugAssign: {
            auto* s = node->as<AugAssign>();
            return expr(s->target) && expr(s->value);
        }
        case StmtKind::AnnAssign: {
            auto* s = node->as<AnnAssign>();
            return expr(s->target) && (m_futureAnnotations || expr(s->annotation)) && expr(s->value);
        }
        case StmtKind::TypeAlias: {
            auto* s = node->as<TypeAlias>();
            return expr(s->name) && typeParams(s->typeParams) && expr(s->value);
        }
        case StmtKind::For:
        case StmtKind::AsyncFor: {
            auto* s = node->as<For>();
            return expr(s->target) && expr(s->iter) && stmts(s->body) && stmts(s->orelse);
        }
        case StmtKind::While: {
            auto* s = node->as<While>();
            return expr(s->test) && stmts(s->body) && stmts(s->orelse);
        }
        case StmtKind::If: {
            auto* s = node->as<If>();
            return expr(s->test) && stmts(s->body) && stmts(s->orelse);
        }
        case StmtKind::With:
        case StmtKind::AsyncWith: {
            auto* s = node->as<With>();
            for (WithItem* item : s->items) {
                if (!expr(item->contextExpr) || !expr(item->optionalVars))
                    return false;
            }
            return stmts(s->body);
        }
        case StmtKind::Raise: {
            auto* s = node->as<Raise>();
            return expr(s->exc) && expr(s->cause);
        }
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* s = node->as<Try>();
            if (!stmts(s->body))
                return false;
            for (ExceptHandler* handler : s->handlers) {
                if (!expr(handler->type) || !stmts(handler->body))
                    return false;
            }
            return stmts(s->orelse) && stmts(s->finalbody);
        }
        case StmtKind::Assert: {
            auto* s = node->as<Assert>();
            return expr(s->test) && expr(s->msg);
        }
        case StmtKind::Expr:
            return expr(node->as<ExprStmt>()->value);
        case StmtKind::Match: {
            auto* s = node->as<Match>();
            if (!expr(s->subject))
                return false;
            for (MatchCase* item : s->cases) {
                if (!pattern(item->pattern) || !expr(item->guard) || !stmts(item->body))
                    return false;
            }
            return true;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
        case StmtKind::Import:
        case StmtKind::ImportFrom:
        case StmtKind::Global:
        case StmtKind::Nonlocal:
        case StmtKind::Pass:
            return true;
        }
        return true;
    }

    bool pattern(Pattern* node)
    {
        if (!node)
            return true;
        if (!enter())
            return false;
        switch (node->kind) {
        case PatternKind::MatchValue:
            foldPatternValue(node->as<MatchValue>()->value);
            return true;
        case PatternKind::MatchSingleton:
        case PatternKind::MatchStar:
            return true;
        case PatternKind::MatchSequence:
            return patterns(node->as<MatchSequence>()->patterns);
        case PatternKind::MatchMapping: {
            auto* p = node->as<MatchMapping>();
            for (Expr*& key : p->keys)
                foldPatternValue(key);
            return patterns(p->patterns);
        }
        case PatternKind::MatchClass: {
            auto* p = node->as<MatchClass>();
            return expr(p->cls) && patterns(p->patterns) && patterns(p->kwdPatterns);
        }
        case PatternKind::MatchAs:
            return pattern(node->as<MatchAs>()->pattern);
        case PatternKind::MatchOr:
            return patterns(node->as<MatchOr>()->patterns);
        }
        return true;
    }

    VM& m_vm;
    Arena& m_arena;
    int m_optimize;
    bool m_futureAnnotations;
    bool m_syntaxCheckOnly;
    SyntaxErrorInfo& m_error;
};

} // namespace AstPreprocess

bool preprocessTree(VM& vm, ParseTree& tree, int optimize, bool futureAnnotations, bool syntaxCheckOnly, SyntaxErrorInfo& error)
{
    FutureFeatures future;
    future.annotations = futureAnnotations;
    if (!analyzeFutureStatements(tree.root, future, error))
        return false;
    AstPreprocess::Preprocessor preprocessor(vm, tree, optimize, future.annotations, syntaxCheckOnly, error);
    return preprocessor.run(tree.root);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
