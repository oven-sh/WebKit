/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyAstModule.h"

#include "Identifier.h"
#include "JSCInlines.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyFunction.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyParser.h"
#include "PySet.h"
#include "PyStr.h"

namespace JSC {
namespace Python {

// From the tree the parser makes to the objects programs see. (The ast2obj functions of Python/Python-ast.c in CPython.)

namespace AstToObject {

using namespace AST;

class AstObjectBuilder {
public:
    AstObjectBuilder(JSGlobalObject* globalObject, const AstState& state)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
        , m_vm(m_runtime.vm())
        , m_state(state)
    {
    }

    JSValue build(Mod* module)
    {
        JSValue result = mod(module);
        return m_failed ? JSValue() : result;
    }

    bool ranOutOfStack() const { return m_ranOutOfStack; }

private:
    using Fields = std::initializer_list<JSValue>;

    // The values of the fields are in the order of the grammar.
    JSValue node(AstClass id, Fields fields, const Range* range = nullptr)
    {
        if (m_failed)
            return { };
        PyTuple* names = m_state.fields(id);
        ASSERT(names->length() == fields.size());
        PyDict* dict = PyDict::createWithCapacity(m_globalObject, fields.size() + (range ? 4 : 0));
        unsigned index = 0;
        for (JSValue value : fields)
            dict->set(m_vm, asString(names->at(index++)), value);
        if (range) {
            dict->set(m_vm, m_state.name(AstName::Lineno), jsNumber(range->lineno));
            dict->set(m_vm, m_state.name(AstName::ColOffset), jsNumber(range->colOffset));
            dict->set(m_vm, m_state.name(AstName::EndLineno), jsNumber(range->endLineno));
            dict->set(m_vm, m_state.name(AstName::EndColOffset), jsNumber(range->endColOffset));
        }
        PyInstance* instance = PyInstance::create(m_vm, m_state.type(id));
        instance->setDict(m_globalObject, dict);
        return instance;
    }

    template<typename T, typename Function>
    JSValue list(Seq<T> items, const Function& convert)
    {
        if (m_failed)
            return { };
        JSArray* result = newList(m_globalObject, items.size());
        if (!result) {
            m_failed = true;
            return { };
        }
        for (T item : items) {
            JSValue value = convert(item);
            if (m_failed)
                return { };
            listAppend(m_globalObject, result, value);
        }
        return result;
    }

    JSValue exprs(Seq<Expr*> items) { return list(items, [&](Expr* item) { return expr(item); }); }
    JSValue stmts(Seq<Stmt*> items) { return list(items, [&](Stmt* item) { return stmt(item); }); }
    JSValue patterns(Seq<Pattern*> items) { return list(items, [&](Pattern* item) { return pattern(item); }); }
    JSValue identifiers(Seq<Name> items) { return list(items, [&](Name item) { return identifier(item); }); }
    JSValue args(Seq<Arg*> items) { return list(items, [&](Arg* item) { return arg(item); }); }
    JSValue keywords(Seq<Keyword*> items) { return list(items, [&](Keyword* item) { return node(AstClass::Keyword, { identifier(item->arg), expr(item->value) }, &item->range); }); }
    JSValue aliases(Seq<Alias*> items) { return list(items, [&](Alias* item) { return node(AstClass::Alias, { identifier(item->name), identifier(item->asname) }, &item->range); }); }
    JSValue withItems(Seq<WithItem*> items) { return list(items, [&](WithItem* item) { return node(AstClass::WithItem, { expr(item->contextExpr), expr(item->optionalVars) }); }); }
    JSValue comprehensions(Seq<Comprehension*> items)
    {
        return list(items, [&](Comprehension* item) {
            return node(AstClass::Comprehension, { expr(item->target), expr(item->iter), exprs(item->ifs), jsNumber(item->isAsync ? 1 : 0) });
        });
    }
    JSValue handlers(Seq<ExceptHandler*> items)
    {
        return list(items, [&](ExceptHandler* item) {
            return node(AstClass::ExceptHandler, { expr(item->type), identifier(item->name), stmts(item->body) }, &item->range);
        });
    }
    JSValue typeParams(Seq<TypeParam*> items)
    {
        return list(items, [&](TypeParam* item) -> JSValue {
            switch (item->kind) {
            case TypeParamKind::TypeVar:
                return node(AstClass::TypeVar, { identifier(item->name), expr(item->bound), expr(item->defaultValue) }, &item->range);
            case TypeParamKind::ParamSpec:
                return node(AstClass::ParamSpec, { identifier(item->name), expr(item->defaultValue) }, &item->range);
            case TypeParamKind::TypeVarTuple:
                return node(AstClass::TypeVarTuple, { identifier(item->name), expr(item->defaultValue) }, &item->range);
            }
            RELEASE_ASSERT_NOT_REACHED();
        });
    }

