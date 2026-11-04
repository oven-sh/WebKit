/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCompilerUnit.h"

#include "JSCInlines.h"
#include "PyRuntime.h"
#include "PyTuple.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

// Functions, classes and what comes with them: decorators, defaults, annotations, type parameters.

RegisterID* Unit::emitMakeFunction(RegisterID* dst, UnlinkedFunctionExecutable* executable, Scope* scope, const FunctionParts& parts)
{
    unsigned index = g.addPythonFunction(executable);
    noteCode(index);
    return emitMakeFunction(dst, index, scope, parts);
}

RegisterID* Unit::emitMakeFunction(RegisterID* dst, unsigned index, Scope* scope, const FunctionParts& parts)
{
    Vector<Name> freeVariables = freeVariablesOf(scope);
    Arguments arguments(*this, 4 + freeVariables.size());
    load(arguments[0], jsNumber(index));
    g.move(arguments[1], parts.defaults ? parts.defaults.get() : emptyValue());
    g.move(arguments[2], parts.kwDefaults ? parts.kwDefaults.get() : emptyValue());
    g.move(arguments[3], parts.annotate ? parts.annotate.get() : emptyValue());
    for (unsigned i = 0; i < freeVariables.size(); ++i)
        g.move(arguments[4 + i], cellFor(freeVariables[i]));
    return callRuntime(dst, RuntimeFunction::MakeFunction, arguments);
}

void Unit::emitDecorators(Seq<Expr*> decorators, Vector<Reg, 4>& registers)
{
    for (Expr* decorator : decorators) {
        Reg reg = newTemporary();
        expr(decorator, reg.get());
        registers.append(WTF::move(reg));
    }
}

RegisterID* Unit::applyDecorators(Seq<Expr*> decorators, Vector<Reg, 4>& registers, RegisterID* value)
{
    for (unsigned i = decorators.size(); i--;)
        emitSimpleCall(value, registers[i].get(), { value }, decorators[i]->range);
    return value;
}

void Unit::emitDefaults(AST::Arguments* arguments, FunctionParts& parts, const Range& range)
{
    if (!arguments->defaults.isEmpty()) {
        parts.defaults = newTemporary();
        // The tuple is of the line the definition begins on: all there is, if it is a constant.
        std::optional<SameLine> sameLine;
        if (!m_sameLineDepth && areConstants(arguments->defaults)) {
            line(range);
            sameLine.emplace(*this);
        }
        sequence(SequenceKind::Tuple, arguments->defaults, parts.defaults.get(), range);
    }
    unsigned count = 0;
    for (Expr* value : arguments->kwDefaults)
        count += !!value;
    if (!count)
        return;
    parts.kwDefaults = newTemporary();
    Arguments items(*this, count * 2);
    unsigned next = 0;
    for (unsigned i = 0; i < arguments->kwonlyargs.size(); ++i) {
        if (!arguments->kwDefaults[i])
            continue;
        line(range);
        g.move(items[next++], nameConstant(mangle(arguments->kwonlyargs[i]->arg)));
        expr(arguments->kwDefaults[i], items[next++]);
    }
    callRuntime(parts.kwDefaults.get(), RuntimeFunction::BuildMap, items);
}

