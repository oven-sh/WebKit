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

#include "PythonASDL.h"
#include "PythonAST.h"

namespace JSC { namespace Python {

// Goes through what the parser makes as if it were the tree that a program sees: the classes and the fields of PythonASDL.h, in their order. What is made of it is up to `Sink`, which is derived from
// this and has
//
//     void open(ASTClass)                a node begins
//     void name(ASCIILiteral)            what comes next is this field of it
//     void close(const Node&)            it ends, and this is where it is in the source
//     void close()                       it ends, and is of a class that does not say where
//     void null()                        None
//     void identifier(const Identifier&)
//     void string(ASCIILiteral)
//     void integer(int)
//     void singleton(ASTClass)           Load(), Add() and the like, of which there is one each
//     void constant(Constant&)           the value of a Constant
//     void constant(Constant::Type)      None, True or False
//     void openList(size_t)              a list of so many begins
//     void element()                     what comes next is the next of them
//     void closeList()
//     bool canGoDeeper()                 false to leave off, when something has gone wrong or there is no more room on the stack
template<typename Sink>
class ASTWalker {
public:
    void walk(Module& module)
    {
        switch (module.kind) {
        case Module::Kind::Module:
            open(ASTClass::Module);
            field("body"_s, module.body);
            name("type_ignores"_s);
            sink().openList(0);
            sink().closeList();
            break;
        case Module::Kind::Interactive:
            open(ASTClass::Interactive);
            field("body"_s, module.body);
            break;
        case Module::Kind::Expression:
            open(ASTClass::Expression);
            field("body"_s, module.expression);
            break;
        }
        close();
    }

    void walk(Statement& statement) { value(&statement); }
    void walk(Expression& expression) { value(&expression); }

private:
    Sink& sink() { return static_cast<Sink&>(*this); }

    void open(ASTClass astClass) { sink().open(astClass); }
    void name(ASCIILiteral name) { sink().name(name); }
    void close(const Node& node) { sink().close(node); }
    void close() { sink().close(); }

    void value(const Identifier* identifier)
    {
        if (identifier)
            sink().identifier(*identifier);
        else
            sink().null();
    }

    void value(int number) { sink().integer(number); }
    void value(unsigned number) { sink().integer(number); }
    void value(bool flag) { sink().integer(flag); }

    template<typename T>
    void value(Sequence<T> sequence)
    {
        sink().openList(sequence.size());
        for (auto& item : sequence) {
            sink().element();
            value(item);
        }
        sink().closeList();
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
        sink().null();
    }

    // ---- Enumerations. They are in the order of PythonASDL.h.

    template<typename T>
    void singleton(ASTClass first, T which) { sink().singleton(static_cast<ASTClass>(static_cast<unsigned>(first) + static_cast<unsigned>(which))); }

    void value(ExpressionContext context) { singleton(ASTClass::Load, context); }
    void value(BooleanOperator op) { singleton(ASTClass::And, op); }
    void value(BinaryOperator op) { singleton(ASTClass::Add, op); }
    void value(UnaryOperator op) { singleton(ASTClass::Invert, op); }
    void value(ComparisonOperator op) { singleton(ASTClass::Eq, op); }

    // ---- What is neither a statement nor an expression

    void value(Argument* node)
    {
        if (!node) {
            sink().null();
            return;
        }
        open(ASTClass::arg);
        field("arg"_s, node->name);
        field("annotation"_s, node->annotation);
        nullField("type_comment"_s);
        close(*node);
    }

    void value(Arguments* node)
    {
        open(ASTClass::arguments);
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
        open(ASTClass::keyword);
        field("arg"_s, node->name);
        field("value"_s, node->value);
        close(*node);
    }

    void value(Alias* node)
    {
        open(ASTClass::alias);
        field("name"_s, node->name);
        field("asname"_s, node->asName);
        close(*node);
    }

    void value(WithItem* node)
    {
        open(ASTClass::withitem);
        field("context_expr"_s, node->contextExpression);
        field("optional_vars"_s, node->optionalVariables);
        close();
    }

    void value(Comprehension* node)
    {
        open(ASTClass::comprehension);
        field("target"_s, node->target);
        field("iter"_s, node->iterable);
        field("ifs"_s, node->conditions);
        field("is_async"_s, node->isAsync);
        close();
    }

    void value(ExceptHandler* node)
    {
        open(ASTClass::ExceptHandler);
        field("type"_s, node->type);
        field("name"_s, node->name);
        field("body"_s, node->body);
        close(*node);
    }

