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
#include "PythonOperations.h"

#include "ErrorInstance.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "PyDict.h"
#include "PyInstance.h"
#include "PyNativeFunction.h"
#include "PyObjects.h"
#include "PythonBuiltins.h"
#include "PythonNumbers.h"
#include "PythonSequences.h"
#include "PythonStrings.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/text/StringBuilder.h>

// repr(), str() and format().

namespace JSC { namespace Python {

String addressOf(const void* cell)
{
    return concatenate("0x"_s, hex(std::bit_cast<uintptr_t>(cell), Lowercase));
}

// What CPython's %T writes: the class of something, with where it is from unless that is builtins or __main__.
String fullyQualifiedTypeName(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    PyType* type = typeOf(globalObject, value);
    String name = qualifiedNameWithoutModule(globalObject, type);
    if (String module = type->moduleOfBuiltin(); !module.isNull())
        return concatenate(module, '.', name);
    JSValue module = type->lookupOwn(vm, vm.pythonNames().dunder_module);
    if (!module || !module.isString())
        return name;
    String moduleName = asString(module)->value(globalObject);
    return moduleName == "builtins"_s || moduleName == "__main__"_s ? name : concatenate(moduleName, '.', name);
}

String qualifiedNameWithoutModule(JSGlobalObject* globalObject, PyType* type)
{
    JSValue qualifiedName = type->getDirect(globalObject->vm(), globalObject->vm().pythonNames().private_qualname);
    return qualifiedName && qualifiedName.isString() ? String(asString(qualifiedName)->value(globalObject).data) : type->nameWithoutModule(globalObject);
}

String qualifiedNameOfType(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    String name = qualifiedNameWithoutModule(globalObject, type);
    if (String module = type->moduleOfBuiltin(); !module.isNull())
        return concatenate(module, '.', name);
    JSValue module = type->lookupOwn(vm, names.dunder_module);
    if (module && module.isString()) {
        String moduleName = asString(module)->value(globalObject);
        if (moduleName != "builtins"_s)
            return concatenate(moduleName, '.', name);
    }
    return name;
}

String nameOfFunction(JSGlobalObject* globalObject, JSFunction* function, bool qualified)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    // What it is called is what JavaScript calls it: its `name`, which is not there until it is asked for or set.
    if (JSValue set = function->getDirect(vm, qualified ? names.private_qualname : vm.propertyNames->name); set && set.isString())
        return asString(set)->value(globalObject);
    if (!function->isHostOrBuiltinFunction()) {
        if (auto* info = function->jsExecutable()->unlinkedExecutable()->pythonInfo())
            return qualified ? info->qualifiedName : info->name.string();
    }
    return function->name(vm);
}

template<typename Get>
static bool appendItems(JSGlobalObject* globalObject, StringBuilder& builder, unsigned count, const Get& get)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (unsigned i = 0; i < count; ++i) {
        if (i)
            builder.append(", "_s);
        String item = repr(globalObject, get(i));
        RETURN_IF_EXCEPTION(scope, false);
        builder.append(item);
    }
    return true;
}

PyTuple* exceptionArguments(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = globalObject->vm();
    if (!exception.isObject())
        return globalObject->pyRealm()->emptyTuple();
    JSValue arguments = asObject(exception)->getDirect(vm, vm.pythonNames().private_args);
    if (arguments && isTuple(arguments))
        return uncheckedDowncast<PyTuple>(arguments.asCell());
    // An Error that JavaScript made was made with a message, unless Python has given it something else since.
    if (auto* error = dynamicDowncast<ErrorInstance>(exception); error && !error->inherits<PyException>()) {
        // The one that the engine makes and Python has words of its own for.
        if (error->isStackOverflowError())
            return PyTuple::create(globalObject, { jsNontrivialString(vm, "maximum recursion depth exceeded"_s) });
        JSValue message = error->getDirect(vm, vm.propertyNames->message);
        if (message && message.isString())
            return PyTuple::create(globalObject, { message });
    }
    return globalObject->pyRealm()->emptyTuple();
}

