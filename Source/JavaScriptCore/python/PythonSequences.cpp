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
#include "PythonSequences.h"

#include "JSCInlines.h"
#include "PyObjects.h"

namespace JSC { namespace Python {

// ---- list

JSArray* newList(JSGlobalObject* globalObject, unsigned length)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (length > maxListLength) [[unlikely]] {
        raiseMemoryError(globalObject, scope);
        return nullptr;
    }
    JSArray* list = constructEmptyArray(globalObject, nullptr, length);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // What that leaves are holes, which Python has no notion of.
    for (unsigned i = 0; i < length; ++i)
        list->putDirectIndex(globalObject, i, jsUndefined());
    return list;
}

JSArray* newList(JSGlobalObject* globalObject, const ArgList& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Structure* structure = globalObject->arrayStructureForProfileDuringAllocation(globalObject, nullptr, JSValue());
    RETURN_IF_EXCEPTION(scope, nullptr);
    JSArray* list = values.size() <= maxListLength ? tryConstructArray(globalObject, structure, values) : nullptr;
    if (!list) [[unlikely]]
        raiseMemoryError(globalObject, scope);
    return list;
}

JSArray* newList(JSGlobalObject* globalObject, MarkedArgumentBuffer& values)
{
    if (values.hasOverflowed()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        raiseMemoryError(globalObject, scope);
        return nullptr;
    }
    return newList(globalObject, ArgList(values));
}

void reverseList(JSGlobalObject* globalObject, JSArray* list)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    unsigned length = list->length();
    for (unsigned i = 0; i < length / 2; ++i) {
        JSValue a = listGet(globalObject, list, i);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue b = listGet(globalObject, list, length - 1 - i);
        RETURN_IF_EXCEPTION(scope, void());
        listSet(globalObject, list, i, b);
        RETURN_IF_EXCEPTION(scope, void());
        listSet(globalObject, list, length - 1 - i, a);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

JSValue listGetSlow(JSGlobalObject* globalObject, JSArray* list, unsigned index)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = list->getDirectIndex(globalObject, index);
    RETURN_IF_EXCEPTION(scope, { });
    return value ? value : jsUndefined();
}

void listSet(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    if (list->trySetIndexQuickly(globalObject->vm(), index, value)) [[likely]]
        return;
    list->methodTable()->putByIndex(list, globalObject, index, value, true);
}

void listInitializeAt(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    list->putDirectIndex(globalObject, index, value);
}

void listAppend(JSGlobalObject* globalObject, JSArray* list, JSValue value)
{
    list->push(globalObject, value);
}

bool listExtend(JSGlobalObject* globalObject, JSArray* list, JSValue iterable)
{
    // list += list: what there is to add is settled before any of it is added.
    if (iterable.isCell() && iterable.asCell() == list) {
        VM& vm = globalObject->vm();
        auto scope = DECLARE_THROW_SCOPE(vm);
        unsigned length = list->length();
        for (unsigned i = 0; i < length; ++i) {
            JSValue value = listGet(globalObject, list, i);
            RETURN_IF_EXCEPTION(scope, false);
            list->push(globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
        }
        return true;
    }
    // What is gone through can do anything to the list meanwhile, and each goes on the end of what is then there.
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue source = iterable;
    if (!isPutInListWithoutAsking(globalObject, iterable)) {
        // list_extend_iter_lock_held(): it is asked how many there will be, and CPython makes room for them now.
        source = getIterator(globalObject, iterable);
        RETURN_IF_EXCEPTION(scope, false);
        auto hint = lengthHint(globalObject, iterable, 8);
        RETURN_IF_EXCEPTION(scope, false);
        int64_t length = list->length();
        if (length <= std::numeric_limits<int64_t>::max() - *hint && length + *hint > static_cast<int64_t>(maxListLength)) {
            raiseMemoryError(globalObject, scope);
            return false;
        }
    }
    RELEASE_AND_RETURN(scope, forEach(globalObject, source, [&] (JSValue value) {
        if (list->length() >= maxListLength) [[unlikely]] {
            raiseMemoryError(globalObject, scope);
            return false;
        }
        list->push(globalObject, value);
        return true;
    }));
}

JSArray* listFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    MarkedArgumentBuffer values;
    collectAsList(globalObject, iterable, values);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, newList(globalObject, values));
}

