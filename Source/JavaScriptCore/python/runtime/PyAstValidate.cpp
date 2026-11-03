/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAstModule.h"

#include "Identifier.h"
#include "JSCInlines.h"
#include "PyErrors.h"

namespace JSC {
namespace Python {

// Whether a tree a program has put together is one the parser could have made, near enough for the compiler.
// (Python/ast.c in CPython is the reference, for the order of the checks too.)

namespace AstValidate {

using namespace AST;

class Validator {
public:
    Validator(JSGlobalObject* globalObject, const AstInvalidConstants& invalidConstants)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_vm(m_runtime.vm())
        , m_invalidConstants(invalidConstants)
    {
    }

    bool mod(Mod* module)
    {
        switch (module->kind) {
        case ModKind::Module:
        case ModKind::Interactive:
            return stmts(module->body);
        case ModKind::Expression:
            return expr(module->expression, ExprContext::Load);
        case ModKind::FunctionType:
            return exprs(module->argtypes, ExprContext::Load, false) && expr(module->expression, ExprContext::Load);
        }
        return false;
    }

private:
    template<typename... Parts>
    bool valueError(Parts&&... parts)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raise(m_globalObject, scope, m_runtime.ValueError(), makeString(std::forward<Parts>(parts)...));
        return false;
    }

    bool typeError(const String& message)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raise(m_globalObject, scope, m_runtime.TypeError(), message);
        return false;
    }

    bool enter()
    {
        if (m_vm.isSafeToRecurseSoft()) [[likely]]
            return true;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded during compilation"_s);
        return false;
    }

    bool positions(const Range& range)
    {
        if (range.lineno > range.endLineno)
            return valueError("AST node line range ("_s, range.lineno, ", "_s, range.endLineno, ") is not valid"_s);
        if ((range.lineno < 0 && range.endLineno != range.lineno) || (range.colOffset < 0 && range.colOffset != range.endColOffset))
            return valueError("AST node column range ("_s, range.colOffset, ", "_s, range.endColOffset, ") for line range ("_s, range.lineno, ", "_s, range.endLineno, ") is not valid"_s);
        if (range.lineno == range.endLineno && range.colOffset > range.endColOffset)
            return valueError("line "_s, range.lineno, ", column "_s, range.colOffset, '-', range.endColOffset, " is not a valid range"_s);
        return true;
    }

    bool name(Name identifier)
    {
        for (ASCIILiteral forbidden : { "None"_s, "True"_s, "False"_s }) {
            if (identifier->string() == forbidden)
                return valueError("identifier field can't represent '"_s, forbidden, "' constant"_s);
        }
        return true;
    }

    // Where CPython would go on with a null pointer.
    bool names(Seq<Name> identifiers)
    {
        for (Name identifier : identifiers) {
            if (!identifier)
                return valueError("None disallowed in identifier list"_s);
        }
        return true;
    }

    bool capture(Name identifier)
    {
        if (identifier->string() == "_"_s)
            return valueError("can't capture name '_' in patterns"_s);
        return name(identifier);
    }

    template<typename T>
    bool nonEmpty(Seq<T> items, ASCIILiteral what, ASCIILiteral owner)
    {
        if (!items.isEmpty())
            return true;
        return valueError("empty "_s, what, " on "_s, owner);
    }

    static ASCIILiteral contextName(ExprContext context)
    {
        switch (context) {
        case ExprContext::Load:
            return "Load"_s;
        case ExprContext::Store:
            return "Store"_s;
        case ExprContext::Del:
            return "Del"_s;
        }
        return { };
    }

    bool comprehensions(Seq<Comprehension*> generators)
    {
        if (generators.isEmpty())
            return valueError("comprehension with no generators"_s);
        for (Comprehension* generator : generators) {
            if (!expr(generator->target, ExprContext::Store) || !expr(generator->iter, ExprContext::Load) || !exprs(generator->ifs, ExprContext::Load, false))
                return false;
        }
        return true;
    }

    bool keywords(Seq<Keyword*> items)
    {
        for (Keyword* keyword : items) {
            if (!expr(keyword->value, ExprContext::Load))
                return false;
        }
        return true;
    }

