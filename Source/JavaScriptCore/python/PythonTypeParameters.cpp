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
#include "PythonBuiltins.h"

#include "PyTuple.h"

// What `def f[T]`, `class C[T]` and `type A = ...` make: type variables, and aliases. This is CPython's Objects/typevarobject.c, and Modules/_typingmodule.c,
// which is where a program gets at the classes. Much of what they do they leave to the `typing` module, which is written in Python and is imported when it
// is first wanted, as it is in CPython.

namespace JSC { namespace Python {

using Kind = PyNativeFunction::Kind;

static PyNativeObject* asNativeObject(JSValue value) { return uncheckedDowncast<PyNativeObject>(value.asCell()); }
static PyTypingObject* asTypingObject(JSValue value) { return uncheckedDowncast<PyTypingObject>(value.asCell()); }
static PyTuple* asTuple(JSValue value) { return uncheckedDowncast<PyTuple>(value.asCell()); }

// The fields of a TypeVar. A ParamSpec and a TypeVarTuple have some of them, in the same places.
namespace VariableField {
enum Field : unsigned { Name, Bound, EvaluateBound, Constraints, EvaluateConstraints, Default, EvaluateDefault, Variance };
}
enum Variance : int { IsCovariant = 1, IsContravariant = 2, IsInferred = 4 };

namespace AliasTypeField {
enum Field : unsigned { Name, TypeParameters, ComputeValue, Value, Module };
}

static bool isExactly(JSGlobalObject* globalObject, JSValue value, PyType* type)
{
    return value.isCell() && typeOf(globalObject, value) == type;
}

bool isTypeAlias(JSGlobalObject* globalObject, JSValue value)
{
    return isExactly(globalObject, value, globalObject->pyRealm()->typeTypeAliasType());
}

static bool isTypeParameter(JSGlobalObject* globalObject, JSValue value)
{
    PyRealm* realm = globalObject->pyRealm();
    return isExactly(globalObject, value, realm->typeTypeVar()) || isExactly(globalObject, value, realm->typeParamSpec()) || isExactly(globalObject, value, realm->typeTypeVarTuple());
}

// typing.name
static JSValue fromTyping(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue typing = importModule(globalObject, nullptr, "typing"_s, jsUndefined(), 0, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, typing, Identifier::fromString(vm, name)));
}

template<typename... Arguments>
static JSValue callTyping(JSGlobalObject* globalObject, ASCIILiteral name, Arguments... arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = fromTyping(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, function, arguments...));
}

static JSValue checkIsType(JSGlobalObject* globalObject, JSValue argument, ASCIILiteral message)
{
    if (isNone(argument))
        return globalObject->pyRealm()->typeNoneType();
    return callTyping(globalObject, "_type_check"_s, argument, jsNontrivialString(globalObject->vm(), message));
}

// The module of the function that is calling: what a type variable made by hand says that it is from.
static JSValue moduleOfCaller(JSGlobalObject* globalObject, CallFrame* callFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallFrame* caller = callerOf(callFrame);
    if (!caller || !caller->jsCallee())
        return jsUndefined();
    JSValue module = getAttributeIfPresent(globalObject, caller->jsCallee(), vm.pythonNames().dunder_module);
    RETURN_IF_EXCEPTION(scope, { });
    return module ? module : jsUndefined();
}

// Unpack[value], which is what *value is
static JSValue unpack(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue unpackForm = fromTyping(globalObject, "Unpack"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getItem(globalObject, unpackForm, value));
}

// Where type parameters are arguments, a TypeVarTuple among them is unpacked.
static JSValue unpackTypeVarTuples(JSGlobalObject* globalObject, PyTuple* parameters)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* typeVarTuple = globalObject->pyRealm()->typeTypeVarTuple();
    bool hasOne = false;
    for (auto& parameter : parameters->span())
        hasOne |= isExactly(globalObject, parameter.get(), typeVarTuple);
    if (!hasOne)
        return parameters;
    MarkedArgumentBuffer result;
    for (unsigned i = 0; i < parameters->length(); ++i) {
        JSValue parameter = parameters->at(i);
        if (isExactly(globalObject, parameter, typeVarTuple)) {
            parameter = unpack(globalObject, parameter);
            RETURN_IF_EXCEPTION(scope, { });
        }
        result.append(parameter);
    }
    return PyTuple::createFromArguments(globalObject, result);
}