PyTuple* tupleFromIterable(JSGlobalObject* globalObject, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isTuple(iterable) && typeOf(globalObject, iterable) == globalObject->pyRealm()->typeTuple())
        return uncheckedDowncast<PyTuple>(iterable.asCell());
    MarkedArgumentBuffer values;
    collect(globalObject, iterable, values);
    RETURN_IF_EXCEPTION(scope, nullptr);
    RELEASE_AND_RETURN(scope, PyTuple::createFromArguments(globalObject, values));
}

void listReplaceRange(JSGlobalObject* globalObject, JSArray* list, unsigned start, unsigned count, const ArgList& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = list->length();
    start = std::min(start, length);
    count = std::min(count, length - start);
    unsigned added = values.size();
    if (added > maxListLength - (length - count)) {
        raiseMemoryError(globalObject, scope);
        return;
    }
    // All at once, if the elements are side by side, which they are unless JavaScript has been at it.
    if (list->tryResizeRangeKeepingIndexingType(vm, start, count, added)) {
        for (unsigned i = 0; i < added; ++i) {
            listSet(globalObject, list, start + i, values.at(i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        return;
    }
    unsigned tail = length - start - count;
    auto move = [&] (unsigned from, unsigned to) {
        JSValue value = listGet(globalObject, list, from);
        RETURN_IF_EXCEPTION(scope, void());
        listSet(globalObject, list, to, value);
    };

    if (added > count) {
        // Make room, from the end.
        unsigned growth = added - count;
        for (unsigned i = 0; i < growth; ++i) {
            list->push(globalObject, jsUndefined());
            RETURN_IF_EXCEPTION(scope, void());
        }
        for (unsigned i = tail; i--;) {
            move(start + count + i, start + added + i);
            RETURN_IF_EXCEPTION(scope, void());
        }
    } else if (added < count) {
        for (unsigned i = 0; i < tail; ++i) {
            move(start + count + i, start + added + i);
            RETURN_IF_EXCEPTION(scope, void());
        }
        list->setLength(globalObject, length - (count - added), true);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned i = 0; i < added; ++i) {
        listSet(globalObject, list, start + i, values.at(i));
        RETURN_IF_EXCEPTION(scope, void());
    }
}

void listInsert(JSGlobalObject* globalObject, JSArray* list, unsigned index, JSValue value)
{
    MarkedArgumentBuffer values;
    values.append(value);
    listReplaceRange(globalObject, list, index, 0, values);
}

void listRemoveRange(JSGlobalObject* globalObject, JSArray* list, unsigned start, unsigned count)
{
    listReplaceRange(globalObject, list, start, count, ArgList());
}

JSArray* listRepeat(JSGlobalObject* globalObject, JSArray* list, int64_t count)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    count = std::max<int64_t>(count, 0);
    unsigned length = list->length();
    // By dividing, since the product may be more than can be counted.
    if (length && count > static_cast<int64_t>(maxListLength / length)) {
        raiseMemoryError(globalObject, scope);
        return nullptr;
    }
    uint64_t total = static_cast<uint64_t>(length) * count;
    JSArray* result = newList(globalObject, total);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (unsigned i = 0; i < total; ++i) {
        JSValue value = listGet(globalObject, list, i % length);
        RETURN_IF_EXCEPTION(scope, nullptr);
        listInitializeAt(globalObject, result, i, value);
    }
    return result;
}

// ---- set

PySet* setFromIterable(JSGlobalObject* globalObject, Structure* structure, JSValue iterable)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PySet* set = PySet::create(vm, structure);
    if (!iterable)
        return set;
    forEach(globalObject, iterable, [&] (JSValue value) {
        return set->add(globalObject, value);
    });
    RETURN_IF_EXCEPTION(scope, nullptr);
    return set;
}

// set_contains(), set_remove() and set_discard(): a set cannot be in a set, but it can be asked whether a frozenset that has the same in it is.
JSValue keyToLookForInSet(JSGlobalObject* globalObject, JSValue key)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isSet(key))
        return key;
    hash(globalObject, key);
    if (!scope.exception()) [[likely]]
        return key;
    if (!catchException(globalObject, BuiltinType::TypeError))
        return { };
    PySet* frozen = PySet::create(vm, globalObject->pyRealm()->structureFor(BuiltinType::FrozenSet));
    frozen->copyFrom(vm, globalObject, *uncheckedDowncast<PySet>(key.asCell()));
    return frozen;
}

