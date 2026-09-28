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

#include "PyInstance.h"
#include "PyTuple.h"
#include "PythonSequences.h"

// Struct sequences, which are CPython's Objects/structseq.c, and types.SimpleNamespace, which is Objects/namespaceobject.c.

namespace JSC { namespace Python {

using Kind = PyNativeFunction::Kind;

// ---- Struct sequences
//
// One is a tuple of the items that it has as a sequence. Those of its fields that are not among them are in another tuple, which is a property of it that no program can name.
// The class has the names of the fields, in order, None for one that has no name, under a name of the same kind.

static PyTuple* fieldNamesOf(VM& vm, PyType* type)
{
    for (auto& ancestor : type->mro()->span()) {
        if (JSValue names = asType(ancestor.get())->getDirect(vm, vm.pythonNames().private_fieldNames))
            return asTuple(names);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSValue fieldAt(VM& vm, PyTuple* self, unsigned index)
{
    if (index < self->length())
        return self->at(index);
    return asTuple(self->getDirect(vm, vm.pythonNames().private_hiddenFields))->at(index - self->length());
}

JSValue newStructSequence(JSGlobalObject* globalObject, PyType* type, const ArgList& values)
{
    VM& vm = globalObject->vm();
    unsigned count = values.size();
    ASSERT(count == fieldNamesOf(vm, type)->length());
    unsigned countInSequence = type->lookup(vm, Identifier::fromString(vm, "n_sequence_fields"_s)).asInt32();
    PyTuple* result = PyTuple::create(vm, type->instanceStructure(), countInSequence);
    for (unsigned i = 0; i < countInSequence; ++i)
        result->initializeAt(vm, i, values.at(i));
    if (count > countInSequence) {
        PyTuple* hidden = PyTuple::create(globalObject, count - countInSequence);
        for (unsigned i = countInSequence; i < count; ++i)
            hidden->initializeAt(vm, i - countInSequence, values.at(i));
        result->putDirect(vm, vm.pythonNames().private_hiddenFields, hidden);
    }
    return result;
}

// cls(sequence, dict={})
PYTHON_NATIVE(structSequenceNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    String name = type->nameString(globalObject);
    PyTuple* fields = fieldNamesOf(vm, type);
    unsigned maximum = fields->length();
    unsigned minimum = type->lookup(vm, Identifier::fromString(vm, "n_sequence_fields"_s)).asInt32();

    MarkedArgumentBuffer values;
    collect(globalObject, args.at(1), values);
    if (scope.exception()) {
        if (catchException(globalObject, BuiltinType::TypeError))
            raiseTypeError(globalObject, scope, "constructor requires a sequence"_s);
        return { };
    }
    JSValue dict = args.at(2);
    if (dict && !isDict(dict))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, "() takes a dict as second arg, if any"_s)));
    unsigned given = values.size();
    if (minimum != maximum && given < minimum)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, "() takes an at least "_s, minimum, "-sequence ("_s, given, "-sequence given)"_s)));
    if (minimum != maximum && given > maximum)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, "() takes an at most "_s, maximum, "-sequence ("_s, given, "-sequence given)"_s)));
    if (minimum == maximum && given != minimum)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, "() takes a "_s, minimum, "-sequence ("_s, given, "-sequence given)"_s)));

    // What the sequence does not reach is looked for in the dict by name.
    uint64_t found = 0;
    for (unsigned i = given; i < maximum; ++i) {
        JSValue value = dict ? asDict(dict)->get(globalObject, fields->at(i)) : JSValue();
        RETURN_IF_EXCEPTION(scope, { });
        found += !!value;
        values.append(value ? value : jsUndefined());
    }
    if (dict && asDict(dict)->size() > found)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(name, "() got duplicate or unexpected field name(s)"_s)));
    return JSValue::encode(newStructSequence(globalObject, type, values));
}

