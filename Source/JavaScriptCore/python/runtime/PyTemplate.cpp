/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "JSCInlines.h"
#include "PyErrors.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyTypes.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// What a template string literal, t"...", evaluates to: string.templatelib.Template and Interpolation.
// (Objects/templateobject.c and Objects/interpolationobject.c in CPython are the reference.)
//
// The instances are PyRecords. Neither type can be derived from.

// In PyTyping.cpp.
bool unpackKeywordArguments(JSGlobalObject*, ASCIILiteral functionName, std::span<const ASCIILiteral> keywords, unsigned minPositional, unsigned maxPositional, bool hasVarPositional, const CallArgs&, JSValue* values);

// repr(value), for a longer text. A null string: an exception.
static String templateReprOf(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    String string = text->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return string.isNull() ? emptyString() : string;
}

// ---- Interpolation ----

enum : unsigned {
    interpolationValue,
    interpolationExpression,
    interpolationConversion, // None, or one of "s", "r" and "a".
    interpolationFormatSpec,
    interpolationFieldCount,
};

static PyRecord* newInterpolation(JSGlobalObject* globalObject, JSValue value, JSValue expression, JSValue conversion, JSValue formatSpec)
{
    VM& vm = getVM(globalObject);
    PyRecord* interpolation = PyRecord::create(globalObject, runtime(globalObject).interpolationType(), interpolationFieldCount);
    interpolation->set(vm, interpolationValue, value);
    interpolation->set(vm, interpolationExpression, expression);
    interpolation->set(vm, interpolationConversion, conversion);
    interpolation->set(vm, interpolationFormatSpec, formatSpec);
    return interpolation;
}

static JSValue interpolationRaiseNotStr(JSGlobalObject* globalObject, ASCIILiteral argument, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseTypeError(globalObject, scope, "Interpolation() argument '"_s, argument, "' must be str, not "_s, isNone(value) ? "None"_str : typeName(globalObject, value));
}

static JSValue interpolationNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "value"_s, "expression"_s, "conversion"_s, "format_spec"_s };
    JSValue values[std::size(keywords)];
    if (failed(scope, unpackKeywordArguments(globalObject, "Interpolation"_s, keywords, 1, 4, false, args, values)))
        return { };

    JSValue expression = values[1] ? values[1] : JSValue(rt.names.empty());
    if (!isStr(expression))
        RELEASE_AND_RETURN(scope, interpolationRaiseNotStr(globalObject, "expression"_s, expression));

    JSValue conversion = jsUndefined();
    if (values[2] && !isNone(values[2])) {
        conversion = values[2];
        if (!isStr(conversion))
            return raiseTypeError(globalObject, scope, "Interpolation() argument 'conversion' must be str, not "_s, typeNameForMessage(globalObject, rt.typeOf(conversion)));
        String text = strValue(conversion)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (text.length() != 1 || (text[0] != 'a' && text[0] != 'r' && text[0] != 's'))
            return raiseValueError(globalObject, scope, "Interpolation() argument 'conversion' must be one of 's', 'a' or 'r'"_s);
    }

    JSValue formatSpec = values[3] ? values[3] : JSValue(rt.names.empty());
    if (!isStr(formatSpec))
        RELEASE_AND_RETURN(scope, interpolationRaiseNotStr(globalObject, "format_spec"_s, formatSpec));

    RELEASE_AND_RETURN(scope, newInterpolation(globalObject, values[0], expression, conversion, formatSpec));
}