void Unit::emitFunctionAnnotations(AST::Arguments* arguments, Expr* returns, const Range& range, FunctionParts& parts)
{
    Scope* scope = c.m_symbols->scopeFor(arguments);
    if (!scope || !scope->usesAnnotations)
        return;

    struct Annotation {
        Name name;
        Expr* value;
    };
    Vector<Annotation, 8> annotations;
    auto add = [&](Arg* parameter) {
        if (parameter && parameter->annotation)
            annotations.append({ parameter->arg, parameter->annotation });
    };
    for (Arg* parameter : arguments->posonlyargs)
        add(parameter);
    for (Arg* parameter : arguments->args)
        add(parameter);
    add(arguments->vararg);
    for (Arg* parameter : arguments->kwonlyargs)
        add(parameter);
    add(arguments->kwarg);
    if (returns)
        annotations.append({ c.name("return"_s), returns });

    CodeSpec spec { UnitKind::Annotations, scope, scope->name, range, range.lineno };
    spec.positionalOnlyCount = 1;
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        unit.emitAnnotationsPrologue(range);
        Arguments items(unit, annotations.size() * 2);
        unsigned next = 0;
        for (auto& annotation : annotations) {
            unit.line(range.lineno);
            unit.g.move(items[next++], unit.nameConstant(unit.mangle(annotation.name)));
            RegisterID* value = items[next++];
            if (c.m_future.annotations) {
                unit.line(annotation.value->range.lineno);
                unit.g.move(value, unit.stringConstant(annotationText(annotation.value)));
            } else if (auto* starred = annotation.value->tryAs<Starred>()) {
                // *args: *Ts
                unit.expr(starred->value, value);
                unit.loc(range);
                unit.g.emitPyUnpack(value, value, 1, -1);
            } else
                unit.expr(annotation.value, value);
        }
        unit.line(range.lineno);
        Reg result = unit.newTemporary();
        unit.callRuntime(result.get(), RuntimeFunction::BuildMap, items);
        unit.g.emitReturn(result.get());
    });
    executable->pythonCodeInfo()->parameterNames[0] = c.identifier("format"_s);

    parts.annotate = newTemporary();
    emitMakeFunction(parts.annotate.get(), executable, scope, { });
}

RegisterID* Unit::emitFunctionObject(RegisterID* dst, FunctionDef* node, int firstLineNumber, FunctionParts& parts)
{
    Scope* scope = c.m_symbols->scopeFor(node);
    CodeSpec spec { node->isAsync() ? UnitKind::AsyncFunction : UnitKind::Function, scope, node->name, node->range, firstLineNumber };
    spec.arguments = node->args;
    if (scope->hasDocstring)
        spec.docstring = docstringOf(node->body);
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        unit.compileFunctionBody(node->body);
    });
    loc(node->range);
    return emitMakeFunction(dst, executable, scope, parts);
}

void Unit::functionDefinition(FunctionDef* node)
{
    Vector<Reg, 4> decorators;
    emitDecorators(node->decoratorList, decorators);
    int firstLineNumber = node->decoratorList.isEmpty() ? node->range.lineno : node->decoratorList[0]->range.lineno;

    FunctionParts parts;
    emitDefaults(node->args, parts, node->range);

    Reg function = newTemporary();
    if (node->typeParams.isEmpty()) {
        emitFunctionAnnotations(node->args, node->returns, node->range, parts);
        emitFunctionObject(function.get(), node, firstLineNumber, parts);
    } else {
        // def f[T](...): the function is made in a scope of its own, where the type parameters are variables.
        Scope* scope = c.m_symbols->scopeFor(node->typeParams.begin());
        CodeSpec spec { UnitKind::Annotations, scope, c.name(makeString("<generic parameters of "_s, node->name->string(), '>')), node->range, firstLineNumber };
        auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
            Reg typeParams = unit.newTemporary();
            unit.emitTypeParams(typeParams.get(), node->typeParams);
            FunctionParts inner;
            unsigned parameter = 0;
            if (parts.defaults)
                inner.defaults = unit.g.pythonParameter(parameter++);
            if (parts.kwDefaults)
                inner.kwDefaults = unit.g.pythonParameter(parameter++);
            unit.emitFunctionAnnotations(node->args, node->returns, node->range, inner);
            Reg result = unit.newTemporary();
            unit.emitFunctionObject(result.get(), node, firstLineNumber, inner);
            unit.callRuntime(result.get(), RuntimeFunction::SetFunctionTypeParams, { result.get(), typeParams.get() });
            unit.g.emitReturn(result.get());
        });
        emitMakeFunction(function.get(), executable, scope, { });
        if (parts.defaults && parts.kwDefaults)
            emitSimpleCall(function.get(), function.get(), { parts.defaults.get(), parts.kwDefaults.get() }, node->range);
        else if (parts.defaults || parts.kwDefaults)
            emitSimpleCall(function.get(), function.get(), { parts.defaults ? parts.defaults.get() : parts.kwDefaults.get() }, node->range);
        else
            emitSimpleCall(function.get(), function.get(), { }, node->range);
    }

    applyDecorators(node->decoratorList, decorators, function.get());
    line(node->range);
    storeName(node->name, function.get(), node->range);
}