// sys.version_info(major=3, minor=14, micro=0, releaselevel='final', serial=0)
PYTHON_NATIVE(structSequenceRepr)
{
    NATIVE_PROLOGUE();
    PyTuple* self = asTuple(args[0]);
    PyType* type = typeOf(globalObject, self);
    PyTuple* fields = fieldNamesOf(vm, type);
    StringBuilder result;
    result.append(type->nameString(globalObject), '(');
    // The names go with the items in turn, and take no notice of an item that has none.
    unsigned named = 0;
    for (unsigned i = 0; i < self->length(); ++i) {
        while (named < fields->length() && isNone(fields->at(named)))
            ++named;
        String text = repr(globalObject, self->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        result.append(i ? ", "_s : ""_s, asString(fields->at(named++))->value(globalObject).data, '=', text);
    }
    result.append(')');
    return JSValue::encode(jsString(vm, result.toString()));
}

// (cls, (the items, {the rest by name}))
PYTHON_NATIVE(structSequenceReduce)
{
    NATIVE_PROLOGUE();
    PyTuple* self = asTuple(args[0]);
    PyType* type = typeOf(globalObject, self);
    PyTuple* fields = fieldNamesOf(vm, type);
    MarkedArgumentBuffer items;
    for (auto& item : self->span())
        items.append(item.get());
    PyDict* rest = PyDict::create(globalObject);
    for (unsigned i = self->length(); i < fields->length(); ++i) {
        rest->set(globalObject, fields->at(i), fieldAt(vm, self, i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(PyTuple::create(globalObject, { type->object(), PyTuple::create(globalObject, { PyTuple::createFromArguments(globalObject, items), rest }) }));
}

// self.__replace__(**changes)
PYTHON_NATIVE(structSequenceReplace)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "__replace__() takes no positional arguments"_s));
    PyTuple* self = asTuple(args[0]);
    PyType* type = typeOf(globalObject, self);
    PyTuple* fields = fieldNamesOf(vm, type);
    for (auto& field : fields->span()) {
        if (isNone(field.get()))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString("__replace__() is not supported for "_s, type->nameString(globalObject), " because it has unnamed field(s)"_s)));
    }
    auto isNamed = [&] (unsigned field, unsigned keyword) { return asString(fields->at(field))->equal(globalObject, args.keywordName(keyword)); };
    MarkedArgumentBuffer values;
    for (unsigned i = 0; i < fields->length(); ++i) {
        JSValue value = fieldAt(vm, self, i);
        for (unsigned k = 0; k < args.keywordCount(); ++k) {
            if (isNamed(i, k))
                value = args.keywordValue(k);
        }
        values.append(value);
    }
    MarkedArgumentBuffer unexpected;
    for (unsigned k = 0; k < args.keywordCount(); ++k) {
        bool isField = false;
        for (unsigned i = 0; i < fields->length() && !isField; ++i)
            isField = isNamed(i, k);
        if (!isField)
            unexpected.append(args.keywordName(k));
    }
    if (unexpected.size()) {
        String names = repr(globalObject, newList(globalObject, unexpected));
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("Got unexpected field name(s): "_s, names)));
    }
    return JSValue::encode(newStructSequence(globalObject, type, values));
}

// A getter is a function and nothing else, so there is one for each place that a field can be in.
template<unsigned index>
static JSValue getStructSequenceField(JSGlobalObject* globalObject, JSValue self)
{
    return fieldAt(globalObject->vm(), asTuple(self), index);
}

static constexpr unsigned maximumFieldCount = 32;

template<size_t... indices>
static constexpr std::array<PyGetSetDescriptor::Getter, sizeof...(indices)> makeFieldGetters(std::index_sequence<indices...>)
{
    return { getStructSequenceField<indices>... };
}