    bool args(Seq<Arg*> items)
    {
        for (Arg* arg : items) {
            if (!positions(arg->range))
                return false;
            if (arg->annotation && !expr(arg->annotation, ExprContext::Load))
                return false;
        }
        return true;
    }

    bool arguments(Arguments* node)
    {
        if (!args(node->posonlyargs) || !args(node->args))
            return false;
        if (node->vararg && node->vararg->annotation && !expr(node->vararg->annotation, ExprContext::Load))
            return false;
        if (!args(node->kwonlyargs))
            return false;
        if (node->kwarg && node->kwarg->annotation && !expr(node->kwarg->annotation, ExprContext::Load))
            return false;
        if (node->defaults.size() > node->posonlyargs.size() + node->args.size())
            return valueError("more positional defaults than args on arguments"_s);
        if (node->kwDefaults.size() != node->kwonlyargs.size())
            return valueError("length of kwonlyargs is not the same as kw_defaults on arguments"_s);
        return exprs(node->defaults, ExprContext::Load, false) && exprs(node->kwDefaults, ExprContext::Load, true);
    }

    bool constant(const ConstantValue& value)
    {
        if (m_invalidConstants.isEmpty()) [[likely]]
            return true;
        auto it = m_invalidConstants.find(&value);
        if (it == m_invalidConstants.end())
            return true;
        return typeError(makeString("got an invalid type in Constant: "_s, it->value));
    }

