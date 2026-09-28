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

#pragma once

#include "Identifier.h"
#include "PythonOperators.h"
#include <span>

namespace JSC { namespace Python {

// The syntax tree. It is Python's own, node for node and field for field: Parser/Python.asdl in CPython says what each is.
// That is what the `ast` module has to give out, and it lets this parser be checked against that one.
// Everything is in an Arena. A field that may be absent is a null pointer, and a sequence is a span.

template<typename T> using Sequence = std::span<T>;

struct Node {
    // Lines from 1. Columns from 0 and in bytes of UTF-8.
    unsigned line { 0 };
    unsigned column { 0 };
    unsigned endLine { 0 };
    unsigned endColumn { 0 };
    // In code units of the source.
    unsigned start { 0 };
    unsigned end { 0 };
};

struct Expression;
struct Statement;
struct Pattern;

enum class ExpressionContext : uint8_t { Load, Store, Del };
enum class BooleanOperator : uint8_t { And, Or };

// ---- What is neither a statement nor an expression

struct Argument : Node {
    const Identifier* name { nullptr };
    Expression* annotation { nullptr };
};

struct Arguments {
    Sequence<Argument*> positionalOnly;
    Sequence<Argument*> positional;
    Argument* variadic { nullptr };
    Sequence<Argument*> keywordOnly;
    Sequence<Expression*> keywordDefaults; // One for each of keywordOnly, null where there is none.
    Argument* keywordVariadic { nullptr };
    Sequence<Expression*> defaults; // Of the last of positionalOnly and positional.
};

struct Keyword : Node {
    const Identifier* name { nullptr }; // Null for **value.
    Expression* value { nullptr };
};

struct Alias : Node {
    const Identifier* name { nullptr };
    const Identifier* asName { nullptr };
};

struct WithItem {
    Expression* contextExpression { nullptr };
    Expression* optionalVariables { nullptr };
};

struct Comprehension {
    Expression* target { nullptr };
    Expression* iterable { nullptr };
    Sequence<Expression*> conditions;
    bool isAsync { false };
};

struct ExceptHandler : Node {
    Expression* type { nullptr };
    const Identifier* name { nullptr };
    Sequence<Statement*> body;
};

struct MatchCase {
    Pattern* pattern { nullptr };
    Expression* guard { nullptr };
    Sequence<Statement*> body;
};

struct TypeParameter : Node {
    enum class Kind : uint8_t { TypeVar, ParamSpec, TypeVarTuple };
    Kind kind { Kind::TypeVar };
    const Identifier* name { nullptr };
    Expression* bound { nullptr }; // TypeVar only.
    Expression* defaultValue { nullptr };
};

// ---- Expressions

#define FOR_EACH_PYTHON_EXPRESSION(macro) \
    macro(BoolOp) \
    macro(NamedExpr) \
    macro(BinOp) \
    macro(UnaryOp) \
    macro(Lambda) \
    macro(IfExp) \
    macro(Dict) \
    macro(Set) \
    macro(ListComp) \
    macro(SetComp) \
    macro(DictComp) \
    macro(GeneratorExp) \
    macro(Await) \
    macro(Yield) \
    macro(YieldFrom) \
    macro(Compare) \
    macro(Call) \
    macro(FormattedValue) \
    macro(Interpolation) \
    macro(JoinedStr) \
    macro(TemplateStr) \
    macro(Constant) \
    macro(Attribute) \
    macro(Subscript) \
    macro(Starred) \
    macro(Name) \
    macro(List) \
    macro(Tuple) \
    macro(Slice)

struct Expression : Node {
    enum class Kind : uint8_t {
#define DECLARE(name) name,
        FOR_EACH_PYTHON_EXPRESSION(DECLARE)
#undef DECLARE
    };

    explicit Expression(Kind kind)
        : kind(kind)
    {
    }

    template<typename T> bool is() const { return kind == T::expressionKind; }
    template<typename T> T& as()
    {
        ASSERT(is<T>());
        return static_cast<T&>(*this);
    }
    template<typename T> T* tryAs() { return is<T>() ? static_cast<T*>(this) : nullptr; }

