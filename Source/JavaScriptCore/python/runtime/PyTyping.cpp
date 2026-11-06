/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// What type parameter lists and `type` statements make, and the module _typing, which hands it all to typing.py.
// (Objects/typevarobject.c and Modules/_typingmodule.c in CPython are the reference.)
//
// The instances are PyRecords. Much of what they do is left to functions in typing.py, as in CPython.

// ---- Arguments ----

// Matches the arguments of a call with parameters that can all be given by keyword, the first `maxPositional` of them by
// position too, and complains as a function whose arguments Argument Clinic parses does. `values` has room for one value
// for each keyword, all empty; what the caller left out stays empty. With `hasVarPositional`, positional arguments beyond
// `maxPositional` are for the function to pick up. False: an exception.
bool unpackKeywordArguments(JSGlobalObject*, ASCIILiteral functionName, std::span<const ASCIILiteral> keywords, unsigned minPositional, unsigned maxPositional, bool hasVarPositional, const CallArgs&, JSValue* values);

bool unpackKeywordArguments(JSGlobalObject* globalObject, ASCIILiteral functionName, std::span<const ASCIILiteral> keywords, unsigned minPositional, unsigned maxPositional, bool hasVarPositional, const CallArgs& args, JSValue* values)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned maxArguments = keywords.size();
    unsigned count = args.size();
    unsigned remainingKeywords = args.keywordCount();
    auto plural = [](unsigned number) { return number == 1 ? ""_s : "s"_s; };

    if (!hasVarPositional && count + remainingKeywords > maxArguments) {
        raiseTypeError(globalObject, scope, functionName, "() takes at most "_s, maxArguments, ' ', !count ? "keyword "_s : ""_s, "argument"_s, plural(maxArguments), " ("_s, count + remainingKeywords, " given)"_s);
        return false;
    }
    if (!hasVarPositional && count > maxPositional) {
        if (!maxPositional)
            raiseTypeError(globalObject, scope, functionName, "() takes no positional arguments"_s);
        else
            raiseTypeError(globalObject, scope, functionName, "() takes "_s, minPositional < maxPositional ? "at most "_s : "exactly "_s, maxPositional, " positional argument"_s, plural(maxPositional), " ("_s, count, " given)"_s);
        return false;
    }
    count = std::min(count, maxPositional);

    for (unsigned i = 0; i < count; ++i)
        values[i] = args[i];
    for (unsigned i = count; i < maxArguments; ++i) {
        if (!remainingKeywords && i >= minPositional)
            break;
        JSValue value = remainingKeywords ? args.keyword(keywords[i]) : JSValue();
        values[i] = value;
        if (value)
            --remainingKeywords;
        else if (i < minPositional) {
            raiseTypeError(globalObject, scope, functionName, "() missing required argument '"_s, keywords[i], "' (pos "_s, i + 1, ')');
            return false;
        }
    }
    if (!remainingKeywords)
        return true;

    for (unsigned i = 0; i < count; ++i) {
        if (args.keyword(keywords[i])) {
            raiseTypeError(globalObject, scope, "argument for "_s, functionName, "() given by name ('"_s, keywords[i], "') and position ("_s, i + 1, ')');
            return false;
        }
    }
    for (unsigned k = 0; k < args.keywordCount(); ++k) {
        String given = stringOf(args.keywordName(k));
        bool isKnown = false;
        Vector<String> candidates;
        for (ASCIILiteral keyword : keywords) {
            isKnown |= given == keyword;
            candidates.append(keyword);
        }
        if (isKnown)
            continue;
        String suggestion = calculateSuggestion(candidates, given);
        if (suggestion.isNull())
            raiseTypeError(globalObject, scope, functionName, "() got an unexpected keyword argument '"_s, given, '\'');
        else
            raiseTypeError(globalObject, scope, functionName, "() got an unexpected keyword argument '"_s, given, "'. Did you mean '"_s, suggestion, "'?"_s);
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// "f() argument 'x' must be str, not int"
static JSValue typingRaiseBadArgument(JSGlobalObject* globalObject, ASCIILiteral functionName, ASCIILiteral argument, ASCIILiteral expected, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, functionName, "() argument '"_s, argument, "' must be "_s, expected, ", not "_s, isNone(value) ? "None"_str : typeName(globalObject, value));
}

// An argument that is a flag. Ask the VM whether it threw.
static bool typingFlagArgument(JSGlobalObject* globalObject, JSValue value)
{
    return value && isTrue(globalObject, value);
}

// ---- What typing.py does ----

static JSValue typingAttribute(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue typing = importModule(globalObject, "typing"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttr(globalObject, typing, internString(vm, name)));
}

template<typename... Arguments>
static JSValue typingCall(JSGlobalObject* globalObject, ASCIILiteral name, Arguments... arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = typingAttribute(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, function, arguments...));
}

// typing.name(cls, *args, **kwargs)
static JSValue typingCallWithClass(JSGlobalObject* globalObject, ASCIILiteral name, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue function = typingAttribute(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, withFirstArgument(cls, args, [&](const CallArgs& all) {
        return call(globalObject, function, all);
    }));
}

static JSValue typingCheckType(JSGlobalObject* globalObject, JSValue value, ASCIILiteral message)
{
    // typing.py may be the one asking, while it is being imported.
    if (isNone(value))
        return runtime(globalObject).noneType();
    return typingCall(globalObject, "_type_check"_s, value, makeStr(getVM(globalObject), message));
}

// Union[left, right]. Not left | right as types do it: next to a type variable, a str is a forward reference.
static JSValue typingUnionOf(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    return getItem(globalObject, runtime(globalObject).unionType(), PyTuple::create(globalObject, { left, right }));
}

// The __module__ of the function that is running: where what it makes by calling a type says it is from.
static JSValue typingCallerModule(JSGlobalObject* globalObject)
{
    PyFrame* frame = currentFrame(globalObject);
    if (!frame)
        return jsUndefined();
    JSValue module = frame->function()->codeOwner()->module();
    return module ? module : jsUndefined();
}