    bool expr(Expr* node, ExprContext context)
    {
        if (!positions(node->range) || !enter())
            return false;

        std::optional<ExprContext> actual;
        switch (node->kind) {
        case ExprKind::Attribute:
            actual = node->as<Attribute>()->ctx;
            break;
        case ExprKind::Subscript:
            actual = node->as<Subscript>()->ctx;
            break;
        case ExprKind::Starred:
            actual = node->as<Starred>()->ctx;
            break;
        case ExprKind::Name:
            if (!name(node->as<NameExpr>()->id))
                return false;
            actual = node->as<NameExpr>()->ctx;
            break;
        case ExprKind::List:
            actual = node->as<List>()->ctx;
            break;
        case ExprKind::Tuple:
            actual = node->as<Tuple>()->ctx;
            break;
        default:
            if (context != ExprContext::Load)
                return valueError("expression which can't be assigned to in "_s, contextName(context), " context"_s);
        }
        if (actual && *actual != context)
            return valueError("expression must have "_s, contextName(context), " context but has "_s, contextName(*actual), " instead"_s);

        constexpr ExprContext load = ExprContext::Load;
        switch (node->kind) {
        case ExprKind::BoolOp: {
            auto* e = node->as<BoolOp>();
            if (e->values.size() < 2)
                return valueError("BoolOp with less than 2 values"_s);
            return exprs(e->values, load, false);
        }
        case ExprKind::BinOp: {
            // A long chain of operators leans to the left, however long: that side is not worth a call for each.
            Vector<BinOp*, 16> chain;
            Expr* leftmost = node;
            while (auto* binary = leftmost->tryAs<BinOp>()) {
                if (leftmost != node && !positions(leftmost->range))
                    return false;
                chain.append(binary);
                leftmost = binary->left;
            }
            if (!expr(leftmost, load))
                return false;
            for (size_t i = chain.size(); i--;) {
                if (!expr(chain[i]->right, load))
                    return false;
            }
            return true;
        }
        case ExprKind::UnaryOp:
            return expr(node->as<UnaryOp>()->operand, load);
        case ExprKind::Lambda: {
            auto* e = node->as<Lambda>();
            return arguments(e->args) && expr(e->body, load);
        }
        case ExprKind::IfExp: {
            auto* e = node->as<IfExp>();
            return expr(e->test, load) && expr(e->body, load) && expr(e->orelse, load);
        }
        case ExprKind::Dict: {
            auto* e = node->as<Dict>();
            if (e->keys.size() != e->values.size())
                return valueError("Dict doesn't have the same number of keys as values"_s);
            return exprs(e->keys, load, true) && exprs(e->values, load, false);
        }
        case ExprKind::Set:
            return exprs(node->as<Set>()->elts, load, false);
        case ExprKind::ListComp: {
            auto* e = node->as<ListComp>();
            return comprehensions(e->generators) && expr(e->elt, load);
        }
        case ExprKind::SetComp: {
            auto* e = node->as<SetComp>();
            return comprehensions(e->generators) && expr(e->elt, load);
        }
        case ExprKind::GeneratorExp: {
            auto* e = node->as<GeneratorExp>();
            return comprehensions(e->generators) && expr(e->elt, load);
        }
        case ExprKind::DictComp: {
            auto* e = node->as<DictComp>();
            return comprehensions(e->generators) && expr(e->key, load) && (!e->value || expr(e->value, load));
        }
        case ExprKind::Yield: {
            auto* e = node->as<Yield>();
            return !e->value || expr(e->value, load);
        }
        case ExprKind::YieldFrom:
            return expr(node->as<YieldFrom>()->value, load);
        case ExprKind::Await:
            return expr(node->as<Await>()->value, load);
        case ExprKind::Compare: {
            auto* e = node->as<Compare>();
            if (e->comparators.isEmpty())
                return valueError("Compare with no comparators"_s);
            if (e->comparators.size() != e->ops.size())
                return valueError("Compare has a different number of comparators and operands"_s);
            return exprs(e->comparators, load, false) && expr(e->left, load);
        }
        case ExprKind::Call: {
            auto* e = node->as<Call>();
            return expr(e->func, load) && exprs(e->args, load, false) && keywords(e->keywords);
        }
        case ExprKind::Constant:
            return constant(node->as<Constant>()->value);
        case ExprKind::JoinedStr:
            return exprs(node->as<JoinedStr>()->values, load, false);
        case ExprKind::TemplateStr:
            return exprs(node->as<TemplateStr>()->values, load, false);
        case ExprKind::FormattedValue: {
            auto* e = node->as<FormattedValue>();
            return expr(e->value, load) && (!e->formatSpec || expr(e->formatSpec, load));
        }
        case ExprKind::Interpolation: {
            auto* e = node->as<Interpolation>();
            // CPython takes anything for `str`, and the code then has it as a constant.
            return expr(e->value, load) && (!e->formatSpec || expr(e->formatSpec, load)) && constant(e->str);
        }
        case ExprKind::Attribute:
            return expr(node->as<Attribute>()->value, load);
        case ExprKind::Subscript: {
            auto* e = node->as<Subscript>();
            return expr(e->slice, load) && expr(e->value, load);
        }
        case ExprKind::Starred:
            return expr(node->as<Starred>()->value, context);
        case ExprKind::Slice: {
            auto* e = node->as<Slice>();
            return (!e->lower || expr(e->lower, load)) && (!e->upper || expr(e->upper, load)) && (!e->step || expr(e->step, load));
        }
        case ExprKind::List:
            return exprs(node->as<List>()->elts, context, false);
        case ExprKind::Tuple:
            return exprs(node->as<Tuple>()->elts, context, false);
        case ExprKind::NamedExpr: {
            auto* e = node->as<NamedExpr>();
            if (!e->target->is<NameExpr>())
                return typeError("NamedExpr target must be a Name"_s);
            return expr(e->value, load);
        }
        case ExprKind::Name:
            return true;
        }
        return false;
    }

    // ---- Patterns ----

    static bool isLiteralNumber(Expr* node, bool allowReal, bool allowImaginary)
    {
        switch (node->as<Constant>()->value.kind) {
        case ConstantValue::Kind::Int:
        case ConstantValue::Kind::BigInt:
        case ConstantValue::Kind::Float:
            return allowReal;
        case ConstantValue::Kind::Complex:
            return allowImaginary;
        default:
            return false;
        }
    }

    static bool isSignedLiteral(Expr* node, bool allowReal, bool allowImaginary)
    {
        auto* unary = node->as<UnaryOp>();
        if (unary->op != UnaryKind::USub && unary->op != UnaryKind::UAdd)
            return false;
        return unary->operand->is<Constant>() && isLiteralNumber(unary->operand, allowReal, allowImaginary);
    }

    static bool isComplexLiteral(Expr* node)
    {
        auto* binary = node->as<BinOp>();
        if (binary->op != Operator::Add && binary->op != Operator::Sub)
            return false;
        if (binary->left->is<Constant>()) {
            if (!isLiteralNumber(binary->left, true, false))
                return false;
        } else if (binary->left->is<UnaryOp>()) {
            if (!isSignedLiteral(binary->left, true, false))
                return false;
        } else
            return false;
        return binary->right->is<Constant>() && isLiteralNumber(binary->right, false, true);
    }