    JSValue identifier(Name name) { return name ? JSValue(internString(m_vm, name->string())) : jsUndefined(); }
    JSValue string(StringImpl* string) { return string ? JSValue(makeStr(m_vm, String(string))) : jsUndefined(); }

    template<typename Enum>
    JSValue singleton(AstClass first, Enum value) { return m_state.singleton(static_cast<AstClass>(static_cast<unsigned>(first) + static_cast<unsigned>(value))); }
    JSValue context(ExprContext value) { return singleton(AstClass::Load, value); }

    JSValue constant(const ConstantValue& value)
    {
        if (m_failed)
            return { };
        switch (value.kind) {
        case ConstantValue::Kind::None:
            return jsUndefined();
        case ConstantValue::Kind::True:
            return jsBoolean(true);
        case ConstantValue::Kind::False:
            return jsBoolean(false);
        case ConstantValue::Kind::Ellipsis:
            return m_runtime.ellipsis();
        case ConstantValue::Kind::Str:
            return makeStr(m_vm, String(value.string));
        case ConstantValue::Kind::Bytes:
            return PyBytes::create(m_globalObject, std::span<const uint8_t> { value.bytes.data, value.bytes.size });
        case ConstantValue::Kind::Int:
            return makeInt(m_globalObject, value.integer);
        case ConstantValue::Kind::BigInt: {
            JSValue result = intFromDigits(m_globalObject, StringView(std::span<const char> { value.digits.data, value.digits.size }), value.radix);
            if (result && value.isNegative)
                result = intNegate(m_globalObject, result);
            if (!result)
                m_failed = true;
            return result;
        }
        case ConstantValue::Kind::Float:
            return makeFloat(m_globalObject, value.number);
        case ConstantValue::Kind::Complex:
            return PyComplex::create(m_globalObject, value.complex.real, value.complex.imaginary);
        case ConstantValue::Kind::Tuple: {
            if (!enter())
                return { };
            PyTuple* tuple = PyTuple::create(m_globalObject, static_cast<unsigned>(value.items.size));
            for (size_t i = 0; i < value.items.size; ++i) {
                JSValue item = constant(value.items.data[i]);
                if (m_failed)
                    return { };
                tuple->initializeAt(m_vm, i, item);
            }
            return tuple;
        }
        case ConstantValue::Kind::FrozenSet: {
            if (!enter())
                return { };
            PySet* set = PySet::createFrozen(m_globalObject);
            for (size_t i = 0; i < value.items.size; ++i) {
                JSValue item = constant(value.items.data[i]);
                if (m_failed)
                    return { };
                set->add(m_globalObject, item);
            }
            return set;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool enter()
    {
        if (m_failed)
            return false;
        if (m_vm.isSafeToRecurseSoft()) [[likely]]
            return true;
        m_failed = true;
        m_ranOutOfStack = true;
        return false;
    }

    JSValue mod(Mod* module)
    {
        switch (module->kind) {
        case ModKind::Module:
            return node(AstClass::Module, { stmts(module->body), list(module->typeIgnores, [&](TypeIgnore* item) { return item ? node(AstClass::TypeIgnore, { jsNumber(item->lineno), string(item->tag) }) : jsUndefined(); }) });
        case ModKind::Interactive:
            return node(AstClass::Interactive, { stmts(module->body) });
        case ModKind::Expression:
            return node(AstClass::Expression, { expr(module->expression) });
        case ModKind::FunctionType:
            return node(AstClass::FunctionType, { exprs(module->argtypes), expr(module->expression) });
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    JSValue arg(Arg* item)
    {
        if (!item)
            return jsUndefined();
        return node(AstClass::Arg, { identifier(item->arg), expr(item->annotation), string(item->typeComment) }, &item->range);
    }

    JSValue arguments(Arguments* item)
    {
        return node(AstClass::Arguments, { args(item->posonlyargs), args(item->args), arg(item->vararg), args(item->kwonlyargs), exprs(item->kwDefaults), arg(item->kwarg), exprs(item->defaults) });
    }

    JSValue stmt(Stmt* statement)
    {
        if (!enter())
            return { };
        const Range* range = &statement->range;
        switch (statement->kind) {
        case StmtKind::FunctionDef:
        case StmtKind::AsyncFunctionDef: {
            auto* s = statement->as<FunctionDef>();
            return node(s->isAsync() ? AstClass::AsyncFunctionDef : AstClass::FunctionDef, { identifier(s->name), arguments(s->args), stmts(s->body), exprs(s->decoratorList), expr(s->returns), string(s->typeComment), typeParams(s->typeParams) }, range);
        }
        case StmtKind::ClassDef: {
            auto* s = statement->as<ClassDef>();
            return node(AstClass::ClassDef, { identifier(s->name), exprs(s->bases), keywords(s->keywords), stmts(s->body), exprs(s->decoratorList), typeParams(s->typeParams) }, range);
        }
        case StmtKind::Return:
            return node(AstClass::Return, { expr(statement->as<Return>()->value) }, range);
        case StmtKind::Delete:
            return node(AstClass::Delete, { exprs(statement->as<Delete>()->targets) }, range);
        case StmtKind::Assign: {
            auto* s = statement->as<Assign>();
            return node(AstClass::Assign, { exprs(s->targets), expr(s->value), string(s->typeComment) }, range);
        }
        case StmtKind::TypeAlias: {
            auto* s = statement->as<TypeAlias>();
            return node(AstClass::TypeAlias, { expr(s->name), typeParams(s->typeParams), expr(s->value) }, range);
        }
        case StmtKind::AugAssign: {
            auto* s = statement->as<AugAssign>();
            return node(AstClass::AugAssign, { expr(s->target), singleton(AstClass::Add, s->op), expr(s->value) }, range);
        }
        case StmtKind::AnnAssign: {
            auto* s = statement->as<AnnAssign>();
            return node(AstClass::AnnAssign, { expr(s->target), expr(s->annotation), expr(s->value), jsNumber(s->simple ? 1 : 0) }, range);
        }
        case StmtKind::For:
        case StmtKind::AsyncFor: {
            auto* s = statement->as<For>();
            return node(s->isAsync() ? AstClass::AsyncFor : AstClass::For, { expr(s->target), expr(s->iter), stmts(s->body), stmts(s->orelse), string(s->typeComment) }, range);
        }
        case StmtKind::While: {
            auto* s = statement->as<While>();
            return node(AstClass::While, { expr(s->test), stmts(s->body), stmts(s->orelse) }, range);
        }
        case StmtKind::If: {
            auto* s = statement->as<If>();
            return node(AstClass::If, { expr(s->test), stmts(s->body), stmts(s->orelse) }, range);
        }
        case StmtKind::With:
        case StmtKind::AsyncWith: {
            auto* s = statement->as<With>();
            return node(s->isAsync() ? AstClass::AsyncWith : AstClass::With, { withItems(s->items), stmts(s->body), string(s->typeComment) }, range);
        }
        case StmtKind::Match: {
            auto* s = statement->as<Match>();
            return node(AstClass::Match, { expr(s->subject), list(s->cases, [&](MatchCase* item) { return node(AstClass::MatchCase, { pattern(item->pattern), expr(item->guard), stmts(item->body) }); }) }, range);
        }
        case StmtKind::Raise: {
            auto* s = statement->as<Raise>();
            return node(AstClass::Raise, { expr(s->exc), expr(s->cause) }, range);
        }
        case StmtKind::Try:
        case StmtKind::TryStar: {
            auto* s = statement->as<Try>();
            return node(s->isStar() ? AstClass::TryStar : AstClass::Try, { stmts(s->body), handlers(s->handlers), stmts(s->orelse), stmts(s->finalbody) }, range);
        }
        case StmtKind::Assert: {
            auto* s = statement->as<Assert>();
            return node(AstClass::Assert, { expr(s->test), expr(s->msg) }, range);
        }
        case StmtKind::Import: {
            auto* s = statement->as<Import>();
            return node(AstClass::Import, { aliases(s->names), jsNumber(s->isLazy ? 1 : 0) }, range);
        }
        case StmtKind::ImportFrom: {
            auto* s = statement->as<ImportFrom>();
            return node(AstClass::ImportFrom, { identifier(s->module), aliases(s->names), jsNumber(s->level), jsNumber(s->isLazy ? 1 : 0) }, range);
        }
        case StmtKind::Global:
            return node(AstClass::Global, { identifiers(statement->as<Global>()->names) }, range);
        case StmtKind::Nonlocal:
            return node(AstClass::Nonlocal, { identifiers(statement->as<Nonlocal>()->names) }, range);
        case StmtKind::Expr:
            return node(AstClass::Expr, { expr(statement->as<ExprStmt>()->value) }, range);
        case StmtKind::Pass:
            return node(AstClass::Pass, { }, range);
        case StmtKind::Break:
            return node(AstClass::Break, { }, range);
        case StmtKind::Continue:
            return node(AstClass::Continue, { }, range);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    JSValue expr(Expr* expression)
    {
        if (!expression)
            return jsUndefined();
        if (!enter())
            return { };
        const Range* range = &expression->range;
        switch (expression->kind) {
        case ExprKind::BoolOp: {
            auto* e = expression->as<BoolOp>();
            return node(AstClass::BoolOp, { singleton(AstClass::And, e->op), exprs(e->values) }, range);
        }
        case ExprKind::NamedExpr: {
            auto* e = expression->as<NamedExpr>();
            return node(AstClass::NamedExpr, { expr(e->target), expr(e->value) }, range);
        }
        case ExprKind::BinOp: {
            // A long chain of operators leans to the left, however long: that side is not worth a call for each.
            Vector<BinOp*, 16> chain;
            Expr* leftmost = expression;
            while (auto* binary = leftmost->tryAs<BinOp>()) {
                chain.append(binary);
                leftmost = binary->left;
            }
            JSValue left = expr(leftmost);
            for (size_t i = chain.size(); i--;)
                left = node(AstClass::BinOp, { left, singleton(AstClass::Add, chain[i]->op), expr(chain[i]->right) }, &chain[i]->range);
            return left;
        }
        case ExprKind::UnaryOp: {
            auto* e = expression->as<UnaryOp>();
            return node(AstClass::UnaryOp, { singleton(AstClass::Invert, e->op), expr(e->operand) }, range);
        }
        case ExprKind::Lambda: {
            auto* e = expression->as<Lambda>();
            return node(AstClass::Lambda, { arguments(e->args), expr(e->body) }, range);
        }
        case ExprKind::IfExp: {
            auto* e = expression->as<IfExp>();
            return node(AstClass::IfExp, { expr(e->test), expr(e->body), expr(e->orelse) }, range);
        }
        case ExprKind::Dict: {
            auto* e = expression->as<Dict>();
            return node(AstClass::Dict, { exprs(e->keys), exprs(e->values) }, range);
        }
        case ExprKind::Set:
            return node(AstClass::Set, { exprs(expression->as<Set>()->elts) }, range);
        case ExprKind::ListComp: {
            auto* e = expression->as<ListComp>();
            return node(AstClass::ListComp, { expr(e->elt), comprehensions(e->generators) }, range);
        }
        case ExprKind::SetComp: {
            auto* e = expression->as<SetComp>();
            return node(AstClass::SetComp, { expr(e->elt), comprehensions(e->generators) }, range);
        }
        case ExprKind::DictComp: {
            auto* e = expression->as<DictComp>();
            return node(AstClass::DictComp, { expr(e->key), expr(e->value), comprehensions(e->generators) }, range);
        }
        case ExprKind::GeneratorExp: {
            auto* e = expression->as<GeneratorExp>();
            return node(AstClass::GeneratorExp, { expr(e->elt), comprehensions(e->generators) }, range);
        }
        case ExprKind::Await:
            return node(AstClass::Await, { expr(expression->as<Await>()->value) }, range);
        case ExprKind::Yield:
            return node(AstClass::Yield, { expr(expression->as<Yield>()->value) }, range);
        case ExprKind::YieldFrom:
            return node(AstClass::YieldFrom, { expr(expression->as<YieldFrom>()->value) }, range);
        case ExprKind::Compare: {
            auto* e = expression->as<Compare>();
            return node(AstClass::Compare, { expr(e->left), list(e->ops, [&](CompareKind op) { return singleton(AstClass::Eq, op); }), exprs(e->comparators) }, range);
        }
        case ExprKind::Call: {
            auto* e = expression->as<Call>();
            return node(AstClass::Call, { expr(e->func), exprs(e->args), keywords(e->keywords) }, range);
        }
        case ExprKind::FormattedValue: {
            auto* e = expression->as<FormattedValue>();
            return node(AstClass::FormattedValue, { expr(e->value), jsNumber(e->conversion), expr(e->formatSpec) }, range);
        }
        case ExprKind::Interpolation: {
            auto* e = expression->as<Interpolation>();
            return node(AstClass::Interpolation, { expr(e->value), constant(e->str), jsNumber(e->conversion), expr(e->formatSpec) }, range);
        }
        case ExprKind::JoinedStr:
            return node(AstClass::JoinedStr, { exprs(expression->as<JoinedStr>()->values) }, range);
        case ExprKind::TemplateStr:
            return node(AstClass::TemplateStr, { exprs(expression->as<TemplateStr>()->values) }, range);
        case ExprKind::Constant: {
            auto* e = expression->as<Constant>();
            return node(AstClass::Constant, { constant(e->value), e->kindIsU ? JSValue(internString(m_vm, "u"_s)) : jsUndefined() }, range);
        }
        case ExprKind::Attribute: {
            auto* e = expression->as<Attribute>();
            return node(AstClass::Attribute, { expr(e->value), identifier(e->attr), context(e->ctx) }, range);
        }
        case ExprKind::Subscript: {
            auto* e = expression->as<Subscript>();
            return node(AstClass::Subscript, { expr(e->value), expr(e->slice), context(e->ctx) }, range);
        }
        case ExprKind::Starred: {
            auto* e = expression->as<Starred>();
            return node(AstClass::Starred, { expr(e->value), context(e->ctx) }, range);
        }
        case ExprKind::Name: {
            auto* e = expression->as<NameExpr>();
            return node(AstClass::Name, { identifier(e->id), context(e->ctx) }, range);
        }
        case ExprKind::List: {
            auto* e = expression->as<List>();
            return node(AstClass::List, { exprs(e->elts), context(e->ctx) }, range);
        }
        case ExprKind::Tuple: {
            auto* e = expression->as<Tuple>();
            return node(AstClass::Tuple, { exprs(e->elts), context(e->ctx) }, range);
        }
        case ExprKind::Slice: {
            auto* e = expression->as<Slice>();
            return node(AstClass::Slice, { expr(e->lower), expr(e->upper), expr(e->step) }, range);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    JSValue pattern(Pattern* item)
    {
        if (!item)
            return jsUndefined();
        if (!enter())
            return { };
        const Range* range = &item->range;
        switch (item->kind) {
        case PatternKind::MatchValue:
            return node(AstClass::MatchValue, { expr(item->as<MatchValue>()->value) }, range);
        case PatternKind::MatchSingleton:
            return node(AstClass::MatchSingleton, { constant(item->as<MatchSingleton>()->value) }, range);
        case PatternKind::MatchSequence:
            return node(AstClass::MatchSequence, { patterns(item->as<MatchSequence>()->patterns) }, range);
        case PatternKind::MatchMapping: {
            auto* p = item->as<MatchMapping>();
            return node(AstClass::MatchMapping, { exprs(p->keys), patterns(p->patterns), identifier(p->rest) }, range);
        }
        case PatternKind::MatchClass: {
            auto* p = item->as<MatchClass>();
            return node(AstClass::MatchClass, { expr(p->cls), patterns(p->patterns), identifiers(p->kwdAttrs), patterns(p->kwdPatterns) }, range);
        }
        case PatternKind::MatchStar:
            return node(AstClass::MatchStar, { identifier(item->as<MatchStar>()->name) }, range);
        case PatternKind::MatchAs: {
            auto* p = item->as<MatchAs>();
            return node(AstClass::MatchAs, { pattern(p->pattern), identifier(p->name) }, range);
        }
        case PatternKind::MatchOr:
            return node(AstClass::MatchOr, { patterns(item->as<MatchOr>()->patterns) }, range);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    VM& m_vm;
    const AstState& m_state;
    bool m_failed { false };
    bool m_ranOutOfStack { false };
};

} // namespace AstToObject

JSValue astFromTree(JSGlobalObject* globalObject, AST::Mod* module)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    AstState state(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    AstToObject::AstObjectBuilder builder(globalObject, state);
    JSValue result = builder.build(module);
    if (builder.ranOutOfStack())
        return raiseRecursionError(globalObject, scope, "maximum recursion depth exceeded during ast construction"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

JSValue parseToAST(JSGlobalObject* globalObject, std::span<const uint8_t> source, const String& filename, ASCIILiteral mode, int flags, int optimize, int featureVersion, JSValue module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto host = makeParserHost(globalObject, filename, module);
    ParseTree tree;
    bool parsed = parseSource(globalObject, tree, *host, source, filename, mode, flags, featureVersion);
    RETURN_IF_EXCEPTION(scope, { });
    if (!parsed)
        return { };

    SyntaxErrorInfo error;
    bool syntaxCheckOnly = (flags & CompileOptimizedAST) == CompileOnlyAST;
    if (!preprocessTree(vm, tree, optimize < 0 ? rt.optimizationLevel : optimize, flags & static_cast<int>(CodeFlag::FutureAnnotations), syntaxCheckOnly, error))
        RELEASE_AND_RETURN(scope, raiseSyntaxErrorInfo(globalObject, error, filename));
    RELEASE_AND_RETURN(scope, astFromTree(globalObject, tree.root));
}

} // namespace Python
} // namespace JSC
