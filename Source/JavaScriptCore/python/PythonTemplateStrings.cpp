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

// t"..." : PEP 750. This is CPython's Objects/templateobject.c and Objects/interpolationobject.c.

namespace JSC { namespace Python {

using Kind = PyNativeFunction::Kind;


namespace TemplateField {
enum Field : unsigned { Strings, Interpolations };
}
namespace InterpolationField {
enum Field : unsigned { Value, Expression, Conversion, FormatSpecification };
}
namespace TemplateIterField {
enum Field : unsigned { Strings, Interpolations, IsFromStrings };
}

// ---- Interpolation

JSValue newInterpolation(JSGlobalObject* globalObject, JSValue value, JSValue expression, JSValue conversion, JSValue formatSpecification)
{
    return PyNativeObject::create(globalObject, BuiltinType::Interpolation, value, expression, conversion, formatSpecification);
}

PYTHON_NATIVE(interpolationNew)
{
    NATIVE_PROLOGUE();
    JSValue expression = args.at(2);
    JSValue conversion = args.at(3);
    JSValue formatSpecification = args.at(4);
    auto checkIsString = [&] (JSValue argument, ASCIILiteral name) {
        if (!argument || stringIn(argument))
            return true;
        raiseTypeError(globalObject, scope, makeString("Interpolation() argument '"_s, name, "' must be str, not "_s, typeNameOfArgument(globalObject, argument)));
        return false;
    };
    if (!checkIsString(expression, "expression"_s))
        return { };
    if (conversion && !isNone(conversion)) {
        if (!stringIn(conversion))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Interpolation() argument 'conversion' must be str, not "_s, fullyQualifiedTypeName(globalObject, conversion))));
        String text = stringIn(conversion)->value(globalObject);
        if (text != "a"_s && text != "r"_s && text != "s"_s)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::ValueError, "Interpolation() argument 'conversion' must be one of 's', 'a' or 'r'"_s));
    }
    if (!checkIsString(formatSpecification, "format_spec"_s))
        return { };
    return JSValue::encode(newInterpolation(globalObject, args.at(1), expression ? expression : JSValue(jsEmptyString(vm)), conversion ? conversion : jsUndefined(), formatSpecification ? formatSpecification : JSValue(jsEmptyString(vm))));
}

PYTHON_NATIVE(interpolationRepr)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    StringBuilder out;
    out.append("Interpolation("_s);
    for (unsigned i = 0; i < 4; ++i) {
        if (i)
            out.append(", "_s);
        String text = repr(globalObject, self->field(i));
        RETURN_IF_EXCEPTION(scope, { });
        out.append(text);
    }
    out.append(')');
    return JSValue::encode(jsString(vm, out.toString()));
}

PYTHON_NATIVE(interpolationReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* self = asNativeObject(args[0]);
    return JSValue::encode(PyTuple::create(globalObject, { realm->typeInterpolation(), PyTuple::create(globalObject, { self->field(0), self->field(1), self->field(2), self->field(3) }) }));
}

// ---- Template

// There is a string before, between and after the interpolations, if only an empty one.
JSValue newTemplate(JSGlobalObject* globalObject, JSValue strings, JSValue interpolations)
{
    return PyNativeObject::create(globalObject, BuiltinType::Template, strings, interpolations);
}

