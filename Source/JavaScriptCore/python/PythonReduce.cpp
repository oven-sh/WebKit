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
#include "PythonBytes.h"
#include "PythonNumbers.h"
#include "PythonSequences.h"
#include "PythonStrings.h"

// object.__reduce__(), __reduce_ex__(), __getstate__() and __sizeof__(): what copying and pickling go by. This is the part of CPython's Objects/typeobject.c that is
// about them. As there, some of it is left to the copyreg module, which is written in Python and is imported when it is first wanted.

namespace JSC { namespace Python {

using Function = PyRealm::WellKnownFunction;

static JSValue fromCopyreg(JSGlobalObject* globalObject, ASCIILiteral name)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue copyreg = importModule(globalObject, nullptr, "copyreg"_s, jsUndefined(), 0, true);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, getAttribute(globalObject, copyreg, Identifier::fromString(vm, name)));
}

// The names of the slots of a class and of its bases. A list, or None. It is kept in the class.
static JSValue slotNamesOf(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue kept = type->lookupOwn(vm, Identifier::fromString(vm, "__slotnames__"_s))) {
        if (!isNone(kept) && !isList(kept))
            return raiseTypeError(globalObject, scope, concatenate(type->nameString(globalObject), ".__slotnames__ should be a list or None, not "_s, typeName(globalObject, kept)));
        return kept;
    }
    JSValue function = fromCopyreg(globalObject, "_slotnames"_s);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue slotNames = call(globalObject, function, type->object());
    RETURN_IF_EXCEPTION(scope, { });
    if (!isNone(slotNames) && !isList(slotNames))
        return raiseTypeError(globalObject, scope, "copyreg._slotnames didn't return a list or None"_s);
    return slotNames;
}

// The __dict__, or None if there is nothing in it, and with it what is in the slots if anything is. If it is `required`, this is all that there will be to make a
// copy from, and it is an error for the object to have anything that this does not know of.
static JSValue defaultState(JSGlobalObject* globalObject, JSValue object, bool required)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, object);
    if (required && type->itemSize())
        return raiseTypeError(globalObject, scope, concatenate("cannot pickle "_s, type->nameString(globalObject), " objects"_s));

    JSValue state = jsUndefined();
    if (type->hasFlag(PyType::HasInstanceDict)) {
        JSValue dict = getInstanceDict(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        if (uncheckedDowncast<PyDict>(dict.asCell())->size())
            state = dict;
    }

    JSValue slotNames = slotNamesOf(globalObject, type);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned slotCount = isNone(slotNames) ? 0 : asList(slotNames)->length();

    if (required) {
        constexpr unsigned long hasManagedDict = 1ul << 4;
        int basicSize = globalObject->pyRealm()->typeObject()->basicSize();
        if (type->dictOffset() && !(type->flagsForPython() & hasManagedDict))
            basicSize += sizeof(void*);
        if (type->weakReferenceOffset() > 0)
            basicSize += sizeof(void*);
        basicSize += sizeof(void*) * slotCount;
        if (type->basicSize() > basicSize)
            return raiseTypeError(globalObject, scope, concatenate("cannot pickle '"_s, type->nameString(globalObject), "' object"_s));
    }
    if (!slotCount)
        return state;

    PyDict* slots = PyDict::create(globalObject);
    for (unsigned i = 0; i < slotCount; ++i) {
        JSValue name = listGet(globalObject, asList(slotNames), i);
        RETURN_IF_EXCEPTION(scope, { });
        if (!name.isString())
            return raiseTypeError(globalObject, scope, concatenate("attribute name must be string, not '"_s, typeName(globalObject, name), '\''));
        auto identifier = asString(name)->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        // One that has nothing in it is left out.
        JSValue value = getAttributeIfPresent(globalObject, object, identifier);
        RETURN_IF_EXCEPTION(scope, { });
        if (value) {
            slots->set(globalObject, name, value);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (asList(slotNames)->length() != slotCount)
            return raise(globalObject, scope, BuiltinType::RuntimeError, "__slotnames__ changed size during iteration"_s);
    }
    if (!slots->size())
        return state;
    return PyTuple::create(globalObject, { state, slots });
}

static JSValue stateOf(JSGlobalObject* globalObject, JSValue object, bool required)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue getState = getAttribute(globalObject, object, Identifier::fromString(vm, "__getstate__"_s));
    RETURN_IF_EXCEPTION(scope, { });
    // Only object's own can be told what is required of it.
    if (auto* method = tryBoundMethod(getState); method && method->self() == object && method->function() == JSValue(globalObject->pyRealm()->function(Function::ObjectGetState)))
        RELEASE_AND_RETURN(scope, defaultState(globalObject, object, required));
    RELEASE_AND_RETURN(scope, call(globalObject, getState));
}