// ---- What gives, when it is called, a value that was there all along. It stands in for a function that would work one out.

static JSValue newConstEvaluator(JSGlobalObject* globalObject, JSValue value)
{
    return PyNativeObject::create(globalObject, BuiltinType::ConstEvaluator, value);
}

PYTHON_NATIVE(constEvaluatorRepr)
{
    NATIVE_PROLOGUE();
    String text = repr(globalObject, asNativeObject(args[0])->field(0));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, makeString("<constevaluator "_s, text, '>')));
}

PYTHON_NATIVE(constEvaluatorCall)
{
    NATIVE_PROLOGUE();
    if (!args.checkNoKeywords(globalObject, scope, "constevaluator.__call__"_s))
        return { };
    if (args.size() != 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("constevaluator.__call__() takes exactly 1 argument ("_s, args.size() - 1, " given)"_s)));
    auto format = toIndex(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = asNativeObject(args[0])->field(0);
    constexpr int64_t formatString = 4;
    if (format != formatString)
        return JSValue::encode(value);
    StringBuilder out;
    if (isTuple(value)) {
        out.append('(');
        for (unsigned i = 0; i < asTuple(value)->length(); ++i) {
            if (i)
                out.append(", "_s);
            appendTypeRepr(globalObject, out, asTuple(value)->at(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        out.append(')');
    } else {
        appendTypeRepr(globalObject, out, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(jsString(vm, out.toString()));
}

// ---- NoDefault

PYTHON_NATIVE(noDefaultNew)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1 || args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "NoDefaultType takes no arguments"_s));
    return JSValue::encode(realm->noDefault());
}

PYTHON_NATIVE(noDefaultRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsNontrivialString(vm, "typing.NoDefault"_s));
}

PYTHON_NATIVE(noDefaultReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(jsNontrivialString(vm, "NoDefault"_s));
}

// ---- What TypeVar, ParamSpec and TypeVarTuple have in common

static PyTypingObject* newVariable(JSGlobalObject* globalObject, BuiltinType type, JSValue name, int variance)
{
    VM& vm = globalObject->vm();
    auto* variable = PyTypingObject::create(vm, globalObject->pyRealm()->structureFor(type));
    variable->setField(vm, VariableField::Name, name);
    variable->setField(vm, VariableField::Variance, jsNumber(variance));
    return variable;
}

// What is kept in `valueField`, or failing that what the function in `evaluatorField` gives, which is then kept. Failing that too, empty.
template<unsigned valueField, unsigned evaluatorField>
static JSValue evaluated(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = asTypingObject(self);
    if (JSValue value = object->field(valueField))
        return value;
    JSValue evaluator = object->field(evaluatorField);
    if (!evaluator)
        return { };
    JSValue value = call(globalObject, evaluator);
    RETURN_IF_EXCEPTION(scope, { });
    object->setField(vm, valueField, value);
    return value;
}

// The function, or failing that one that gives what is kept. Failing that too, None.
template<unsigned valueField, unsigned evaluatorField>
static JSValue getEvaluator(JSGlobalObject* globalObject, JSValue self)
{
    auto* object = asTypingObject(self);
    if (JSValue evaluator = object->field(evaluatorField))
        return evaluator;
    if (JSValue value = object->field(valueField))
        return newConstEvaluator(globalObject, value);
    return jsUndefined();
}

static JSValue getDefault(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = evaluated<VariableField::Default, VariableField::EvaluateDefault>(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    return value ? value : JSValue(globalObject->pyRealm()->noDefault());
}

template<unsigned index>
static JSValue getTypingField(JSGlobalObject*, JSValue self)
{
    JSValue value = asTypingObject(self)->field(index);
    return value ? value : jsUndefined();
}

template<int bit>
static JSValue getVariance(JSGlobalObject*, JSValue self)
{
    return jsBoolean(asTypingObject(self)->field(VariableField::Variance).asInt32() & bit);
}

PYTHON_NATIVE(variableRepr)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* variable = asTypingObject(args[0]);
    int variance = variable->field(VariableField::Variance).asInt32();
    JSString* name = asString(variable->field(VariableField::Name));
    if (variance & IsInferred)
        return JSValue::encode(name);
    return JSValue::encode(jsString(vm, makeString(variance & IsCovariant ? '+' : variance & IsContravariant ? '-' : '~', name->value(globalObject).data)));
}

// __repr__ of what has no variance to show, and __reduce__ of them all: what is pickled is where to find it again.
PYTHON_NATIVE(returnName)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(asTypingObject(args[0])->field(0));
}

