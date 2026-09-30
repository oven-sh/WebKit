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
        raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, method, "' for 'list' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
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
    auto index = toSsize(globalObject, args[1]);
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
    int64_t given = -1;
    if (args.size() > 1) {
        auto index = toSsize(globalObject, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        given = *index;
    }
    // How long it is is asked after what was given has been asked what it is, which can change it.
    int64_t length = self->length();
    if (!length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop from empty list"_s));
    int64_t at = given < 0 ? given + length : given;
    if (at < 0 || at >= length)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "pop index out of range"_s));
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
    reverseList(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// index(value, start=0, stop=the end), of a list or a tuple. How long a list is can be changed by what its elements are compared by, so it is asked every time.
template<typename Length, typename Get>
static EncodedJSValue sequenceIndex(JSGlobalObject* globalObject, const NativeArguments& args, const Length& currentLength, ASCIILiteral typeName, const Get& get)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t length = currentLength();
    auto resolve = [&] (JSValue value, int64_t whenAbsent) -> int64_t {
        if (!value)
            return whenAbsent;
        auto index = toSliceIndex(globalObject, value, false);
        RETURN_IF_EXCEPTION(scope, 0);
        return *index < 0 ? std::max<int64_t>(*index + length, 0) : *index;
    };
    int64_t start = resolve(args.at(2), 0);
    RETURN_IF_EXCEPTION(scope, { });
    // Not the length as it is now: what is added to a list meanwhile is looked at as well.
    int64_t stop = resolve(args.at(3), std::numeric_limits<int64_t>::max());
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = start; i < stop && i < static_cast<int64_t>(currentLength()); ++i) {
        JSValue item = get(i);
        RETURN_IF_EXCEPTION(scope, { });
        bool same = isEqual(globalObject, item, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        if (same)
            return JSValue::encode(jsNumber(static_cast<int32_t>(i)));
    }
    return JSValue::encode(raiseValueError(globalObject, scope, concatenate(typeName, ".index(x): x not in "_s, typeName)));
}

template<typename Length, typename Get>
static EncodedJSValue sequenceCount(JSGlobalObject* globalObject, const NativeArguments& args, const Length& currentLength, const Get& get)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int32_t count = 0;
    for (unsigned i = 0; i < currentLength(); ++i) {
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
    RELEASE_AND_RETURN(scope, sequenceIndex(globalObject, args, [&] { return self->length(); }, "list"_s, [&] (unsigned i) { return listGet(globalObject, self, i); }));
}

PYTHON_NATIVE(listCount)
{
    LIST_PROLOGUE("count");
    RELEASE_AND_RETURN(scope, sequenceCount(globalObject, args, [&] { return self->length(); }, [&] (unsigned i) { return listGet(globalObject, self, i); }));
}

PYTHON_NATIVE(listSort)
{
    LIST_PROLOGUE("sort");
    JSValue reverseValue = args.keyword(globalObject, "reverse"_s);
    bool reverse = reverseValue && isTrue(globalObject, reverseValue);
    RETURN_IF_EXCEPTION(scope, { });
    sortList(globalObject, self, args.keyword(globalObject, "key"_s), reverse);
    RETURN_IF_EXCEPTION(scope, { });
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
    RELEASE_AND_RETURN(scope, sequenceIndex(globalObject, args, [&] { return self->length(); }, "tuple"_s, [&] (unsigned i) { return self->at(i); }));
}

PYTHON_NATIVE(tupleCount)
{
    NATIVE_PROLOGUE();
    auto* self = uncheckedDowncast<PyTuple>(args.at(0).asCell());
    RELEASE_AND_RETURN(scope, sequenceCount(globalObject, args, [&] { return self->length(); }, [&] (unsigned i) { return self->at(i); }));
}

// ---- dict