    bool patternValue(Expr* node)
    {
        if (!node)
            return valueError("None disallowed in expression list"_s);
        if (!expr(node, ExprContext::Load))
            return false;
        switch (node->kind) {
        case ExprKind::Constant:
            switch (node->as<Constant>()->value.kind) {
            case ConstantValue::Kind::Int:
            case ConstantValue::Kind::BigInt:
            case ConstantValue::Kind::Float:
            case ConstantValue::Kind::Bytes:
            case ConstantValue::Kind::Complex:
            case ConstantValue::Kind::Str:
                return true;
            default:
                return valueError("unexpected constant inside of a literal pattern"_s);
            }
        case ExprKind::Attribute:
        case ExprKind::JoinedStr:
        case ExprKind::TemplateStr:
            return true;
        case ExprKind::UnaryOp:
            if (isSignedLiteral(node, true, true))
                return true;
            break;
        case ExprKind::BinOp:
            if (isComplexLiteral(node))
                return true;
            break;
        default:
            break;
        }
        return valueError("patterns may only match literals and attribute lookups"_s);
    }

    bool patterns(Seq<Pattern*> items, bool starIsFine)
    {
        for (Pattern* item : items) {
            if (!item)
                return valueError("None disallowed in pattern list"_s);
            if (!pattern(item, starIsFine))
                return false;
        }
        return true;
    }

    bool pattern(Pattern* node, bool starIsFine)
    {
        if (!positions(node->range) || !enter())
            return false;
        switch (node->kind) {
        case PatternKind::MatchValue:
            return patternValue(node->as<MatchValue>()->value);
        case PatternKind::MatchSingleton: {
            auto& value = node->as<MatchSingleton>()->value;
            bool isSingleton = value.kind == ConstantValue::Kind::None || value.kind == ConstantValue::Kind::True || value.kind == ConstantValue::Kind::False;
            if (!isSingleton || m_invalidConstants.contains(&value))
                return valueError("MatchSingleton can only contain True, False and None"_s);
            return true;
        }
        case PatternKind::MatchSequence:
            return patterns(node->as<MatchSequence>()->patterns, true);
        case PatternKind::MatchMapping: {
            auto* p = node->as<MatchMapping>();
            if (p->keys.size() != p->patterns.size())
                return valueError("MatchMapping doesn't have the same number of keys as patterns"_s);
            if (p->rest && !capture(p->rest))
                return false;
            for (Expr* key : p->keys) {
                if (auto* literal = key ? key->tryAs<Constant>() : nullptr) {
                    // None, True and False are fine as keys, though not as values.
                    auto kind = literal->value.kind;
                    if ((kind == ConstantValue::Kind::None || kind == ConstantValue::Kind::True || kind == ConstantValue::Kind::False) && !m_invalidConstants.contains(&literal->value))
                        continue;
                }
                if (!patternValue(key))
                    return false;
            }
            return patterns(p->patterns, false);
        }
        case PatternKind::MatchClass: {
            auto* p = node->as<MatchClass>();
            if (p->kwdAttrs.size() != p->kwdPatterns.size())
                return valueError("MatchClass doesn't have the same number of keyword attributes as patterns"_s);
            if (!expr(p->cls, ExprContext::Load))
                return false;
            for (Expr* cls = p->cls; !cls->is<NameExpr>(); cls = cls->as<Attribute>()->value) {
                if (!cls->is<Attribute>())
                    return valueError("MatchClass cls field can only contain Name or Attribute nodes."_s);
            }
            if (!names(p->kwdAttrs))
                return false;
            for (Name attribute : p->kwdAttrs) {
                if (!name(attribute))
                    return false;
            }
            return patterns(p->patterns, false) && patterns(p->kwdPatterns, false);
        }
        case PatternKind::MatchStar: {
            if (!starIsFine)
                return valueError("can't use MatchStar here"_s);
            Name target = node->as<MatchStar>()->name;
            return !target || capture(target);
        }
        case PatternKind::MatchAs: {
            auto* p = node->as<MatchAs>();
            if (p->name && !capture(p->name))
                return false;
            if (!p->pattern)
                return true;
            if (!p->name)
                return valueError("MatchAs must specify a target name if a pattern is given"_s);
            return pattern(p->pattern, false);
        }
        case PatternKind::MatchOr: {
            auto* p = node->as<MatchOr>();
            if (p->patterns.size() < 2)
                return valueError("MatchOr requires at least 2 patterns"_s);
            return patterns(p->patterns, false);
        }
        }
        return false;
    }