PYTHON_NATIVE(variableHasDefault)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* variable = asTypingObject(args[0]);
    JSValue value = variable->field(VariableField::Default);
    return JSValue::encode(jsBoolean(variable->field(VariableField::EvaluateDefault) || (value && value != realm->noDefault())));
}

PYTHON_NATIVE(cannotSubclassInstance)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Cannot subclass an instance of "_s, typeOf(globalObject, args[0])->nameWithoutModule(globalObject))));
}

// T | int
PYTHON_NATIVE(variableOr)
{
    NATIVE_PROLOGUE();
    bool isReflected = unpack<bool>(callFrame, 0);
    RELEASE_AND_RETURN(scope, JSValue::encode(unionFrom(globalObject, PyTuple::create(globalObject, { args[isReflected], args[!isReflected] }))));
}

// *Ts and *Alias go through it, which gives one thing: it, unpacked.
PYTHON_NATIVE(unpackIter)
{
    NATIVE_PROLOGUE();
    JSValue unpacked = unpack(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(getIterator(globalObject, PyTuple::create(globalObject, { unpacked }))));
}

static bool checkNameIsString(JSGlobalObject* globalObject, ThrowScope& scope, ASCIILiteral function, JSValue name)
{
    if (name.isString())
        return true;
    raiseTypeError(globalObject, scope, makeString(function, "() argument 'name' must be str, not "_s, isNone(name) ? "None"_str : typeName(globalObject, name)));
    return false;
}

// The three that say how it varies. -1 if something has been raised.
static int varianceFrom(JSGlobalObject* globalObject, const NativeArguments& args)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int variance = 0;
    for (auto [name, bit] : { std::pair { "covariant"_s, IsCovariant }, std::pair { "contravariant"_s, IsContravariant }, std::pair { "infer_variance"_s, IsInferred } }) {
        JSValue flag = args.keyword(globalObject, name);
        if (!flag)
            continue;
        bool isSet = isTrue(globalObject, flag);
        RETURN_IF_EXCEPTION(scope, -1);
        if (isSet)
            variance |= bit;
    }
    if ((variance & IsCovariant) && (variance & IsContravariant)) {
        raise(globalObject, scope, BuiltinType::ValueError, "Bivariant types are not supported."_s);
        return -1;
    }
    if ((variance & IsInferred) && (variance & (IsCovariant | IsContravariant))) {
        raise(globalObject, scope, BuiltinType::ValueError, "Variance cannot be specified with infer_variance."_s);
        return -1;
    }
    return variance;
}

static void setModule(JSGlobalObject* globalObject, CallFrame* callFrame, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = moduleOfCaller(globalObject, callFrame);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    setAttribute(globalObject, object, vm.pythonNames().dunder_module, module);
}

// ---- TypeVar

PYTHON_NATIVE(typeVarNew)
{
    NATIVE_PROLOGUE();
    JSValue name = args.size() > 1 ? args[1] : args.keyword(globalObject, "name"_s);
    if (!checkNameIsString(globalObject, scope, "typevar"_s, name))
        return { };
    int variance = varianceFrom(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue bound = args.keyword(globalObject, "bound"_s);
    if (bound && isNone(bound))
        bound = { };
    if (bound) {
        bound = checkIsType(globalObject, bound, "Bound must be a type."_s);
        RETURN_IF_EXCEPTION(scope, { });
    }
    unsigned constraintCount = args.size() > 2 ? args.size() - 2 : 0;
    if (constraintCount == 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "A single constraint is not allowed"_s));
    if (constraintCount && bound)
        return JSValue::encode(raiseTypeError(globalObject, scope, "Constraints cannot be combined with bound=..."_s));

    auto* variable = newVariable(globalObject, BuiltinType::TypeVar, name, variance);
    if (bound)
        variable->setField(vm, VariableField::Bound, bound);
    if (constraintCount) {
        MarkedArgumentBuffer constraints;
        for (unsigned i = 0; i < constraintCount; ++i)
            constraints.append(args[i + 2]);
        variable->setField(vm, VariableField::Constraints, PyTuple::createFromArguments(globalObject, constraints));
    }
    JSValue defaultValue = args.keyword(globalObject, "default"_s);
    variable->setField(vm, VariableField::Default, defaultValue ? defaultValue : JSValue(realm->noDefault()));
    setModule(globalObject, callFrame, variable);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(variable);
}