// Unpack[object]
static JSValue typingUnpack(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue unpack = typingAttribute(globalObject, "Unpack"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getItem(globalObject, unpack, object));
}

// iter((Unpack[self],)), so that *self stands for that.
static JSValue typingUnpackIter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue unpacked = typingUnpack(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getIter(globalObject, PyTuple::create(globalObject, { unpacked })));
}

// The parameters as Generic wants them: each TypeVarTuple unpacked.
static JSValue typingUnpackTypeVarTuples(JSGlobalObject* globalObject, PyTuple* parameters)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool hasAny = false;
    for (JSValue parameter : parameters->span())
        hasAny |= !!PyRecord::tryFrom(parameter, rt.typeVarTupleType());
    if (!hasAny)
        return parameters;
    unsigned count = parameters->length();
    PyTuple* result = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i) {
        JSValue parameter = parameters->at(i);
        if (PyRecord::tryFrom(parameter, rt.typeVarTupleType())) {
            parameter = typingUnpack(globalObject, parameter);
            RETURN_IF_EXCEPTION(scope, { });
        }
        result->initializeAt(vm, i, parameter);
    }
    return result;
}

static JSValue typingRaiseCannotSubclass(JSGlobalObject* globalObject, ASCIILiteral what)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Cannot subclass an instance of "_s, what);
}

static ALWAYS_INLINE JSValue typingOrNone(JSValue value) { return value ? value : jsUndefined(); }

// ---- NoDefault ----

// FIXME: The runtime should have a place for it. The type, which has no __dict__ of that kind, keeps it under that key.
static JSValue noDefaultObject(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.noDefaultType()->getDirect(rt.vm(), rt.dictKey());
}

static ALWAYS_INLINE bool isNoDefault(JSGlobalObject* globalObject, JSValue value)
{
    return value.isCell() && value.asCell()->type() == PyObjectType && uncheckedDowncast<PyObject>(value.asCell())->type() == runtime(globalObject).noDefaultType();
}

static JSValue noDefaultNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount())
        return raiseTypeError(globalObject, scope, "NoDefaultType takes no arguments"_s);
    RELEASE_AND_RETURN(scope, noDefaultObject(globalObject));
}

static JSValue noDefaultRepr(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "typing.NoDefault"_s); }
static JSValue noDefaultReduce(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "NoDefault"_s); }

static constexpr NativeDef noDefaultMethods[] = {
    PY_NEW(noDefaultNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", noDefaultRepr, "($self, /)"),
    PY_METHOD("__reduce__", noDefaultReduce, "($self, /)"),
};

// ---- Evaluators of what is known already ----
//
// The bound of T in `def f[T: int]` is a function, called when the bound is asked for. For TypeVar("T", bound=int) there is
// no such function, and evaluate_bound is one of these.

enum : unsigned { constEvaluatorValue, constEvaluatorFieldCount };

static constexpr int annotationFormatString = 4; // annotationlib.Format.STRING

static JSValue newConstEvaluator(JSGlobalObject* globalObject, JSValue value)
{
    PyRecord* evaluator = PyRecord::create(globalObject, runtime(globalObject).constEvaluatorType(), constEvaluatorFieldCount);
    evaluator->set(getVM(globalObject), constEvaluatorValue, value);
    return evaluator;
}

static bool constEvaluatorAppendText(JSGlobalObject* globalObject, StringBuilder& builder, JSString* text)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    builder.append(string);
    return true;
}

// A type, as source code would name it.
static bool constEvaluatorAppendTypeRepr(JSGlobalObject* globalObject, StringBuilder& builder, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value == rt.ellipsis()) {
        builder.append("..."_s);
        return true;
    }
    if (value == JSValue(rt.noneType())) {
        builder.append("None"_s);
        return true;
    }

    auto appendRepr = [&]() -> bool {
        JSString* text = repr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, constEvaluatorAppendText(globalObject, builder, text));
    };

    // Something like list[int] shows itself well.
    JSValue origin = getAttrIfPresent(globalObject, value, internString(vm, "__origin__"_s));
    RETURN_IF_EXCEPTION(scope, false);
    if (origin) {
        JSValue arguments = getAttrIfPresent(globalObject, value, internString(vm, "__args__"_s));
        RETURN_IF_EXCEPTION(scope, false);
        if (arguments)
            return appendRepr();
    }

    JSValue qualname = getAttrIfPresent(globalObject, value, rt.names.dQualname());
    RETURN_IF_EXCEPTION(scope, false);
    if (!qualname)
        return appendRepr();
    JSValue module = getAttrIfPresent(globalObject, value, rt.names.dModule());
    RETURN_IF_EXCEPTION(scope, false);
    if (!module || isNone(module))
        return appendRepr();

    bool isBuiltin = false;
    if (isStr(module)) {
        String moduleName = strValue(module)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        isBuiltin = moduleName == "builtins"_s;
    }
    if (!isBuiltin) {
        JSString* text = str(globalObject, module);
        RETURN_IF_EXCEPTION(scope, false);
        constEvaluatorAppendText(globalObject, builder, text);
        RETURN_IF_EXCEPTION(scope, false);
        builder.append('.');
    }
    JSString* text = str(globalObject, qualname);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, constEvaluatorAppendText(globalObject, builder, text));
}