void makeStructSequenceType(JSGlobalObject* globalObject, PyType* type, std::span<const ASCIILiteral> fields, unsigned countInSequence)
{
    VM& vm = globalObject->vm();
    static constexpr auto getters = makeFieldGetters(std::make_index_sequence<maximumFieldCount>());
    RELEASE_ASSERT(fields.size() <= maximumFieldCount);
    type->setInstanceStructure(vm, PyTuple::createStructure(vm, globalObject, type));

    // One that is only ever made by the interpreter has no __new__.
    addMethodsThatCPythonHas(globalObject, type, { { "__new__"_s, structSequenceNew, Kind::New, 0, "structseq(sequence, dict={})"_s } });
    addMethods(globalObject, type, {
        { "__repr__"_s, structSequenceRepr },
        { "__reduce__"_s, structSequenceReduce },
        { "__replace__"_s, structSequenceReplace, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
    });

    PyTuple* names = PyTuple::create(globalObject, fields.size());
    MarkedArgumentBuffer matched;
    unsigned unnamed = 0;
    for (unsigned i = 0; i < fields.size(); ++i) {
        if (fields[i].isNull()) {
            ++unnamed;
            continue;
        }
        JSString* name = jsString(vm, String(fields[i]));
        names->initializeAt(vm, i, name);
        addMember(globalObject, type, fields[i], getters[i]);
        if (i < countInSequence)
            matched.append(name);
    }
    type->putDirect(vm, vm.pythonNames().private_fieldNames, names);
    auto set = [&] (ASCIILiteral name, JSValue value) { type->putDirect(vm, Identifier::fromString(vm, name), value); };
    set("n_sequence_fields"_s, jsNumber(countInSequence));
    set("n_fields"_s, jsNumber(static_cast<unsigned>(fields.size())));
    set("n_unnamed_fields"_s, jsNumber(unnamed));
    set("__match_args__"_s, PyTuple::createFromArguments(globalObject, matched));
}

// ---- types.SimpleNamespace

JSObject* newSimpleNamespace(JSGlobalObject* globalObject)
{
    return PyInstance::create(globalObject->vm(), globalObject->pyRealm()->typeSimpleNamespace()->instanceStructure());
}

static PyDict* dictOfNamespace(JSGlobalObject* globalObject, JSValue self)
{
    return asDict(getInstanceDict(globalObject, self));
}

PYTHON_NATIVE(namespaceNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyInstance::create(vm, asType(args[0])->instanceStructure()));
}