// What the class is to be called with to make another: __getnewargs_ex__(), or failing that __getnewargs__(). Both are empty if it has neither.
static void getNewArguments(JSGlobalObject* globalObject, JSValue object, JSValue& arguments, JSValue& keywords)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue self;
    JSValue method = lookupSpecial(globalObject, object, Identifier::fromString(vm, "__getnewargs_ex__"_s), self);
    RETURN_IF_EXCEPTION(scope, void());
    if (method) {
        JSValue both = callMethod(globalObject, method, self);
        RETURN_IF_EXCEPTION(scope, void());
        if (!isTuple(both)) {
            raiseTypeError(globalObject, scope, concatenate("__getnewargs_ex__ should return a tuple, not '"_s, typeName(globalObject, both), '\''));
            return;
        }
        if (asTuple(both)->length() != 2) {
            raise(globalObject, scope, BuiltinType::ValueError, concatenate("__getnewargs_ex__ should return a tuple of length 2, not "_s, asTuple(both)->length()));
            return;
        }
        arguments = asTuple(both)->at(0);
        keywords = asTuple(both)->at(1);
        if (!isTuple(arguments))
            raiseTypeError(globalObject, scope, concatenate("first item of the tuple returned by __getnewargs_ex__ must be a tuple, not '"_s, typeName(globalObject, arguments), '\''));
        else if (!isDict(keywords))
            raiseTypeError(globalObject, scope, concatenate("second item of the tuple returned by __getnewargs_ex__ must be a dict, not '"_s, typeName(globalObject, keywords), '\''));
        return;
    }
    method = lookupSpecial(globalObject, object, Identifier::fromString(vm, "__getnewargs__"_s), self);
    RETURN_IF_EXCEPTION(scope, void());
    if (!method)
        return;
    arguments = callMethod(globalObject, method, self);
    RETURN_IF_EXCEPTION(scope, void());
    if (!isTuple(arguments))
        raiseTypeError(globalObject, scope, concatenate("__getnewargs__ should return a tuple, not '"_s, typeName(globalObject, arguments), '\''));
}

// (copyreg.__newobj__, (cls, *args), state, what is in it if it is a list, what is in it if it is a dict)
static JSValue reduceToNewObject(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyType* type = typeOf(globalObject, object);
    if (type->cannotBeInstantiated(vm))
        return raiseTypeError(globalObject, scope, concatenate("cannot pickle '"_s, type->nameString(globalObject), "' object"_s));
    JSValue arguments;
    JSValue keywords;
    getNewArguments(globalObject, object, arguments, keywords);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue newObject;
    JSValue newArguments;
    if (!keywords || !uncheckedDowncast<PyDict>(keywords.asCell())->size()) {
        newObject = fromCopyreg(globalObject, "__newobj__"_s);
        RETURN_IF_EXCEPTION(scope, { });
        MarkedArgumentBuffer all;
        all.append(type->object());
        if (arguments) {
            for (auto& argument : asTuple(arguments)->span())
                all.append(argument.get());
        }
        newArguments = PyTuple::createFromArguments(globalObject, all);
    } else {
        newObject = fromCopyreg(globalObject, "__newobj_ex__"_s);
        RETURN_IF_EXCEPTION(scope, { });
        newArguments = PyTuple::create(globalObject, { type->object(), arguments, keywords });
    }

    JSValue state = stateOf(globalObject, object, !(arguments || isList(object) || isDict(object)));
    RETURN_IF_EXCEPTION(scope, { });
    JSValue listItems = jsUndefined();
    if (isList(object)) {
        listItems = getIterator(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
    }
    JSValue dictItems = jsUndefined();
    if (isDict(object)) {
        JSValue method = getAttribute(globalObject, object, Identifier::fromString(vm, "items"_s));
        RETURN_IF_EXCEPTION(scope, { });
        JSValue items = call(globalObject, method);
        RETURN_IF_EXCEPTION(scope, { });
        dictItems = getIterator(globalObject, items);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return PyTuple::create(globalObject, { newObject, newArguments, state, listItems, dictItems });
}

static JSValue reduce(JSGlobalObject* globalObject, JSValue object, int64_t protocol)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (protocol >= 2)
        RELEASE_AND_RETURN(scope, reduceToNewObject(globalObject, object));
    JSValue function = fromCopyreg(globalObject, "_reduce_ex"_s);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, call(globalObject, function, object, intFromInt64(globalObject, protocol)));
}