    Kind kind;
    bool isParenthesized { false }; // (x), which is x to everything but a few rules of the grammar.
};

#define PYTHON_EXPRESSION(name) \
    static constexpr Kind expressionKind = Kind::name; \
    name() \
        : Expression(Kind::name) \
    { \
    }

struct BoolOp : Expression {
    PYTHON_EXPRESSION(BoolOp)
    BooleanOperator op { BooleanOperator::And };
    Sequence<Expression*> values;
};

struct NamedExpr : Expression {
    PYTHON_EXPRESSION(NamedExpr)
    Expression* target { nullptr };
    Expression* value { nullptr };
};

struct BinOp : Expression {
    PYTHON_EXPRESSION(BinOp)
    Expression* left { nullptr };
    BinaryOperator op { BinaryOperator::Add };
    Expression* right { nullptr };
};

struct UnaryOp : Expression {
    PYTHON_EXPRESSION(UnaryOp)
    UnaryOperator op { UnaryOperator::Not };
    Expression* operand { nullptr };
};

struct Lambda : Expression {
    PYTHON_EXPRESSION(Lambda)
    Arguments* arguments { nullptr };
    Expression* body { nullptr };
};

struct IfExp : Expression {
    PYTHON_EXPRESSION(IfExp)
    Expression* test { nullptr };
    Expression* body { nullptr };
    Expression* orElse { nullptr };
};

struct Dict : Expression {
    PYTHON_EXPRESSION(Dict)
    Sequence<Expression*> keys; // Null for **value.
    Sequence<Expression*> values;
};

struct Set : Expression {
    PYTHON_EXPRESSION(Set)
    Sequence<Expression*> elements;
};

struct ListComp : Expression {
    PYTHON_EXPRESSION(ListComp)
    Expression* element { nullptr };
    Sequence<Comprehension*> generators;
};

struct SetComp : Expression {
    PYTHON_EXPRESSION(SetComp)
    Expression* element { nullptr };
    Sequence<Comprehension*> generators;
};

struct DictComp : Expression {
    PYTHON_EXPRESSION(DictComp)
    Expression* key { nullptr };
    Expression* value { nullptr };
    Sequence<Comprehension*> generators;
};

struct GeneratorExp : Expression {
    PYTHON_EXPRESSION(GeneratorExp)
    Expression* element { nullptr };
    Sequence<Comprehension*> generators;
};

struct Await : Expression {
    PYTHON_EXPRESSION(Await)
    Expression* value { nullptr };
};

struct Yield : Expression {
    PYTHON_EXPRESSION(Yield)
    Expression* value { nullptr };
};

struct YieldFrom : Expression {
    PYTHON_EXPRESSION(YieldFrom)
    Expression* value { nullptr };
};

struct Compare : Expression {
    PYTHON_EXPRESSION(Compare)
    Expression* left { nullptr };
    Sequence<ComparisonOperator> ops;
    Sequence<Expression*> comparators;
};

struct Call : Expression {
    PYTHON_EXPRESSION(Call)
    Expression* function { nullptr };
    Sequence<Expression*> arguments;
    Sequence<Keyword*> keywords;
};

struct FormattedValue : Expression {
    PYTHON_EXPRESSION(FormattedValue)
    Expression* value { nullptr };
    int conversion { -1 }; // 's', 'r', 'a', or -1 for none.
    Expression* formatSpecification { nullptr };
};

struct Interpolation : Expression {
    PYTHON_EXPRESSION(Interpolation)
    Expression* value { nullptr };
    const Identifier* source { nullptr };
    int conversion { -1 };
    Expression* formatSpecification { nullptr };
};

struct JoinedStr : Expression {
    PYTHON_EXPRESSION(JoinedStr)
    Sequence<Expression*> values;
};

struct TemplateStr : Expression {
    PYTHON_EXPRESSION(TemplateStr)
    Sequence<Expression*> values;
};

struct Constant : Expression {
    PYTHON_EXPRESSION(Constant)
    enum class Type : uint8_t {
        None,
        True,
        False,
        Ellipsis,
        Integer, // In `integer`.
        BigInteger, // Its digits are in `text`, in `radix`.
        Float,
        Imaginary,
        String,
        Bytes, // A character of `text` for each.
    };
    Type type { Type::None };
    uint8_t radix { 10 };
    bool hasUnicodePrefix { false };
    const Identifier* text { nullptr };
    union {
        uint64_t integer { 0 };
        double real;
    };
};

struct Attribute : Expression {
    PYTHON_EXPRESSION(Attribute)
    Expression* value { nullptr };
    const Identifier* attribute { nullptr };
    ExpressionContext context { ExpressionContext::Load };
};

struct Subscript : Expression {
    PYTHON_EXPRESSION(Subscript)
    Expression* value { nullptr };
    Expression* slice { nullptr };
    ExpressionContext context { ExpressionContext::Load };
};

struct Starred : Expression {
    PYTHON_EXPRESSION(Starred)
    Expression* value { nullptr };
    ExpressionContext context { ExpressionContext::Load };
};

struct Name : Expression {
    PYTHON_EXPRESSION(Name)
    const Identifier* id { nullptr };
    ExpressionContext context { ExpressionContext::Load };
};

struct List : Expression {
    PYTHON_EXPRESSION(List)
    Sequence<Expression*> elements;
    ExpressionContext context { ExpressionContext::Load };
};

struct Tuple : Expression {
    PYTHON_EXPRESSION(Tuple)
    Sequence<Expression*> elements;
    ExpressionContext context { ExpressionContext::Load };
};

struct Slice : Expression {
    PYTHON_EXPRESSION(Slice)
    Expression* lower { nullptr };
    Expression* upper { nullptr };
    Expression* step { nullptr };
};

#undef PYTHON_EXPRESSION

// ---- Patterns

#define FOR_EACH_PYTHON_PATTERN(macro) \
    macro(MatchValue) \
    macro(MatchSingleton) \
    macro(MatchSequence) \
    macro(MatchMapping) \
    macro(MatchClass) \
    macro(MatchStar) \
    macro(MatchAs) \
    macro(MatchOr)

struct Pattern : Node {
    enum class Kind : uint8_t {
#define DECLARE(name) name,
        FOR_EACH_PYTHON_PATTERN(DECLARE)
#undef DECLARE
    };