static JSValue constEvaluatorCall(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "constevaluator.__call__"_s, args)))
        return { };
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "constevaluator.__call__() takes exactly 1 argument ("_s, args.size(), " given)"_s);
    int format = toCInt(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = PyRecord::from(self)->get(constEvaluatorValue);
    if (format != annotationFormatString)
        return value;

    StrBuilder builder;
    if (auto* tuple = PyTuple::tryFrom(value)) {
        builder.append('(');
        for (unsigned i = 0; i < tuple->length(); ++i) {
            if (i)
                builder.append(", "_s);
            constEvaluatorAppendTypeRepr(globalObject, builder, tuple->at(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
        builder.append(')');
    } else {
        constEvaluatorAppendTypeRepr(globalObject, builder, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue constEvaluatorRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = repr(globalObject, PyRecord::from(self)->get(constEvaluatorValue));
    RETURN_IF_EXCEPTION(scope, { });
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString("<constevaluator "_s, string, '>'));
}

static constexpr NativeDef constEvaluatorMethods[] = {
    PY_SLOT("__call__", constEvaluatorCall, "($self, *args, **kwargs)"),
    PY_SLOT("__repr__", constEvaluatorRepr, "($self, /)"),
};

// ---- What TypeVar, ParamSpec and TypeVarTuple have in common ----

enum : unsigned {
    typeParameterName,
    typeParameterBound, // Empty: none, or, for a TypeVar, not evaluated yet.
    typeParameterDefault, // Empty: not evaluated yet, or none. NoDefault: none was given to the constructor.
    typeParameterEvaluateDefault, // Empty unless the parameter is from a type parameter list and has a default.
    typeParameterFieldCount,
    // A TypeVar has more.
    typeVarEvaluateBound = typeParameterFieldCount,
    typeVarConstraints,
    typeVarEvaluateConstraints,
    typeVarFieldCount,
};

// In PyRecord::number.
static constexpr int64_t typeParameterIsCovariant = 1;
static constexpr int64_t typeParameterIsContravariant = 2;
static constexpr int64_t typeParameterInfersVariance = 4;

// `bound`, `defaultValue` and `module` may be empty. Null: an exception.
static PyRecord* newTypeParameter(JSGlobalObject* globalObject, PyType* type, unsigned fieldCount, JSValue name, JSValue bound, JSValue defaultValue, int64_t variance, JSValue module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* parameter = PyRecord::create(globalObject, type, fieldCount);
    parameter->set(vm, typeParameterName, name);
    if (bound)
        parameter->set(vm, typeParameterBound, bound);
    if (defaultValue)
        parameter->set(vm, typeParameterDefault, defaultValue);
    parameter->number = variance;
    if (module) {
        setAttr(globalObject, parameter, rt.names.dModule(), module);
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return parameter;
}

// The variance the constructors are given, as it is kept. -1: an exception.
static int64_t typeParameterVariance(JSGlobalObject* globalObject, JSValue covariantArgument, JSValue contravariantArgument, JSValue inferVarianceArgument)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool covariant = typingFlagArgument(globalObject, covariantArgument);
    RETURN_IF_EXCEPTION(scope, -1);
    bool contravariant = typingFlagArgument(globalObject, contravariantArgument);
    RETURN_IF_EXCEPTION(scope, -1);
    bool infersVariance = typingFlagArgument(globalObject, inferVarianceArgument);
    RETURN_IF_EXCEPTION(scope, -1);
    if (covariant && contravariant) {
        raiseValueError(globalObject, scope, "Bivariant types are not supported."_s);
        return -1;
    }
    if (infersVariance && (covariant || contravariant)) {
        raiseValueError(globalObject, scope, "Variance cannot be specified with infer_variance."_s);
        return -1;
    }
    return (covariant ? typeParameterIsCovariant : 0) | (contravariant ? typeParameterIsContravariant : 0) | (infersVariance ? typeParameterInfersVariance : 0);
}

// What one field holds, once the function in another has computed it. `missing`: what it is without such a function.
static JSValue typingEvaluateField(JSGlobalObject* globalObject, PyRecord* record, unsigned valueField, unsigned evaluatorField, JSValue missing)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue value = record->get(valueField))
        return value;
    JSValue evaluator = record->get(evaluatorField);
    if (!evaluator)
        return missing;
    JSValue value = call(globalObject, evaluator);
    RETURN_IF_EXCEPTION(scope, { });
    record->set(vm, valueField, value);
    return value;
}

// The function, or one that stands in for it.
static JSValue typingEvaluatorOfField(JSGlobalObject* globalObject, PyRecord* record, unsigned valueField, unsigned evaluatorField)
{
    if (JSValue evaluator = record->get(evaluatorField))
        return evaluator;
    if (JSValue value = record->get(valueField))
        return newConstEvaluator(globalObject, value);
    return jsUndefined();
}

static JSValue typeParameterGetName(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(typeParameterName); }
static JSValue typeParameterGetStoredBound(JSGlobalObject*, JSValue self) { return typingOrNone(PyRecord::from(self)->get(typeParameterBound)); }
static JSValue typeParameterReduce(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(typeParameterName); }

template<int64_t flag>
static JSValue typeParameterGetFlag(JSGlobalObject*, JSValue self) { return jsBoolean(PyRecord::from(self)->number & flag); }

static JSValue typeParameterGetDefault(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluateField(globalObject, PyRecord::from(self), typeParameterDefault, typeParameterEvaluateDefault, noDefaultObject(globalObject));
}

static JSValue typeParameterGetEvaluateDefault(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluatorOfField(globalObject, PyRecord::from(self), typeParameterDefault, typeParameterEvaluateDefault);
}

static JSValue typeParameterHasDefault(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* parameter = PyRecord::from(self);
    if (parameter->get(typeParameterEvaluateDefault))
        return jsBoolean(true);
    JSValue value = parameter->get(typeParameterDefault);
    return jsBoolean(value && !isNoDefault(globalObject, value));
}

static JSValue typeParameterRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* parameter = PyRecord::from(self);
    JSValue name = parameter->get(typeParameterName);
    if (parameter->number & typeParameterInfersVariance)
        return name;
    String text = strValue(name)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    char variance = parameter->number & typeParameterIsCovariant ? '+' : parameter->number & typeParameterIsContravariant ? '-' : '~';
    return makeStr(vm, makeString(variance, text));
}

static JSValue typeParameterOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return typingUnionOf(globalObject, self, other); }
static JSValue typeParameterROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return typingUnionOf(globalObject, other, self); }