PYTHON_NATIVE(typeVarSubstitute)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTyping(globalObject, "_typevar_subst"_s, args[0], args[1])));
}

// If there are not arguments enough to reach this one, its default is added.
PYTHON_NATIVE(typeVarPrepareSubstitution)
{
    NATIVE_PROLOGUE();
    JSValue alias = args[1];
    JSValue given = args[2];
    JSValue parameters = getAttribute(globalObject, alias, names.dunder_parameters);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer all;
    collect(globalObject, parameters, all);
    RETURN_IF_EXCEPTION(scope, { });
    size_t index = notFound;
    for (size_t i = 0; i < all.size() && index == notFound; ++i) {
        bool isSame = isEqual(globalObject, all.at(i), args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (isSame)
            index = i;
    }
    if (index == notFound)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, "sequence.index(x): x not in sequence"_s));
    int64_t count = length(globalObject, given);
    RETURN_IF_EXCEPTION(scope, { });
    if (static_cast<int64_t>(index) < count)
        return JSValue::encode(given);
    if (static_cast<int64_t>(index) == count) {
        JSValue defaultValue = getDefault(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (defaultValue != realm->noDefault())
            RELEASE_AND_RETURN(scope, JSValue::encode(binaryOperation(globalObject, BinaryOperator::Add, false, given, PyTuple::create(globalObject, { defaultValue }))));
    }
    String text = str(globalObject, alias);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Too few arguments for "_s, text, "; actual "_s, count, ", expected at least "_s, index + 1)));
}

// ---- ParamSpec, and P.args and P.kwargs

PYTHON_NATIVE(paramSpecNew)
{
    NATIVE_PROLOGUE();
    JSValue name = args.size() > 1 ? args[1] : args.keyword(globalObject, "name"_s);
    if (!checkNameIsString(globalObject, scope, "paramspec"_s, name))
        return { };
    int variance = varianceFrom(globalObject, args);
    RETURN_IF_EXCEPTION(scope, { });
    // None if it is not given, and NoneType is what comes of checking that.
    JSValue bound = args.keyword(globalObject, "bound"_s);
    bound = checkIsType(globalObject, bound ? bound : jsUndefined(), "Bound must be a type."_s);
    RETURN_IF_EXCEPTION(scope, { });
    auto* variable = newVariable(globalObject, BuiltinType::ParamSpec, name, variance);
    variable->setField(vm, VariableField::Bound, bound);
    JSValue defaultValue = args.keyword(globalObject, "default"_s);
    variable->setField(vm, VariableField::Default, defaultValue ? defaultValue : JSValue(realm->noDefault()));
    setModule(globalObject, callFrame, variable);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(variable);
}

PYTHON_NATIVE(paramSpecSubstitute)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTyping(globalObject, "_paramspec_subst"_s, args[0], args[1])));
}

PYTHON_NATIVE(paramSpecPrepareSubstitution)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTyping(globalObject, "_paramspec_prepare_subst"_s, args[0], args[1], args[2])));
}

PYTHON_NATIVE(paramSpecPartNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* part = PyNativeObject::create(vm, asType(args[0])->instanceStructure());
    part->setField(vm, 0, args.at(1));
    return JSValue::encode(part);
}

PYTHON_NATIVE(paramSpecPartRepr)
{
    NATIVE_PROLOGUE();
    JSValue origin = asNativeObject(args[0])->field(0);
    ASCIILiteral suffix = typeOf(globalObject, args[0]) == realm->typeParamSpecArgs() ? ".args"_s : ".kwargs"_s;
    if (isExactly(globalObject, origin, realm->typeParamSpec()))
        return JSValue::encode(jsString(vm, makeString(asString(asTypingObject(origin)->field(VariableField::Name))->value(globalObject).data, suffix)));
    String text = repr(globalObject, origin);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, makeString(text, suffix)));
}

PYTHON_NATIVE(paramSpecPartCompare)
{
    NATIVE_PROLOGUE();
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    if (typeOf(globalObject, args[0]) != typeOf(globalObject, args[1]) || (op != ComparisonOperator::Eq && op != ComparisonOperator::NotEq))
        RETURN_NOT_IMPLEMENTED();
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, asNativeObject(args[0])->field(0), asNativeObject(args[1])->field(0))));
}

// ---- TypeVarTuple

