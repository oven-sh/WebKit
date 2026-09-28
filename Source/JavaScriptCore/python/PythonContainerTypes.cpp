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

// list, tuple, dict, set, frozenset, range, slice, the views of a dict, and the iterators.

namespace JSC { namespace Python {

// ---- list

static JSArray* selfList(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral method)
{
    JSValue self = args.at(0);
    if (!self || !isList(self)) {
        raiseTypeError(globalObject, scope, makeString("descriptor '"_s, method, "' for 'list' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
        return nullptr;
    }
    return asList(self);
}

#define LIST_PROLOGUE(method) \
    NATIVE_PROLOGUE(); \
    JSArray* self = selfList(globalObject, scope, args, method ""_s); \
    RETURN_IF_EXCEPTION(scope, { });

PYTHON_NATIVE(listNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args.at(0));
    if (type == realm->typeList())
        RELEASE_AND_RETURN(scope, JSValue::encode(newList(globalObject)));
    return JSValue::encode(JSArray::create(vm, type->instanceStructure()));
}

// list(iterable=())
PYTHON_NATIVE(listInit)
{
    LIST_PROLOGUE("__init__");
    self->setLength(globalObject, 0, true);
    RETURN_IF_EXCEPTION(scope, { });
    if (args.size() > 1) {
        scope.release();
        listExtend(globalObject, self, args[1]);
    }
    RETURN_NONE();
}

PYTHON_NATIVE(listAppendMethod)
{
    LIST_PROLOGUE("append");
    scope.release();
    listAppend(globalObject, self, args[1]);
    RETURN_NONE();
}

PYTHON_NATIVE(listExtendMethod)
{
    LIST_PROLOGUE("extend");
    scope.release();
    listExtend(globalObject, self, args[1]);
    RETURN_NONE();
}

PYTHON_NATIVE(listInsertMethod)
{
    LIST_PROLOGUE("insert");
    auto index = toIndex(globalObject, args[1], true);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t length = self->length();
    int64_t at = *index < 0 ? std::max<int64_t>(*index + length, 0) : std::min(*index, length);
    scope.release();
    listInsert(globalObject, self, at, args[2]);
    RETURN_NONE();
}

PYTHON_NATIVE(listPop)
{
    LIST_PROLOGUE("pop");
    int64_t length = self->length();
    if (!length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop from empty list"_s));
    int64_t at = length - 1;
    if (args.size() > 1) {
        auto index = toIndex(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        at = *index < 0 ? *index + length : *index;
        if (at < 0 || at >= length)
            return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop index out of range"_s));
    }
    JSValue value = listGet(globalObject, self, at);
    RETURN_IF_EXCEPTION(scope, { });
    listRemoveRange(globalObject, self, at, 1);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value);
}

PYTHON_NATIVE(listRemove)
{
    LIST_PROLOGUE("remove");
    for (unsigned i = 0; i < self->length(); ++i) {
        JSValue item = listGet(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        bool same = isEqual(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (same) {
            scope.release();
            listRemoveRange(globalObject, self, i, 1);
            RETURN_NONE();
        }
    }
    return JSValue::encode(raiseValueError(globalObject, scope, "list.remove(x): x not in list"_s));
}

PYTHON_NATIVE(listClear)
{
    LIST_PROLOGUE("clear");
    scope.release();
    self->setLength(globalObject, 0, true);
    RETURN_NONE();
}

PYTHON_NATIVE(listCopy)
{
    LIST_PROLOGUE("copy");
    RELEASE_AND_RETURN(scope, JSValue::encode(listFromIterable(globalObject, self)));
}

PYTHON_NATIVE(listReverse)
{
    LIST_PROLOGUE("reverse");
    unsigned length = self->length();
    for (unsigned i = 0; i < length / 2; ++i) {
        JSValue a = listGet(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue b = listGet(globalObject, self, length - 1 - i);
        RETURN_IF_EXCEPTION(scope, { });
        listSet(globalObject, self, i, b);
        RETURN_IF_EXCEPTION(scope, { });
        listSet(globalObject, self, length - 1 - i, a);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// index(value, start=0, stop=the end), of a list or a tuple.
template<typename Get>
static EncodedJSValue sequenceIndex(JSGlobalObject* globalObject, const NativeArguments& args, int64_t length, ASCIILiteral typeName, const Get& get)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto resolve = [&] (JSValue value, int64_t whenAbsent) -> int64_t {
        if (!value)
            return whenAbsent;
        auto index = toIndex(globalObject, value, true);
        RETURN_IF_EXCEPTION(scope, 0);
        return *index < 0 ? std::max<int64_t>(*index + length, 0) : *index;
    };
    int64_t start = resolve(args.at(2), 0);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t stop = std::min(resolve(args.at(3), length), length);
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = start; i < stop; ++i) {
        JSValue item = get(i);
        RETURN_IF_EXCEPTION(scope, { });
        bool same = isEqual(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (same)
            return JSValue::encode(jsNumber(static_cast<int32_t>(i)));
    }
    return JSValue::encode(raiseValueError(globalObject, scope, makeString(typeName, ".index(x): x not in "_s, typeName)));
}

template<typename Get>
static EncodedJSValue sequenceCount(JSGlobalObject* globalObject, const NativeArguments& args, unsigned length, const Get& get)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t count = 0;
    for (unsigned i = 0; i < length; ++i) {
        JSValue item = get(i);
        RETURN_IF_EXCEPTION(scope, { });
        bool same = isEqual(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        count += same;
    }
    return JSValue::encode(jsNumber(count));
}

PYTHON_NATIVE(listIndex)
{
    LIST_PROLOGUE("index");
    RELEASE_AND_RETURN(scope, sequenceIndex(globalObject, args, self->length(), "list"_s, [&] (unsigned i) { return listGet(globalObject, self, i); }));
}

PYTHON_NATIVE(listCount)
{
    LIST_PROLOGUE("count");
    RELEASE_AND_RETURN(scope, sequenceCount(globalObject, args, self->length(), [&] (unsigned i) { return listGet(globalObject, self, i); }));
}

// A stable merge sort of `values`, by `keys` if there are any. False if a comparison raised.
bool sortValues(JSGlobalObject* globalObject, MarkedArgumentBuffer& values, JSValue keyFunction, bool reverse, MarkedArgumentBuffer& sorted)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = values.size();
    MarkedArgumentBuffer keys;
    bool hasKeys = keyFunction && !isNone(keyFunction);
    for (unsigned i = 0; hasKeys && i < count; ++i) {
        keys.append(call(globalObject, keyFunction, values.at(i)));
        RETURN_IF_EXCEPTION(scope, false);
    }
    auto keyOf = [&] (unsigned index) { return hasKeys ? keys.at(index) : values.at(index); };
    // a comes before b. Only < is ever asked, as Python promises.
    auto isLess = [&] (unsigned a, unsigned b) -> bool {
        JSValue result = compare(globalObject, ComparisonOperator::Lt, keyOf(a), keyOf(b));
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, isTrue(globalObject, result));
    };

    Vector<unsigned> order(count);
    Vector<unsigned> scratch(count);
    for (unsigned i = 0; i < count; ++i)
        order[i] = reverse ? count - 1 - i : i; // Turned round twice, so that equal elements stay in order.
    for (unsigned width = 1; width < count; width *= 2) {
        for (unsigned low = 0; low < count; low += 2 * width) {
            unsigned middle = std::min(low + width, count);
            unsigned high = std::min(low + 2 * width, count);
            unsigned i = low;
            unsigned j = middle;
            unsigned k = low;
            while (i < middle && j < high) {
                bool takeRight = isLess(order[j], order[i]);
                RETURN_IF_EXCEPTION(scope, false);
                scratch[k++] = takeRight ? order[j++] : order[i++];
            }
            while (i < middle)
                scratch[k++] = order[i++];
            while (j < high)
                scratch[k++] = order[j++];
        }
        std::swap(order, scratch);
    }
    for (unsigned i = 0; i < count; ++i)
        sorted.append(values.at(order[reverse ? count - 1 - i : i]));
    return true;
}

// sort(*, key=None, reverse=False)
PYTHON_NATIVE(listSort)
{
    LIST_PROLOGUE("sort");
    JSValue reverseValue = args.keyword(globalObject, "reverse"_s);
    bool reverse = reverseValue && isTrue(globalObject, reverseValue);
    RETURN_IF_EXCEPTION(scope, { });
    MarkedArgumentBuffer values;
    for (unsigned i = 0; i < self->length(); ++i) {
        values.append(listGet(globalObject, self, i));
        RETURN_IF_EXCEPTION(scope, { });
    }
    MarkedArgumentBuffer sorted;
    sortValues(globalObject, values, args.keyword(globalObject, "key"_s), reverse, sorted);
    RETURN_IF_EXCEPTION(scope, { });
    scope.release();
    listReplaceRange(globalObject, self, 0, self->length(), sorted);
    RETURN_NONE();
}

PYTHON_NATIVE(listReversed)
{
    LIST_PROLOGUE("__reversed__");
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::ListReverse, self, JSValue(), static_cast<int64_t>(self->length()) - 1));
}

// ---- tuple

PYTHON_NATIVE(tupleNew)
{
    NATIVE_PROLOGUE();
    PyType* type = asType(args[0]);
    if (type == realm->typeTuple()) {
        if (args.size() == 1)
            return JSValue::encode(realm->emptyTuple());
        RELEASE_AND_RETURN(scope, JSValue::encode(tupleFromIterable(globalObject, args[1])));
    }
    MarkedArgumentBuffer values;
    if (args.size() > 1) {
        collect(globalObject, args[1], values);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyTuple* tuple = PyTuple::create(vm, type->instanceStructure(), values.size());
    for (unsigned i = 0; i < values.size(); ++i)
        tuple->initializeAt(vm, i, values.at(i));
    return JSValue::encode(tuple);
}

PYTHON_NATIVE(tupleIndex)
{
    NATIVE_PROLOGUE();
    auto* self = uncheckedDowncast<PyTuple>(args.at(0).asCell());
    RELEASE_AND_RETURN(scope, sequenceIndex(globalObject, args, self->length(), "tuple"_s, [&] (unsigned i) { return self->at(i); }));
}

PYTHON_NATIVE(tupleCount)
{
    NATIVE_PROLOGUE();
    auto* self = uncheckedDowncast<PyTuple>(args.at(0).asCell());
    RELEASE_AND_RETURN(scope, sequenceCount(globalObject, args, self->length(), [&] (unsigned i) { return self->at(i); }));
}

// ---- dict

static PyDict* selfDict(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral method)
{
    JSValue self = args.at(0);
    if (!self || !isDict(self)) {
        raiseTypeError(globalObject, scope, makeString("descriptor '"_s, method, "' for 'dict' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
        return nullptr;
    }
    return uncheckedDowncast<PyDict>(self.asCell());
}

#define DICT_PROLOGUE(method) \
    NATIVE_PROLOGUE(); \
    PyDict* self = selfDict(globalObject, scope, args, method ""_s); \
    RETURN_IF_EXCEPTION(scope, { });

PYTHON_NATIVE(dictNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyDict::create(vm, asType(args.at(0))->instanceStructure()));
}

// What dict(source, **keywords) and dict.update(source, **keywords) do.
static void updateDict(JSGlobalObject* globalObject, PyDict* dict, const NativeArguments& args, ASCIILiteral method)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() > 2) {
        raiseTypeError(globalObject, scope, makeString(method, " expected at most 1 argument, got "_s, args.size() - 1));
        return;
    }
    if (args.size() == 2) {
        JSValue source = args[1];
        if (isDict(source) && !typeOf(globalObject, source)->hasFlag(PyType::IsHeapType)) {
            asDict(source)->forEach(globalObject, [&] (JSValue key, JSValue value) {
                return dict->set(globalObject, key, value);
            });
            RETURN_IF_EXCEPTION(scope, void());
        } else {
            JSValue keysMethod = getAttributeIfPresent(globalObject, source, Identifier::fromString(vm, "keys"_s));
            RETURN_IF_EXCEPTION(scope, void());
            if (keysMethod) {
                // A mapping.
                JSValue keys = call(globalObject, keysMethod);
                RETURN_IF_EXCEPTION(scope, void());
                MarkedArgumentBuffer collected;
                collect(globalObject, keys, collected);
                RETURN_IF_EXCEPTION(scope, void());
                for (unsigned i = 0; i < collected.size(); ++i) {
                    JSValue value = getItem(globalObject, source, collected.at(i));
                    RETURN_IF_EXCEPTION(scope, void());
                    dict->set(globalObject, collected.at(i), value);
                    RETURN_IF_EXCEPTION(scope, void());
                }
            } else {
                // Pairs.
                unsigned position = 0;
                forEach(globalObject, source, [&] (JSValue pair) {
                    MarkedArgumentBuffer parts;
                    collect(globalObject, pair, parts);
                    if (scope.exception()) {
                        if (catchException(globalObject, BuiltinType::TypeError))
                            raiseTypeError(globalObject, scope, makeString("cannot convert dictionary update sequence element #"_s, position, " to a sequence"_s));
                        return false;
                    }
                    if (parts.size() != 2) {
                        raiseValueError(globalObject, scope, makeString("dictionary update sequence element #"_s, position, " has length "_s, parts.size(), "; 2 is required"_s));
                        return false;
                    }
                    ++position;
                    return dict->set(globalObject, parts.at(0), parts.at(1));
                });
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        dict->set(globalObject, args.keywordName(i), args.keywordValue(i));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

PYTHON_NATIVE(dictInit)
{
    DICT_PROLOGUE("__init__");
    scope.release();
    updateDict(globalObject, self, args, "dict"_s);
    RETURN_NONE();
}

PYTHON_NATIVE(dictUpdateMethod)
{
    DICT_PROLOGUE("update");
    scope.release();
    updateDict(globalObject, self, args, "update"_s);
    RETURN_NONE();
}

PYTHON_NATIVE(dictGet)
{
    DICT_PROLOGUE("get");
    JSValue value = self->get(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    return JSValue::encode(args.size() > 2 ? args[2] : jsUndefined());
}

PYTHON_NATIVE(dictSetDefault)
{
    DICT_PROLOGUE("setdefault");
    JSValue value = self->get(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    value = args.size() > 2 ? args[2] : jsUndefined();
    self->set(globalObject, args[1], value);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(value);
}

PYTHON_NATIVE(dictPop)
{
    DICT_PROLOGUE("pop");
    JSValue value = self->remove(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (value)
        return JSValue::encode(value);
    if (args.size() > 2)
        return JSValue::encode(args[2]);
    return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
}

PYTHON_NATIVE(dictPopItem)
{
    DICT_PROLOGUE("popitem");
    JSValue key;
    JSValue value;
    if (self->removeLast(globalObject, key, value))
        return JSValue::encode(PyTuple::create(globalObject, { key, value }));
    return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, jsNontrivialString(vm, "popitem(): dictionary is empty"_s)));
}

PYTHON_NATIVE(dictClear)
{
    DICT_PROLOGUE("clear");
    self->clear(globalObject);
    RETURN_NONE();
}

PYTHON_NATIVE(dictCopy)
{
    DICT_PROLOGUE("copy");
    PyDict* copy = PyDict::create(globalObject);
    copy->copyFrom(globalObject, *self);
    return JSValue::encode(copy);
}

// dict.fromkeys(iterable, value=None)
PYTHON_NATIVE(dictFromKeys)
{
    NATIVE_PROLOGUE();
    JSValue result = call(globalObject, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = args.size() > 2 ? args[2] : jsUndefined();
    forEach(globalObject, args[1], [&] (JSValue key) {
        setItem(globalObject, result, key, value);
        return !scope.exception();
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(dictView)
{
    auto view = unpack<BuiltinType>(callFrame, 0);
    DICT_PROLOGUE("keys");
    return JSValue::encode(PyNativeObject::create(globalObject, view, self));
}

PYTHON_NATIVE(dictReversed)
{
    DICT_PROLOGUE("__reversed__");
    return JSValue::encode(PyIterator::create(globalObject, PyIterator::Kind::DictReverseKeys, self));
}

// ---- The views of a dict

// ---- set and frozenset

static PySet* selfSet(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral method)
{
    JSValue self = args.at(0);
    if (!self || !isSet(self)) {
        raiseTypeError(globalObject, scope, makeString("descriptor '"_s, method, "' for 'set' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
        return nullptr;
    }
    return uncheckedDowncast<PySet>(self.asCell());
}

#define SET_PROLOGUE(method) \
    NATIVE_PROLOGUE(); \
    PySet* self = selfSet(globalObject, scope, args, method ""_s); \
    RETURN_IF_EXCEPTION(scope, { });

PYTHON_NATIVE(setNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PySet::create(vm, asType(args.at(0))->instanceStructure()));
}

PYTHON_NATIVE(setInit)
{
    SET_PROLOGUE("__init__");
    self->clear(vm);
    if (args.size() > 1) {
        forEach(globalObject, args[1], [&] (JSValue value) {
            return self->add(globalObject, value);
        });
    }
    RETURN_NONE();
}

PYTHON_NATIVE(frozenSetNew)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(setFromIterable(globalObject, asType(args[0])->instanceStructure(), args.at(1))));
}

PYTHON_NATIVE(setAddMethod)
{
    SET_PROLOGUE("add");
    scope.release();
    self->add(globalObject, args[1]);
    RETURN_NONE();
}

PYTHON_NATIVE(setRemove)
{
    auto raises = unpack<bool>(callFrame, 0);
    SET_PROLOGUE("remove");
    JSValue removed = self->remove(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed && raises)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
    RETURN_NONE();
}

PYTHON_NATIVE(setPop)
{
    SET_PROLOGUE("pop");
    for (unsigned entry = 0; entry < self->entryCount(); ++entry) {
        if (JSValue key = self->keyAt(entry)) {
            self->removeEntry(vm, entry);
            return JSValue::encode(key);
        }
    }
    return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, jsNontrivialString(vm, "pop from an empty set"_s)));
}

PYTHON_NATIVE(setClear)
{
    SET_PROLOGUE("clear");
    self->clear(vm);
    RETURN_NONE();
}

PYTHON_NATIVE(setCopy)
{
    SET_PROLOGUE("copy");
    PySet* copy = PySet::create(vm, self->structure());
    copy->copyFrom(vm, globalObject, *self);
    return JSValue::encode(copy);
}

// union(*others) and the rest, which take any iterables, and update(*others) and the rest, which change the set.
PYTHON_NATIVE(setMethod)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    auto inPlace = unpack<bool>(callFrame, 1);
    SET_PROLOGUE("union");
    PySet* result = self;
    if (!inPlace) {
        result = PySet::create(vm, self->structure());
        result->copyFrom(vm, globalObject, *self);
    }
    for (unsigned i = 1; i < args.size(); ++i) {
        PySet* other = setFromIterable(globalObject, realm->structureFor(BuiltinType::Set), args[i]);
        RETURN_IF_EXCEPTION(scope, { });
        setOperation(globalObject, op, true, result, other);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (inPlace)
        RETURN_NONE();
    return JSValue::encode(result);
}

PYTHON_NATIVE(setRelation)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    SET_PROLOGUE("issubset");
    PySet* other = setFromIterable(globalObject, realm->structureFor(BuiltinType::Set), args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(setCompare(globalObject, op, self, other)));
}

PYTHON_NATIVE(setIsDisjoint)
{
    SET_PROLOGUE("isdisjoint");
    bool isDisjoint = true;
    forEach(globalObject, args[1], [&] (JSValue value) {
        int entry = self->find(globalObject, value);
        if (entry >= 0)
            isDisjoint = false;
        return entry == PyHashTable::notFound;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(isDisjoint));
}

// ---- slice

PYTHON_NATIVE(sliceNew)
{
    NATIVE_PROLOGUE();
    if (!args.check(globalObject, scope, "slice"_s, 2, 4))
        return { };
    if (args.size() == 2)
        return JSValue::encode(PySlice::create(globalObject, jsUndefined(), args[1], jsUndefined()));
    return JSValue::encode(PySlice::create(globalObject, args[1], args[2], args.size() > 3 ? args[3] : jsUndefined()));
}

// ---- enumerate, zip, map, filter, reversed

// enumerate(iterable, start=0)
PYTHON_NATIVE(enumerateNew)
{
    NATIVE_PROLOGUE();
    // It puts things in its own way: enumerate_vectorcall() of CPython's Objects/enumobject.c.
    unsigned given = args.size() - 1 + args.keywordCount();
    if (!given || given > 2) {
        if (args.size() == 1)
            return JSValue::encode(raiseTypeError(globalObject, scope, "enumerate() missing required argument 'iterable'"_s));
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString("enumerate() takes at most 2 arguments ("_s, given, " given)"_s)));
    }
    for (unsigned i = 0; i < args.keywordCount(); ++i) {
        // Each is to be the name of what has not been given yet.
        String keyword = args.keywordName(i)->value(globalObject);
        bool isExpected = given == 1 ? keyword == "iterable"_s : args.keywordCount() == 1 ? keyword == "start"_s : keyword == "iterable"_s || keyword == "start"_s;
        if (!isExpected || (i && keyword == String(args.keywordName(0)->value(globalObject))))
            return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', keyword, "' is an invalid keyword argument for enumerate()"_s)));
    }
    JSValue startValue = args.at(2);
    int64_t start = 0;
    JSValue bigStart;
    if (startValue) {
        JSValue integer = toInt(globalObject, startValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (auto small = tryInt64(integer))
            start = *small;
        else
            bigStart = integer;
    }
    JSValue iterator = getIterator(globalObject, args.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, asType(args[0])->instanceStructure(), PyIterator::Kind::Enumerate, iterator, bigStart, start));
}

static PyTuple* iteratorsOf(JSGlobalObject* globalObject, const NativeArguments& args, unsigned first, ASCIILiteral function)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* iterators = PyTuple::create(globalObject, args.size() - first);
    for (unsigned i = first; i < args.size(); ++i) {
        JSValue iterator = getIterator(globalObject, args[i]);
        if (scope.exception()) {
            if (function == "zip"_s && catchException(globalObject, BuiltinType::TypeError))
                raiseTypeError(globalObject, scope, makeString("'"_s, typeName(globalObject, args[i]), "' object is not iterable"_s));
            return nullptr;
        }
        iterators->initializeAt(vm, i - first, iterator);
    }
    return iterators;
}

// zip(*iterables, strict=False)
PYTHON_NATIVE(zipNew)
{
    NATIVE_PROLOGUE();
    JSValue strictValue = args.keyword(globalObject, "strict"_s);
    bool strict = strictValue && isTrue(globalObject, strictValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* iterators = iteratorsOf(globalObject, args, 1, "zip"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, asType(args[0])->instanceStructure(), PyIterator::Kind::Zip, iterators, JSValue(), strict));
}

PYTHON_NATIVE(mapNew)
{
    NATIVE_PROLOGUE();
    if (args.size() < 3)
        return JSValue::encode(raiseTypeError(globalObject, scope, "map() must have at least two arguments."_s));
    JSValue strictValue = args.keyword(globalObject, "strict"_s);
    bool strict = strictValue && isTrue(globalObject, strictValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* iterators = iteratorsOf(globalObject, args, 2, "map"_s);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, asType(args[0])->instanceStructure(), PyIterator::Kind::Map, args[1], iterators, strict));
}

PYTHON_NATIVE(filterNew)
{
    NATIVE_PROLOGUE();
    JSValue iterator = getIterator(globalObject, args[2]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, asType(args[0])->instanceStructure(), PyIterator::Kind::Filter, args[1], iterator));
}

PYTHON_NATIVE(reversedNew)
{
    NATIVE_PROLOGUE();
    JSValue self;
    JSValue method = lookupSpecial(globalObject, args[1], names.dunder_reversed, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (method && !isNone(method))
        RELEASE_AND_RETURN(scope, JSValue::encode(callMethod(globalObject, method, self)));
    PyType* type = typeOf(globalObject, args[1]);
    if (method || !type->lookup(vm, names.dunder_getitem) || !type->lookup(vm, names.dunder_len))
        return JSValue::encode(raiseTypeError(globalObject, scope, makeString('\'', type->nameString(globalObject), "' object is not reversible"_s)));
    int64_t size = length(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, asType(args[0])->instanceStructure(), PyIterator::Kind::Reversed, args[1], JSValue(), size - 1));
}

// ---- Setting them up

void initializeContainerTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    auto makeUnhashable = [&] (PyType* type) { type->putDirect(vm, names.dunder_hash, jsUndefined()); };

    PyType* list = realm->typeList();
    // What JavaScript can do with an Array it can do with an instance of a class derived from list.
    list->setPrototypeDirect(vm, globalObject->arrayPrototype());
    addMethods(globalObject, list, {
        { "__new__"_s, listNew, Kind::New },
        { "__init__"_s, listInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, nativeRepr },
        { "__len__"_s, nativeLen },
        { "__getitem__"_s, nativeGetItem },
        { "__setitem__"_s, nativeSetItem },
        { "__delitem__"_s, nativeDelItem },
        { "__contains__"_s, nativeContains },
        { "__iter__"_s, nativeIter },
        { "__reversed__"_s, listReversed },
        { "append"_s, listAppendMethod },
        { "extend"_s, listExtendMethod },
        { "insert"_s, listInsertMethod },
        { "pop"_s, listPop },
        { "remove"_s, listRemove },
        { "clear"_s, listClear },
        { "copy"_s, listCopy },
        { "reverse"_s, listReverse },
        { "index"_s, listIndex },
        { "count"_s, listCount },
        { "sort"_s, listSort },
    });
    addComparisons(globalObject, list);
    addBinaryOperators(globalObject, list, { BinaryOperator::Add }, false, true);
    addBinaryOperators(globalObject, list, { BinaryOperator::Mult }, true, true);
    makeUnhashable(list);

    PyType* tuple = realm->typeTuple();
    addMethods(globalObject, tuple, {
        { "__new__"_s, tupleNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__repr__"_s, nativeRepr },
        { "__hash__"_s, nativeHash },
        { "__len__"_s, nativeLen },
        { "__getitem__"_s, nativeGetItem },
        { "__contains__"_s, nativeContains },
        { "__iter__"_s, nativeIter },
        { "index"_s, tupleIndex },
        { "count"_s, tupleCount },
    });
    addComparisons(globalObject, tuple);
    addBinaryOperators(globalObject, tuple, { BinaryOperator::Add }, false, false);
    addBinaryOperators(globalObject, tuple, { BinaryOperator::Mult }, true, false);

    PyType* dict = realm->typeDict();
    addMethods(globalObject, dict, {
        { "__new__"_s, dictNew, Kind::New },
        { "__init__"_s, dictInit },
        { "__repr__"_s, nativeRepr },
        { "__len__"_s, nativeLen },
        { "__getitem__"_s, nativeGetItem },
        { "__setitem__"_s, nativeSetItem },
        { "__delitem__"_s, nativeDelItem },
        { "__contains__"_s, nativeContains },
        { "__iter__"_s, nativeIter },
        { "__reversed__"_s, dictReversed },
        { "get"_s, dictGet },
        { "setdefault"_s, dictSetDefault },
        { "pop"_s, dictPop },
        { "popitem"_s, dictPopItem },
        { "update"_s, dictUpdateMethod, Kind::Method, 0, "($self, /, *args, **kwargs)"_s },
        { "clear"_s, dictClear },
        { "copy"_s, dictCopy },
        { "keys"_s, dictView, PyNativeFunction::Kind::Method, pack(BuiltinType::DictKeys) },
        { "values"_s, dictView, PyNativeFunction::Kind::Method, pack(BuiltinType::DictValues) },
        { "items"_s, dictView, PyNativeFunction::Kind::Method, pack(BuiltinType::DictItems) },
        { "fromkeys"_s, dictFromKeys, Kind::ClassMethod },
    });
    addComparisons(globalObject, dict);
    addBinaryOperators(globalObject, dict, { BinaryOperator::BitOr }, true, true);
    makeUnhashable(dict);

    initializeDictViews(globalObject);

    for (PyType* set : { realm->typeSet(), realm->typeFrozenSet() }) {
        addMethods(globalObject, set, {
            { "__repr__"_s, nativeRepr },
            { "__len__"_s, nativeLen },
            { "__contains__"_s, nativeContains },
            { "__iter__"_s, nativeIter },
            { "copy"_s, setCopy },
            { "union"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::BitOr, false) },
            { "intersection"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::BitAnd, false) },
            { "difference"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::Sub, false) },
            { "symmetric_difference"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::BitXor, false) },
            { "issubset"_s, setRelation, PyNativeFunction::Kind::Method, pack(ComparisonOperator::LtE) },
            { "issuperset"_s, setRelation, PyNativeFunction::Kind::Method, pack(ComparisonOperator::GtE) },
            { "isdisjoint"_s, setIsDisjoint },
        });
        addComparisons(globalObject, set);
        addBinaryOperators(globalObject, set, { BinaryOperator::BitOr, BinaryOperator::BitAnd, BinaryOperator::Sub, BinaryOperator::BitXor }, true, set == realm->typeSet());
    }
    addMethods(globalObject, realm->typeSet(), {
        { "__new__"_s, setNew, Kind::New },
        { "__init__"_s, setInit, Kind::Method, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "add"_s, setAddMethod },
        { "remove"_s, setRemove, PyNativeFunction::Kind::Method, pack(true) },
        { "discard"_s, setRemove, PyNativeFunction::Kind::Method, pack(false) },
        { "pop"_s, setPop },
        { "clear"_s, setClear },
        { "update"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::BitOr, true) },
        { "intersection_update"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::BitAnd, true) },
        { "difference_update"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::Sub, true) },
        { "symmetric_difference_update"_s, setMethod, PyNativeFunction::Kind::Method, pack(BinaryOperator::BitXor, true) },
    });
    makeUnhashable(realm->typeSet());
    addMethods(globalObject, realm->typeFrozenSet(), {
        { "__new__"_s, frozenSetNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass },
        { "__hash__"_s, nativeHash },
    });

    PyType* slice = realm->typeSlice();
    slice->setInstanceStructure(vm, PySlice::createStructure(vm, globalObject, slice));
    addMethods(globalObject, slice, {
        { "__new__"_s, sliceNew, Kind::New },
        { "__repr__"_s, nativeRepr },
        { "indices"_s, sliceIndices },
        { "__hash__"_s, nativeHash },
    });
    addComparisons(globalObject, slice);
    addMember(globalObject, slice, "start"_s, [] (JSGlobalObject*, JSValue self) { return uncheckedDowncast<PySlice>(self.asCell())->start(); });
    addMember(globalObject, slice, "stop"_s, [] (JSGlobalObject*, JSValue self) { return uncheckedDowncast<PySlice>(self.asCell())->stop(); });
    addMember(globalObject, slice, "step"_s, [] (JSGlobalObject*, JSValue self) { return uncheckedDowncast<PySlice>(self.asCell())->step(); });
}

void initializeIteratorTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    for (BuiltinType builtin : { BuiltinType::ListIterator, BuiltinType::ListReverseIterator, BuiltinType::TupleIterator, BuiltinType::RangeIterator, BuiltinType::LongRangeIterator, BuiltinType::StrAsciiIterator, BuiltinType::StrIterator, BuiltinType::BytesIterator, BuiltinType::ByteArrayIterator, BuiltinType::MemoryIterator,
        BuiltinType::DictKeyIterator, BuiltinType::DictValueIterator, BuiltinType::DictItemIterator, BuiltinType::DictReverseKeyIterator, BuiltinType::DictReverseValueIterator, BuiltinType::DictReverseItemIterator, BuiltinType::SetIterator, BuiltinType::SequenceIterator,
        BuiltinType::CallableIterator, BuiltinType::Enumerate, BuiltinType::Zip, BuiltinType::Map, BuiltinType::Filter, BuiltinType::Reversed, BuiltinType::LineIterator, BuiltinType::PositionsIterator }) {
        PyType* type = realm->type(builtin);
        type->setInstanceStructure(vm, PyIterator::createStructure(vm, globalObject, type));
        addMethods(globalObject, type, {
            { "__iter__"_s, nativeSelf },
            { "__next__"_s, nativeNext },
        });
        addIteratorProtocol(globalObject, type);
    }
    addMethods(globalObject, realm->typeEnumerate(), { { "__new__"_s, enumerateNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClassButNotChecked } });
    addMethods(globalObject, realm->typeZip(), { { "__new__"_s, zipNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass } });
    addMethods(globalObject, realm->typeMap(), { { "__new__"_s, mapNew, Kind::New, 0, "(*iterables, strict=False)"_s, PyNativeFunction::Arguments::AreThoseOfTheClass } });
    addMethods(globalObject, realm->typeFilter(), { { "__new__"_s, filterNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass } });
    addMethods(globalObject, realm->typeReversed(), { { "__new__"_s, reversedNew, Kind::New, 0, { }, PyNativeFunction::Arguments::AreThoseOfTheClass } });
}

} } // namespace JSC::Python
