/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyArena.h"
#include "PyToken.h"

namespace JSC {

class Identifier;

namespace Python {
namespace AST {

// The syntax tree, node for node and field for field the abstract grammar that the `ast` module documents, because
// programs see it through that module. Nodes live in the Arena of the parse.

struct Range {
    int lineno { 0 };
    int colOffset { 0 };
    int endLineno { 0 };
    int endColOffset { 0 };

    Position start() const { return { lineno, colOffset }; }
    Position end() const { return { endLineno, endColOffset }; }
};

using Name = const Identifier*; // Null where the grammar says `identifier?` and there is none.

struct Expr;
struct Stmt;
struct Pattern;
struct Arguments;

enum class ExprContext : uint8_t { Load, Store, Del };
enum class BoolOperator : uint8_t { And, Or };
enum class Operator : uint8_t { Add, Sub, Mult, MatMult, Div, Mod, Pow, LShift, RShift, BitOr, BitXor, BitAnd, FloorDiv };
enum class UnaryOperator : uint8_t { Invert, Not, UAdd, USub };
enum class CompareOperator : uint8_t { Eq, NotEq, Lt, LtE, Gt, GtE, Is, IsNot, In, NotIn };

// ---- Constants ----

struct ConstantValue {
    enum class Kind : uint8_t {
        None,
        True,
        False,
        Ellipsis,
        Str,
        Bytes,
        Int, // Fits int64_t.
        BigInt, // Digits in a radix, most significant first, no sign, prefix or underscores.
        Float,
        Complex,
    };

    Kind kind { Kind::None };
    uint8_t radix { 0 };
    union {
        StringImpl* string;
        struct {
            const uint8_t* data;
            size_t size;
        } bytes;
        int64_t integer;
        struct {
            const char* data;
            size_t size;
        } digits;
        double number;
        struct {
            double real;
            double imaginary;
        } complex;
    };

    ConstantValue()
        : integer(0)
    {
    }
    explicit ConstantValue(Kind kind)
        : kind(kind)
        , integer(0)
    {
    }

    bool isString() const { return kind == Kind::Str; }
    bool isBytes() const { return kind == Kind::Bytes; }
};

// ---- Expressions ----

enum class ExprKind : uint8_t {
    BoolOp,
    NamedExpr,
    BinOp,
    UnaryOp,
    Lambda,
    IfExp,
    Dict,
    Set,
    ListComp,
    SetComp,
    DictComp,
    GeneratorExp,
    Await,
    Yield,
    YieldFrom,
    Compare,
    Call,
    FormattedValue,
    Interpolation,
    JoinedStr,
    TemplateStr,
    Constant,
    Attribute,
    Subscript,
    Starred,
    Name,
    List,
    Tuple,
    Slice,
};

struct Expr {
    ExprKind kind;
    // Not part of the tree: the expression was written in parentheses of its own, which leave no node. The parser asks.
    bool parenthesized { false };
    Range range;

    Expr(ExprKind kind, Range range)
        : kind(kind)
        , range(range)
    {
    }