PYTHON_NATIVE(typeVarTupleNew)
{
    NATIVE_PROLOGUE();
    JSValue name = args.size() > 1 ? args[1] : args.keyword(globalObject, "name"_s);
    if (!checkNameIsString(globalObject, scope, "typevartuple"_s, name))
        return { };
    auto* variable = newVariable(globalObject, BuiltinType::TypeVarTuple, name, 0);
    JSValue defaultValue = args.keyword(globalObject, "default"_s);
    variable->setField(vm, VariableField::Default, defaultValue ? defaultValue : JSValue(realm->noDefault()));
    setModule(globalObject, callFrame, variable);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(variable);
}

PYTHON_NATIVE(typeVarTupleSubstitute)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raiseTypeError(globalObject, scope, "Substitution of bare TypeVarTuple is not supported"_s));
}

PYTHON_NATIVE(typeVarTuplePrepareSubstitution)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTyping(globalObject, "_typevartuple_prepare_subst"_s, args[0], args[1], args[2])));
}

// ---- TypeAliasType

static PyTypingObject* newTypeAliasObject(JSGlobalObject* globalObject, JSValue name, JSValue typeParameters, JSValue computeValue, JSValue value, JSValue module)
{
    VM& vm = globalObject->vm();
    auto* alias = PyTypingObject::create(vm, globalObject->pyRealm()->structureFor(BuiltinType::TypeAliasType));
    alias->setField(vm, AliasTypeField::Name, name);
    alias->setField(vm, AliasTypeField::TypeParameters, typeParameters);
    alias->setField(vm, AliasTypeField::ComputeValue, computeValue);
    alias->setField(vm, AliasTypeField::Value, value);
    alias->setField(vm, AliasTypeField::Module, module);
    return alias;
}

PYTHON_NATIVE(typeAliasNew)
{
    NATIVE_PROLOGUE();
    JSValue name = args.at(1);
    if (!checkNameIsString(globalObject, scope, "typealias"_s, name))
        return { };
    JSValue typeParameters = args.keyword(globalObject, "type_params"_s);
    if (typeParameters && !isTuple(typeParameters))
        return JSValue::encode(raiseTypeError(globalObject, scope, "type_params must be a tuple"_s));
    // None at all is the same as not being generic.
    if (typeParameters && !asTuple(typeParameters)->length())
        typeParameters = { };
    if (typeParameters) {
        bool hasSeenDefault = false;
        for (unsigned i = 0; i < asTuple(typeParameters)->length(); ++i) {
            JSValue parameter = asTuple(typeParameters)->at(i);
            if (!isTypeParameter(globalObject, parameter)) {
                String text = repr(globalObject, parameter);
                RETURN_IF_EXCEPTION(scope, { });
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Expected a type param, got "_s, text)));
            }
            JSValue defaultValue = getDefault(globalObject, parameter);
            RETURN_IF_EXCEPTION(scope, { });
            if (defaultValue != realm->noDefault()) {
                hasSeenDefault = true;
                continue;
            }
            if (hasSeenDefault) {
                String text = repr(globalObject, parameter);
                RETURN_IF_EXCEPTION(scope, { });
                return JSValue::encode(raiseTypeError(globalObject, scope, makeString("non-default type parameter '"_s, text, "' follows default type parameter"_s)));
            }
        }
    }
    JSValue module = moduleOfCaller(globalObject, callFrame);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(newTypeAliasObject(globalObject, name, typeParameters, JSValue(), args.at(2), module));
}

PYTHON_NATIVE(typeAliasGetItem)
{
    NATIVE_PROLOGUE();
    if (!asTypingObject(args[0])->field(AliasTypeField::TypeParameters))
        return JSValue::encode(raiseTypeError(globalObject, scope, "Only generic type aliases are subscriptable"_s));
    return JSValue::encode(newGenericAlias(globalObject, args[0], args[1]));
}

// ---- Generic, which leaves everything to the library

static JSValue callTypingWithClass(JSGlobalObject* globalObject, CallFrame* callFrame, const NativeArguments& args, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = fromTyping(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer arguments;
    for (unsigned i = 0; i < callFrame->argumentCount(); ++i)
        arguments.append(callFrame->uncheckedArgument(i));
    RELEASE_AND_RETURN(scope, callWithKeywords(globalObject, function, arguments, args.keywordNames()));
}

PYTHON_NATIVE(genericClassGetItem)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTypingWithClass(globalObject, callFrame, args, "_generic_class_getitem"_s)));
}