static PyDict* selfDict(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral method)
{
    JSValue self = args.at(0);
    if (!self || !isDict(self)) {
        raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, method, "' for 'dict' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
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

bool isGoneThroughAsDict(JSGlobalObject* globalObject, JSValue value)
{
    if (!isDict(value))
        return false;
    PyRealm* realm = globalObject->pyRealm();
    if (value.asCell()->structure() == realm->structureFor(BuiltinType::Dict)) [[likely]]
        return true;
    VM& vm = globalObject->vm();
    return typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_iter) == realm->typeDict()->lookup(vm, vm.pythonNames().dunder_iter);
}

// dict_update_arg(): from a mapping, or from what gives pairs.
void updateDictFrom(JSGlobalObject* globalObject, PyDict* dict, JSValue source)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    {
        if (isGoneThroughAsDict(globalObject, source)) {
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
                        if (catchException(globalObject, BuiltinType::TypeError)) {
                            raiseTypeError(globalObject, scope, "object is not iterable"_s);
                            addNoteToRaised(globalObject, concatenate("Cannot convert dictionary update sequence element #"_s, position, " to a sequence"_s));
                        }
                        return false;
                    }
                    if (parts.size() != 2) {
                        raiseValueError(globalObject, scope, concatenate("dictionary update sequence element #"_s, position, " has length "_s, parts.size(), "; 2 is required"_s));
                        return false;
                    }
                    ++position;
                    return dict->set(globalObject, parts.at(0), parts.at(1));
                });
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
    }
}

// What dict(source, **keywords) and dict.update(source, **keywords) do.
static void updateDict(JSGlobalObject* globalObject, PyDict* dict, const NativeArguments& args, ASCIILiteral method)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.size() > 2) {
        raiseTypeError(globalObject, scope, concatenate(method, " expected at most 1 argument, got "_s, args.size() - 1));
        return;
    }
    if (args.size() == 2) {
        updateDictFrom(globalObject, dict, args[1]);
        RETURN_IF_EXCEPTION(scope, void());
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
    // Nothing is in a dict that has nothing in it, and the key is not so much as asked for its hash.
    if (!self->size()) {
        if (args.size() > 2)
            return JSValue::encode(args[2]);
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
    }
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
        raiseTypeError(globalObject, scope, concatenate("descriptor '"_s, method, "' for 'set' objects doesn't apply to a '"_s, self ? typeName(globalObject, self) : "NULL"_str, "' object"_s));
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
    // A frozenset of a frozenset is that one.
    if (asType(args[0]) == realm->typeFrozenSet() && args.size() > 1 && isExactly(globalObject, args[1], BuiltinType::FrozenSet))
        return JSValue::encode(args[1]);
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
    JSValue key = keyToLookForInSet(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue removed = self->remove(globalObject, key);
    RETURN_IF_EXCEPTION(scope, { });
    if (!removed && raises)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::KeyError, args[1]));
    RETURN_NONE();
}