PYTHON_NATIVE(objectReduce)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(reduce(globalObject, args[0], 0)));
}

PYTHON_NATIVE(objectReduceEx)
{
    NATIVE_PROLOGUE();
    auto protocol = toCInt(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    // A class that has a __reduce__() of its own has that called.
    JSValue own = getAttributeIfPresent(globalObject, args[0], names.dunder_reduce);
    RETURN_IF_EXCEPTION(scope, { });
    if (own) {
        JSValue ofClass = getAttribute(globalObject, typeOf(globalObject, args[0])->object(), names.dunder_reduce);
        RETURN_IF_EXCEPTION(scope, { });
        if (ofClass != JSValue(realm->function(Function::ObjectReduce)))
            RELEASE_AND_RETURN(scope, JSValue::encode(call(globalObject, own)));
    }
    RELEASE_AND_RETURN(scope, JSValue::encode(reduce(globalObject, args[0], *protocol)));
}

PYTHON_NATIVE(objectGetState)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(defaultState(globalObject, args[0], false)));
}

// ---- __sizeof__()
//
// What CPython would say. Nothing here is laid out as it is there, but the number is one that programs add up, so it is worked out as it is there wherever it follows from
// what the object is. Where it depends on how the object came to be so, it is what it would be had it been added to one thing at a time, or for a list made all at once.

PYTHON_NATIVE(objectSizeOf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    PyType* type = typeOf(globalObject, args[0]);
    int64_t size = type->basicSize();
    if (type->itemSize() > 0) {
        if (isTuple(args[0]))
            size += static_cast<int64_t>(type->itemSize()) * asTuple(args[0])->length();
        else if (auto* bytes = dynamicDowncast<JSUint8Array>(args[0]))
            size += static_cast<int64_t>(type->itemSize()) * bytes->length();
    }
    return JSValue::encode(intFromInt64(globalObject, size));
}

// The table of a dict that has had so many keys put in it: _PyDict_KeysSize() of CPython's Objects/dictobject.c, and what insertion_resize() comes to.
static int64_t sizeOfDictKeys(uint64_t used, bool areAllStrings)
{
    auto usable = [] (unsigned log2) { return ((uint64_t { 1 } << log2) << 1) / 3; };
    unsigned log2 = 3;
    // It is full at `filled`, and is then made big enough for three times that.
    for (uint64_t filled = usable(log2); filled < used; filled = usable(log2)) {
        while ((uint64_t { 1 } << log2) < filled * 3)
            ++log2;
    }
    unsigned log2OfIndexBytes = log2 < 8 ? log2 : log2 < 16 ? log2 + 1 : log2 < 32 ? log2 + 2 : log2 + 3;
    return 32 + (int64_t { 1 } << log2OfIndexBytes) + usable(log2) * (areAllStrings ? 16 : 24);
}

// The table of a set that has had so many things added to it: set_add_entry() and set_table_resize() of Objects/setobject.c. Nothing if they fit in the set itself.
static int64_t sizeOfSetTable(uint64_t used)
{
    uint64_t slots = 8;
    for (uint64_t filled = 1; filled <= used; ++filled) {
        if (filled * 5 < (slots - 1) * 3)
            continue;
        uint64_t wanted = filled > 50000 ? filled * 2 : filled * 4;
        while (slots <= wanted)
            slots <<= 1;
    }
    return slots == 8 ? 0 : slots * 16;
}