PYTHON_NATIVE(genericInitSubclass)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(callTypingWithClass(globalObject, callFrame, args, "_generic_init_subclass"_s)));
}

// ---- What compiled code calls

// T, T: bound and T: (a, b). What follows the colon is worked out when it is asked for, by a function.
JSValue newTypeVar(JSGlobalObject* globalObject, JSString* name, JSValue evaluator, bool isConstraints)
{
    auto* variable = newVariable(globalObject, BuiltinType::TypeVar, name, IsInferred);
    if (evaluator)
        variable->setField(globalObject->vm(), isConstraints ? VariableField::EvaluateConstraints : VariableField::EvaluateBound, evaluator);
    return variable;
}

JSValue newParamSpec(JSGlobalObject* globalObject, JSString* name)
{
    return newVariable(globalObject, BuiltinType::ParamSpec, name, IsInferred);
}

JSValue newTypeVarTuple(JSGlobalObject* globalObject, JSString* name)
{
    return newVariable(globalObject, BuiltinType::TypeVarTuple, name, 0);
}

// T = default
void setTypeParameterDefault(JSGlobalObject* globalObject, JSValue parameter, JSValue evaluator)
{
    asTypingObject(parameter)->setField(globalObject->vm(), VariableField::EvaluateDefault, evaluator);
}

// type name[typeParameters] = ..., where a function works out what follows the equals sign
JSValue newTypeAlias(JSGlobalObject* globalObject, JSString* name, JSValue typeParameters, JSValue computeValue)
{
    if (typeParameters && (isNone(typeParameters) || !asTuple(typeParameters)->length()))
        typeParameters = { };
    return newTypeAliasObject(globalObject, name, typeParameters, computeValue, JSValue(), JSValue());
}

// Generic[typeParameters], which a generic class is derived from besides what it says
JSValue subscriptGeneric(JSGlobalObject* globalObject, PyTuple* typeParameters)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue parameters = unpackTypeVarTuples(globalObject, typeParameters);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, callTyping(globalObject, "_GenericAlias"_s, JSValue(globalObject->pyRealm()->typeGeneric()), parameters));
}

// ---- _typing

PYTHON_NATIVE(identity)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(args[0]);
}

JSObject* createTypingModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    JSObject* module = newBuiltinModule(globalObject, "_typing"_s);
    addFunction(globalObject, module, "_idfunc"_s, identity);
    for (PyType* type : { realm->typeTypeVar(), realm->typeTypeVarTuple(), realm->typeParamSpec(), realm->typeParamSpecArgs(), realm->typeParamSpecKwargs(), realm->typeGeneric(), realm->typeTypeAliasType(), realm->typeUnion() })
        module->putDirect(vm, Identifier::fromString(vm, type->nameWithoutModule(globalObject)), type);
    module->putDirect(vm, Identifier::fromString(vm, "NoDefault"_s), realm->noDefault());
    return module;
}