PYTHON_NATIVE(setPop)
{
    SET_PROLOGUE("pop");
    if (self->size()) {
        JSValue key = self->keyAt(self->firstEntry());
        self->removeEntry(vm, self->firstEntry());
        return JSValue::encode(key);
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
    // A frozenset is as good as a copy of it. And of an instance of a class derived from either, the copy is of the class that it is derived from.
    if (isExactly(globalObject, self, BuiltinType::FrozenSet))
        return JSValue::encode(self);
    bool isFrozen = typeOf(globalObject, self)->isSubtypeOf(realm->typeFrozenSet());
    PySet* copy = PySet::create(vm, realm->structureFor(isFrozen ? BuiltinType::FrozenSet : BuiltinType::Set));
    copy->copyFrom(vm, globalObject, *self);
    return JSValue::encode(copy);
}

// union(*others) and the rest, which take any iterables, and update(*others) and the rest, which change the set.
PYTHON_NATIVE(setMethod)
{
    auto op = unpack<BinaryOperator>(callFrame, 0);
    auto inPlace = unpack<bool>(callFrame, 1);
    SET_PROLOGUE("union");
    switch (op) {
    case BinaryOperator::BitOr: {
        // set_union() and set_update()
        PySet* result = inPlace ? self : setCopy(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        for (unsigned i = 1; i < args.size(); ++i) {
            if (args[i] == JSValue(self))
                continue;
            setUpdate(globalObject, result, args[i]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (inPlace)
            RETURN_NONE();
        return JSValue::encode(result);
    }
    case BinaryOperator::BitAnd: {
        // set_intersection_multi() and set_intersection_update_multi(), which does nothing if it cannot do it all.
        PySet* result = self;
        for (unsigned i = 1; i < args.size(); ++i) {
            result = setIntersection(globalObject, result, args[i]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (result == self) {
            result = setCopy(globalObject, self);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!inPlace)
            return JSValue::encode(result);
        self->takeFrom(vm, *result);
        RETURN_NONE();
    }
    case BinaryOperator::Sub: {
        // set_difference_multi() and set_difference_update()
        PySet* result = self;
        unsigned i = 1;
        if (!inPlace) {
            result = args.size() > 1 ? setDifference(globalObject, self, args[i++]) : setCopy(globalObject, self);
            RETURN_IF_EXCEPTION(scope, { });
        }
        for (; i < args.size(); ++i) {
            setDifferenceUpdate(globalObject, result, args[i]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (inPlace)
            RETURN_NONE();
        return JSValue::encode(result);
    }
    case BinaryOperator::BitXor:
        // Which take one and no more.
        if (!inPlace)
            RELEASE_AND_RETURN(scope, JSValue::encode(setSymmetricDifference(globalObject, self, args[1])));
        setSymmetricDifferenceUpdate(globalObject, self, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        RETURN_NONE();
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

PYTHON_NATIVE(setRelation)
{
    auto op = unpack<ComparisonOperator>(callFrame, 0);
    SET_PROLOGUE("issubset");
    if (isSet(args[1]))
        RELEASE_AND_RETURN(scope, JSValue::encode(setCompare(globalObject, op, self, uncheckedDowncast<PySet>(args[1].asCell()))));
    if (op == ComparisonOperator::LtE) {
        // set_issubset(): whether what the two have in common is all of it.
        PySet* common = setIntersection(globalObject, self, args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        return JSValue::encode(jsBoolean(common->size() == self->size()));
    }
    // set_issuperset(): until there is one that it does not have.
    bool hasAll = true;
    forEach(globalObject, args[1], [&] (JSValue value) {
        int entry = self->find(globalObject, value);
        hasAll = entry >= 0;
        return hasAll;
    });
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsBoolean(hasAll));
}

PYTHON_NATIVE(setIsDisjoint)
{
    SET_PROLOGUE("isdisjoint");
    if (args[1] == JSValue(self))
        return JSValue::encode(jsBoolean(!self->size()));
    bool isDisjoint = true;
    // The smaller is gone through, if it is a set and not of a class derived from one.
    if (PyType* type = typeOf(globalObject, args[1]); type == realm->typeSet() || type == realm->typeFrozenSet()) {
        PySet* goneThrough = uncheckedDowncast<PySet>(args[1].asCell());
        PySet* lookedIn = self;
        if (goneThrough->size() > lookedIn->size())
            std::swap(goneThrough, lookedIn);
        for (unsigned entry = goneThrough->firstEntry(); isDisjoint && entry < goneThrough->entryCount(); ++entry) {
            JSValue key = goneThrough->keyAt(entry);
            if (!key)
                continue;
            isDisjoint = lookedIn->find(globalObject, key, goneThrough->hashAt(entry)) < 0;
            RETURN_IF_EXCEPTION(scope, { });
        }
        return JSValue::encode(jsBoolean(isDisjoint));
    }
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
    // It puts things in its own way: enumerate_vectorcall() of CPython's Objects/enumobject.c. A class derived from it is not called that way, and puts them as anything else does.
    unsigned given = args.size() - 1 + args.keywordCount();
    bool isExact = asType(args[0]) == realm->typeEnumerate();
    if (!isExact && !checkArgumentsSlow(globalObject, callFrame))
        return { };
    if (isExact && (!given || given > 2)) {
        if (args.size() == 1)
            return JSValue::encode(raiseTypeError(globalObject, scope, "enumerate() missing required argument 'iterable'"_s));
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate("enumerate() takes at most 2 arguments ("_s, given, " given)"_s)));
    }
    for (unsigned i = 0; isExact && i < args.keywordCount(); ++i) {
        // Each is to be the name of what has not been given yet.
        String keyword = args.keywordName(i)->value(globalObject);
        bool isExpected = given == 1 ? keyword == "iterable"_s : args.keywordCount() == 1 ? keyword == "start"_s : keyword == "iterable"_s || keyword == "start"_s;
        if (!isExpected || (i && keyword == String(args.keywordName(0)->value(globalObject))))
            return JSValue::encode(raiseTypeError(globalObject, scope, concatenate('\'', keyword, "' is an invalid keyword argument for enumerate()"_s)));
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

static PyTuple* iteratorsOf(JSGlobalObject* globalObject, const NativeArguments& args, unsigned first)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyTuple* iterators = PyTuple::create(globalObject, args.size() - first);
    for (unsigned i = first; i < args.size(); ++i) {
        JSValue iterator = getIterator(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, nullptr);
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
    PyTuple* iterators = iteratorsOf(globalObject, args, 1);
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
    PyTuple* iterators = iteratorsOf(globalObject, args, 2);
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
    // That it has no __len__() is for len() to say.
    if (method || !isSequence(globalObject, args[1]))
        return JSValue::encode(raiseTypeError(globalObject, scope, concatenate('\'', type->nameString(globalObject), "' object is not reversible"_s)));
    int64_t size = length(globalObject, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyIterator::create(globalObject, asType(args[0])->instanceStructure(), PyIterator::Kind::Reversed, args[1], JSValue(), size - 1));
}

// ---- Setting them up

// list_vectorcall()
static JSValue listVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (arguments.size() > 1)
        return { };
    JSArray* list = newList(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (arguments.size()) {
        listExtend(globalObject, list, arguments.at(0));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

// tuple_vectorcall()
static JSValue tupleVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    if (!arguments.size())
        return globalObject->pyRealm()->emptyTuple();
    if (arguments.size() == 1)
        return tupleFromIterable(globalObject, arguments.at(0));
    return { };
}

// dict_vectorcall(), with nothing to put in it.
static JSValue dictVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    if (arguments.size())
        return { };
    return PyDict::create(globalObject->vm(), globalObject->pyRealm()->typeDict()->instanceStructure());
}

// set_vectorcall()
static JSValue setVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    if (arguments.size() > 1)
        return { };
    return setFromIterable(globalObject, globalObject->pyRealm()->typeSet()->instanceStructure(), arguments.size() ? arguments.at(0) : JSValue());
}

// frozenset_vectorcall()
static JSValue frozenSetVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    if (arguments.size() > 1)
        return { };
    // A frozenset of a frozenset is that one.
    if (arguments.size() && isExactly(globalObject, arguments.at(0), BuiltinType::FrozenSet))
        return arguments.at(0);
    return setFromIterable(globalObject, globalObject->pyRealm()->typeFrozenSet()->instanceStructure(), arguments.size() ? arguments.at(0) : JSValue());
}

// enumerate_vectorcall(), from the beginning.
static JSValue enumerateVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (arguments.size() != 1)
        return { };
    JSValue iterator = getIterator(globalObject, arguments.at(0));
    RETURN_IF_EXCEPTION(scope, { });
    return PyIterator::create(globalObject, globalObject->pyRealm()->typeEnumerate()->instanceStructure(), PyIterator::Kind::Enumerate, iterator, JSValue(), 0);
}

// map_vectorcall(), of one thing.
static JSValue mapVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (arguments.size() != 2)
        return { };
    JSValue iterator = getIterator(globalObject, arguments.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    PyTuple* iterators = PyTuple::create(globalObject, 1);
    iterators->initializeAt(vm, 0, iterator);
    return PyIterator::create(globalObject, globalObject->pyRealm()->type(BuiltinType::Map)->instanceStructure(), PyIterator::Kind::Map, arguments.at(0), iterators, false);
}

// filter_vectorcall()
static JSValue filterVectorcall(JSGlobalObject* globalObject, const ArgList& arguments)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (arguments.size() != 2)
        return { };
    JSValue iterator = getIterator(globalObject, arguments.at(1));
    RETURN_IF_EXCEPTION(scope, { });
    return PyIterator::create(globalObject, globalObject->pyRealm()->type(BuiltinType::Filter)->instanceStructure(), PyIterator::Kind::Filter, arguments.at(0), iterator);
}

void initializeContainerTypes(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto& names = vm.pythonNames();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    realm->typeList()->setVectorcall(listVectorcall);
    realm->typeTuple()->setVectorcall(tupleVectorcall);
    realm->typeDict()->setVectorcall(dictVectorcall);
    realm->typeSet()->setVectorcall(setVectorcall);
    realm->typeFrozenSet()->setVectorcall(frozenSetVectorcall);
    realm->typeEnumerate()->setVectorcall(enumerateVectorcall);
    realm->type(BuiltinType::Map)->setVectorcall(mapVectorcall);
    realm->type(BuiltinType::Filter)->setVectorcall(filterVectorcall);
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
        BuiltinType::CallableIterator, BuiltinType::Enumerate, BuiltinType::Zip, BuiltinType::Map, BuiltinType::Filter, BuiltinType::Reversed, BuiltinType::LineIterator, BuiltinType::PositionsIterator, BuiltinType::ContextKeys, BuiltinType::ContextValues, BuiltinType::ContextItems }) {
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