// Template(*args), where each is a string or an Interpolation. Strings that are side by side become one.
PYTHON_NATIVE(templateNew)
{
    NATIVE_PROLOGUE();
    if (args.keywordCount())
        return JSValue::encode(raiseTypeError(globalObject, scope, "Template.__new__ only accepts *args arguments"_s));
    for (unsigned i = 1; i < args.size(); ++i) {
        if (!stringIn(args[i]) && !isExactly(globalObject, args[i], BuiltinType::Interpolation))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Template.__new__ *args need to be of type 'str' or 'Interpolation', got "_s, fullyQualifiedTypeName(globalObject, args[i]))));
    }
    MarkedArgumentBuffer strings;
    MarkedArgumentBuffer interpolations;
    bool lastWasString = false;
    for (unsigned i = 1; i < args.size(); ++i) {
        JSValue item = args[i];
        if (stringIn(item)) {
            if (lastWasString) {
                JSString* joined = jsString(globalObject, stringIn(strings.last()), stringIn(item));
                RETURN_IF_EXCEPTION(scope, { });
                strings.removeLast();
                strings.append(joined);
            } else
                strings.append(item);
            lastWasString = true;
            continue;
        }
        if (!lastWasString)
            strings.append(jsEmptyString(vm));
        interpolations.append(item);
        lastWasString = false;
    }
    if (!lastWasString)
        strings.append(jsEmptyString(vm));
    return JSValue::encode(newTemplate(globalObject, PyTuple::createFromArguments(globalObject, strings), PyTuple::createFromArguments(globalObject, interpolations)));
}

PYTHON_NATIVE(templateRepr)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    String strings = repr(globalObject, self->field(TemplateField::Strings));
    RETURN_IF_EXCEPTION(scope, { });
    String interpolations = repr(globalObject, self->field(TemplateField::Interpolations));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, makeString("Template(strings="_s, strings, ", interpolations="_s, interpolations, ')')));
}

// The strings and the interpolations by turns, without the strings that are empty.
PYTHON_NATIVE(templateIter)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    JSValue strings = getIterator(globalObject, self->field(TemplateField::Strings));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue interpolations = getIterator(globalObject, self->field(TemplateField::Interpolations));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyNativeObject::create(globalObject, BuiltinType::TemplateIter, strings, interpolations, jsBoolean(true)));
}

PYTHON_NATIVE(templateIterSelf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(args[0]);
}

PYTHON_NATIVE(templateIterNext)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    // Empty when there are no more.
    auto nextOf = [&] (unsigned field) { return iteratorNext(globalObject, self->field(field)); };
    JSValue item;
    if (self->field(TemplateIterField::IsFromStrings).isTrue()) {
        item = nextOf(TemplateIterField::Strings);
        RETURN_IF_EXCEPTION(scope, { });
        self->setField(vm, TemplateIterField::IsFromStrings, jsBoolean(false));
        // In a tree that a program made, what is between the interpolations can be anything.
        if (JSString* string = item ? stringIn(item) : nullptr; string && !string->length()) {
            item = nextOf(TemplateIterField::Interpolations);
            RETURN_IF_EXCEPTION(scope, { });
            self->setField(vm, TemplateIterField::IsFromStrings, jsBoolean(true));
        }
    } else {
        item = nextOf(TemplateIterField::Interpolations);
        RETURN_IF_EXCEPTION(scope, { });
        self->setField(vm, TemplateIterField::IsFromStrings, jsBoolean(true));
    }
    if (!item)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::StopIteration, JSValue()));
    return JSValue::encode(item);
}

// The last string of the one and the first of the other become one.
PYTHON_NATIVE(templateAdd)
{
    NATIVE_PROLOGUE();
    if (!isExactly(globalObject, args[1], BuiltinType::Template))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("can only concatenate string.templatelib.Template (not \""_s, fullyQualifiedTypeName(globalObject, args[1]), "\") to string.templatelib.Template"_s)));
    auto* left = asNativeObject(args[0]);
    auto* right = asNativeObject(args[1]);
    PyTuple* leftStrings = asTuple(left->field(TemplateField::Strings));
    PyTuple* rightStrings = asTuple(right->field(TemplateField::Strings));
    MarkedArgumentBuffer strings;
    for (unsigned i = 0; i + 1 < leftStrings->length(); ++i)
        strings.append(leftStrings->at(i));
    // PyUnicode_Concat()
    JSValue last = leftStrings->at(leftStrings->length() - 1);
    JSValue first = rightStrings->at(0);
    if (!stringIn(last))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("must be str, not "_s, typeName(globalObject, last))));
    if (!stringIn(first))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("can only concatenate str (not \""_s, typeName(globalObject, first), "\") to str"_s)));
    JSString* joined = jsString(globalObject, stringIn(last), stringIn(first));
    RETURN_IF_EXCEPTION(scope, { });
    strings.append(joined);
    for (unsigned i = 1; i < rightStrings->length(); ++i)
        strings.append(rightStrings->at(i));
    MarkedArgumentBuffer interpolations;
    for (auto* side : { left, right }) {
        for (auto& interpolation : asTuple(side->field(TemplateField::Interpolations))->span())
            interpolations.append(interpolation.get());
    }
    return JSValue::encode(newTemplate(globalObject, PyTuple::createFromArguments(globalObject, strings), PyTuple::createFromArguments(globalObject, interpolations)));
}