    template<typename T> T* as()
    {
        ASSERT(kind == T::staticKind);
        return static_cast<T*>(this);
    }
    template<typename T> T* tryAs() { return kind == T::staticKind ? static_cast<T*>(this) : nullptr; }
    template<typename T> bool is() const { return kind == T::staticKind; }
};

#define PY_AST_EXPR(name) \
    static constexpr ExprKind staticKind = ExprKind::name; \
    explicit name(Range range) \
        : Expr(staticKind, range) \
    { \
    }

struct Comprehension {
    Expr* target;
    Expr* iter;
    Seq<Expr*> ifs;
    bool isAsync;
};

struct Keyword {
    Name arg; // Null: **value
    Expr* value;
    Range range;
};

struct BoolOp : Expr {
    PY_AST_EXPR(BoolOp)
    BoolOperator op;
    Seq<Expr*> values;
};

struct NamedExpr : Expr {
    PY_AST_EXPR(NamedExpr)
    Expr* target;
    Expr* value;
};

struct BinOp : Expr {
    PY_AST_EXPR(BinOp)
    Expr* left;
    Operator op;
    Expr* right;
};

struct UnaryOp : Expr {
    PY_AST_EXPR(UnaryOp)
    UnaryOperator op;
    Expr* operand;
};

struct Lambda : Expr {
    PY_AST_EXPR(Lambda)
    Arguments* args;
    Expr* body;
};

struct IfExp : Expr {
    PY_AST_EXPR(IfExp)
    Expr* test;
    Expr* body;
    Expr* orelse;
};

struct Dict : Expr {
    PY_AST_EXPR(Dict)
    Seq<Expr*> keys; // A null key: **value
    Seq<Expr*> values;
};

struct Set : Expr {
    PY_AST_EXPR(Set)
    Seq<Expr*> elts;
};

struct ListComp : Expr {
    PY_AST_EXPR(ListComp)
    Expr* elt;
    Seq<Comprehension*> generators;
};

struct SetComp : Expr {
    PY_AST_EXPR(SetComp)
    Expr* elt;
    Seq<Comprehension*> generators;
};

struct DictComp : Expr {
    PY_AST_EXPR(DictComp)
    Expr* key;
    Expr* value; // Null: {**key for ...}
    Seq<Comprehension*> generators;
};

struct GeneratorExp : Expr {
    PY_AST_EXPR(GeneratorExp)
    Expr* elt;
    Seq<Comprehension*> generators;
};

struct Await : Expr {
    PY_AST_EXPR(Await)
    Expr* value;
};

struct Yield : Expr {
    PY_AST_EXPR(Yield)
    Expr* value; // May be null.
};

struct YieldFrom : Expr {
    PY_AST_EXPR(YieldFrom)
    Expr* value;
};

struct Compare : Expr {
    PY_AST_EXPR(Compare)
    Expr* left;
    Seq<CompareOperator> ops;
    Seq<Expr*> comparators;
};

struct Call : Expr {
    PY_AST_EXPR(Call)
    Expr* func;
    Seq<Expr*> args;
    Seq<Keyword*> keywords;
};

struct FormattedValue : Expr {
    PY_AST_EXPR(FormattedValue)
    Expr* value;
    int conversion; // -1, 's', 'r' or 'a'
    Expr* formatSpec; // A JoinedStr, or null.
};

struct Interpolation : Expr {
    PY_AST_EXPR(Interpolation)
    Expr* value;
    ConstantValue str;
    int conversion;
    Expr* formatSpec;
};

struct JoinedStr : Expr {
    PY_AST_EXPR(JoinedStr)
    Seq<Expr*> values;
};

struct TemplateStr : Expr {
    PY_AST_EXPR(TemplateStr)
    Seq<Expr*> values;
};

struct Constant : Expr {
    PY_AST_EXPR(Constant)
    ConstantValue value;
    bool kindIsU { false }; // `kind`: "u" for a literal written u"...", else None.
};

struct Attribute : Expr {
    PY_AST_EXPR(Attribute)
    Expr* value;
    Name attr;
    ExprContext ctx;
};

struct Subscript : Expr {
    PY_AST_EXPR(Subscript)
    Expr* value;
    Expr* slice;
    ExprContext ctx;
};

struct Starred : Expr {
    PY_AST_EXPR(Starred)
    Expr* value;
    ExprContext ctx;
};

struct NameExpr : Expr {
    static constexpr ExprKind staticKind = ExprKind::Name;
    explicit NameExpr(Range range)
        : Expr(staticKind, range)
    {
    }
    Name id;
    ExprContext ctx;
};

struct List : Expr {
    PY_AST_EXPR(List)
    Seq<Expr*> elts;
    ExprContext ctx;
};

struct Tuple : Expr {
    PY_AST_EXPR(Tuple)
    Seq<Expr*> elts;
    ExprContext ctx;
};

struct Slice : Expr {
    PY_AST_EXPR(Slice)
    Expr* lower;
    Expr* upper;
    Expr* step;
};

#undef PY_AST_EXPR

// ---- Parameters ----

struct Arg {
    Name arg;
    Expr* annotation;
    StringImpl* typeComment;
    Range range;
};

struct Arguments {
    Seq<Arg*> posonlyargs;
    Seq<Arg*> args;
    Arg* vararg;
    Seq<Arg*> kwonlyargs;
    Seq<Expr*> kwDefaults; // As many as kwonlyargs; null where there is none.
    Arg* kwarg;
    Seq<Expr*> defaults; // Of the last of posonlyargs + args.
};

// ---- Type parameters ----

enum class TypeParamKind : uint8_t { TypeVar, ParamSpec, TypeVarTuple };

struct TypeParam {
    TypeParamKind kind;
    Name name;
    Expr* bound; // TypeVar only.
    Expr* defaultValue;
    Range range;
};

// ---- Patterns ----

enum class PatternKind : uint8_t {
    MatchValue,
    MatchSingleton,
    MatchSequence,
    MatchMapping,
    MatchClass,
    MatchStar,
    MatchAs,
    MatchOr,
};

struct Pattern {
    PatternKind kind;
    Range range;