// ---- What sets do with one another. This is CPython's Objects/setobject.c, function for function. Each takes as long as the smaller of the two is, or as what it is given is, and not as long as the set is: `s |= {x}`
// in a loop is as common as `l.append(x)`. Which of two keys that are equal is kept is as it is there too, and a key that comes out of a set is not asked for its hash again.

static PySet* asSet(JSValue value) { return uncheckedDowncast<PySet>(value.asCell()); }
// PyDict_CheckExact(). Its keys have their hashes with them as those of a set do. That is not so of what is another way of getting at the properties of an object, whose keys are strings, of which nothing is asked.
static PyDict* tryExactDict(JSGlobalObject* globalObject, JSValue value)
{
    if (!isDict(value) || typeOf(globalObject, value) != globalObject->pyRealm()->typeDict() || asDict(value)->backing())
        return nullptr;
    return asDict(value);
}

// make_new_set_basetype(): a set or a frozenset, as it is or is derived from, with nothing in it.
static PySet* newSetOfBaseType(JSGlobalObject* globalObject, PySet* so)
{
    PyRealm* realm = globalObject->pyRealm();
    bool isFrozen = typeOf(globalObject, so)->isSubtypeOf(realm->typeFrozenSet());
    return PySet::create(globalObject->vm(), realm->structureFor(isFrozen ? BuiltinType::FrozenSet : BuiltinType::Set));
}

PySet* setCopy(JSGlobalObject* globalObject, PySet* so)
{
    PySet* result = newSetOfBaseType(globalObject, so);
    result->copyFrom(globalObject->vm(), globalObject, *so);
    return result;
}

// Calls the function with each key of a set or a dict and its hash, until it returns false. What it calls can change the set, and then it goes on with what is there.
template<typename Function>
static void forEachEntry(PyHashTable* set, const Function& function)
{
    for (unsigned entry = set->firstEntry(); entry < set->entryCount(); ++entry) {
        if (JSValue key = set->keyAt(entry)) {
            if (!function(key, set->hashAt(entry)))
                return;
        }
    }
}

// set_discard_entry(). Whether it was there. It may have raised.
static bool discardEntry(JSGlobalObject* globalObject, PySet* so, JSValue key, uint32_t hash)
{
    int entry = so->find(globalObject, key, hash);
    if (entry < 0)
        return false;
    so->removeEntry(globalObject->vm(), entry);
    return true;
}

// set_update_internal()
bool setUpdate(JSGlobalObject* globalObject, PySet* so, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (isSet(other)) {
        if (asSet(other) == so)
            return true;
        forEachEntry(asSet(other), [&] (JSValue key, uint32_t hash) {
            return so->addWithHash(globalObject, key, hash);
        });
    } else if (PyDict* dict = tryExactDict(globalObject, other)) {
        forEachEntry(dict, [&] (JSValue key, uint32_t hash) {
            return so->addWithHash(globalObject, key, hash);
        });
    } else {
        forEach(globalObject, other, [&] (JSValue key) {
            return so->add(globalObject, key);
        });
    }
    return !scope.exception();
}