// The parameter as a record, if it is one of the three kinds.
static PyRecord* tryTypeParameter(JSGlobalObject* globalObject, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    if (!PyRecord::is(value))
        return nullptr;
    PyRecord* record = PyRecord::from(value);
    PyType* type = record->type();
    return type == rt.typeVarType() || type == rt.paramSpecType() || type == rt.typeVarTupleType() ? record : nullptr;
}

static JSValue raiseNotTypeParameter(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Expected a type param, got "_s, reprForMessage(globalObject, value));
}

// ---- TypeVar ----

static JSValue typeVarNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "name"_s, "bound"_s, "default"_s, "covariant"_s, "contravariant"_s, "infer_variance"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "typevar"_s, keywords, 1, 1, true, args, values)))
        return { };
    JSValue name = values[0];
    if (!isStr(name))
        RELEASE_AND_RETURN(scope, typingRaiseBadArgument(globalObject, "typevar"_s, "name"_s, "str"_s, name));
    int64_t variance = typeParameterVariance(globalObject, values[3], values[4], values[5]);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue bound = values[1];
    if (bound && isNone(bound))
        bound = JSValue();
    if (bound) {
        bound = typingCheckType(globalObject, bound, "Bound must be a type."_s);
        RETURN_IF_EXCEPTION(scope, { });
    }

    unsigned constraintCount = args.size() > 1 ? args.size() - 1 : 0;
    if (constraintCount == 1)
        return raiseTypeError(globalObject, scope, "A single constraint is not allowed"_s);
    if (constraintCount && bound)
        return raiseTypeError(globalObject, scope, "Constraints cannot be combined with bound=..."_s);

    JSValue defaultValue = values[2] ? values[2] : noDefaultObject(globalObject);
    PyRecord* typeVar = newTypeParameter(globalObject, rt.typeVarType(), typeVarFieldCount, name, bound, defaultValue, variance, typingCallerModule(globalObject));
    RETURN_IF_EXCEPTION(scope, { });
    if (constraintCount)
        typeVar->set(vm, typeVarConstraints, PyTuple::create(globalObject, args.positional().subspan(1)));
    return typeVar;
}

static JSValue typeVarGetBound(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluateField(globalObject, PyRecord::from(self), typeParameterBound, typeVarEvaluateBound, jsUndefined());
}

static JSValue typeVarGetConstraints(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluateField(globalObject, PyRecord::from(self), typeVarConstraints, typeVarEvaluateConstraints, runtime(globalObject).emptyTuple());
}

static JSValue typeVarGetEvaluateBound(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluatorOfField(globalObject, PyRecord::from(self), typeParameterBound, typeVarEvaluateBound);
}

static JSValue typeVarGetEvaluateConstraints(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluatorOfField(globalObject, PyRecord::from(self), typeVarConstraints, typeVarEvaluateConstraints);
}

static JSValue typeVarTypingSubst(JSGlobalObject* globalObject, JSValue self, JSValue argument)
{
    return typingCall(globalObject, "_typevar_subst"_s, self, argument);
}

// The arguments given to alias[...], with the default of this parameter added if it is the first that has none.
static JSValue typeVarTypingPrepareSubst(JSGlobalObject* globalObject, JSValue self, JSValue alias, JSValue arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue parameters = getAttr(globalObject, alias, internString(vm, "__parameters__"_s));
    RETURN_IF_EXCEPTION(scope, { });
    int64_t index = -1;
    int64_t position = 0;
    forEach(globalObject, parameters, [&](JSValue parameter) {
        if (isEqual(globalObject, parameter, self)) {
            index = position;
            return false;
        }
        ++position;
        return true;
    });
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "sequence.index(x): x not in sequence"_s);
    int64_t argumentCount = length(globalObject, arguments);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < argumentCount)
        return arguments;
    if (index == argumentCount) {
        JSValue defaultValue = typeParameterGetDefault(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNoDefault(globalObject, defaultValue))
            RELEASE_AND_RETURN(scope, binaryOperation(globalObject, BinaryOperator::Add, arguments, PyTuple::create(globalObject, { defaultValue })));
    }
    JSString* aliasText = str(globalObject, alias);
    RETURN_IF_EXCEPTION(scope, { });
    String aliasString = aliasText->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return raiseTypeError(globalObject, scope, "Too few arguments for "_s, aliasString, "; actual "_s, argumentCount, ", expected at least "_s, index + 1);
}

static JSValue typeVarMroEntries(JSGlobalObject* globalObject, JSValue, JSValue) { return typingRaiseCannotSubclass(globalObject, "TypeVar"_s); }

static constexpr NativeDef typeVarMethods[] = {
    PY_NEW(typeVarNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", typeParameterRepr, "($self, /)"),
    PY_SLOT("__or__", typeParameterOr, "($self, value, /)"),
    PY_SLOT("__ror__", typeParameterROr, "($self, value, /)"),
    PY_METHOD("__typing_subst__", typeVarTypingSubst, "($self, arg, /)"),
    PY_METHOD("__typing_prepare_subst__", typeVarTypingPrepareSubst, "($self, alias, args, /)"),
    PY_METHOD("__reduce__", typeParameterReduce, "($self, /)"),
    PY_METHOD("has_default", typeParameterHasDefault, "($self, /)"),
    PY_METHOD("__mro_entries__", typeVarMroEntries, "($self, bases, /)"),
};

static constexpr NativeGetSet typeVarGetSets[] = {
    PY_MEMBER("__name__", typeParameterGetName),
    PY_MEMBER("__covariant__", typeParameterGetFlag<typeParameterIsCovariant>),
    PY_MEMBER("__contravariant__", typeParameterGetFlag<typeParameterIsContravariant>),
    PY_MEMBER("__infer_variance__", typeParameterGetFlag<typeParameterInfersVariance>),
    { "__bound__"_s, typeVarGetBound },
    { "__constraints__"_s, typeVarGetConstraints },
    { "__default__"_s, typeParameterGetDefault },
    { "evaluate_bound"_s, typeVarGetEvaluateBound },
    { "evaluate_constraints"_s, typeVarGetEvaluateConstraints },
    { "evaluate_default"_s, typeParameterGetEvaluateDefault },
};

// ---- ParamSpecArgs, ParamSpecKwargs ----

enum : unsigned { paramSpecAttributeOrigin, paramSpecAttributeFieldCount };

static JSValue newParamSpecAttribute(JSGlobalObject* globalObject, PyType* type, JSValue origin)
{
    PyRecord* attribute = PyRecord::create(globalObject, type, paramSpecAttributeFieldCount);
    attribute->set(getVM(globalObject), paramSpecAttributeOrigin, origin);
    return attribute;
}

static JSValue paramSpecAttributeNew(JSGlobalObject* globalObject, ASCIILiteral functionName, JSValue cls, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "origin"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, functionName, keywords, 1, 1, false, args, values)))
        return { };
    RELEASE_AND_RETURN(scope, newParamSpecAttribute(globalObject, PyType::from(cls), values[0]));
}