    // ---- Statements ----

    bool importNames(Seq<Alias*> items, ASCIILiteral owner)
    {
        if (!nonEmpty(items, "names"_s, owner))
            return false;
        for (Alias* alias : items) {
            if (!name(alias->name) || (alias->asname && !name(alias->asname)))
                return false;
        }
        return true;
    }

    bool assignList(Seq<Expr*> targets, ExprContext context)
    {
        return nonEmpty(targets, "targets"_s, context == ExprContext::Del ? "Delete"_s : "Assign"_s) && exprs(targets, context, false);
    }

    bool body(Seq<Stmt*> statements, ASCIILiteral owner)
    {
        return nonEmpty(statements, "body"_s, owner) && stmts(statements);
    }

    bool typeParams(Seq<TypeParam*> items)
    {
        for (TypeParam* item : items) {
            if (!item)
                return valueError("None disallowed in type parameter list"_s);
            if (!positions(item->range) || !name(item->name))
                return false;
            if (item->bound && !expr(item->bound, ExprContext::Load))
                return false;
            if (item->defaultValue && !expr(item->defaultValue, ExprContext::Load))
                return false;
        }
        return true;
    }

    bool stmt(Stmt* node)
    {
        if (!positions(node->range) || !enter())
            return false;
        constexpr ExprContext load = ExprContext::Load;
        constexpr ExprContext store = ExprContext::Store;
        switch (node->kind) {
        case StmtKind::FunctionDef:
        case StmtKind::AsyncFunctionDef: {
            auto* s = node->as<FunctionDef>();
            return body(s->body, s->isAsync() ? "AsyncFunctionDef"_s : "FunctionDef"_s) && name(s->name) && typeParams(s->typeParams) && arguments(s->args)
                && exprs(s->decoratorList, load, false) && (!s->returns || expr(s->returns, load));
        }
        case StmtKind::ClassDef: {
            auto* s = node->as<ClassDef>();
            return body(s->body, "ClassDef"_s) && name(s->name) && typeParams(s->typeParams) && exprs(s->bases, load, false) && keywords(s->keywords) && exprs(s->decoratorList, load, false);
        }
        case StmtKind::Return: {
            auto* s = node->as<Return>();
            return !s->value || expr(s->value, load);
        }
        case StmtKind::Delete:
            return assignList(node->as<Delete>()->targets, ExprContext::Del);
        case StmtKind::Assign: {
            auto* s = node->as<Assign>();
            return assignList(s->targets, store) && expr(s->value, load);
        }
        case StmtKind::AugAssign: {
            auto* s = node->as<AugAssign>();
            return expr(s->target, store) && expr(s->value, load);
        }
        case StmtKind::AnnAssign: {
            auto* s = node->as<AnnAssign>();
            if (!s->target->is<NameExpr>() && s->simple)
                return typeError("AnnAssign with simple non-Name target"_s);
            return expr(s->target, store) && (!s->value || expr(s->value, load)) && expr(s->annotation, load);
        }
        case StmtKind::TypeAlias: {
            auto* s = node->as<TypeAlias>();
            if (!s->name->is<NameExpr>())
                return typeError("TypeAlias with non-Name name"_s);
            return expr(s->name, store) && typeParams(s->typeParams) && expr(s->value, load);
        }
        case StmtKind::For:
        case StmtKind::AsyncFor: {
            auto* s = node->as<For>();
            return expr(s->target, store) && expr(s->iter, load) && body(s->body, s->isAsync() ? "AsyncFor"_s : "For"_s) && stmts(s->orelse);
        }
        case StmtKind::While: {
            auto* s = node->as<While>();
            return expr(s->test, load) && body(s->body, "While"_s) && stmts(s->orelse);
        }
        case StmtKind::If: {
            auto* s = node->as<If>();
            return expr(s->test, load) && body(s->body, "If"_s) && stmts(s->orelse);
        }
        case StmtKind::With:
        case StmtKind::AsyncWith: {
            auto* s = node->as<With>();
            ASCIILiteral owner = s->isAsync() ? "AsyncWith"_s : "With"_s;
            if (!nonEmpty(s->items, "items"_s, owner))
                return false;
            for (WithItem* item : s->items) {
                if (!expr(item->contextExpr, load) || (item->optionalVars && !expr(item->optionalVars, store)))
                    return false;
            }
            return body(s->body, owner);
        }
        case StmtKind::Match: {
            auto* s = node->as<Match>();
            if (!expr(s->subject, load) || !nonEmpty(s->cases, "cases"_s, "Match"_s))
                return false;
            for (MatchCase* item : s->cases) {
                if (!pattern(item->pattern, false) || (item->guard && !expr(item->guard, load)) || !body(item->body, "match_case"_s))
                    return false;
            }
            return true;
        }
        case StmtKind::Raise: {
            auto* s = node->as<Raise>();
            if (s->exc)
                return expr(s->exc, load) && (!s->cause || expr(s->cause, load));
            if (s->cause)
                return valueError("Raise with cause but no exception"_s);
            return true;
        }
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* s = node->as<Try>();
            ASCIILiteral owner = s->isStar() ? "TryStar"_s : "Try"_s;
            if (!body(s->body, owner))
                return false;
            if (s->handlers.isEmpty() && s->finalbody.isEmpty())
                return valueError(owner, " has neither except handlers nor finalbody"_s);
            if (s->handlers.isEmpty() && !s->orelse.isEmpty())
                return valueError(owner, " has orelse but no except handlers"_s);
            for (ExceptHandler* handler : s->handlers) {
                if (!handler)
                    return valueError("None disallowed in handler list"_s);
                if (!s->isStar() && !positions(handler->range))
                    return false;
                if (handler->type && !expr(handler->type, load))
                    return false;
                if (!s->isStar() && handler->name && !name(handler->name))
                    return false;
                if (!body(handler->body, "ExceptHandler"_s))
                    return false;
            }
            return stmts(s->finalbody) && stmts(s->orelse);
        }
        case StmtKind::Assert: {
            auto* s = node->as<Assert>();
            return expr(s->test, load) && (!s->msg || expr(s->msg, load));
        }
        case StmtKind::Import:
            return importNames(node->as<Import>()->names, "Import"_s);
        case StmtKind::ImportFrom: {
            auto* s = node->as<ImportFrom>();
            if (s->level < 0)
                return valueError("Negative ImportFrom level"_s);
            return importNames(s->names, "ImportFrom"_s);
        }
        case StmtKind::Global:
            return nonEmpty(node->as<Global>()->names, "names"_s, "Global"_s) && names(node->as<Global>()->names);
        case StmtKind::Nonlocal:
            return nonEmpty(node->as<Nonlocal>()->names, "names"_s, "Nonlocal"_s) && names(node->as<Nonlocal>()->names);
        case StmtKind::Expr:
            return expr(node->as<ExprStmt>()->value, load);
        case StmtKind::Pass:
        case StmtKind::Break:
        case StmtKind::Continue:
            return true;
        }
        return false;
    }

    bool stmts(Seq<Stmt*> items)
    {
        for (Stmt* item : items) {
            if (!item)
                return valueError("None disallowed in statement list"_s);
            if (!stmt(item))
                return false;
        }
        return true;
    }

    bool exprs(Seq<Expr*> items, ExprContext context, bool noneIsFine)
    {
        for (Expr* item : items) {
            if (item) {
                if (!expr(item, context))
                    return false;
            } else if (!noneIsFine)
                return valueError("None disallowed in expression list"_s);
        }
        return true;
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    VM& m_vm;
    const AstInvalidConstants& m_invalidConstants;
};

} // namespace AstValidate

bool validateTree(JSGlobalObject* globalObject, AST::Mod* module, const AstInvalidConstants& invalidConstants)
{
    AstValidate::Validator validator(globalObject, invalidConstants);
    return validator.mod(module);
}

} // namespace Python
} // namespace JSC