// set_intersection()
PySet* setIntersection(JSGlobalObject* globalObject, PySet* so, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (other == JSValue(so))
        RELEASE_AND_RETURN(scope, setCopy(globalObject, so));

    PySet* result = newSetOfBaseType(globalObject, so);
    if (isSet(other)) {
        // The smaller is gone through, and it is its keys that are kept.
        PySet* goneThrough = asSet(other);
        if (goneThrough->size() > so->size())
            std::swap(goneThrough, so);
        forEachEntry(goneThrough, [&] (JSValue key, uint32_t hash) {
            int entry = so->find(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, false);
            return entry < 0 || result->addWithHash(globalObject, key, hash);
        });
    } else {
        forEach(globalObject, other, [&] (JSValue key) {
            // PyObject_Hash(), so what cannot be hashed is not said to have been meant for a set.
            uint32_t hash = PyHashTable::foldHash(Python::hash(globalObject, key));
            RETURN_IF_EXCEPTION(scope, false);
            int entry = so->find(globalObject, key, hash);
            RETURN_IF_EXCEPTION(scope, false);
            if (entry < 0)
                return true;
            if (!result->addWithHash(globalObject, key, hash))
                return false;
            // There can be no more.
            return result->size() < so->size();
        });
    }
    RETURN_IF_EXCEPTION(scope, nullptr);
    return result;
}

// set_difference_update_internal()
bool setDifferenceUpdate(JSGlobalObject* globalObject, PySet* so, JSValue other)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (other == JSValue(so)) {
        so->clear(vm);
        return true;
    }
    if (isSet(other)) {
        // If the other is more than eight times as large, what the two have in common is found first, by going through this one.
        PySet* toRemove = asSet(other);
        if ((toRemove->size() >> 3) > so->size()) {
            toRemove = setIntersection(globalObject, so, other);
            RETURN_IF_EXCEPTION(scope, false);
        }
        forEachEntry(toRemove, [&] (JSValue key, uint32_t hash) {
            discardEntry(globalObject, so, key, hash);
            return !scope.exception();
        });
    } else {
        forEach(globalObject, other, [&] (JSValue key) {
            // set_discard_key(), which is not what set.discard() is: a set is not looked for as a frozenset.
            so->remove(globalObject, key);
            return !scope.exception();
        });
    }
    RETURN_IF_EXCEPTION(scope, false);
    so->tidyAfterRemoving(vm, globalObject);
    return true;
}

// set_difference()
PySet* setDifference(JSGlobalObject* globalObject, PySet* so, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    auto copyAndRemove = [&] () -> PySet* {
        PySet* result = setCopy(globalObject, so);
        RETURN_IF_EXCEPTION(scope, nullptr);
        setDifferenceUpdate(globalObject, result, other);
        RETURN_IF_EXCEPTION(scope, nullptr);
        return result;
    };
    PyHashTable* table = isSet(other) ? static_cast<PyHashTable*>(asSet(other)) : tryExactDict(globalObject, other);
    // If this one is much the larger, it is quicker to copy it and go through the other.
    if (!table || (so->size() >> 2) > table->size())
        return copyAndRemove();

    PySet* result = newSetOfBaseType(globalObject, so);
    forEachEntry(so, [&] (JSValue key, uint32_t hash) {
        bool isInOther = table->find(globalObject, key, hash) >= 0;
        RETURN_IF_EXCEPTION(scope, false);
        return isInOther || result->addWithHash(globalObject, key, hash);
    });
    RETURN_IF_EXCEPTION(scope, nullptr);
    return result;
}

// set_symmetric_difference_update_set() and set_symmetric_difference_update_dict()
static bool symmetricDifferenceUpdateFromSet(JSGlobalObject* globalObject, PySet* so, PyHashTable* other)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    forEachEntry(other, [&] (JSValue key, uint32_t hash) {
        bool wasThere = discardEntry(globalObject, so, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        return wasThere || so->addWithHash(globalObject, key, hash);
    });
    return !scope.exception();
}