RegisterID* Unit::lambda(Lambda* node, RegisterID* dst)
{
    FunctionParts parts;
    emitDefaults(node->args, parts, node->range);
    Scope* scope = c.m_symbols->scopeFor(node);
    CodeSpec spec { UnitKind::Lambda, scope, names().lambda, node->range, node->range.lineno };
    spec.arguments = node->args;
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        Reg value = unit.expr(node->body);
        unit.g.emitReturn(value.get());
    });
    Reg result = g.finalDestination(dst);
    loc(node->range);
    return emitMakeFunction(result.get(), executable, scope, parts);
}

// ---- Classes ----

void Unit::compileClassBody(ClassDef* node, int firstLineNumber)
{
    Range start { firstLineNumber, 0, firstLineNumber, 0 };

    {
        Reg moduleName = loadName(c.name("__name__"_s), nullptr, start);
        storeName(c.name("__module__"_s), moduleName.get(), start);
    }
    noteConstant(g.addStringConstant(*c.name(m_info.qualname)));
    storeName(c.name("__qualname__"_s), stringConstant(m_info.qualname), start);
    noteConstant(jsNumber(firstLineNumber));
    storeName(c.name("__firstlineno__"_s), constant(jsNumber(firstLineNumber)), start);
    if (!node->typeParams.isEmpty()) {
        Reg typeParams = loadName(names().dotTypeParams, nullptr, start);
        storeName(names().typeParamsAttribute, typeParams.get(), start);
    }
    if (m_scope->needsClassDict)
        g.emitPyStoreCell(cellFor(names().classDict), m_namespace);
    if (m_scope->hasConditionalAnnotations) {
        Reg set = newTemporary();
        callRuntime(set.get(), RuntimeFunction::BuildSet);
        g.emitPyStoreCell(cellFor(names().conditionalAnnotations), set.get());
    }

    if (c.m_future.annotations) {
        if (m_scope->usesAnnotations)
            callRuntimeForEffect(RuntimeFunction::SetupAnnotations, { m_namespace });
    } else
        collectDeferredAnnotations(node->body, false);

    unsigned first = 0;
    if (m_scope->hasDocstring) {
        first = 1;
        String docstring = docstringOf(node->body);
        if (!docstring.isNull()) {
            noteConstant(g.addStringConstant(*c.name(docstring)));
            storeName(c.name("__doc__"_s), stringConstant(docstring), node->body[0]->range);
        }
    }
    for (unsigned i = first; i < node->body.size(); ++i)
        statement(node->body[i]);
    if (!c.m_future.annotations)
        emitDeferredAnnotations(start);

    SameLine sameLine(*this);
    std::ranges::sort(m_staticAttributes, [](Name a, Name b) { return codePointCompareLessThan(a->string(), b->string()); });
    Vector<JSValue, 8> attributes;
    for (Name name : m_staticAttributes)
        attributes.append(g.addStringConstant(*name));
    JSValue staticAttributes = PyTuple::create(globalObject(), attributes.span());
    noteConstant(staticAttributes);
    storeName(c.name("__static_attributes__"_s), constant(staticAttributes), start);

    if (m_scope->needsClassDict)
        storeName(c.name("__classdictcell__"_s), cellFor(names().classDict), start);
    if (m_scope->needsClassClosure) {
        // type.__new__ puts the class in the cell, for super() and __class__ in the methods.
        RegisterID* cell = cellFor(names().classCell);
        storeName(c.name("__classcell__"_s), cell, start);
        g.emitReturn(cell);
    } else
        emitReturnNone();
}