    explicit Pattern(Kind kind)
        : kind(kind)
    {
    }

    template<typename T> bool is() const { return kind == T::patternKind; }
    template<typename T> T& as()
    {
        ASSERT(is<T>());
        return static_cast<T&>(*this);
    }

    Kind kind;
};

#define PYTHON_PATTERN(name) \
    static constexpr Kind patternKind = Kind::name; \
    name() \
        : Pattern(Kind::name) \
    { \
    }

struct MatchValue : Pattern {
    PYTHON_PATTERN(MatchValue)
    Expression* value { nullptr };
};

struct MatchSingleton : Pattern {
    PYTHON_PATTERN(MatchSingleton)
    Constant::Type value { Constant::Type::None }; // None, True or False.
};

struct MatchSequence : Pattern {
    PYTHON_PATTERN(MatchSequence)
    Sequence<Pattern*> patterns;
};

struct MatchMapping : Pattern {
    PYTHON_PATTERN(MatchMapping)
    Sequence<Expression*> keys;
    Sequence<Pattern*> patterns;
    const Identifier* rest { nullptr };
};

struct MatchClass : Pattern {
    PYTHON_PATTERN(MatchClass)
    Expression* cls { nullptr };
    Sequence<Pattern*> patterns;
    Sequence<const Identifier*> keywordAttributes;
    Sequence<Pattern*> keywordPatterns;
};

struct MatchStar : Pattern {
    PYTHON_PATTERN(MatchStar)
    const Identifier* name { nullptr };
};

struct MatchAs : Pattern {
    PYTHON_PATTERN(MatchAs)
    Pattern* pattern { nullptr };
    const Identifier* name { nullptr };
};

struct MatchOr : Pattern {
    PYTHON_PATTERN(MatchOr)
    Sequence<Pattern*> patterns;
};

#undef PYTHON_PATTERN

// ---- Statements

#define FOR_EACH_PYTHON_STATEMENT(macro) \
    macro(FunctionDef) \
    macro(ClassDef) \
    macro(Return) \
    macro(Delete) \
    macro(Assign) \
    macro(TypeAlias) \
    macro(AugAssign) \
    macro(AnnAssign) \
    macro(For) \
    macro(While) \
    macro(If) \
    macro(With) \
    macro(Match) \
    macro(Raise) \
    macro(Try) \
    macro(Assert) \
    macro(Import) \
    macro(ImportFrom) \
    macro(Global) \
    macro(Nonlocal) \
    macro(Expr) \
    macro(Pass) \
    macro(Break) \
    macro(Continue)

struct Statement : Node {
    enum class Kind : uint8_t {
#define DECLARE(name) name,
        FOR_EACH_PYTHON_STATEMENT(DECLARE)
#undef DECLARE
    };

    explicit Statement(Kind kind)
        : kind(kind)
    {
    }

    template<typename T> bool is() const { return kind == T::statementKind; }
    template<typename T> T& as()
    {
        ASSERT(is<T>());
        return static_cast<T&>(*this);
    }

