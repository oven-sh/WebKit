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
#include "PythonASTValidator.h"
#include "PythonArena.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace Python {

// Makes what the parser makes out of a tree that is described as a program sees it: the classes and the fields of PythonASDL.h. It is PythonASTWalker.h the other way about, and CPython's obj2ast_ functions
// together with the _PyAST_ functions that they call. Where the tree comes from is up to `Source`, which has
//
//     using Value = ...                                          a node, a list, or whatever else can be the value of a field
//     static constexpr bool attributesComeFirst                  whether it is written out, so that things have to be asked for in the order that they are written in. What class a node is of comes first, and
//                                                                then where it is, which is got with bool positions(ASTClass, Node&) and not as if it were so many fields
//     bool isNone(Value)
//     ASTClass constructorOf(Value, ASTClass sum)                which of what a sum is the sum of it is
//     void leave(Value, Node&)                                   its fields have all been asked for. What part of the source it is, if it is part of any, is filled in
//     unsigned attributeStart()                                  Attribute::attributeStart of the node whose fields are being asked for
//     std::optional<Value> field(Value, ASTClass, const ASDLField&)   nothing if there is no such field, or it has failed
//     bool forEachElement(Value, ASTClass, const ASDLField&, function)   calls the function with each of a list
//     const Identifier* identifier(Value), Text string(Value), std::optional<int> integer(Value), Constant* constant(Value)
//     bool canGoDeeper(ASTClass owner)                           whether there is room on the stack for a field of one of these
//     void fail(ASTError::Kind, String&&), bool hasFailed()
//
// Nothing is looked into here but that what has to be there is there. The rest is for PythonASTValidator.cpp.
template<typename Source>
class ASTBuilder {
public:
    using Value = typename Source::Value;

    ASTBuilder(Arena& arena, Source& source)
        : m_arena(arena)
        , m_source(source)
    {
    }

    Module* buildModule(Value value) { return static_cast<Module*>(build(value, ASTClass::mod)); }
    Statement* buildStatement(Value value) { return static_cast<Statement*>(build(value, ASTClass::stmt)); }
    Expression* buildExpression(Value value) { return static_cast<Expression*>(build(value, ASTClass::expr)); }

private:
    static constexpr unsigned maximumFields = 7;

    // The value of a field.
    struct Field {
        void* node { nullptr };
        std::span<void*> nodes;
        std::span<ASTClass> singletons; // cmpop*
        const Identifier* identifier { nullptr };
        std::span<const Identifier*> identifiers;
        Text text;
        Constant* constant { nullptr };
        int integer { 0 };
        ASTClass singleton { ASTClass::AST }; // Load, Add and the like. AST if there is none.
    };

    static bool isSimpleSum(ASTClass type)
    {
        switch (type) {
        case ASTClass::expr_context:
        case ASTClass::boolop:
        case ASTClass::operator_:
        case ASTClass::unaryop:
        case ASTClass::cmpop:
            return true;
        default:
            return false;
        }
    }

    // Null for None, and if it has failed.
    void* build(Value value, ASTClass type)
    {
        bool isSum = descriptionOf(type).kind == ASDLClass::Kind::Sum;
        if (isSum && m_source.isNone(value))
            return nullptr;

        Node where;
        // What a sum says of where it is, is asked for before which of them it is.
        std::span<const ASDLField> attributes = descriptionOf(type).attributes;
        bool attributesComeFirst = isSum || Source::attributesComeFirst;
        ASTClass constructor = type;
        unsigned attributeStart = 0;
        if constexpr (Source::attributesComeFirst) {
            constructor = m_source.constructorOf(value, type);
            if (m_source.hasFailed() || !m_source.positions(constructor, where))
                return nullptr;
            attributeStart = m_source.attributeStart();
        } else {
            if (isSum) {
                if (!readAttributes(value, type, attributes, where))
                    return nullptr;
                constructor = m_source.constructorOf(value, type);
            }
            if (m_source.hasFailed())
                return nullptr;
        }

        const ASDLClass& description = descriptionOf(constructor);
        std::array<Field, maximumFields> fields;
        for (size_t i = 0; i < description.fields.size(); ++i) {
            if (!readField(value, constructor, description.fields[i], fields[i]))
                return nullptr;
        }
        if (!attributesComeFirst && !readAttributes(value, type, attributes, where))
            return nullptr;
        m_source.leave(value, where);
        if (m_source.hasFailed())
            return nullptr;

        // What the _PyAST_ functions ask.
        for (size_t i = 0; i < description.fields.size(); ++i) {
            const ASDLField& field = description.fields[i];
            if (field.quantifier != ASDLField::Quantifier::One || field.type == ASTClass::int_)
                continue;
            bool isThere = field.type == ASTClass::identifier ? !!fields[i].identifier : field.type == ASTClass::constant ? !!fields[i].constant : field.type == ASTClass::string ? !!fields[i].text
                : isSimpleSum(field.type) ? fields[i].singleton != ASTClass::AST : !!fields[i].node;
            if (!isThere) {
                m_source.fail(ASTError::Kind::ValueError, makeString("field '"_s, field.name, "' is required for "_s, description.name));
                return nullptr;
            }
        }
        void* result = construct(constructor, fields, where);
        if (constructor == ASTClass::Attribute && result)
            static_cast<Expression*>(result)->template as<Attribute>().attributeStart = attributeStart;
        return result;
    }