// set_symmetric_difference_update()
bool setSymmetricDifferenceUpdate(JSGlobalObject* globalObject, PySet* so, JSValue other)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (other == JSValue(so)) {
        so->clear(vm);
        return true;
    }
    if (isSet(other))
        RELEASE_AND_RETURN(scope, symmetricDifferenceUpdateFromSet(globalObject, so, asSet(other)));
    if (PyDict* dict = tryExactDict(globalObject, other))
        RELEASE_AND_RETURN(scope, symmetricDifferenceUpdateFromSet(globalObject, so, dict));
    // Each once, however often it is given.
    PySet* otherSet = newSetOfBaseType(globalObject, so);
    setUpdate(globalObject, otherSet, other);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, symmetricDifferenceUpdateFromSet(globalObject, so, otherSet));
}

// set_symmetric_difference()
PySet* setSymmetricDifference(JSGlobalObject* globalObject, PySet* so, JSValue other)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    // There it is a copy of the other, from which what is in this one is taken, and to which what is not is added. What is looked for in what is the same here. But a set here is in the order in which it was added to,
    // so what is only in this one is put first.
    PySet* onlyInOther = newSetOfBaseType(globalObject, so);
    setUpdate(globalObject, onlyInOther, other);
    RETURN_IF_EXCEPTION(scope, nullptr);
    PySet* result = newSetOfBaseType(globalObject, so);
    forEachEntry(so, [&] (JSValue key, uint32_t hash) {
        bool wasThere = discardEntry(globalObject, onlyInOther, key, hash);
        RETURN_IF_EXCEPTION(scope, false);
        return wasThere || result->addWithHash(globalObject, key, hash);
    });
    RETURN_IF_EXCEPTION(scope, nullptr);
    setUpdate(globalObject, result, onlyInOther);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return result;
}

// set_or(), set_ior() and the rest.
JSValue setOperation(JSGlobalObject* globalObject, BinaryOperator op, bool inPlace, PySet* left, PySet* right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    switch (op) {
    case BinaryOperator::BitOr: {
        PySet* result = inPlace ? left : setCopy(globalObject, left);
        RETURN_IF_EXCEPTION(scope, { });
        if (left != right)
            setUpdate(globalObject, result, right);
        RETURN_IF_EXCEPTION(scope, { });
        return result;
    }
    case BinaryOperator::BitAnd: {
        PySet* result = setIntersection(globalObject, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        if (!inPlace)
            return result;
        left->takeFrom(vm, *result);
        return left;
    }
    case BinaryOperator::Sub:
        if (!inPlace)
            RELEASE_AND_RETURN(scope, setDifference(globalObject, left, right));
        setDifferenceUpdate(globalObject, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        return left;
    case BinaryOperator::BitXor:
        if (!inPlace)
            RELEASE_AND_RETURN(scope, setSymmetricDifference(globalObject, left, right));
        setSymmetricDifferenceUpdate(globalObject, left, right);
        RETURN_IF_EXCEPTION(scope, { });
        return left;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool isSubset(JSGlobalObject* globalObject, PySet* left, PySet* right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (left->size() > right->size())
        return false;
    for (unsigned entry = 0; entry < left->entryCount(); ++entry) {
        JSValue key = left->keyAt(entry);
        if (!key)
            continue;
        int found = right->find(globalObject, key, left->hashAt(entry));
        RETURN_IF_EXCEPTION(scope, false);
        if (found < 0)
            return false;
    }
    return true;
}

JSValue setCompare(JSGlobalObject* globalObject, ComparisonOperator op, PySet* left, PySet* right)
{
    switch (op) {
    case ComparisonOperator::Eq:
        return jsBoolean(left->size() == right->size() && isSubset(globalObject, left, right));
    case ComparisonOperator::NotEq:
        return jsBoolean(!(left->size() == right->size() && isSubset(globalObject, left, right)));
    case ComparisonOperator::LtE:
        return jsBoolean(isSubset(globalObject, left, right));
    case ComparisonOperator::Lt:
        return jsBoolean(left->size() < right->size() && isSubset(globalObject, left, right));
    case ComparisonOperator::GtE:
        return jsBoolean(isSubset(globalObject, right, left));
    case ComparisonOperator::Gt:
        return jsBoolean(right->size() < left->size() && isSubset(globalObject, right, left));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } // namespace JSC::Python