static JSValue paramSpecArgsNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return paramSpecAttributeNew(globalObject, "paramspecargs"_s, cls, args); }
static JSValue paramSpecKwargsNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return paramSpecAttributeNew(globalObject, "paramspeckwargs"_s, cls, args); }

static JSValue paramSpecAttributeRepr(JSGlobalObject* globalObject, JSValue self, ASCIILiteral suffix)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue origin = PyRecord::from(self)->get(paramSpecAttributeOrigin);
    JSString* text;
    if (PyRecord* paramSpec = PyRecord::tryFrom(origin, runtime(globalObject).paramSpecType()))
        text = strValue(paramSpec->get(typeParameterName));
    else {
        text = repr(globalObject, origin);
        RETURN_IF_EXCEPTION(scope, { });
    }
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(string, suffix));
}

static JSValue paramSpecArgsRepr(JSGlobalObject* globalObject, JSValue self) { return paramSpecAttributeRepr(globalObject, self, ".args"_s); }
static JSValue paramSpecKwargsRepr(JSGlobalObject* globalObject, JSValue self) { return paramSpecAttributeRepr(globalObject, self, ".kwargs"_s); }

template<CompareOperator op>
static JSValue paramSpecAttributeCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    if (rt.typeOf(self) != rt.typeOf(other))
        return rt.notImplemented();
    return richCompare(globalObject, PyRecord::from(self)->get(paramSpecAttributeOrigin), PyRecord::from(other)->get(paramSpecAttributeOrigin), op);
}

static JSValue paramSpecAttributeGetOrigin(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(paramSpecAttributeOrigin); }
static JSValue paramSpecArgsMroEntries(JSGlobalObject* globalObject, JSValue, JSValue) { return typingRaiseCannotSubclass(globalObject, "ParamSpecArgs"_s); }
static JSValue paramSpecKwargsMroEntries(JSGlobalObject* globalObject, JSValue, JSValue) { return typingRaiseCannotSubclass(globalObject, "ParamSpecKwargs"_s); }

static constexpr NativeDef paramSpecArgsMethods[] = {
    PY_NEW(paramSpecArgsNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", paramSpecArgsRepr, "($self, /)"),
    PY_SLOT("__eq__", paramSpecAttributeCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", paramSpecAttributeCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_METHOD("__mro_entries__", paramSpecArgsMroEntries, "($self, bases, /)"),
};

static constexpr NativeDef paramSpecKwargsMethods[] = {
    PY_NEW(paramSpecKwargsNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", paramSpecKwargsRepr, "($self, /)"),
    PY_SLOT("__eq__", paramSpecAttributeCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", paramSpecAttributeCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_METHOD("__mro_entries__", paramSpecKwargsMroEntries, "($self, bases, /)"),
};

static constexpr NativeGetSet paramSpecAttributeGetSets[] = {
    PY_MEMBER("__origin__", paramSpecAttributeGetOrigin),
};

// ---- ParamSpec ----

static JSValue paramSpecNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "name"_s, "bound"_s, "default"_s, "covariant"_s, "contravariant"_s, "infer_variance"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "paramspec"_s, keywords, 1, 1, false, args, values)))
        return { };
    JSValue name = values[0];
    if (!isStr(name))
        RELEASE_AND_RETURN(scope, typingRaiseBadArgument(globalObject, "paramspec"_s, "name"_s, "str"_s, name));
    int64_t variance = typeParameterVariance(globalObject, values[3], values[4], values[5]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue defaultValue = values[2] ? values[2] : noDefaultObject(globalObject);
    RELEASE_AND_RETURN(scope, newTypeParameter(globalObject, runtime(globalObject).paramSpecType(), typeParameterFieldCount, name, typingOrNone(values[1]), defaultValue, variance, typingCallerModule(globalObject)));
}

static JSValue paramSpecGetArgs(JSGlobalObject* globalObject, JSValue self) { return newParamSpecAttribute(globalObject, runtime(globalObject).paramSpecArgsType(), self); }
static JSValue paramSpecGetKwargs(JSGlobalObject* globalObject, JSValue self) { return newParamSpecAttribute(globalObject, runtime(globalObject).paramSpecKwargsType(), self); }
static JSValue paramSpecTypingSubst(JSGlobalObject* globalObject, JSValue self, JSValue argument) { return typingCall(globalObject, "_paramspec_subst"_s, self, argument); }
static JSValue paramSpecTypingPrepareSubst(JSGlobalObject* globalObject, JSValue self, JSValue alias, JSValue arguments) { return typingCall(globalObject, "_paramspec_prepare_subst"_s, self, alias, arguments); }
static JSValue paramSpecMroEntries(JSGlobalObject* globalObject, JSValue, JSValue) { return typingRaiseCannotSubclass(globalObject, "ParamSpec"_s); }

static constexpr NativeDef paramSpecMethods[] = {
    PY_NEW(paramSpecNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", typeParameterRepr, "($self, /)"),
    PY_SLOT("__or__", typeParameterOr, "($self, value, /)"),
    PY_SLOT("__ror__", typeParameterROr, "($self, value, /)"),
    PY_METHOD("__typing_subst__", paramSpecTypingSubst, "($self, arg, /)"),
    PY_METHOD("__typing_prepare_subst__", paramSpecTypingPrepareSubst, "($self, alias, args, /)"),
    PY_METHOD("has_default", typeParameterHasDefault, "($self, /)"),
    PY_METHOD("__reduce__", typeParameterReduce, "($self, /)"),
    PY_METHOD("__mro_entries__", paramSpecMroEntries, "($self, bases, /)"),
};

static constexpr NativeGetSet paramSpecGetSets[] = {
    PY_MEMBER("__name__", typeParameterGetName),
    PY_MEMBER("__bound__", typeParameterGetStoredBound),
    PY_MEMBER("__covariant__", typeParameterGetFlag<typeParameterIsCovariant>),
    PY_MEMBER("__contravariant__", typeParameterGetFlag<typeParameterIsContravariant>),
    PY_MEMBER("__infer_variance__", typeParameterGetFlag<typeParameterInfersVariance>),
    { "args"_s, paramSpecGetArgs },
    { "kwargs"_s, paramSpecGetKwargs },
    { "__default__"_s, typeParameterGetDefault },
    { "evaluate_default"_s, typeParameterGetEvaluateDefault },
};

// ---- TypeVarTuple ----

static JSValue typeVarTupleNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "name"_s, "bound"_s, "covariant"_s, "contravariant"_s, "infer_variance"_s, "default"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "typevartuple"_s, keywords, 1, 1, false, args, values)))
        return { };
    JSValue name = values[0];
    if (!isStr(name))
        RELEASE_AND_RETURN(scope, typingRaiseBadArgument(globalObject, "typevartuple"_s, "name"_s, "str"_s, name));
    int64_t variance = typeParameterVariance(globalObject, values[2], values[3], values[4]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue defaultValue = values[5] ? values[5] : noDefaultObject(globalObject);
    RELEASE_AND_RETURN(scope, newTypeParameter(globalObject, runtime(globalObject).typeVarTupleType(), typeParameterFieldCount, name, typingOrNone(values[1]), defaultValue, variance, typingCallerModule(globalObject)));
}