static JSValue interpolationRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* interpolation = PyRecord::from(self);
    StrBuilder builder;
    builder.append(interpolation->type()->name(), '(');
    for (unsigned i = 0; i < interpolationFieldCount; ++i) {
        String text = templateReprOf(globalObject, interpolation->get(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (i)
            builder.append(", "_s);
        builder.append(text);
    }
    builder.append(')');
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

static JSValue interpolationReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* interpolation = PyRecord::from(self);
    PyTuple* arguments = PyTuple::create(globalObject, { interpolation->get(interpolationValue), interpolation->get(interpolationExpression), interpolation->get(interpolationConversion), interpolation->get(interpolationFormatSpec) });
    return PyTuple::create(globalObject, { interpolation->type(), arguments });
}

template<unsigned field>
static JSValue interpolationGetField(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(field); }

static constexpr NativeDef interpolationMethods[] = {
    PY_NEW(interpolationNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", interpolationRepr, "($self, /)"),
    PY_METHOD("__reduce__", interpolationReduce, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet interpolationGetSets[] = {
    PY_MEMBER("value", interpolationGetField<interpolationValue>),
    PY_MEMBER("expression", interpolationGetField<interpolationExpression>),
    PY_MEMBER("conversion", interpolationGetField<interpolationConversion>),
    PY_MEMBER("format_spec", interpolationGetField<interpolationFormatSpec>),
};

// The conversion may also be as the compiler has it: 0, or the character as a number.
JSValue buildInterpolation(JSGlobalObject* globalObject, JSValue value, JSValue expression, JSValue conversion, JSValue formatSpec)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (conversion.isInt32()) {
        int32_t character = conversion.asInt32();
        if (!character)
            conversion = jsUndefined();
        else if (character == 'a' || character == 'r' || character == 's')
            conversion = jsSingleCharacterString(vm, static_cast<Latin1Character>(character));
        else
            return raiseSystemError(globalObject, scope, "Interpolation() argument 'conversion' must be one of 's', 'a' or 'r'"_s);
    }
    RELEASE_AND_RETURN(scope, newInterpolation(globalObject, value, expression, conversion, formatSpec ? formatSpec : JSValue(rt.names.empty())));
}

// ---- Template ----

enum : unsigned {
    templateStrings, // A tuple of str, one more than there are interpolations: what is before, between and after them.
    templateInterpolations, // A tuple.
    templateFieldCount,
};

static ALWAYS_INLINE PyTuple* templateStringsOf(PyRecord* object) { return PyTuple::from(object->get(templateStrings)); }
static ALWAYS_INLINE PyTuple* templateInterpolationsOf(PyRecord* object) { return PyTuple::from(object->get(templateInterpolations)); }

JSValue buildTemplate(JSGlobalObject* globalObject, JSValue strings, JSValue interpolations)
{
    VM& vm = getVM(globalObject);
    PyRecord* object = PyRecord::create(globalObject, runtime(globalObject).templateStringType(), templateFieldCount);
    object->set(vm, templateStrings, strings);
    object->set(vm, templateInterpolations, interpolations);
    return object;
}

// Template(*parts): strings next to each other are joined, and there is an empty one wherever one is missing.
static JSValue templateNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.hasKeywords())
        return raiseTypeError(globalObject, scope, "Template.__new__ only accepts *args arguments"_s);

    MarkedArgumentBuffer strings;
    MarkedArgumentBuffer interpolations;
    bool lastWasString = false;
    for (JSValue item : args.positional()) {
        if (!isStr(item) && !PyRecord::tryFrom(item, rt.interpolationType()))
            return raiseTypeError(globalObject, scope, "Template.__new__ *args need to be of type 'str' or 'Interpolation', got "_s, typeNameForMessage(globalObject, rt.typeOf(item)));
    }
    for (JSValue item : args.positional()) {
        if (isStr(item)) {
            if (lastWasString) {
                JSString* joined = jsString(globalObject, strValue(strings.last()), strValue(item));
                RETURN_IF_EXCEPTION(scope, { });
                strings.removeLast();
                strings.append(joined);
            } else
                strings.append(item);
            lastWasString = true;
            continue;
        }
        if (!lastWasString)
            strings.append(rt.names.empty());
        interpolations.append(item);
        lastWasString = false;
    }
    if (!lastWasString)
        strings.append(rt.names.empty());
    RELEASE_AND_RETURN(scope, buildTemplate(globalObject, PyTuple::create(globalObject, spanOf(strings)), PyTuple::create(globalObject, spanOf(interpolations))));
}

static JSValue templateRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* object = PyRecord::from(self);
    String strings = templateReprOf(globalObject, object->get(templateStrings));
    RETURN_IF_EXCEPTION(scope, { });
    String interpolations = templateReprOf(globalObject, object->get(templateInterpolations));
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(object->type()->name(), "(strings="_s, strings, ", interpolations="_s, interpolations, ')'));
}

static JSValue templateConcat(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* left = PyRecord::from(self);
    PyRecord* right = PyRecord::tryFrom(other, rt.templateStringType());
    if (!right)
        return raiseTypeError(globalObject, scope, "can only concatenate string.templatelib.Template (not \""_s, typeNameForMessage(globalObject, rt.typeOf(other)), "\") to string.templatelib.Template"_s);

    // The last string of one and the first of the other become one.
    PyTuple* leftStrings = templateStringsOf(left);
    PyTuple* rightStrings = templateStringsOf(right);
    unsigned leftCount = leftStrings->length();
    unsigned rightCount = rightStrings->length();
    JSString* joined = jsString(globalObject, strValue(leftStrings->at(leftCount - 1)), strValue(rightStrings->at(0)));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* strings = PyTuple::create(globalObject, leftCount + rightCount - 1);
    unsigned index = 0;
    for (unsigned i = 0; i + 1 < leftCount; ++i)
        strings->initializeAt(vm, index++, leftStrings->at(i));
    strings->initializeAt(vm, index++, joined);
    for (unsigned i = 1; i < rightCount; ++i)
        strings->initializeAt(vm, index++, rightStrings->at(i));

    PyTuple* leftInterpolations = templateInterpolationsOf(left);
    PyTuple* rightInterpolations = templateInterpolationsOf(right);
    PyTuple* interpolations = PyTuple::create(globalObject, leftInterpolations->length() + rightInterpolations->length());
    index = 0;
    for (JSValue item : leftInterpolations->span())
        interpolations->initializeAt(vm, index++, item);
    for (JSValue item : rightInterpolations->span())
        interpolations->initializeAt(vm, index++, item);
    RELEASE_AND_RETURN(scope, buildTemplate(globalObject, strings, interpolations));
}