// False if it raised.
static bool updateNamespace(JSGlobalObject* globalObject, PyDict* target, PyDict* source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool areAllStrings = true;
    MarkedArgumentBuffer pairs;
    source->forEach(globalObject, [&] (JSValue key, JSValue value) {
        areAllStrings = !!stringIn(key);
        pairs.append(key);
        pairs.append(value);
        return areAllStrings;
    });
    if (!areAllStrings) {
        raiseTypeError(globalObject, scope, "keywords must be strings"_s);
        return false;
    }
    for (size_t i = 0; i < pairs.size(); i += 2) {
        target->set(globalObject, pairs.at(i), pairs.at(i + 1));
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// SimpleNamespace(mapping_or_iterable=(), /, **kwargs)
PYTHON_NATIVE(namespaceInit)
{
    NATIVE_PROLOGUE();
    if (args.size() > 2)
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString(typeOf(globalObject, args[0])->nameWithoutModule(globalObject), " expected at most 1 argument, got "_s, args.size() - 1)));
    PyDict* dict = dictOfNamespace(globalObject, args[0]);
    if (args.size() == 2) {
        JSValue given = args[1];
        if (!isExactly(globalObject, given, realm->typeDict())) {
            given = call(globalObject, realm->typeDict(), given);
            RETURN_IF_EXCEPTION(scope, { });
        }
        updateNamespace(globalObject, dict, asDict(given));
        RETURN_IF_EXCEPTION(scope, { });
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        dict->set(globalObject, args.keywordName(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// namespace(a=1, b=2)
PYTHON_NATIVE(namespaceRepr)
{
    NATIVE_PROLOGUE();
    PyType* type = typeOf(globalObject, args[0]);
    String name = type == realm->typeSimpleNamespace() ? "namespace"_str : type->nameString(globalObject);
    ReprGuard guard(globalObject, args[0].asCell());
    if (guard.isRecursive())
        return JSValue::encode(jsString(vm, makeString(name, "(...)"_s)));
    MarkedArgumentBuffer pairs;
    dictOfNamespace(globalObject, args[0])->forEach(globalObject, [&] (JSValue key, JSValue value) {
        pairs.append(key);
        pairs.append(value);
        return true;
    });
    StringBuilder result;
    result.append(name, '(');
    bool isFirst = true;
    for (size_t i = 0; i < pairs.size(); i += 2) {
        JSString* key = stringIn(pairs.at(i));
        if (!key || !key->length())
            continue;
        String text = repr(globalObject, pairs.at(i + 1));
        RETURN_IF_EXCEPTION(scope, { });
        result.append(isFirst ? ""_s : ", "_s, key->value(globalObject).data, '=', text);
        isFirst = false;
    }
    result.append(')');
    return JSValue::encode(jsString(vm, result.toString()));
}

// As their __dict__s compare.
PYTHON_NATIVE(namespaceCompare)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    NATIVE_PROLOGUE();
    if (!isInstance(globalObject, args[1], realm->typeSimpleNamespace()))
        RETURN_NOT_IMPLEMENTED();
    RELEASE_AND_RETURN(scope, JSValue::encode(compare(globalObject, op, dictOfNamespace(globalObject, args[0]), dictOfNamespace(globalObject, args[1]))));
}

PYTHON_NATIVE(namespaceReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), PyTuple::create(globalObject, { }), dictOfNamespace(globalObject, args[0]) }));
}

// self.__replace__(**changes)
PYTHON_NATIVE(namespaceReplace)
{
    NATIVE_PROLOGUE();
    if (args.size() > 1)
        return JSValue::encode(raiseTypeError(globalObject, scope, "__replace__() takes no positional arguments"_s));
    PyType* type = typeOf(globalObject, args[0]);
    JSValue result = call(globalObject, type->object());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isInstance(globalObject, result, realm->typeSimpleNamespace()))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("expect types.SimpleNamespace type, but "_s, fullyQualifiedTypeName(globalObject, args[0]), "() returned '"_s, fullyQualifiedTypeName(globalObject, result), "' object"_s)));
    PyDict* dict = dictOfNamespace(globalObject, result);
    MarkedArgumentBuffer pairs;
    dictOfNamespace(globalObject, args[0])->forEach(globalObject, [&] (JSValue key, JSValue value) {
        pairs.append(key);
        pairs.append(value);
        return true;
    });
    for (size_t i = 0; i < pairs.size(); i += 2) {
        dict->set(globalObject, pairs.at(i), pairs.at(i + 1));
        RETURN_IF_EXCEPTION(scope, { });
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        dict->set(globalObject, args.keywordName(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return JSValue::encode(result);
}

void initializeStructSequences(JSGlobalObject* globalObject)
{
    PyRealm* realm = globalObject->pyRealm();
    PyType* simpleNamespace = realm->typeSimpleNamespace();
    addMethods(globalObject, simpleNamespace, {
        { "__new__"_s, namespaceNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__init__"_s, namespaceInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
        { "__repr__"_s, namespaceRepr },
        { "__reduce__"_s, namespaceReduce },
        { "__replace__"_s, namespaceReplace, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreNotChecked },
    });
    addComparisons(globalObject, simpleNamespace, namespaceCompare);
    simpleNamespace->putDirect(globalObject->vm(), globalObject->vm().pythonNames().dunder_hash, jsUndefined());
    addMember(globalObject, simpleNamespace, "__dict__"_s, getInstanceDict);
}

} } // namespace JSC::Python