PYTHON_NATIVE(templateReduce)
{
    NATIVE_PROLOGUE();
    auto* self = asNativeObject(args[0]);
    JSValue module = importModule(globalObject, nullptr, "string.templatelib"_s, jsUndefined(), 0, true);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue function = getAttribute(globalObject, module, Identifier::fromString(vm, "_template_unpickle"_s));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { function, PyTuple::create(globalObject, { self->field(TemplateField::Strings), self->field(TemplateField::Interpolations) }) }));
}

template<unsigned index>
static JSValue getTemplateField(JSGlobalObject*, JSValue self)
{
    return asNativeObject(self)->field(index);
}

void initializeTemplateStrings(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    for (PyType* type : { realm->typeTemplate(), realm->typeTemplateIter(), realm->typeInterpolation() })
        type->setInstanceStructure(vm, PyNativeObject::createStructure(vm, globalObject, type));

    PyType* interpolation = realm->typeInterpolation();
    addMethods(globalObject, interpolation, {
        { "__new__"_s, interpolationNew, Kind::New, 0, "(value, expression='', conversion=None, format_spec='')"_s, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, interpolationRepr },
        { "__reduce__"_s, interpolationReduce },
    });
    addMember(globalObject, interpolation, "value"_s, getTemplateField<InterpolationField::Value>);
    addMember(globalObject, interpolation, "expression"_s, getTemplateField<InterpolationField::Expression>);
    addMember(globalObject, interpolation, "conversion"_s, getTemplateField<InterpolationField::Conversion>);
    addMember(globalObject, interpolation, "format_spec"_s, getTemplateField<InterpolationField::FormatSpecification>);
    interpolation->putDirect(vm, vm.pythonNames().dunder_match_args, PyTuple::create(globalObject, {
        jsNontrivialString(vm, "value"_s), jsNontrivialString(vm, "expression"_s), jsNontrivialString(vm, "conversion"_s), jsNontrivialString(vm, "format_spec"_s) }));

    PyType* templateType = realm->typeTemplate();
    addMethods(globalObject, templateType, {
        { "__new__"_s, templateNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__repr__"_s, templateRepr },
        { "__iter__"_s, templateIter },
        { "__add__"_s, templateAdd },
        { "__reduce__"_s, templateReduce },
    });
    addMember(globalObject, templateType, "strings"_s, getTemplateField<TemplateField::Strings>);
    addMember(globalObject, templateType, "interpolations"_s, getTemplateField<TemplateField::Interpolations>);
    addGetSet(globalObject, templateType, "values"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        MarkedArgumentBuffer values;
        for (auto& interpolation : asTuple(asNativeObject(self)->field(TemplateField::Interpolations))->span())
            values.append(asNativeObject(interpolation.get())->field(InterpolationField::Value));
        return PyTuple::createFromArguments(globalObject, values);
    });

    addMethods(globalObject, realm->typeTemplateIter(), {
        { "__iter__"_s, templateIterSelf },
        { "__next__"_s, templateIterNext },
    });
}

} } // namespace JSC::Python
