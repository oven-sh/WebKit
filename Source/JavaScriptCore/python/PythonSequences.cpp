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
    PyTuple* tuple = PyTuple::tryCreate(globalObject, values.size());
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (unsigned i = 0; i < values.size(); ++i)
        tuple->initializeAt(vm, i, values.at(i));
    return tuple;
}

void listReplaceRange(JSGlobalObject* globalObject, JSArray* list, unsigned start, unsigned count, const ArgList& values)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned length = list->length();
    start = std::min(start, length);
    count = std::min(count, length - start);
    unsigned added = values.size();
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

JSValue setOperation(JSGlobalObject* globalObject, BinaryOperator op, bool inPlace, PySet* left, PySet* right)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // What comes of it is a set or a frozenset, as the left operand is or is derived from.
    PyRealm* realm = globalObject->pyRealm();
    bool isFrozen = typeOf(globalObject, left)->isSubtypeOf(realm->typeFrozenSet());
    PySet* result = PySet::create(vm, realm->structureFor(isFrozen ? BuiltinType::FrozenSet : BuiltinType::Set));
    auto addAll = [&] (PySet* from, PySet* unlessIn, PySet* onlyIfIn) -> bool {
        for (unsigned entry = 0; entry < from->entryCount(); ++entry) {
            JSValue key = from->keyAt(entry);
            if (!key)
                continue;
            if (unlessIn) {
                int found = unlessIn->find(globalObject, key);
                RETURN_IF_EXCEPTION(scope, false);
                if (found >= 0)
                    continue;
            }
            if (onlyIfIn) {
                int found = onlyIfIn->find(globalObject, key);
                RETURN_IF_EXCEPTION(scope, false);
                if (found < 0)
                    continue;
            }
            result->add(globalObject, key);
            RETURN_IF_EXCEPTION(scope, false);
        }
        return true;
    };

    switch (op) {
    case BinaryOperator::BitOr:
        addAll(left, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
        addAll(right, nullptr, nullptr);
        break;
    case BinaryOperator::BitAnd:
        addAll(left, nullptr, right);
        break;
    case BinaryOperator::Sub:
        addAll(left, right, nullptr);
        break;
    case BinaryOperator::BitXor:
        addAll(left, right, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
        addAll(right, left, nullptr);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    RETURN_IF_EXCEPTION(scope, { });
    if (!inPlace)
        return result;
    left->clear(vm);
    left->copyFrom(vm, globalObject, *result);
    return left;
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
        int found = right->find(globalObject, key);
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