static JSValue typeVarTupleTypingSubst(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Substitution of bare TypeVarTuple is not supported"_s);
}

static JSValue typeVarTupleTypingPrepareSubst(JSGlobalObject* globalObject, JSValue self, JSValue alias, JSValue arguments) { return typingCall(globalObject, "_typevartuple_prepare_subst"_s, self, alias, arguments); }
static JSValue typeVarTupleMroEntries(JSGlobalObject* globalObject, JSValue, JSValue) { return typingRaiseCannotSubclass(globalObject, "TypeVarTuple"_s); }

static constexpr NativeDef typeVarTupleMethods[] = {
    PY_NEW(typeVarTupleNew, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", typingUnpackIter, "($self, /)"),
    PY_SLOT("__repr__", typeParameterRepr, "($self, /)"),
    PY_METHOD("__typing_subst__", typeVarTupleTypingSubst, "($self, arg, /)"),
    PY_METHOD("__typing_prepare_subst__", typeVarTupleTypingPrepareSubst, "($self, alias, args, /)"),
    PY_METHOD("__reduce__", typeParameterReduce, "($self, /)"),
    PY_METHOD("has_default", typeParameterHasDefault, "($self, /)"),
    PY_METHOD("__mro_entries__", typeVarTupleMroEntries, "($self, bases, /)"),
};

static constexpr NativeGetSet typeVarTupleGetSets[] = {
    PY_MEMBER("__name__", typeParameterGetName),
    PY_MEMBER("__bound__", typeParameterGetStoredBound),
    PY_MEMBER("__covariant__", typeParameterGetFlag<typeParameterIsCovariant>),
    PY_MEMBER("__contravariant__", typeParameterGetFlag<typeParameterIsContravariant>),
    PY_MEMBER("__infer_variance__", typeParameterGetFlag<typeParameterInfersVariance>),
    { "__default__"_s, typeParameterGetDefault },
    { "evaluate_default"_s, typeParameterGetEvaluateDefault },
};

// ---- TypeAliasType ----

enum : unsigned {
    typeAliasName,
    typeAliasQualname,
    typeAliasTypeParams, // A tuple that is not empty, or nothing.
    typeAliasComputeValue, // Empty for one made by calling the type.
    typeAliasValue, // Empty: not computed yet.
    typeAliasModule, // Empty: that of the function that computes the value.
    typeAliasFieldCount,
};

static PyRecord* newTypeAlias(JSGlobalObject* globalObject, JSValue name, JSValue qualname, JSValue typeParams, JSValue computeValue, JSValue value, JSValue module)
{
    VM& vm = getVM(globalObject);
    PyRecord* alias = PyRecord::create(globalObject, runtime(globalObject).typeAliasTypeType(), typeAliasFieldCount);
    alias->set(vm, typeAliasName, name);
    alias->set(vm, typeAliasQualname, qualname);
    if (typeParams)
        alias->set(vm, typeAliasTypeParams, typeParams);
    if (computeValue)
        alias->set(vm, typeAliasComputeValue, computeValue);
    if (value)
        alias->set(vm, typeAliasValue, value);
    if (module)
        alias->set(vm, typeAliasModule, module);
    return alias;
}

// Those with a default come last. False: an exception.
static bool typeAliasCheckTypeParams(JSGlobalObject* globalObject, PyTuple* typeParams)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool sawDefault = false;
    for (unsigned i = 0; i < typeParams->length(); ++i) {
        JSValue parameter = typeParams->at(i);
        if (!tryTypeParameter(globalObject, parameter)) {
            scope.release();
            raiseNotTypeParameter(globalObject, parameter);
            return false;
        }
        JSValue defaultValue = typeParameterGetDefault(globalObject, parameter);
        RETURN_IF_EXCEPTION(scope, false);
        if (!isNoDefault(globalObject, defaultValue))
            sawDefault = true;
        else if (sawDefault) {
            raiseTypeError(globalObject, scope, "non-default type parameter '"_s, reprForMessage(globalObject, parameter), "' follows default type parameter"_s);
            return false;
        }
    }
    return true;
}