static JSValue templateGetStrings(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(templateStrings); }
static JSValue templateGetInterpolations(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(templateInterpolations); }

static JSValue templateGetValues(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyTuple* interpolations = templateInterpolationsOf(PyRecord::from(self));
    PyTuple* values = PyTuple::create(globalObject, interpolations->length());
    for (unsigned i = 0; i < interpolations->length(); ++i)
        values->initializeAt(vm, i, PyRecord::from(interpolations->at(i))->get(interpolationValue));
    return values;
}

static JSValue templateReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "string.templatelib"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue function = getAttr(globalObject, module, internString(vm, "_template_unpickle"_s));
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* object = PyRecord::from(self);
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { function, PyTuple::create(globalObject, { object->get(templateStrings), object->get(templateInterpolations) }) }));
}

// ---- Iterating over a template: its strings, but for the empty ones, and its interpolations, in turn ----

enum : unsigned {
    templateIteratorStrings, // An iterator over the strings.
    templateIteratorInterpolations, // An iterator over the interpolations.
    templateIteratorFieldCount,
};

// PyRecord::number tells whether a string comes next.

static JSValue templateIter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* object = PyRecord::from(self);
    JSValue strings = getIter(globalObject, object->get(templateStrings));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue interpolations = getIter(globalObject, object->get(templateInterpolations));
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* iterator = PyRecord::create(globalObject, runtime(globalObject).templateStringIteratorType(), templateIteratorFieldCount);
    iterator->set(vm, templateIteratorStrings, strings);
    iterator->set(vm, templateIteratorInterpolations, interpolations);
    iterator->number = true;
    return iterator;
}

static JSValue templateIteratorIter(JSGlobalObject*, JSValue self) { return self; }

static JSValue templateIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    JSValue item;
    if (iterator->number) {
        iterator->number = false;
        item = iterNext(globalObject, iterator->get(templateIteratorStrings));
        RETURN_IF_EXCEPTION(scope, { });
        if (item && !strValue(item)->length()) {
            iterator->number = true;
            item = iterNext(globalObject, iterator->get(templateIteratorInterpolations));
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else {
        iterator->number = true;
        item = iterNext(globalObject, iterator->get(templateIteratorInterpolations));
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (!item)
        return raiseStopIteration(globalObject, scope);
    return item;
}

static constexpr NativeDef templateIteratorMethods[] = {
    PY_SLOT("__iter__", templateIteratorIter, "($self, /)"),
    PY_SLOT("__next__", templateIteratorNext, "($self, /)"),
};

static constexpr NativeDef templateMethods[] = {
    PY_NEW(templateNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", templateRepr, "($self, /)"),
    PY_SLOT("__iter__", templateIter, "($self, /)"),
    PY_SEQUENCE_SLOT("__add__", templateConcat, "($self, value, /)"),
    PY_METHOD("__reduce__", templateReduce, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet templateGetSets[] = {
    PY_MEMBER("strings", templateGetStrings),
    PY_MEMBER("interpolations", templateGetInterpolations),
    { "values"_s, templateGetValues },
};

void initializeTemplateTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    rt.templateStringType()->initializeBuiltin(globalObject, { "string.templatelib.Template"_s, { }, templateMethods, templateGetSets, "Template object"_s }, rt.typeType(), rt.objectType());
    rt.templateStringIteratorType()->initializeBuiltin(globalObject, { "string.templatelib.TemplateIter"_s, { }, templateIteratorMethods, { }, "Template iterator object"_s }, rt.typeType(), rt.objectType());

    PyType* interpolation = rt.interpolationType();
    interpolation->initializeBuiltin(globalObject, { "string.templatelib.Interpolation"_s, { }, interpolationMethods, interpolationGetSets, "Interpolation object"_s }, rt.typeType(), rt.objectType());
    interpolation->dict()->set(vm, rt.names.dMatchArgs(), PyTuple::create(globalObject, { internString(vm, "value"_s), internString(vm, "expression"_s), internString(vm, "conversion"_s), internString(vm, "format_spec"_s) }));
    interpolation->didChange(vm);
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