void initializeTypeParameters(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    for (PyType* type : { realm->typeTypeVar(), realm->typeParamSpec(), realm->typeTypeVarTuple(), realm->typeTypeAliasType() })
        type->setInstanceStructure(vm, PyTypingObject::createStructure(vm, globalObject, type));
    for (PyType* type : { realm->typeParamSpecArgs(), realm->typeParamSpecKwargs(), realm->typeConstEvaluator(), realm->typeNoDefaultType() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));

    // In CPython these are made when it starts and not laid down beforehand, and such a class has where it is from among its attributes, where its instances find it.
    for (PyType* type : { realm->typeTypeVar(), realm->typeParamSpec(), realm->typeParamSpecArgs(), realm->typeParamSpecKwargs(), realm->typeTypeVarTuple(), realm->typeGeneric(), realm->typeConstEvaluator() })
        type->putDirect(vm, vm.pythonNames().dunder_module, jsString(vm, type->moduleOfBuiltin()));

    addMethods(globalObject, realm->typeNoDefaultType(), {
        { "__new__"_s, noDefaultNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__repr__"_s, noDefaultRepr },
        { "__reduce__"_s, noDefaultReduce },
    });
    addMethods(globalObject, realm->typeConstEvaluator(), {
        { "__repr__"_s, constEvaluatorRepr },
        { "__call__"_s, constEvaluatorCall },
    });

    for (PyType* type : { realm->typeTypeVar(), realm->typeParamSpec(), realm->typeTypeVarTuple() }) {
        addMethods(globalObject, type, {
            { "__reduce__"_s, returnName },
            { "has_default"_s, variableHasDefault },
            { "__mro_entries__"_s, cannotSubclassInstance },
        });
        addMember(globalObject, type, "__name__"_s, getTypingField<VariableField::Name>);
        addGetSet(globalObject, type, "__default__"_s, getDefault);
        addGetSet(globalObject, type, "evaluate_default"_s, getEvaluator<VariableField::Default, VariableField::EvaluateDefault>);
    }
    for (PyType* type : { realm->typeTypeVar(), realm->typeParamSpec() }) {
        addMethods(globalObject, type, {
            { "__repr__"_s, variableRepr },
            { "__or__"_s, variableOr, Kind::Method, pack(false) },
            { "__ror__"_s, variableOr, Kind::Method, pack(true) },
        });
        addMember(globalObject, type, "__covariant__"_s, getVariance<IsCovariant>);
        addMember(globalObject, type, "__contravariant__"_s, getVariance<IsContravariant>);
        addMember(globalObject, type, "__infer_variance__"_s, getVariance<IsInferred>);
    }

    PyType* typeVar = realm->typeTypeVar();
    addMethods(globalObject, typeVar, {
        { "__new__"_s, typeVarNew, Kind::New, 0, "typevar(name, *constraints, bound=None, default=typing.NoDefault, covariant=False, contravariant=False, infer_variance=False)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__typing_subst__"_s, typeVarSubstitute },
        { "__typing_prepare_subst__"_s, typeVarPrepareSubstitution },
    });
    addGetSet(globalObject, typeVar, "__bound__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue value = evaluated<VariableField::Bound, VariableField::EvaluateBound>(globalObject, self);
        return value || globalObject->vm().exceptionForInspection() ? value : jsUndefined();
    });
    addGetSet(globalObject, typeVar, "__constraints__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue value = evaluated<VariableField::Constraints, VariableField::EvaluateConstraints>(globalObject, self);
        return value || globalObject->vm().exceptionForInspection() ? value : JSValue(PyTuple::create(globalObject, 0));
    });
    addGetSet(globalObject, typeVar, "evaluate_bound"_s, getEvaluator<VariableField::Bound, VariableField::EvaluateBound>);
    addGetSet(globalObject, typeVar, "evaluate_constraints"_s, getEvaluator<VariableField::Constraints, VariableField::EvaluateConstraints>);

    PyType* paramSpec = realm->typeParamSpec();
    addMethods(globalObject, paramSpec, {
        { "__new__"_s, paramSpecNew, Kind::New, 0, "paramspec(name, *, bound=None, default=typing.NoDefault, covariant=False, contravariant=False, infer_variance=False)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__typing_subst__"_s, paramSpecSubstitute },
        { "__typing_prepare_subst__"_s, paramSpecPrepareSubstitution },
    });
    addMember(globalObject, paramSpec, "__bound__"_s, getTypingField<VariableField::Bound>);
    addGetSet(globalObject, paramSpec, "args"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return PyNativeObject::create(globalObject, BuiltinType::ParamSpecArgs, self); });
    addGetSet(globalObject, paramSpec, "kwargs"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue { return PyNativeObject::create(globalObject, BuiltinType::ParamSpecKwargs, self); });

    for (PyType* type : { realm->typeParamSpecArgs(), realm->typeParamSpecKwargs() }) {
        // It says when two are equal and not how to hash one.
        type->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
        addMethods(globalObject, type, {
            { "__new__"_s, paramSpecPartNew, Kind::New, 0, type == realm->typeParamSpecArgs() ? "paramspecargs(origin)"_s : "paramspeckwargs(origin)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
            { "__repr__"_s, paramSpecPartRepr },
            { "__mro_entries__"_s, cannotSubclassInstance },
            { "__lt__"_s, paramSpecPartCompare, Kind::Method, pack(ComparisonOperator::Lt) },
            { "__le__"_s, paramSpecPartCompare, Kind::Method, pack(ComparisonOperator::LtE) },
            { "__eq__"_s, paramSpecPartCompare, Kind::Method, pack(ComparisonOperator::Eq) },
            { "__ne__"_s, paramSpecPartCompare, Kind::Method, pack(ComparisonOperator::NotEq) },
            { "__gt__"_s, paramSpecPartCompare, Kind::Method, pack(ComparisonOperator::Gt) },
            { "__ge__"_s, paramSpecPartCompare, Kind::Method, pack(ComparisonOperator::GtE) },
        });
        addMember(globalObject, type, "__origin__"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return asNativeObject(self)->field(0); });
    }

    addMethods(globalObject, realm->typeTypeVarTuple(), {
        { "__new__"_s, typeVarTupleNew, Kind::New, 0, "typevartuple(name, *, default=typing.NoDefault)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, returnName },
        { "__iter__"_s, unpackIter },
        { "__typing_subst__"_s, typeVarTupleSubstitute },
        { "__typing_prepare_subst__"_s, typeVarTuplePrepareSubstitution },
    });

    PyType* alias = realm->typeTypeAliasType();
    addMethods(globalObject, alias, {
        { "__new__"_s, typeAliasNew, Kind::New, 0, "typealias(name, value, *, type_params=<unrepresentable>)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, returnName },
        { "__reduce__"_s, returnName },
        { "__iter__"_s, unpackIter },
        { "__getitem__"_s, typeAliasGetItem },
        { "__or__"_s, typeOr, Kind::Method, pack(false) },
        { "__ror__"_s, typeOr, Kind::Method, pack(true) },
    });
    addMember(globalObject, alias, "__name__"_s, getTypingField<AliasTypeField::Name>);
    addGetSet(globalObject, alias, "__value__"_s, evaluated<AliasTypeField::Value, AliasTypeField::ComputeValue>);
    addGetSet(globalObject, alias, "evaluate_value"_s, getEvaluator<AliasTypeField::Value, AliasTypeField::ComputeValue>);
    addGetSet(globalObject, alias, "__type_params__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue parameters = asTypingObject(self)->field(AliasTypeField::TypeParameters);
        return parameters ? parameters : JSValue(PyTuple::create(globalObject, 0));
    });
    addGetSet(globalObject, alias, "__parameters__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue parameters = asTypingObject(self)->field(AliasTypeField::TypeParameters);
        return parameters ? unpackTypeVarTuples(globalObject, asTuple(parameters)) : JSValue(PyTuple::create(globalObject, 0));
    });
    addGetSet(globalObject, alias, "__module__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        auto* alias = asTypingObject(self);
        if (JSValue module = alias->field(AliasTypeField::Module))
            return module;
        // One that a statement made is from where the function that works out its value is from.
        if (JSValue computeValue = alias->field(AliasTypeField::ComputeValue))
            return getAttribute(globalObject, computeValue, globalObject->vm().pythonNames().dunder_module);
        return jsUndefined();
    });

    // __type_params__, of a function and of a class
    addGetSet(globalObject, realm->typeFunction(), "__type_params__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        JSValue parameters = asObject(self)->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_typeParams);
        return parameters ? parameters : JSValue(PyTuple::create(globalObject, 0));
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!value || !isTuple(value)) {
            raiseTypeError(globalObject, scope, "__type_params__ must be set to a tuple"_s);
            return;
        }
        asObject(self)->putDirect(vm, vm.pythonNames().private_typeParams, value);
    });
    addGetSet(globalObject, realm->typeType(), "__type_params__"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        // `type` has there the very thing that is asking.
        JSValue parameters = self == globalObject->pyRealm()->typeType() ? JSValue() : asType(self)->lookupOwn(globalObject->vm(), globalObject->vm().pythonNames().dunder_type_params);
        return parameters ? parameters : JSValue(PyTuple::create(globalObject, 0));
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        PyType* type = asType(self);
        if (!type->hasFlag(PyType::IsHeapType) || !value) {
            raiseTypeError(globalObject, scope, makeString("cannot "_s, type->hasFlag(PyType::IsHeapType) ? "delete"_s : "set"_s, " '__type_params__' attribute of immutable type '"_s, type->nameString(globalObject), '\''));
            return;
        }
        type->setAttribute(vm, vm.pythonNames().dunder_type_params, value);
    });

    addMethods(globalObject, realm->typeGeneric(), {
        { "__class_getitem__"_s, genericClassGetItem, Kind::ClassMethod, 0, "($type, /, *args, **kwargs)"_s },
        { "__init_subclass__"_s, genericInitSubclass, Kind::ClassMethod, 0, "($type, /, *args, **kwargs)"_s },
    });
}

} } // namespace JSC::Python