static JSValue typeAliasNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "name"_s, "value"_s, "type_params"_s, "qualname"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "typealias"_s, keywords, 2, 2, false, args, values)))
        return { };
    JSValue name = values[0];
    if (!isStr(name))
        RELEASE_AND_RETURN(scope, typingRaiseBadArgument(globalObject, "typealias"_s, "name"_s, "str"_s, name));

    JSValue typeParams = values[2];
    if (typeParams) {
        auto* tuple = PyTuple::tryFrom(typeParams);
        if (!tuple)
            return raiseTypeError(globalObject, scope, "type_params must be a tuple"_s);
        bool isValid = typeAliasCheckTypeParams(globalObject, tuple);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(isValid, isValid);
        if (!tuple->length())
            typeParams = JSValue();
    }

    JSValue qualname = values[3];
    if (!qualname || isNone(qualname))
        qualname = name;
    else if (!isStr(qualname))
        return raiseTypeError(globalObject, scope, "qualname must be a string"_s);

    RELEASE_AND_RETURN(scope, newTypeAlias(globalObject, name, qualname, typeParams, { }, values[1], typingCallerModule(globalObject)));
}

static JSValue typeAliasGetName(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(typeAliasName); }
static JSValue typeAliasGetQualname(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(typeAliasQualname); }
static JSValue typeAliasRepr(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(typeAliasQualname); }
static JSValue typeAliasReduce(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(typeAliasName); }

static JSValue typeAliasGetValue(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluateField(globalObject, PyRecord::from(self), typeAliasValue, typeAliasComputeValue, jsUndefined());
}

static JSValue typeAliasGetEvaluateValue(JSGlobalObject* globalObject, JSValue self)
{
    return typingEvaluatorOfField(globalObject, PyRecord::from(self), typeAliasValue, typeAliasComputeValue);
}

static JSValue typeAliasGetTypeParams(JSGlobalObject* globalObject, JSValue self)
{
    JSValue typeParams = PyRecord::from(self)->get(typeAliasTypeParams);
    return typeParams ? typeParams : JSValue(runtime(globalObject).emptyTuple());
}

static JSValue typeAliasGetParameters(JSGlobalObject* globalObject, JSValue self)
{
    JSValue typeParams = PyRecord::from(self)->get(typeAliasTypeParams);
    if (!typeParams)
        return runtime(globalObject).emptyTuple();
    return typingUnpackTypeVarTuples(globalObject, PyTuple::from(typeParams));
}

static JSValue typeAliasGetModule(JSGlobalObject*, JSValue self)
{
    PyRecord* alias = PyRecord::from(self);
    if (JSValue module = alias->get(typeAliasModule))
        return module;
    if (JSValue computeValue = alias->get(typeAliasComputeValue))
        return typingOrNone(PyFunction::from(computeValue)->module());
    return jsUndefined();
}

static void typeAliasSetModule(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyRecord::from(self)->set(getVM(globalObject), typeAliasModule, value);
}

static JSValue typeAliasSubscript(JSGlobalObject* globalObject, JSValue self, JSValue arguments)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!PyRecord::from(self)->get(typeAliasTypeParams))
        return raiseTypeError(globalObject, scope, "Only generic type aliases are subscriptable"_s);
    RELEASE_AND_RETURN(scope, makeGenericAlias(globalObject, self, arguments));
}

static JSValue typeAliasOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, self, other); }
static JSValue typeAliasROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return makeUnion(globalObject, other, self); }

static constexpr NativeDef typeAliasMethods[] = {
    PY_NEW(typeAliasNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", typeAliasRepr, "($self, /)"),
    PY_SLOT("__iter__", typingUnpackIter, "($self, /)"),
    PY_SLOT("__getitem__", typeAliasSubscript, "($self, key, /)"),
    PY_SLOT("__or__", typeAliasOr, "($self, value, /)"),
    PY_SLOT("__ror__", typeAliasROr, "($self, value, /)"),
    PY_METHOD("__reduce__", typeAliasReduce, "($self, /)"),
};

static constexpr NativeGetSet typeAliasGetSets[] = {
    PY_MEMBER("__name__", typeAliasGetName),
    PY_MEMBER("__qualname__", typeAliasGetQualname),
    { "__parameters__"_s, typeAliasGetParameters },
    { "__type_params__"_s, typeAliasGetTypeParams },
    { "__value__"_s, typeAliasGetValue },
    { "evaluate_value"_s, typeAliasGetEvaluateValue },
    { "__module__"_s, typeAliasGetModule, typeAliasSetModule },
};

// ---- Generic ----

static JSValue typingGenericClassGetItem(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return typingCallWithClass(globalObject, "_generic_class_getitem"_s, cls, args); }
static JSValue typingGenericInitSubclass(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args) { return typingCallWithClass(globalObject, "_generic_init_subclass"_s, cls, args); }

static constexpr NativeDef typingGenericMethods[] = {
    PY_CLASSMETHOD("__class_getitem__", typingGenericClassGetItem, "($type, *args, **kwargs)"),
    PY_CLASSMETHOD("__init_subclass__", typingGenericInitSubclass, "($type, *args, **kwargs)"),
};

// ---- What the compiled code of type parameter lists and `type` statements calls ----

JSValue makeTypeVar(JSGlobalObject* globalObject, JSValue name, JSValue evaluateBound, JSValue evaluateConstraints)
{
    VM& vm = getVM(globalObject);
    PyRecord* typeVar = newTypeParameter(globalObject, runtime(globalObject).typeVarType(), typeVarFieldCount, name, { }, { }, typeParameterInfersVariance, { });
    if (evaluateBound)
        typeVar->set(vm, typeVarEvaluateBound, evaluateBound);
    if (evaluateConstraints)
        typeVar->set(vm, typeVarEvaluateConstraints, evaluateConstraints);
    return typeVar;
}

JSValue makeParamSpec(JSGlobalObject* globalObject, JSValue name)
{
    return newTypeParameter(globalObject, runtime(globalObject).paramSpecType(), typeParameterFieldCount, name, { }, { }, typeParameterInfersVariance, { });
}

JSValue makeTypeVarTuple(JSGlobalObject* globalObject, JSValue name)
{
    return newTypeParameter(globalObject, runtime(globalObject).typeVarTupleType(), typeParameterFieldCount, name, { }, { }, typeParameterInfersVariance, { });
}

JSValue setTypeParamDefault(JSGlobalObject* globalObject, JSValue typeParameter, JSValue evaluateDefault)
{
    PyRecord* parameter = tryTypeParameter(globalObject, typeParameter);
    if (!parameter)
        return raiseNotTypeParameter(globalObject, typeParameter);
    parameter->set(getVM(globalObject), typeParameterEvaluateDefault, evaluateDefault);
    return typeParameter;
}

JSValue subscriptGeneric(JSGlobalObject* globalObject, JSValue parameters)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue unpacked = typingUnpackTypeVarTuples(globalObject, PyTuple::from(parameters));
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, typingCall(globalObject, "_GenericAlias"_s, runtime(globalObject).genericType(), unpacked));
}