PYTHON_NATIVE(builtinSizeOf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue self = args[0];
    PyType* type = typeOf(globalObject, self);
    int64_t size = type->basicSize();
    if (Number number = classify(self); number.isInt()) {
        // Thirty bits to a digit, and never fewer than one.
        int64_t digits = std::max<int64_t>((bitLengthOfInt(number) + 29) / 30, 1);
        size += digits * 4;
    } else if (JSString* string = stringIn(self)) {
        // As narrow as its widest character allows, and a zero at the end.
        auto view = string->view(globalObject);
        char32_t widest = 0;
        int64_t length = 0;
        for (char32_t character : view->codePoints()) {
            widest = std::max(widest, character);
            ++length;
        }
        // A str itself does without some of what the class allows for.
        if (self.isString())
            size = widest < 0x80 ? 40 : 56;
        size += (length + 1) * (widest < 0x100 ? 1 : widest < 0x10000 ? 2 : 4);
    } else if (isList(self))
        size += static_cast<int64_t>(asList(self)->length()) * sizeof(void*);
    else if (auto* bytes = dynamicDowncast<JSUint8Array>(self))
        size += bytes->length() ? bytes->length() + 1 : 0;
    else if (isSet(self))
        size += sizeOfSetTable(uncheckedDowncast<PySet>(self.asCell())->size());
    else if (isDict(self)) {
        PyDict* dict = asDict(self);
        if (dict->size()) {
            bool areAllStrings = true;
            dict->forEach(globalObject, [&] (JSValue key, JSValue) {
                areAllStrings = key.isString();
                return areAllStrings;
            });
            size += sizeOfDictKeys(dict->size(), areAllStrings);
        }
    } else if (isClass(self)) {
        // One that is built in is a smaller thing than one that a program makes, which has besides room for the names of its instances' attributes.
        PyType* ofClass = asType(self);
        constexpr int64_t sizeOfStaticType = 416;
        constexpr unsigned long hasInlineValues = 1ul << 2;
        size = !ofClass->hasFlag(PyType::IsHeapType) ? sizeOfStaticType : size + (ofClass->flagsForPython() & hasInlineValues ? 32 + 64 + 42 * 16 : 0);
    }
    return JSValue::encode(intFromInt64(globalObject, size));
}

// ---- The built-in classes that have their own way. Each is as in the file of that class in CPython.

// (an exact copy,)
PYTHON_NATIVE(getNewArguments)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    JSValue self = args[0];
    JSValue copy;
    if (isTuple(self)) {
        MarkedArgumentBuffer items;
        for (auto& item : asTuple(self)->span())
            items.append(item.get());
        copy = PyTuple::createFromArguments(globalObject, items);
    } else if (auto* bytes = dynamicDowncast<JSUint8Array>(self))
        copy = newBytes(globalObject, bytes->typedSpan());
    else if (auto* complex = dynamicDowncast<PyComplex>(self))
        return JSValue::encode(PyTuple::create(globalObject, { floatFromDouble(complex->real()), floatFromDouble(complex->imaginary()) }));
    else
        copy = stringIn(self);
    return JSValue::encode(PyTuple::create(globalObject, { copy }));
}

// The __dict__ of an exception if there is anything in it, and otherwise nothing.
static JSValue dictOfException(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue dict = getInstanceDict(globalObject, exception);
    RETURN_IF_EXCEPTION(scope, { });
    return uncheckedDowncast<PyDict>(dict.asCell())->size() ? dict : JSValue();
}

static JSValue fieldOfException(VM& vm, JSValue exception, const Identifier& field)
{
    JSValue value = asObject(exception)->getDirect(vm, field);
    return value && !isNone(value) ? value : JSValue();
}

static JSValue reducedException(JSGlobalObject* globalObject, JSValue exception, JSValue arguments, JSValue state)
{
    JSValue type = typeOf(globalObject, exception)->object();
    return state ? PyTuple::create(globalObject, { type, arguments, state }) : PyTuple::create(globalObject, { type, arguments });
}