// What the built-in types' __repr__ are, by the kind of cell.
String builtinRepr(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();

    if (value.isUndefinedOrNull())
        return "None"_s;
    if (value.isBoolean())
        return value.asBoolean() ? "True"_s : "False"_s;
    if (Number number = classify(value)) {
        if (number.kind == Number::Kind::Float)
            return reprOfDouble(number.real);
        RELEASE_AND_RETURN(scope, reprOfInt(globalObject, number));
    }

    JSCell* cell = value.asCell();
    PyType* type = typeOf(globalObject, value);
    TextBuilder builder;

    switch (cell->type()) {
    case StringType: {
        auto view = uncheckedDowncast<JSString>(cell)->view(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        return reprOfString(view);
    }
    case PyBoxedValueType:
        RELEASE_AND_RETURN(scope, builtinRepr(globalObject, uncheckedDowncast<PyBoxedValue>(cell)->value()));
    case PyTupleType: {
        auto* tuple = uncheckedDowncast<PyTuple>(cell);
        ReprGuard guard(globalObject, cell);
        if (guard.isRecursive())
            return "(...)"_s;
        builder.append('(');
        appendItems(globalObject, builder, tuple->length(), [&] (unsigned i) { return tuple->at(i); });
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(tuple->length() == 1 ? ",)"_s : ")"_s);
        return builder.tryFinish();
    }
    case PyDictType: {
        auto* dict = uncheckedDowncast<PyDict>(cell);
        ReprGuard guard(globalObject, cell);
        if (guard.isRecursive())
            return "{...}"_s;
        builder.append('{');
        bool isFirst = true;
        dict->forEach(globalObject, [&] (JSValue key, JSValue item) {
            if (!isFirst)
                builder.append(", "_s);
            isFirst = false;
            String keyText = repr(globalObject, key);
            RETURN_IF_EXCEPTION(scope, false);
            String itemText = repr(globalObject, item);
            RETURN_IF_EXCEPTION(scope, false);
            builder.append(keyText, ": "_s, itemText);
            return true;
        });
        RETURN_IF_EXCEPTION(scope, { });
        builder.append('}');
        return builder.tryFinish();
    }
    case PySetType: {
        auto* set = uncheckedDowncast<PySet>(cell);
        String name = type->nameString(globalObject);
        bool isPlainSet = type == realm->typeSet();
        ReprGuard guard(globalObject, cell);
        if (guard.isRecursive())
            return concatenate(name, "(...)"_s);
        if (!set->size())
            return concatenate(name, "()"_s);
        if (!isPlainSet)
            builder.append(name, '(');
        builder.append('{');
        bool isFirst = true;
        for (unsigned entry = 0; entry < set->entryCount(); ++entry) {
            JSValue key = set->keyAt(entry);
            if (!key)
                continue;
            if (!isFirst)
                builder.append(", "_s);
            isFirst = false;
            String text = repr(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            builder.append(text);
        }
        builder.append('}');
        if (!isPlainSet)
            builder.append(')');
        return builder.tryFinish();
    }
    case PyRangeType: {
        auto* range = uncheckedDowncast<PyRange>(cell);
        String start = reprOfInt(globalObject, classify(range->start()), 10);
        RETURN_IF_EXCEPTION(scope, { });
        String stop = reprOfInt(globalObject, classify(range->stop()), 10);
        RETURN_IF_EXCEPTION(scope, { });
        builder.append("range("_s, start, ", "_s, stop);
        if (!range->step().isInt32() || range->step().asInt32() != 1) {
            String step = reprOfInt(globalObject, classify(range->step()), 10);
            RETURN_IF_EXCEPTION(scope, { });
            builder.append(", "_s, step);
        }
        builder.append(')');
        return builder.tryFinish();
    }
    case PySliceType: {
        auto* slice = uncheckedDowncast<PySlice>(cell);
        builder.append("slice("_s);
        JSValue parts[] = { slice->start(), slice->stop(), slice->step() };
        appendItems(globalObject, builder, 3, [&] (unsigned i) { return parts[i]; });
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(')');
        return builder.tryFinish();
    }
    case PyTypeType:
        return concatenate("<class '"_s, qualifiedNameOfType(globalObject, uncheckedDowncast<PyType>(cell)), "'>"_s);
    case InternalFunctionType:
        if (isJavaScriptClass(cell))
            return concatenate("<class '"_s, qualifiedNameOfType(globalObject, asType(cell)), "'>"_s);
        break;
    case JSFunctionType: {
        if (isJavaScriptClass(cell))
            return concatenate("<class '"_s, qualifiedNameOfType(globalObject, asType(cell)), "'>"_s);
        auto* function = uncheckedDowncast<JSFunction>(cell);
        if (auto* native = dynamicDowncast<PyNativeFunction>(cell)) {
            if (native->kind() == PyNativeFunction::Kind::Function)
                return concatenate("<built-in function "_s, function->name(vm), '>');
            if (native->kind() == PyNativeFunction::Kind::New || native->kind() == PyNativeFunction::Kind::StaticMethod)
                return concatenate("<built-in method "_s, function->name(vm), " of type object at "_s, addressOf(native->owner()), '>');
            return concatenate(native->kind() == PyNativeFunction::Kind::Wrapper ? "<slot wrapper '"_s : "<method '"_s, function->name(vm), "' of '"_s, asType(native->owner())->nameString(globalObject), "' objects>"_s);
        }
        return concatenate("<function "_s, nameOfFunction(globalObject, function, true), " at "_s, addressOf(cell), '>');
    }
    case PyBoundMethodType: {
        auto* method = uncheckedDowncast<PyBoundMethod>(cell);
        if (auto* native = dynamicDowncast<PyNativeFunction>(method->function())) {
            bool isWrapper = native->kind() == PyNativeFunction::Kind::Wrapper;
            return concatenate(isWrapper ? "<method-wrapper '"_s : "<built-in method "_s, native->name(vm), isWrapper ? "' of "_s : " of "_s, typeName(globalObject, method->self()), " object at "_s, method->self().isCell() ? addressOf(method->self().asCell()) : "0x0"_str, '>');
        }
        String self = repr(globalObject, method->self());
        RETURN_IF_EXCEPTION(scope, { });
        String name = "?"_s;
        if (auto* function = dynamicDowncast<JSFunction>(method->function()))
            name = nameOfFunction(globalObject, function, true);
        return concatenate("<bound method "_s, name, " of "_s, self, '>');
    }
    default:
        break;
    }

    if (isListCell(cell)) {
        auto* list = uncheckedDowncast<JSArray>(cell);
        ReprGuard guard(globalObject, cell);
        if (guard.isRecursive())
            return "[...]"_s;
        builder.append('[');
        for (unsigned i = 0; i < list->length(); ++i) {
            if (i)
                builder.append(", "_s);
            JSValue value = listGet(globalObject, list, i);
            RETURN_IF_EXCEPTION(scope, { });
            String item = repr(globalObject, value);
            RETURN_IF_EXCEPTION(scope, { });
            builder.append(item);
        }
        builder.append(']');
        return builder.tryFinish();
    }
    if (cell == realm->notImplemented())
        return "NotImplemented"_s;
    if (cell == realm->ellipsis())
        return "Ellipsis"_s;

    if (type->isExceptionType()) {
        // ValueError('message')
        PyTuple* arguments = exceptionArguments(globalObject, value);
        builder.append(type->nameString(globalObject), '(');
        appendItems(globalObject, builder, arguments->length(), [&] (unsigned i) { return arguments->at(i); });
        RETURN_IF_EXCEPTION(scope, { });
        builder.append(')');
        return builder.tryFinish();
    }

    return concatenate('<', qualifiedNameOfType(globalObject, type), " object at "_s, addressOf(cell), '>');
}

// What the class has for __repr__(), called, and whatever comes of it: tp_repr.
static JSValue callRepr(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isObject()) {
        String text = builtinRepr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, text));
    }
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_repr, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* native = dynamicDowncast<PyNativeFunction>(method); native && native->nativeFunction() == nativeRepr) {
        String text = builtinRepr(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, strOrMemoryError(globalObject, text));
    }
    RELEASE_AND_RETURN(scope, callMethod(globalObject, method, self));
}