    bool readAttributes(Value value, ASTClass owner, std::span<const ASDLField> attributes, Node& where)
    {
        if (attributes.empty())
            return true;
        ASSERT(attributes.size() == 4);
        std::array<int, 4> numbers { };
        for (size_t i = 0; i < 4; ++i) {
            auto attribute = m_source.field(value, owner, attributes[i]);
            if (m_source.hasFailed())
                return false;
            if (!attribute || (attributes[i].quantifier == ASDLField::Quantifier::Optional && m_source.isNone(*attribute))) {
                // Where it ends, if it does not say, is where it begins.
                ASSERT(attributes[i].quantifier == ASDLField::Quantifier::Optional);
                numbers[i] = numbers[i - 2];
                continue;
            }
            auto number = m_source.integer(*attribute);
            if (!number)
                return false;
            numbers[i] = *number;
        }
        where.line = numbers[0];
        where.column = numbers[1];
        where.endLine = numbers[2];
        where.endColumn = numbers[3];
        return true;
    }

    bool readOne(Value value, ASTClass owner, ASTClass type, Field& result)
    {
        if (!m_source.canGoDeeper(owner))
            return false;
        switch (type) {
        case ASTClass::identifier:
            result.identifier = m_source.identifier(value);
            break;
        case ASTClass::string:
            result.text = m_source.string(value);
            break;
        case ASTClass::int_:
            if (auto number = m_source.integer(value))
                result.integer = *number;
            break;
        case ASTClass::constant:
            result.constant = m_source.constant(value);
            break;
        default:
            if (isSimpleSum(type))
                result.singleton = m_source.constructorOf(value, type);
            else
                result.node = build(value, type);
            break;
        }
        return !m_source.hasFailed();
    }

    bool readField(Value owner, ASTClass ownerClass, const ASDLField& field, Field& result)
    {
        auto value = m_source.field(owner, ownerClass, field);
        if (m_source.hasFailed())
            return false;
        switch (field.quantifier) {
        case ASDLField::Quantifier::One:
            ASSERT(value);
            return readOne(*value, ownerClass, field.type, result);
        case ASDLField::Quantifier::Optional:
            if (!value || m_source.isNone(*value))
                return true;
            return readOne(*value, ownerClass, field.type, result);
        case ASDLField::Quantifier::Sequence:
        case ASDLField::Quantifier::SequenceOfOptional:
            break;
        }
        // A list that is not there is empty.
        if (!value)
            return true;
        Vector<void*, 16> nodes;
        Vector<ASTClass, 4> singletons;
        Vector<const Identifier*, 8> identifiers;
        bool succeeded = m_source.forEachElement(*value, ownerClass, field, [&] (Value element) {
            Field one;
            if (!readOne(element, ownerClass, field.type, one))
                return false;
            if (field.type == ASTClass::identifier)
                identifiers.append(one.identifier);
            else if (isSimpleSum(field.type))
                singletons.append(one.singleton);
            else
                nodes.append(one.node);
            return true;
        });
        if (!succeeded || m_source.hasFailed())
            return false;
        result.nodes = m_arena.copy(nodes);
        result.singletons = m_arena.copy(singletons);
        result.identifiers = m_arena.copy(identifiers);
        return true;
    }