// name = __build_class__(body, "name", *bases, **keywords)
RegisterID* Unit::emitBuildClass(RegisterID* dst, ClassDef* node, int firstLineNumber, Name injectedBase)
{
    Scope* scope = c.m_symbols->scopeFor(node);
    CodeSpec spec { UnitKind::Class, scope, node->name, node->range, firstLineNumber };
    spec.privateName = node->name;
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        unit.compileClassBody(node, firstLineNumber);
    });

    Reg builder = newTemporary();
    loc(node->range);
    callRuntime(builder.get(), RuntimeFunction::LoadBuildClass);
    Reg body = newTemporary();
    emitMakeFunction(body.get(), executable, scope, { });
    noteConstant(g.addStringConstant(*node->name));
    RegisterID* pushed[] = { body.get(), nameConstant(node->name) };
    return emitCall(dst, builder.get(), pushed, node->bases, node->keywords, injectedBase, node->range);
}

void Unit::classDefinition(ClassDef* node)
{
    Vector<Reg, 4> decorators;
    emitDecorators(node->decoratorList, decorators);
    int firstLineNumber = node->decoratorList.isEmpty() ? node->range.lineno : node->decoratorList[0]->range.lineno;

    Reg result = newTemporary();
    if (node->typeParams.isEmpty())
        emitBuildClass(result.get(), node, firstLineNumber, nullptr);
    else {
        Scope* scope = c.m_symbols->scopeFor(node->typeParams.begin());
        CodeSpec spec { UnitKind::Annotations, scope, c.name(makeString("<generic parameters of "_s, node->name->string(), '>')), node->range, firstLineNumber };
        spec.privateName = node->name;
        auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
            {
                Reg typeParams = unit.newTemporary();
                unit.emitTypeParams(typeParams.get(), node->typeParams);
                unit.storeName(names().dotTypeParams, typeParams.get(), node->range);
                unit.loc(node->range);
                unit.callRuntime(typeParams.get(), RuntimeFunction::SubscriptGeneric, { typeParams.get() });
                unit.storeName(names().dotGenericBase, typeParams.get(), node->range);
            }
            Reg value = unit.newTemporary();
            unit.emitBuildClass(value.get(), node, firstLineNumber, names().dotGenericBase);
            unit.g.emitReturn(value.get());
        });
        emitMakeFunction(result.get(), executable, scope, { });
        emitSimpleCall(result.get(), result.get(), { }, node->range);
    }

    applyDecorators(node->decoratorList, decorators, result.get());
    line(node->range);
    storeName(node->name, result.get(), node->range);
}

// ---- Type parameters and aliases ----

// The bound or the default of a type parameter, or the value of a type alias: a function, so that it is evaluated on demand.
RegisterID* Unit::emitTypeParamExpression(RegisterID* dst, Expr* expression, Name name, const void* key, bool allowStarred)
{
    Scope* scope = c.m_symbols->scopeFor(key);
    CodeSpec spec { UnitKind::Annotations, scope, name, expression->range, expression->range.lineno };
    spec.positionalOnlyCount = 1;
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        unit.emitAnnotationsPrologue(expression->range);
        Reg value = unit.newTemporary();
        auto* starred = expression->tryAs<Starred>();
        if (allowStarred && starred) {
            unit.expr(starred->value, value.get());
            unit.loc(expression->range);
            unit.g.emitPyUnpack(value.get(), value.get(), 1, -1);
        } else
            unit.expr(expression, value.get());
        unit.g.emitReturn(value.get());
    });
    // The format defaults to VALUE.
    FunctionParts parts;
    parts.defaults = constant(PyTuple::create(globalObject(), { jsNumber(1) }));
    return emitMakeFunction(dst, executable, scope, parts);
}