JSValue reprObject(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!vm.isSafeToRecurse()) [[unlikely]]
        return raise(globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded while getting the repr of an object"_s);
    JSValue result = callRepr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(result))
        return raiseTypeError(globalObject, scope, concatenate("__repr__ returned non-string (type "_s, typeName(globalObject, result), ')'));
    return result;
}

String repr(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = reprObject(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, stringIn(result)->value(globalObject));
}

// BaseException.__str__
String strOfException(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* arguments = exceptionArguments(globalObject, value);
    if (!arguments->length())
        return emptyString();
    if (arguments->length() == 1)
        RELEASE_AND_RETURN(scope, str(globalObject, arguments->at(0)));
    RELEASE_AND_RETURN(scope, repr(globalObject, arguments));
}

JSValue strObject(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (value.isString())
        return value;
    if (!value.isObject())
        RELEASE_AND_RETURN(scope, callRepr(globalObject, value));

    PyType* type = typeOf(globalObject, value);
    JSValue method = type->lookup(vm, vm.pythonNames().dunder_str);
    // object.__str__() is what the class has for __repr__(), and it is here that what comes of it is looked at.
    bool isThatOfObject = !method || method.asCell() == globalObject->pyRealm()->function(PyRealm::WellKnownFunction::ObjectStr);
    JSValue result = isThatOfObject ? callRepr(globalObject, value) : callSpecial(globalObject, type, method, value);
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(result))
        return raiseTypeError(globalObject, scope, concatenate("__str__ returned non-string (type "_s, typeName(globalObject, result), ')'));
    return result;
}