    Pattern(PatternKind kind, Range range)
        : kind(kind)
        , range(range)
    {
    }

    template<typename T> T* as()
    {
        ASSERT(kind == T::staticKind);
        return static_cast<T*>(this);
    }
};

#define PY_AST_PATTERN(name) \
    static constexpr PatternKind staticKind = PatternKind::name; \
    explicit name(Range range) \
        : Pattern(staticKind, range) \
    { \
    }

struct MatchValue : Pattern {
    PY_AST_PATTERN(MatchValue)
    Expr* value;
};

struct MatchSingleton : Pattern {
    PY_AST_PATTERN(MatchSingleton)
    ConstantValue value; // None, True or False
};

struct MatchSequence : Pattern {
    PY_AST_PATTERN(MatchSequence)
    Seq<Pattern*> patterns;
};

struct MatchMapping : Pattern {
    PY_AST_PATTERN(MatchMapping)
    Seq<Expr*> keys;
    Seq<Pattern*> patterns;
    Name rest;
};

struct MatchClass : Pattern {
    PY_AST_PATTERN(MatchClass)
    Expr* cls;
    Seq<Pattern*> patterns;
    Seq<Name> kwdAttrs;
    Seq<Pattern*> kwdPatterns;
};

struct MatchStar : Pattern {
    PY_AST_PATTERN(MatchStar)
    Name name;
};

struct MatchAs : Pattern {
    PY_AST_PATTERN(MatchAs)
    Pattern* pattern;
    Name name;
};

struct MatchOr : Pattern {
    PY_AST_PATTERN(MatchOr)
    Seq<Pattern*> patterns;
};

#undef PY_AST_PATTERN

// ---- Statements ----

enum class StmtKind : uint8_t {
    FunctionDef,
    AsyncFunctionDef,
    ClassDef,
    Return,
    Delete,
    Assign,
    TypeAlias,
    AugAssign,
    AnnAssign,
    For,
    AsyncFor,
    While,
    If,
    With,
    AsyncWith,
    Match,
    Raise,
    Try,
    TryStar,
    Assert,
    Import,
    ImportFrom,
    Global,
    Nonlocal,
    Expr,
    Pass,
    Break,
    Continue,
};

struct Stmt {
    StmtKind kind;
    Range range;

    Stmt(StmtKind kind, Range range)
        : kind(kind)
        , range(range)
    {
    }