JSValue makeTypeAlias(JSGlobalObject* globalObject, JSValue name, JSValue parametersOrNone, JSValue evaluateValue)
{
    JSValue typeParams = parametersOrNone;
    if (typeParams && (isNone(typeParams) || (PyTuple::is(typeParams) && !PyTuple::from(typeParams)->length())))
        typeParams = JSValue();
    JSValue qualname = makeStr(globalObject, PyFunction::from(evaluateValue)->codeInfo().qualname);
    return newTypeAlias(globalObject, name, qualname, typeParams, evaluateValue, { }, { });
}

// ---- The types, and the module _typing ----

void initializeTypingTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto define = [&](PyType* type, ASCIILiteral name, OptionSet<TypeFlag> flags, std::span<const NativeDef> methods, std::span<const NativeGetSet> getsets) {
        type->initializeBuiltin(globalObject, { name, flags, methods, getsets, { } }, rt.typeType(), rt.objectType());
    };
    // With __eq__ and without __hash__.
    auto makeUnhashable = [&](PyType* type) {
        type->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        type->didChange(vm);
    };

    define(rt.noDefaultType(), "NoDefaultType"_s, { TypeFlag::NothingOfItsOwn }, noDefaultMethods, { });
    rt.noDefaultType()->putDirect(vm, rt.dictKey(), PyObject::create(vm, rt.structure<PyObject>(), rt.noDefaultType()), PropertyAttribute::DontEnum | 0);
    define(rt.constEvaluatorType(), "_typing._ConstEvaluator"_s, { }, constEvaluatorMethods, { });

    // The instances have a dict, for __module__ and whatever else is set on them, but no __dict__ attribute.
    define(rt.typeVarType(), "typing.TypeVar"_s, { TypeFlag::HasDict, TypeFlag::HasWeakref }, typeVarMethods, typeVarGetSets);
    define(rt.paramSpecType(), "typing.ParamSpec"_s, { TypeFlag::HasDict, TypeFlag::HasWeakref }, paramSpecMethods, paramSpecGetSets);
    define(rt.typeVarTupleType(), "typing.TypeVarTuple"_s, { TypeFlag::HasDict, TypeFlag::HasWeakref }, typeVarTupleMethods, typeVarTupleGetSets);
    for (PyType* type : { rt.typeVarType(), rt.paramSpecType(), rt.typeVarTupleType() }) {
        type->dict()->remove(vm, rt.names.dDict());
        // What a type parameter of `def f[T]` has for a __module__: nothing sets one on the instance.
        type->dict()->set(vm, rt.names.dModule(), internString(vm, "typing"_s));
        type->didChange(vm);
    }
    define(rt.paramSpecArgsType(), "typing.ParamSpecArgs"_s, { TypeFlag::HasWeakref }, paramSpecArgsMethods, paramSpecAttributeGetSets);
    makeUnhashable(rt.paramSpecArgsType());
    define(rt.paramSpecKwargsType(), "typing.ParamSpecKwargs"_s, { TypeFlag::HasWeakref }, paramSpecKwargsMethods, paramSpecAttributeGetSets);
    makeUnhashable(rt.paramSpecKwargsType());
    define(rt.typeAliasTypeType(), "typing.TypeAliasType"_s, { }, typeAliasMethods, typeAliasGetSets);

    // Its instances are made as those of object are.
    define(rt.genericType(), "typing.Generic"_s, { TypeFlag::Base, TypeFlag::Mutable }, typingGenericMethods, { });
    rt.genericType()->remove(TypeFlag::DisallowInstantiation);
}

static JSValue typingIdFunc(JSGlobalObject*, JSValue, JSValue x) { return x; }

static constexpr NativeDef typingModuleFunctions[] = {
    PY_FUNCTION("_idfunc", typingIdFunc, "($module, x, /)"),
};

void initializeTypingModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    module->add(globalObject, "__doc__"_s, makeStr(rt.vm(), "Primitives and accelerators for the typing module.\n"_s));
    module->addFunctions(globalObject, typingModuleFunctions);
    module->add(globalObject, "TypeVar"_s, rt.typeVarType());
    module->add(globalObject, "TypeVarTuple"_s, rt.typeVarTupleType());
    module->add(globalObject, "ParamSpec"_s, rt.paramSpecType());
    module->add(globalObject, "ParamSpecArgs"_s, rt.paramSpecArgsType());
    module->add(globalObject, "ParamSpecKwargs"_s, rt.paramSpecKwargsType());
    module->add(globalObject, "Generic"_s, rt.genericType());
    module->add(globalObject, "TypeAliasType"_s, rt.typeAliasTypeType());
    module->add(globalObject, "Union"_s, rt.unionType());
    module->add(globalObject, "NoDefault"_s, noDefaultObject(globalObject));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