String str(JSGlobalObject* globalObject, JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = strObject(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, stringIn(result)->value(globalObject));
}

// What int, float, str and bool do with a format specification. Empty if it is none of them.
JSValue builtinFormat(JSGlobalObject* globalObject, JSValue value, const String& specificationText)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue given = value;
    if (auto* boxed = tryBoxedValue(value))
        value = boxed->value();
    bool isString = value.isString();
    Number number = classify(value);
    if (!isString && !number)
        return { };
    if (specificationText.isEmpty()) {
        String text = str(globalObject, value);
        RETURN_IF_EXCEPTION(scope, { });
        return jsString(vm, text);
    }
    auto specification = parseFormatSpecification(globalObject, specificationText, typeName(globalObject, given), isString);
    RETURN_IF_EXCEPTION(scope, { });
    String result;
    if (isString) {
        String text = asString(value)->value(globalObject);
        result = formatString(globalObject, text, *specification);
        RETURN_IF_EXCEPTION(scope, { });
        // As with `format % values`: what is left as it was is what was given.
        if (result == text)
            return given;
    }
    else if (number.kind == Number::Kind::Float)
        result = formatFloat(globalObject, number.real, *specification);
    else
        result = formatInt(globalObject, value, *specification);
    RETURN_IF_EXCEPTION(scope, { });
    return jsString(vm, result);
}

JSValue format(JSGlobalObject* globalObject, JSValue value, const String& specification)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value.isObject()) {
        JSValue result = builtinFormat(globalObject, value, specification);
        RETURN_IF_EXCEPTION(scope, { });
        if (result)
            return result;
    }
    JSValue self;
    JSValue method = lookupSpecial(globalObject, value, vm.pythonNames().dunder_format, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = callMethod(globalObject, method, self, jsString(vm, specification));
    RETURN_IF_EXCEPTION(scope, { });
    if (!stringIn(result))
        return raiseTypeError(globalObject, scope, concatenate("__format__ must return a str, not "_s, typeName(globalObject, result)));
    return result;
}

} } // namespace JSC::Python