PYTHON_NATIVE(exceptionReduce)
{
    NATIVE_PROLOGUE();
    JSValue dict = dictOfException(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(reducedException(globalObject, args[0], asObject(args[0])->getDirect(vm, names.private_args), dict));
}

PYTHON_NATIVE(exceptionSetState)
{
    NATIVE_PROLOGUE();
    if (isNone(args[1]))
        RETURN_NONE();
    if (!isDict(args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, "state is not a dictionary"_s));
    MarkedArgumentBuffer pairs;
    asDict(args[1])->forEach(globalObject, [&] (JSValue key, JSValue value) {
        pairs.append(key);
        pairs.append(value);
        return true;
    });
    for (size_t i = 0; i < pairs.size(); i += 2) {
        if (!pairs.at(i).isString())
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("attribute name must be string, not '"_s, typeName(globalObject, pairs.at(i)), '\'')));
        auto name = asString(pairs.at(i))->toIdentifier(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        setAttribute(globalObject, args[0], name, pairs.at(i + 1));
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// The __dict__, and with it those of the fields that are not among the arguments. Empty if there is nothing to say.
static JSValue stateWithFields(JSGlobalObject* globalObject, JSValue exception, std::initializer_list<std::pair<ASCIILiteral, JSValue>> fields)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue dict = dictOfException(globalObject, exception);
    RETURN_IF_EXCEPTION(scope, { });
    if (std::ranges::none_of(fields, [] (auto& field) { return !!field.second; }))
        return dict;
    PyDict* state = PyDict::create(globalObject);
    if (dict) {
        asDict(dict)->forEach(globalObject, [&] (JSValue key, JSValue value) {
            state->set(globalObject, key, value);
            return true;
        });
    }
    for (auto& [name, value] : fields) {
        if (value)
            state->set(globalObject, jsString(vm, String(name)), value);
    }
    return state;
}

static JSValue stateOfAttributeError(JSGlobalObject* globalObject, JSValue exception)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    // Not `obj`, which is unlikely to be something that can be pickled.
    return stateWithFields(globalObject, exception, { { "name"_s, fieldOfException(vm, exception, names.field_name) }, { "args"_s, asObject(exception)->getDirect(vm, names.private_args) } });
}

PYTHON_NATIVE(attributeErrorGetState)
{
    NATIVE_PROLOGUE();
    JSValue state = stateOfAttributeError(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(state ? state : jsUndefined());
}

PYTHON_NATIVE(attributeErrorReduce)
{
    NATIVE_PROLOGUE();
    JSValue state = stateOfAttributeError(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(reducedException(globalObject, args[0], asObject(args[0])->getDirect(vm, names.private_args), state ? state : jsUndefined()));
}

PYTHON_NATIVE(importErrorReduce)
{
    NATIVE_PROLOGUE();
    JSValue state = stateWithFields(globalObject, args[0], { { "name"_s, fieldOfException(vm, args[0], names.field_name) }, { "path"_s, fieldOfException(vm, args[0], names.field_path) }, { "name_from"_s, fieldOfException(vm, args[0], names.field_nameFrom) } });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(reducedException(globalObject, args[0], asObject(args[0])->getDirect(vm, names.private_args), state));
}

PYTHON_NATIVE(osErrorReduce)
{
    NATIVE_PROLOGUE();
    JSValue arguments = asObject(args[0])->getDirect(vm, names.private_args);
    // The file names were taken out of the arguments, and are put back.
    JSValue filename = fieldOfException(vm, args[0], names.field_filename);
    if (asTuple(arguments)->length() == 2 && filename) {
        JSValue first = asTuple(arguments)->at(0);
        JSValue second = asTuple(arguments)->at(1);
        if (JSValue filename2 = fieldOfException(vm, args[0], names.field_filename2))
            arguments = PyTuple::create(globalObject, { first, second, filename, jsUndefined(), filename2 });
        else
            arguments = PyTuple::create(globalObject, { first, second, filename });
    }
    JSValue dict = dictOfException(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(reducedException(globalObject, args[0], arguments, dict));
}

// "Ellipsis" and "NotImplemented": what they go by in builtins
PYTHON_NATIVE(singletonReduce)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(jsString(vm, repr(globalObject, args[0]))));
}

PYTHON_NATIVE(setReduce)
{
    NATIVE_PROLOGUE();
    JSValue keys = listFromIterable(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue state = stateOf(globalObject, args[0], false);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, args[0])->object(), PyTuple::create(globalObject, { keys }), state }));
}

PYTHON_NATIVE(sliceReduce)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto* slice = uncheckedDowncast<PySlice>(args[0].asCell());
    return JSValue::encode(PyTuple::create(globalObject, { typeOf(globalObject, slice)->object(), PyTuple::create(globalObject, { slice->start(), slice->stop(), slice->step() }) }));
}