    void value(MatchCase* node)
    {
        open(ASTClass::match_case);
        field("pattern"_s, node->pattern);
        field("guard"_s, node->guard);
        field("body"_s, node->body);
        close();
    }

    void value(TypeParameter* node)
    {
        switch (node->kind) {
        case TypeParameter::Kind::TypeVar:
            open(ASTClass::TypeVar);
            field("name"_s, node->name);
            field("bound"_s, node->bound);
            break;
        case TypeParameter::Kind::ParamSpec:
            open(ASTClass::ParamSpec);
            field("name"_s, node->name);
            break;
        case TypeParameter::Kind::TypeVarTuple:
            open(ASTClass::TypeVarTuple);
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
            sink().null();
            return;
        }
        if (!sink().canGoDeeper())
            return;
        switch (expression->kind) {
        case Expression::Kind::BoolOp: {
            auto& node = expression->as<BoolOp>();
            open(ASTClass::BoolOp);
            field("op"_s, node.op);
            field("values"_s, node.values);
            break;
        }
        case Expression::Kind::NamedExpr: {
            auto& node = expression->as<NamedExpr>();
            open(ASTClass::NamedExpr);
            field("target"_s, node.target);
            field("value"_s, node.value);
            break;
        }
        case Expression::Kind::BinOp: {
            auto& node = expression->as<BinOp>();
            open(ASTClass::BinOp);
            field("left"_s, node.left);
            field("op"_s, node.op);
            field("right"_s, node.right);
            break;
        }
        case Expression::Kind::UnaryOp: {
            auto& node = expression->as<UnaryOp>();
            open(ASTClass::UnaryOp);
            field("op"_s, node.op);
            field("operand"_s, node.operand);
            break;
        }
        case Expression::Kind::Lambda: {
            auto& node = expression->as<Lambda>();
            open(ASTClass::Lambda);
            field("args"_s, node.arguments);
            field("body"_s, node.body);
            break;
        }
        case Expression::Kind::IfExp: {
            auto& node = expression->as<IfExp>();
            open(ASTClass::IfExp);
            field("test"_s, node.test);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            break;
        }
        case Expression::Kind::Dict: {
            auto& node = expression->as<Dict>();
            open(ASTClass::Dict);
            field("keys"_s, node.keys);
            field("values"_s, node.values);
            break;
        }
        case Expression::Kind::Set: {
            auto& node = expression->as<Set>();
            open(ASTClass::Set);
            field("elts"_s, node.elements);
            break;
        }
        case Expression::Kind::ListComp: {
            auto& node = expression->as<ListComp>();
            open(ASTClass::ListComp);
            field("elt"_s, node.element);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::SetComp: {
            auto& node = expression->as<SetComp>();
            open(ASTClass::SetComp);
            field("elt"_s, node.element);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::DictComp: {
            auto& node = expression->as<DictComp>();
            open(ASTClass::DictComp);
            field("key"_s, node.key);
            field("value"_s, node.value);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::GeneratorExp: {
            auto& node = expression->as<GeneratorExp>();
            open(ASTClass::GeneratorExp);
            field("elt"_s, node.element);
            field("generators"_s, node.generators);
            break;
        }
        case Expression::Kind::Await:
            open(ASTClass::Await);
            field("value"_s, expression->as<Await>().value);
            break;
        case Expression::Kind::Yield:
            open(ASTClass::Yield);
            field("value"_s, expression->as<Yield>().value);
            break;
        case Expression::Kind::YieldFrom:
            open(ASTClass::YieldFrom);
            field("value"_s, expression->as<YieldFrom>().value);
            break;
        case Expression::Kind::Compare: {
            auto& node = expression->as<Compare>();
            open(ASTClass::Compare);
            field("left"_s, node.left);
            field("ops"_s, node.ops);
            field("comparators"_s, node.comparators);
            break;
        }
        case Expression::Kind::Call: {
            auto& node = expression->as<Call>();
            open(ASTClass::Call);
            field("func"_s, node.function);
            field("args"_s, node.arguments);
            field("keywords"_s, node.keywords);
            break;
        }
        case Expression::Kind::FormattedValue: {
            auto& node = expression->as<FormattedValue>();
            open(ASTClass::FormattedValue);
            field("value"_s, node.value);
            field("conversion"_s, node.conversion);
            field("format_spec"_s, node.formatSpecification);
            break;
        }
        case Expression::Kind::Interpolation: {
            auto& node = expression->as<Interpolation>();
            open(ASTClass::Interpolation);
            field("value"_s, node.value);
            field("str"_s, node.source);
            field("conversion"_s, node.conversion);
            field("format_spec"_s, node.formatSpecification);
            break;
        }
        case Expression::Kind::JoinedStr:
            open(ASTClass::JoinedStr);
            field("values"_s, expression->as<JoinedStr>().values);
            break;
        case Expression::Kind::TemplateStr:
            open(ASTClass::TemplateStr);
            field("values"_s, expression->as<TemplateStr>().values);
            break;
        case Expression::Kind::Constant: {
            auto& node = expression->as<Constant>();
            open(ASTClass::Constant);
            name("value"_s);
            sink().constant(node);
            name("kind"_s);
            if (node.hasUnicodePrefix)
                sink().string("u"_s);
            else
                sink().null();
            break;
        }
        case Expression::Kind::Attribute: {
            auto& node = expression->as<Attribute>();
            open(ASTClass::Attribute);
            field("value"_s, node.value);
            field("attr"_s, node.attribute);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Subscript: {
            auto& node = expression->as<Subscript>();
            open(ASTClass::Subscript);
            field("value"_s, node.value);
            field("slice"_s, node.slice);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Starred: {
            auto& node = expression->as<Starred>();
            open(ASTClass::Starred);
            field("value"_s, node.value);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Name: {
            auto& node = expression->as<Name>();
            open(ASTClass::Name);
            field("id"_s, node.id);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::List: {
            auto& node = expression->as<List>();
            open(ASTClass::List);
            field("elts"_s, node.elements);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Tuple: {
            auto& node = expression->as<Tuple>();
            open(ASTClass::Tuple);
            field("elts"_s, node.elements);
            field("ctx"_s, node.context);
            break;
        }
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            open(ASTClass::Slice);
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
            sink().null();
            return;
        }
        if (!sink().canGoDeeper())
            return;
        switch (pattern->kind) {
        case Pattern::Kind::MatchValue:
            open(ASTClass::MatchValue);
            field("value"_s, pattern->as<MatchValue>().value);
            break;
        case Pattern::Kind::MatchSingleton:
            open(ASTClass::MatchSingleton);
            name("value"_s);
            sink().constant(pattern->as<MatchSingleton>().value);
            break;
        case Pattern::Kind::MatchSequence:
            open(ASTClass::MatchSequence);
            field("patterns"_s, pattern->as<MatchSequence>().patterns);
            break;
        case Pattern::Kind::MatchMapping: {
            auto& node = pattern->as<MatchMapping>();
            open(ASTClass::MatchMapping);
            field("keys"_s, node.keys);
            field("patterns"_s, node.patterns);
            field("rest"_s, node.rest);
            break;
        }
        case Pattern::Kind::MatchClass: {
            auto& node = pattern->as<MatchClass>();
            open(ASTClass::MatchClass);
            field("cls"_s, node.cls);
            field("patterns"_s, node.patterns);
            field("kwd_attrs"_s, node.keywordAttributes);
            field("kwd_patterns"_s, node.keywordPatterns);
            break;
        }
        case Pattern::Kind::MatchStar:
            open(ASTClass::MatchStar);
            field("name"_s, pattern->as<MatchStar>().name);
            break;
        case Pattern::Kind::MatchAs: {
            auto& node = pattern->as<MatchAs>();
            open(ASTClass::MatchAs);
            field("pattern"_s, node.pattern);
            field("name"_s, node.name);
            break;
        }
        case Pattern::Kind::MatchOr:
            open(ASTClass::MatchOr);
            field("patterns"_s, pattern->as<MatchOr>().patterns);
            break;
        }
        close(*pattern);
    }

    // ---- Statements

    void value(Statement* statement)
    {
        if (!sink().canGoDeeper())
            return;
        switch (statement->kind) {
        case Statement::Kind::FunctionDef: {
            auto& node = statement->as<FunctionDef>();
            open(node.isAsync ? ASTClass::AsyncFunctionDef : ASTClass::FunctionDef);
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
            open(ASTClass::ClassDef);
            field("name"_s, node.name);
            field("bases"_s, node.bases);
            field("keywords"_s, node.keywords);
            field("body"_s, node.body);
            field("decorator_list"_s, node.decorators);
            field("type_params"_s, node.typeParameters);
            break;
        }
        case Statement::Kind::Return:
            open(ASTClass::Return);
            field("value"_s, statement->as<Return>().value);
            break;
        case Statement::Kind::Delete:
            open(ASTClass::Delete);
            field("targets"_s, statement->as<Delete>().targets);
            break;
        case Statement::Kind::Assign: {
            auto& node = statement->as<Assign>();
            open(ASTClass::Assign);
            field("targets"_s, node.targets);
            field("value"_s, node.value);
            nullField("type_comment"_s);
            break;
        }
        case Statement::Kind::TypeAlias: {
            auto& node = statement->as<TypeAlias>();
            open(ASTClass::TypeAlias);
            field("name"_s, node.name);
            field("type_params"_s, node.typeParameters);
            field("value"_s, node.value);
            break;
        }
        case Statement::Kind::AugAssign: {
            auto& node = statement->as<AugAssign>();
            open(ASTClass::AugAssign);
            field("target"_s, node.target);
            field("op"_s, node.op);
            field("value"_s, node.value);
            break;
        }
        case Statement::Kind::AnnAssign: {
            auto& node = statement->as<AnnAssign>();
            open(ASTClass::AnnAssign);
            field("target"_s, node.target);
            field("annotation"_s, node.annotation);
            field("value"_s, node.value);
            field("simple"_s, node.isSimple);
            break;
        }
        case Statement::Kind::For: {
            auto& node = statement->as<For>();
            open(node.isAsync ? ASTClass::AsyncFor : ASTClass::For);
            field("target"_s, node.target);
            field("iter"_s, node.iterable);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            nullField("type_comment"_s);
            break;
        }
        case Statement::Kind::While: {
            auto& node = statement->as<While>();
            open(ASTClass::While);
            field("test"_s, node.test);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            break;
        }
        case Statement::Kind::If: {
            auto& node = statement->as<If>();
            open(ASTClass::If);
            field("test"_s, node.test);
            field("body"_s, node.body);
            field("orelse"_s, node.orElse);
            break;
        }
        case Statement::Kind::With: {
            auto& node = statement->as<With>();
            open(node.isAsync ? ASTClass::AsyncWith : ASTClass::With);
            field("items"_s, node.items);
            field("body"_s, node.body);
            nullField("type_comment"_s);
            break;
        }
        case Statement::Kind::Match: {
            auto& node = statement->as<Match>();
            open(ASTClass::Match);
            field("subject"_s, node.subject);
            field("cases"_s, node.cases);
            break;
        }
        case Statement::Kind::Raise: {
            auto& node = statement->as<Raise>();
            open(ASTClass::Raise);
            field("exc"_s, node.exception);
            field("cause"_s, node.cause);
            break;
        }
        case Statement::Kind::Try: {
            auto& node = statement->as<Try>();
            open(node.isStar ? ASTClass::TryStar : ASTClass::Try);
            field("body"_s, node.body);
            field("handlers"_s, node.handlers);
            field("orelse"_s, node.orElse);
            field("finalbody"_s, node.finalBody);
            break;
        }
        case Statement::Kind::Assert: {
            auto& node = statement->as<Assert>();
            open(ASTClass::Assert);
            field("test"_s, node.test);
            field("msg"_s, node.message);
            break;
        }
        case Statement::Kind::Import:
            open(ASTClass::Import);
            field("names"_s, statement->as<Import>().names);
            break;
        case Statement::Kind::ImportFrom: {
            auto& node = statement->as<ImportFrom>();
            open(ASTClass::ImportFrom);
            field("module"_s, node.module);
            field("names"_s, node.names);
            field("level"_s, node.level);
            break;
        }
        case Statement::Kind::Global:
            open(ASTClass::Global);
            field("names"_s, statement->as<Global>().names);
            break;
        case Statement::Kind::Nonlocal:
            open(ASTClass::Nonlocal);
            field("names"_s, statement->as<Nonlocal>().names);
            break;
        case Statement::Kind::Expr:
            open(ASTClass::Expr);
            field("value"_s, statement->as<Expr>().value);
            break;
        case Statement::Kind::Pass:
            open(ASTClass::Pass);
            break;
        case Statement::Kind::Break:
            open(ASTClass::Break);
            break;
        case Statement::Kind::Continue:
            open(ASTClass::Continue);
            break;
        }
        close(*statement);
    }
};

} } // namespace JSC::Python