RegisterID* Unit::emitTypeParams(RegisterID* dst, Seq<TypeParam*> typeParams)
{
    Arguments values(*this, typeParams.size());
    bool seenDefault = false;
    for (unsigned i = 0; i < typeParams.size(); ++i) {
        TypeParam* param = typeParams[i];
        RegisterID* value = values[i];
        const void* defaultKey = param;
        bool allowStarred = false;
        loc(param->range);
        switch (param->kind) {
        case TypeParamKind::TypeVar:
            if (param->bound) {
                Reg bound = newTemporary();
                emitTypeParamExpression(bound.get(), param->bound, param->name, param, false);
                loc(param->range);
                callRuntime(value, param->bound->is<Tuple>() ? RuntimeFunction::TypeVarWithConstraints : RuntimeFunction::TypeVarWithBound, { nameConstant(param->name), bound.get() });
            } else
                callRuntime(value, RuntimeFunction::TypeVar, { nameConstant(param->name) });
            defaultKey = reinterpret_cast<const char*>(param) + 1;
            break;
        case TypeParamKind::TypeVarTuple:
            callRuntime(value, RuntimeFunction::TypeVarTuple, { nameConstant(param->name) });
            allowStarred = true;
            break;
        case TypeParamKind::ParamSpec:
            callRuntime(value, RuntimeFunction::ParamSpec, { nameConstant(param->name) });
            break;
        }
        if (param->defaultValue) {
            seenDefault = true;
            Reg defaultValue = newTemporary();
            emitTypeParamExpression(defaultValue.get(), param->defaultValue, param->name, defaultKey, allowStarred);
            loc(param->range);
            callRuntime(value, RuntimeFunction::SetTypeParamDefault, { value, defaultValue.get() });
        } else if (seenDefault)
            fail(param->range, makeString("non-default type parameter '"_s, param->name->string(), "' follows default type parameter"_s));
        storeName(param->name, value, param->range);
    }
    loc(typeParams[0]->range);
    return callRuntime(dst, RuntimeFunction::BuildTuple, values);
}

RegisterID* Unit::emitTypeAliasObject(RegisterID* dst, AST::TypeAlias* node, RegisterID* typeParams)
{
    Name name = node->name->as<NameExpr>()->id;
    Reg heldTypeParams = typeParams;
    Reg value = newTemporary();
    {
        Scope* scope = c.m_symbols->scopeFor(node);
        CodeSpec spec { UnitKind::Annotations, scope, name, node->range, node->range.lineno };
        spec.positionalOnlyCount = 1;
        auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
            unit.emitAnnotationsPrologue(node->range);
            Reg result = unit.expr(node->value);
            unit.g.emitReturn(result.get());
        });
        FunctionParts parts;
        parts.defaults = constant(PyTuple::create(globalObject(), { jsNumber(1) }));
        emitMakeFunction(value.get(), executable, scope, parts);
    }
    loc(node->range);
    return callRuntime(dst, RuntimeFunction::TypeAlias, { nameConstant(name), typeParams, value.get() });
}

void Unit::typeAlias(AST::TypeAlias* node)
{
    Name name = node->name->as<NameExpr>()->id;
    Reg result = newTemporary();
    if (node->typeParams.isEmpty())
        emitTypeAliasObject(result.get(), node, constant(jsUndefined()));
    else {
        Scope* scope = c.m_symbols->scopeFor(node->typeParams.begin());
        CodeSpec spec { UnitKind::Annotations, scope, c.name(makeString("<generic parameters of "_s, name->string(), '>')), node->range, node->range.lineno };
        auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
            Reg typeParams = unit.newTemporary();
            unit.emitTypeParams(typeParams.get(), node->typeParams);
            Reg value = unit.newTemporary();
            unit.emitTypeAliasObject(value.get(), node, typeParams.get());
            unit.g.emitReturn(value.get());
        });
        emitMakeFunction(result.get(), executable, scope, { });
        emitSimpleCall(result.get(), result.get(), { }, node->range);
    }
    storeName(name, result.get(), node->range);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