// bytearray.__reduce__() and bytearray.__reduce_ex__(proto=0)
PYTHON_NATIVE(byteArrayReduce)
{
    bool takesProtocol = unpack<bool>(callFrame, 0);
    NATIVE_PROLOGUE();
    int64_t protocol = 2;
    if (takesProtocol) {
        protocol = 0;
        if (JSValue given = args.at(1)) {
            auto index = toIndex(globalObject, given);
            RETURN_IF_EXCEPTION(scope, { });
            protocol = *index;
        }
    }
    JSValue state = stateOf(globalObject, args[0], false);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue type = typeOf(globalObject, args[0])->object();
    auto content = uncheckedDowncast<JSUint8Array>(args[0].asCell())->typedSpan();
    if (content.empty())
        return JSValue::encode(PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { }), state }));
    // Before there were bytes to pickle it went as a str.
    if (protocol < 3) {
        String text = textOfBytes(globalObject, content);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { jsString(vm, text), jsNontrivialString(vm, "latin-1"_s) }), state }));
    }
    return JSValue::encode(PyTuple::create(globalObject, { type, PyTuple::create(globalObject, { newBytes(globalObject, content) }), state }));
}

// (getattr, (what it is bound to, what the function is called))
PYTHON_NATIVE(methodReduce)
{
    NATIVE_PROLOGUE();
    auto* method = tryBoundMethod(args[0]);
    JSValue name = getAttribute(globalObject, method->function(), names.dunder_name);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue getattr = getBuiltin(globalObject, "getattr"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { method->self(), name }) }));
}

// (getattr, (the class that it is an attribute of, what it is called))
PYTHON_NATIVE(memberReduce)
{
    NATIVE_PROLOGUE();
    auto* member = uncheckedDowncast<PyGetSetDescriptor>(args[0].asCell());
    JSValue getattr = getBuiltin(globalObject, "getattr"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { getattr, PyTuple::create(globalObject, { member->owner()->object(), member->name() }) }));
}

void initializeReduce(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    for (PyType* type : { realm->typeTuple(), realm->typeStr(), realm->typeBytes(), realm->typeComplex() })
        addMethods(globalObject, type, { { "__getnewargs__"_s, getNewArguments } });
    addMethods(globalObject, realm->typeBaseException(), { { "__reduce__"_s, exceptionReduce }, { "__setstate__"_s, exceptionSetState } });
    addMethods(globalObject, realm->typeAttributeError(), { { "__reduce__"_s, attributeErrorReduce }, { "__getstate__"_s, attributeErrorGetState } });
    addMethods(globalObject, realm->typeImportError(), { { "__reduce__"_s, importErrorReduce } });
    addMethods(globalObject, realm->typeOSError(), { { "__reduce__"_s, osErrorReduce } });
    for (PyType* type : { realm->typeEllipsis(), realm->typeNotImplementedType() })
        addMethods(globalObject, type, { { "__reduce__"_s, singletonReduce } });
    for (PyType* type : { realm->typeSet(), realm->typeFrozenSet() })
        addMethods(globalObject, type, { { "__reduce__"_s, setReduce } });
    addMethods(globalObject, realm->typeSlice(), { { "__reduce__"_s, sliceReduce } });
    addMethods(globalObject, realm->typeByteArray(), { { "__reduce__"_s, byteArrayReduce, Kind::Method, pack(false) }, { "__reduce_ex__"_s, byteArrayReduce, Kind::Method, pack(true) } });
    addMethods(globalObject, realm->typeMethod(), { { "__reduce__"_s, methodReduce } });
    addMethods(globalObject, realm->typeMemberDescriptor(), { { "__reduce__"_s, memberReduce } });

#define ADD_SIZE_OF(name, pythonName, base, layout, flags) \
    if (realm->type##name() != realm->typeObject()) \
        addMethodsThatCPythonHas(globalObject, realm->type##name(), { { "__sizeof__"_s, builtinSizeOf } });
    FOR_EACH_PYTHON_BUILTIN_TYPE(ADD_SIZE_OF)
#undef ADD_SIZE_OF

    PyType* object = realm->typeObject();
    addMethods(globalObject, object, {
        { "__reduce__"_s, objectReduce },
        { "__reduce_ex__"_s, objectReduceEx },
        { "__getstate__"_s, objectGetState },
        { "__sizeof__"_s, objectSizeOf },
    });
    realm->setFunction(vm, Function::ObjectReduce, asObject(object->lookupOwn(vm, vm.pythonNames().dunder_reduce)));
    realm->setFunction(vm, Function::ObjectGetState, asObject(object->lookupOwn(vm, Identifier::fromString(vm, "__getstate__"_s))));
}

} } // namespace JSC::Python