    template<typename T> T* as()
    {
        ASSERT(T::matches(kind));
        return static_cast<T*>(this);
    }
};

#define PY_AST_STMT(name) \
    static constexpr bool matches(StmtKind kind) { return kind == StmtKind::name; } \
    explicit name(Range range) \
        : Stmt(StmtKind::name, range) \
    { \
    }

// One struct for a statement and its `async` form.
#define PY_AST_STMT_OR_ASYNC(name) \
    static constexpr bool matches(StmtKind kind) { return kind == StmtKind::name || kind == StmtKind::Async##name; } \
    name(bool isAsync, Range range) \
        : Stmt(isAsync ? StmtKind::Async##name : StmtKind::name, range) \
    { \
    } \
    bool isAsync() const { return kind == StmtKind::Async##name; }

struct Alias {
    Name name;
    Name asname;
    Range range;
};

struct WithItem {
    Expr* contextExpr;
    Expr* optionalVars;
};

struct ExceptHandler {
    Expr* type;
    Name name;
    Seq<Stmt*> body;
    Range range;
};

struct MatchCase {
    Pattern* pattern;
    Expr* guard;
    Seq<Stmt*> body;
};

struct TypeIgnore {
    int lineno;
    StringImpl* tag;
};

struct FunctionDef : Stmt {
    PY_AST_STMT_OR_ASYNC(FunctionDef)
    Name name;
    Arguments* args;
    Seq<Stmt*> body;
    Seq<Expr*> decoratorList;
    Expr* returns;
    StringImpl* typeComment;
    Seq<TypeParam*> typeParams;
};

struct ClassDef : Stmt {
    PY_AST_STMT(ClassDef)
    Name name;
    Seq<Expr*> bases;
    Seq<Keyword*> keywords;
    Seq<Stmt*> body;
    Seq<Expr*> decoratorList;
    Seq<TypeParam*> typeParams;
};

struct Return : Stmt {
    PY_AST_STMT(Return)
    Expr* value;
};

struct Delete : Stmt {
    PY_AST_STMT(Delete)
    Seq<Expr*> targets;
};

struct Assign : Stmt {
    PY_AST_STMT(Assign)
    Seq<Expr*> targets;
    Expr* value;
    StringImpl* typeComment;
};

struct TypeAlias : Stmt {
    PY_AST_STMT(TypeAlias)
    Expr* name;
    Seq<TypeParam*> typeParams;
    Expr* value;
};

struct AugAssign : Stmt {
    PY_AST_STMT(AugAssign)
    Expr* target;
    Operator op;
    Expr* value;
};

struct AnnAssign : Stmt {
    PY_AST_STMT(AnnAssign)
    Expr* target;
    Expr* annotation;
    Expr* value;
    bool simple;
};

struct For : Stmt {
    PY_AST_STMT_OR_ASYNC(For)
    Expr* target;
    Expr* iter;
    Seq<Stmt*> body;
    Seq<Stmt*> orelse;
    StringImpl* typeComment;
};

struct While : Stmt {
    PY_AST_STMT(While)
    Expr* test;
    Seq<Stmt*> body;
    Seq<Stmt*> orelse;
};

struct If : Stmt {
    PY_AST_STMT(If)
    Expr* test;
    Seq<Stmt*> body;
    Seq<Stmt*> orelse;
};

struct With : Stmt {
    PY_AST_STMT_OR_ASYNC(With)
    Seq<WithItem*> items;
    Seq<Stmt*> body;
    StringImpl* typeComment;
};

struct Match : Stmt {
    PY_AST_STMT(Match)
    Expr* subject;
    Seq<MatchCase*> cases;
};

struct Raise : Stmt {
    PY_AST_STMT(Raise)
    Expr* exc;
    Expr* cause;
};

struct Try : Stmt {
    static constexpr bool matches(StmtKind kind) { return kind == StmtKind::Try || kind == StmtKind::TryStar; }
    Try(bool isStar, Range range)
        : Stmt(isStar ? StmtKind::TryStar : StmtKind::Try, range)
    {
    }
    bool isStar() const { return kind == StmtKind::TryStar; }
    Seq<Stmt*> body;
    Seq<ExceptHandler*> handlers;
    Seq<Stmt*> orelse;
    Seq<Stmt*> finalbody;
};

struct Assert : Stmt {
    PY_AST_STMT(Assert)
    Expr* test;
    Expr* msg;
};

struct Import : Stmt {
    PY_AST_STMT(Import)
    Seq<Alias*> names;
    bool isLazy;
};

struct ImportFrom : Stmt {
    PY_AST_STMT(ImportFrom)
    Name module;
    Seq<Alias*> names;
    int level;
    bool isLazy;
};

struct Global : Stmt {
    PY_AST_STMT(Global)
    Seq<Name> names;
};

struct Nonlocal : Stmt {
    PY_AST_STMT(Nonlocal)
    Seq<Name> names;
};

struct ExprStmt : Stmt {
    static constexpr bool matches(StmtKind kind) { return kind == StmtKind::Expr; }
    explicit ExprStmt(Range range)
        : Stmt(StmtKind::Expr, range)
    {
    }
    Expr* value;
};

struct Pass : Stmt {
    PY_AST_STMT(Pass)
};

struct Break : Stmt {
    PY_AST_STMT(Break)
};

struct Continue : Stmt {
    PY_AST_STMT(Continue)
};

#undef PY_AST_STMT
#undef PY_AST_STMT_OR_ASYNC

// ---- Roots ----

enum class ModKind : uint8_t { Module, Interactive, Expression, FunctionType };

struct Mod {
    ModKind kind;
    Seq<Stmt*> body; // Module, Interactive
    Seq<TypeIgnore*> typeIgnores; // Module
    Expr* expression { nullptr }; // Expression: body. FunctionType: returns.
    Seq<Expr*> argtypes; // FunctionType
};

} // namespace AST
} // namespace Python
} // namespace JSC