    // ---- Making what the parser makes

    template<typename T>
    static Sequence<T*> sequenceOf(const Field& field)
    {
        return { reinterpret_cast<T**>(field.nodes.data()), field.nodes.size() };
    }

    template<typename T>
    static T enumerationOf(const Field& field, ASTClass first) { return static_cast<T>(static_cast<unsigned>(field.singleton) - static_cast<unsigned>(first)); }

    static ExpressionContext contextOf(const Field& field) { return enumerationOf<ExpressionContext>(field, ASTClass::Load); }
    static Expression* expressionOf(const Field& field) { return static_cast<Expression*>(field.node); }
    static Pattern* patternOf(const Field& field) { return static_cast<Pattern*>(field.node); }
    static Sequence<Expression*> expressionsOf(const Field& field) { return sequenceOf<Expression>(field); }
    static Sequence<Statement*> statementsOf(const Field& field) { return sequenceOf<Statement>(field); }
    static Sequence<Pattern*> patternsOf(const Field& field) { return sequenceOf<Pattern>(field); }

    template<typename T>
    T* make(const Node& where)
    {
        T* node = m_arena.create<T>();
        static_cast<Node&>(*node) = where;
        return node;
    }

    void* construct(ASTClass constructor, std::array<Field, maximumFields>& f, const Node& where)
    {
        switch (constructor) {
        // ---- mod
        case ASTClass::Module:
        case ASTClass::Interactive: {
            auto* module = m_arena.create<Module>();
            module->kind = constructor == ASTClass::Module ? Module::Kind::Module : Module::Kind::Interactive;
            module->body = statementsOf(f[0]);
            if (constructor == ASTClass::Module)
                module->typeIgnores = sequenceOf<TypeIgnore>(f[1]);
            return module;
        }
        case ASTClass::Expression: {
            auto* module = m_arena.create<Module>();
            module->kind = Module::Kind::Expression;
            module->expression = expressionOf(f[0]);
            return module;
        }

        // ---- stmt
        case ASTClass::FunctionDef:
        case ASTClass::AsyncFunctionDef: {
            auto* node = make<FunctionDef>(where);
            node->isAsync = constructor == ASTClass::AsyncFunctionDef;
            node->name = f[0].identifier;
            node->arguments = static_cast<Arguments*>(f[1].node);
            node->body = statementsOf(f[2]);
            node->decorators = expressionsOf(f[3]);
            node->returns = expressionOf(f[4]);
            node->typeComment = f[5].text;
            node->typeParameters = sequenceOf<TypeParameter>(f[6]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::ClassDef: {
            auto* node = make<ClassDef>(where);
            node->name = f[0].identifier;
            node->bases = expressionsOf(f[1]);
            node->keywords = sequenceOf<Keyword>(f[2]);
            node->body = statementsOf(f[3]);
            node->decorators = expressionsOf(f[4]);
            node->typeParameters = sequenceOf<TypeParameter>(f[5]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Return: {
            auto* node = make<Return>(where);
            node->value = expressionOf(f[0]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Delete: {
            auto* node = make<Delete>(where);
            node->targets = expressionsOf(f[0]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Assign: {
            auto* node = make<Assign>(where);
            node->targets = expressionsOf(f[0]);
            node->value = expressionOf(f[1]);
            node->typeComment = f[2].text;
            return static_cast<Statement*>(node);
        }
        case ASTClass::TypeAlias: {
            auto* node = make<TypeAlias>(where);
            node->name = expressionOf(f[0]);
            node->typeParameters = sequenceOf<TypeParameter>(f[1]);
            node->value = expressionOf(f[2]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::AugAssign: {
            auto* node = make<AugAssign>(where);
            node->target = expressionOf(f[0]);
            node->op = enumerationOf<BinaryOperator>(f[1], ASTClass::Add);
            node->value = expressionOf(f[2]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::AnnAssign: {
            auto* node = make<AnnAssign>(where);
            node->target = expressionOf(f[0]);
            node->annotation = expressionOf(f[1]);
            node->value = expressionOf(f[2]);
            node->isSimple = f[3].integer;
            return static_cast<Statement*>(node);
        }
        case ASTClass::For:
        case ASTClass::AsyncFor: {
            auto* node = make<For>(where);
            node->isAsync = constructor == ASTClass::AsyncFor;
            node->target = expressionOf(f[0]);
            node->iterable = expressionOf(f[1]);
            node->body = statementsOf(f[2]);
            node->orElse = statementsOf(f[3]);
            node->typeComment = f[4].text;
            return static_cast<Statement*>(node);
        }
        case ASTClass::While: {
            auto* node = make<While>(where);
            node->test = expressionOf(f[0]);
            node->body = statementsOf(f[1]);
            node->orElse = statementsOf(f[2]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::If: {
            auto* node = make<If>(where);
            node->test = expressionOf(f[0]);
            node->body = statementsOf(f[1]);
            node->orElse = statementsOf(f[2]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::With:
        case ASTClass::AsyncWith: {
            auto* node = make<With>(where);
            node->isAsync = constructor == ASTClass::AsyncWith;
            node->items = sequenceOf<WithItem>(f[0]);
            node->body = statementsOf(f[1]);
            node->typeComment = f[2].text;
            return static_cast<Statement*>(node);
        }
        case ASTClass::Match: {
            auto* node = make<Match>(where);
            node->subject = expressionOf(f[0]);
            node->cases = sequenceOf<MatchCase>(f[1]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Raise: {
            auto* node = make<Raise>(where);
            node->exception = expressionOf(f[0]);
            node->cause = expressionOf(f[1]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Try:
        case ASTClass::TryStar: {
            auto* node = make<Try>(where);
            node->isStar = constructor == ASTClass::TryStar;
            node->body = statementsOf(f[0]);
            node->handlers = sequenceOf<ExceptHandler>(f[1]);
            node->orElse = statementsOf(f[2]);
            node->finalBody = statementsOf(f[3]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Assert: {
            auto* node = make<Assert>(where);
            node->test = expressionOf(f[0]);
            node->message = expressionOf(f[1]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Import: {
            auto* node = make<Import>(where);
            node->names = sequenceOf<Alias>(f[0]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::ImportFrom: {
            auto* node = make<ImportFrom>(where);
            node->module = f[0].identifier;
            node->names = sequenceOf<Alias>(f[1]);
            node->level = f[2].integer;
            return static_cast<Statement*>(node);
        }
        case ASTClass::Global: {
            auto* node = make<Global>(where);
            node->names = f[0].identifiers;
            return static_cast<Statement*>(node);
        }
        case ASTClass::Nonlocal: {
            auto* node = make<Nonlocal>(where);
            node->names = f[0].identifiers;
            return static_cast<Statement*>(node);
        }
        case ASTClass::Expr: {
            auto* node = make<Expr>(where);
            node->value = expressionOf(f[0]);
            return static_cast<Statement*>(node);
        }
        case ASTClass::Pass:
            return static_cast<Statement*>(make<Pass>(where));
        case ASTClass::Break:
            return static_cast<Statement*>(make<Break>(where));
        case ASTClass::Continue:
            return static_cast<Statement*>(make<Continue>(where));

        // ---- expr
        case ASTClass::BoolOp: {
            auto* node = make<BoolOp>(where);
            node->op = enumerationOf<BooleanOperator>(f[0], ASTClass::And);
            node->values = expressionsOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::NamedExpr: {
            auto* node = make<NamedExpr>(where);
            node->target = expressionOf(f[0]);
            node->value = expressionOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::BinOp: {
            auto* node = make<BinOp>(where);
            node->left = expressionOf(f[0]);
            node->op = enumerationOf<BinaryOperator>(f[1], ASTClass::Add);
            node->right = expressionOf(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::UnaryOp: {
            auto* node = make<UnaryOp>(where);
            node->op = enumerationOf<UnaryOperator>(f[0], ASTClass::Invert);
            node->operand = expressionOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Lambda: {
            auto* node = make<Lambda>(where);
            node->arguments = static_cast<Arguments*>(f[0].node);
            node->body = expressionOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::IfExp: {
            auto* node = make<IfExp>(where);
            node->test = expressionOf(f[0]);
            node->body = expressionOf(f[1]);
            node->orElse = expressionOf(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Dict: {
            auto* node = make<Dict>(where);
            node->keys = expressionsOf(f[0]);
            node->values = expressionsOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Set: {
            auto* node = make<Set>(where);
            node->elements = expressionsOf(f[0]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::ListComp: {
            auto* node = make<ListComp>(where);
            node->element = expressionOf(f[0]);
            node->generators = sequenceOf<Comprehension>(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::SetComp: {
            auto* node = make<SetComp>(where);
            node->element = expressionOf(f[0]);
            node->generators = sequenceOf<Comprehension>(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::DictComp: {
            auto* node = make<DictComp>(where);
            node->key = expressionOf(f[0]);
            node->value = expressionOf(f[1]);
            node->generators = sequenceOf<Comprehension>(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::GeneratorExp: {
            auto* node = make<GeneratorExp>(where);
            node->element = expressionOf(f[0]);
            node->generators = sequenceOf<Comprehension>(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Await: {
            auto* node = make<Await>(where);
            node->value = expressionOf(f[0]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Yield: {
            auto* node = make<Yield>(where);
            node->value = expressionOf(f[0]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::YieldFrom: {
            auto* node = make<YieldFrom>(where);
            node->value = expressionOf(f[0]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Compare: {
            auto* node = make<Compare>(where);
            node->left = expressionOf(f[0]);
            Vector<ComparisonOperator, 4> ops;
            for (ASTClass op : f[1].singletons)
                ops.append(static_cast<ComparisonOperator>(static_cast<unsigned>(op) - static_cast<unsigned>(ASTClass::Eq)));
            node->ops = m_arena.copy(ops);
            node->comparators = expressionsOf(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Call: {
            auto* node = make<Call>(where);
            node->function = expressionOf(f[0]);
            node->arguments = expressionsOf(f[1]);
            node->keywords = sequenceOf<Keyword>(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::FormattedValue: {
            auto* node = make<FormattedValue>(where);
            node->value = expressionOf(f[0]);
            node->conversion = f[1].integer;
            node->formatSpecification = expressionOf(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Interpolation: {
            auto* node = make<Interpolation>(where);
            node->value = expressionOf(f[0]);
            node->source = f[1].constant;
            node->conversion = f[2].integer;
            node->formatSpecification = expressionOf(f[3]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::JoinedStr: {
            auto* node = make<JoinedStr>(where);
            node->values = expressionsOf(f[0]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::TemplateStr: {
            auto* node = make<TemplateStr>(where);
            node->values = expressionsOf(f[0]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Constant: {
            Constant* node = f[0].constant;
            static_cast<Node&>(*node) = where;
            node->kind = f[1].text;
            node->hasUnicodePrefix = node->kind && !node->kind.isBytes && *node->kind.text == "u"_s;
            return static_cast<Expression*>(node);
        }
        case ASTClass::Attribute: {
            auto* node = make<Attribute>(where);
            node->value = expressionOf(f[0]);
            node->attribute = f[1].identifier;
            node->context = contextOf(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Subscript: {
            auto* node = make<Subscript>(where);
            node->value = expressionOf(f[0]);
            node->slice = expressionOf(f[1]);
            node->context = contextOf(f[2]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Starred: {
            auto* node = make<Starred>(where);
            node->value = expressionOf(f[0]);
            node->context = contextOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Name: {
            auto* node = make<Name>(where);
            node->id = f[0].identifier;
            node->context = contextOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::List: {
            auto* node = make<List>(where);
            node->elements = expressionsOf(f[0]);
            node->context = contextOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Tuple: {
            auto* node = make<Tuple>(where);
            node->elements = expressionsOf(f[0]);
            node->context = contextOf(f[1]);
            return static_cast<Expression*>(node);
        }
        case ASTClass::Slice: {
            auto* node = make<Slice>(where);
            node->lower = expressionOf(f[0]);
            node->upper = expressionOf(f[1]);
            node->step = expressionOf(f[2]);
            return static_cast<Expression*>(node);
        }

        // ---- What is neither
        case ASTClass::comprehension: {
            auto* node = m_arena.create<Comprehension>();
            node->target = expressionOf(f[0]);
            node->iterable = expressionOf(f[1]);
            node->conditions = expressionsOf(f[2]);
            node->isAsync = f[3].integer;
            return node;
        }
        case ASTClass::ExceptHandler: {
            auto* node = make<ExceptHandler>(where);
            node->type = expressionOf(f[0]);
            node->name = f[1].identifier;
            node->body = statementsOf(f[2]);
            return node;
        }
        case ASTClass::arguments: {
            auto* node = m_arena.create<Arguments>();
            node->positionalOnly = sequenceOf<Argument>(f[0]);
            node->positional = sequenceOf<Argument>(f[1]);
            node->variadic = static_cast<Argument*>(f[2].node);
            node->keywordOnly = sequenceOf<Argument>(f[3]);
            node->keywordDefaults = expressionsOf(f[4]);
            node->keywordVariadic = static_cast<Argument*>(f[5].node);
            node->defaults = expressionsOf(f[6]);
            return node;
        }
        case ASTClass::arg: {
            auto* node = make<Argument>(where);
            node->name = f[0].identifier;
            node->annotation = expressionOf(f[1]);
            node->typeComment = f[2].text;
            return node;
        }
        case ASTClass::keyword: {
            auto* node = make<Keyword>(where);
            node->name = f[0].identifier;
            node->value = expressionOf(f[1]);
            return node;
        }
        case ASTClass::alias: {
            auto* node = make<Alias>(where);
            node->name = f[0].identifier;
            node->asName = f[1].identifier;
            return node;
        }
        case ASTClass::withitem: {
            auto* node = m_arena.create<WithItem>();
            node->contextExpression = expressionOf(f[0]);
            node->optionalVariables = expressionOf(f[1]);
            return node;
        }
        case ASTClass::match_case: {
            auto* node = m_arena.create<MatchCase>();
            node->pattern = patternOf(f[0]);
            node->guard = expressionOf(f[1]);
            node->body = statementsOf(f[2]);
            return node;
        }

        // ---- pattern
        case ASTClass::MatchValue: {
            auto* node = make<MatchValue>(where);
            node->value = expressionOf(f[0]);
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchSingleton: {
            auto* node = make<MatchSingleton>(where);
            node->value = f[0].constant->type;
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchSequence: {
            auto* node = make<MatchSequence>(where);
            node->patterns = patternsOf(f[0]);
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchMapping: {
            auto* node = make<MatchMapping>(where);
            node->keys = expressionsOf(f[0]);
            node->patterns = patternsOf(f[1]);
            node->rest = f[2].identifier;
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchClass: {
            auto* node = make<MatchClass>(where);
            node->cls = expressionOf(f[0]);
            node->patterns = patternsOf(f[1]);
            node->keywordAttributes = f[2].identifiers;
            node->keywordPatterns = patternsOf(f[3]);
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchStar: {
            auto* node = make<MatchStar>(where);
            node->name = f[0].identifier;
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchAs: {
            auto* node = make<MatchAs>(where);
            node->pattern = patternOf(f[0]);
            node->name = f[1].identifier;
            return static_cast<Pattern*>(node);
        }
        case ASTClass::MatchOr: {
            auto* node = make<MatchOr>(where);
            node->patterns = patternsOf(f[0]);
            return static_cast<Pattern*>(node);
        }

        // ---- type_param
        case ASTClass::TypeVar:
        case ASTClass::ParamSpec:
        case ASTClass::TypeVarTuple: {
            auto* node = make<TypeParameter>(where);
            node->kind = constructor == ASTClass::TypeVar ? TypeParameter::Kind::TypeVar : constructor == ASTClass::ParamSpec ? TypeParameter::Kind::ParamSpec : TypeParameter::Kind::TypeVarTuple;
            node->name = f[0].identifier;
            node->bound = constructor == ASTClass::TypeVar ? expressionOf(f[1]) : nullptr;
            node->defaultValue = expressionOf(f[constructor == ASTClass::TypeVar ? 2 : 1]);
            return node;
        }

        case ASTClass::TypeIgnore: {
            auto* node = m_arena.create<TypeIgnore>();
            node->line = f[0].integer;
            node->tag = f[1].text;
            return node;
        }

        default:
            break;
        }
        m_source.fail(ASTError::Kind::SystemError, "impossible node"_s);
        return nullptr;
    }

    Arena& m_arena;
    Source& m_source;
};

} } // namespace JSC::Python