    Kind kind;
};

#define PYTHON_STATEMENT(name) \
    static constexpr Kind statementKind = Kind::name; \
    name() \
        : Statement(Kind::name) \
    { \
    }

// AsyncFunctionDef, AsyncFor, AsyncWith and TryStar are these with a flag: they have the same fields.
struct FunctionDef : Statement {
    PYTHON_STATEMENT(FunctionDef)
    bool isAsync { false };
    const Identifier* name { nullptr };
    Arguments* arguments { nullptr };
    Sequence<Statement*> body;
    Sequence<Expression*> decorators;
    Expression* returns { nullptr };
    Sequence<TypeParameter*> typeParameters;
};

struct ClassDef : Statement {
    PYTHON_STATEMENT(ClassDef)
    const Identifier* name { nullptr };
    Sequence<Expression*> bases;
    Sequence<Keyword*> keywords;
    Sequence<Statement*> body;
    Sequence<Expression*> decorators;
    Sequence<TypeParameter*> typeParameters;
};

struct Return : Statement {
    PYTHON_STATEMENT(Return)
    Expression* value { nullptr };
};

struct Delete : Statement {
    PYTHON_STATEMENT(Delete)
    Sequence<Expression*> targets;
};

struct Assign : Statement {
    PYTHON_STATEMENT(Assign)
    Sequence<Expression*> targets;
    Expression* value { nullptr };
};

struct TypeAlias : Statement {
    PYTHON_STATEMENT(TypeAlias)
    Expression* name { nullptr };
    Sequence<TypeParameter*> typeParameters;
    Expression* value { nullptr };
};

struct AugAssign : Statement {
    PYTHON_STATEMENT(AugAssign)
    Expression* target { nullptr };
    BinaryOperator op { BinaryOperator::Add };
    Expression* value { nullptr };
};

struct AnnAssign : Statement {
    PYTHON_STATEMENT(AnnAssign)
    Expression* target { nullptr };
    Expression* annotation { nullptr };
    Expression* value { nullptr };
    bool isSimple { false }; // The target is a name, and not in parentheses.
};

struct For : Statement {
    PYTHON_STATEMENT(For)
    bool isAsync { false };
    Expression* target { nullptr };
    Expression* iterable { nullptr };
    Sequence<Statement*> body;
    Sequence<Statement*> orElse;
};

struct While : Statement {
    PYTHON_STATEMENT(While)
    Expression* test { nullptr };
    Sequence<Statement*> body;
    Sequence<Statement*> orElse;
};

struct If : Statement {
    PYTHON_STATEMENT(If)
    Expression* test { nullptr };
    Sequence<Statement*> body;
    Sequence<Statement*> orElse;
};

struct With : Statement {
    PYTHON_STATEMENT(With)
    bool isAsync { false };
    Sequence<WithItem*> items;
    Sequence<Statement*> body;
};

struct Match : Statement {
    PYTHON_STATEMENT(Match)
    Expression* subject { nullptr };
    Sequence<MatchCase*> cases;
};

struct Raise : Statement {
    PYTHON_STATEMENT(Raise)
    Expression* exception { nullptr };
    Expression* cause { nullptr };
};

struct Try : Statement {
    PYTHON_STATEMENT(Try)
    bool isStar { false }; // except*
    Sequence<Statement*> body;
    Sequence<ExceptHandler*> handlers;
    Sequence<Statement*> orElse;
    Sequence<Statement*> finalBody;
};

struct Assert : Statement {
    PYTHON_STATEMENT(Assert)
    Expression* test { nullptr };
    Expression* message { nullptr };
};

struct Import : Statement {
    PYTHON_STATEMENT(Import)
    Sequence<Alias*> names;
};

struct ImportFrom : Statement {
    PYTHON_STATEMENT(ImportFrom)
    const Identifier* module { nullptr };
    Sequence<Alias*> names;
    unsigned level { 0 };
};

struct Global : Statement {
    PYTHON_STATEMENT(Global)
    Sequence<const Identifier*> names;
};

struct Nonlocal : Statement {
    PYTHON_STATEMENT(Nonlocal)
    Sequence<const Identifier*> names;
};

struct Expr : Statement {
    PYTHON_STATEMENT(Expr)
    Expression* value { nullptr };
};

struct Pass : Statement {
    PYTHON_STATEMENT(Pass)
};

struct Break : Statement {
    PYTHON_STATEMENT(Break)
};

struct Continue : Statement {
    PYTHON_STATEMENT(Continue)
};

#undef PYTHON_STATEMENT

// ---- What a whole source is

struct Module {
    enum class Kind : uint8_t {
        Module, // A file.
        Interactive, // What is typed at a prompt.
        Expression, // What is given to eval().
    };
    Kind kind { Kind::Module };
    Sequence<Statement*> body;
    Expression* expression { nullptr };
};

} } // namespace JSC::Python
