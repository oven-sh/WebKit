/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyDict.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PyStr.h"
#include "PyTuple.h"

namespace JSC {
namespace Python {

// The module _collections: deque, defaultdict, and two helpers of collections.
// (Modules/_collectionsmodule.c in CPython is the reference.)

// ---- deque ----
//
// The items are in a ring: a tuple nothing else sees, whose length is a power of two, from `head` on and around.

enum DequeField : unsigned {
    DequeBuffer, // A tuple.
    DequeHead, // Where in the buffer the leftmost item is.
    DequeSize,
    DequeMaxLen, // None, or an int.
    DequeFieldCount,
};
// PyRecord::number changes whenever items are added, removed or rotated: that is how iterators notice.

static constexpr unsigned dequeMinCapacity = 8;
static constexpr unsigned dequeMaxCapacity = 1u << 30;

extern const TypeDef dequeTypeDef;
extern const TypeDef dequeIteratorTypeDef;
extern const TypeDef dequeReverseIteratorTypeDef;

static PyType* dequeType(JSGlobalObject* globalObject) { return runtime(globalObject).typeFor(dequeTypeDef); }

static ALWAYS_INLINE PyRecord* dequeOf(JSValue value) { return PyRecord::from(value); }
static ALWAYS_INLINE PyTuple* dequeBuffer(PyRecord* deque) { return uncheckedDowncast<PyTuple>(deque->get(DequeBuffer).asCell()); }
static ALWAYS_INLINE unsigned dequeHead(PyRecord* deque) { return deque->get(DequeHead).asInt32(); }
static ALWAYS_INLINE unsigned dequeSize(PyRecord* deque) { return deque->get(DequeSize).asInt32(); }
static ALWAYS_INLINE void dequeSetHead(VM& vm, PyRecord* deque, unsigned head) { deque->set(vm, DequeHead, jsNumber(static_cast<int32_t>(head))); }
static ALWAYS_INLINE void dequeSetSize(VM& vm, PyRecord* deque, unsigned size) { deque->set(vm, DequeSize, jsNumber(static_cast<int32_t>(size))); }

// -1: none.
static ALWAYS_INLINE int64_t dequeMaxLen(PyRecord* deque)
{
    JSValue value = deque->get(DequeMaxLen);
    if (value.isInt32())
        return value.asInt32();
    if (isNone(value))
        return -1;
    return tryInt64(value).value_or(std::numeric_limits<int64_t>::max());
}

static ALWAYS_INLINE JSValue dequeAt(PyRecord* deque, unsigned index)
{
    PyTuple* buffer = dequeBuffer(deque);
    return buffer->at((dequeHead(deque) + index) & (buffer->length() - 1));
}

static ALWAYS_INLINE void dequePut(VM& vm, PyRecord* deque, unsigned index, JSValue value)
{
    PyTuple* buffer = dequeBuffer(deque);
    buffer->initializeAt(vm, (dequeHead(deque) + index) & (buffer->length() - 1), value);
}

static PyRecord* dequeCreate(JSGlobalObject* globalObject, PyType* type)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* deque = PyRecord::create(vm, rt.structure<PyRecord>(), type, DequeFieldCount);
    deque->set(vm, DequeBuffer, rt.emptyTuple());
    dequeSetHead(vm, deque, 0);
    dequeSetSize(vm, deque, 0);
    deque->set(vm, DequeMaxLen, jsUndefined());
    return deque;
}

// Moves the items to a buffer of that capacity, which has room for them.
static void dequeReallocate(JSGlobalObject* globalObject, PyRecord* deque, unsigned capacity)
{
    VM& vm = getVM(globalObject);
    unsigned size = dequeSize(deque);
    ASSERT(capacity >= size);
    PyTuple* buffer = PyTuple::create(globalObject, capacity);
    for (unsigned i = 0; i < size; ++i)
        buffer->initializeAt(vm, i, dequeAt(deque, i));
    deque->set(vm, DequeBuffer, buffer);
    dequeSetHead(vm, deque, 0);
}

// False: a MemoryError.
static bool dequeMakeRoom(JSGlobalObject* globalObject, PyRecord* deque)
{
    unsigned capacity = dequeBuffer(deque)->length();
    if (dequeSize(deque) < capacity) [[likely]]
        return true;
    if (capacity >= dequeMaxCapacity) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseMemoryError(globalObject, scope, String());
        return false;
    }
    dequeReallocate(globalObject, deque, capacity ? capacity * 2 : dequeMinCapacity);
    return true;
}

static void dequeShrinkIfSparse(JSGlobalObject* globalObject, PyRecord* deque)
{
    unsigned capacity = dequeBuffer(deque)->length();
    if (capacity > 64 && dequeSize(deque) < capacity / 4) [[unlikely]]
        dequeReallocate(globalObject, deque, capacity / 2);
}

// The deque is not empty.
static JSValue dequeTakeRight(JSGlobalObject* globalObject, PyRecord* deque)
{
    VM& vm = getVM(globalObject);
    unsigned size = dequeSize(deque) - 1;
    JSValue item = dequeAt(deque, size);
    dequePut(vm, deque, size, jsUndefined());
    dequeSetSize(vm, deque, size);
    ++deque->number;
    dequeShrinkIfSparse(globalObject, deque);
    return item;
}

static JSValue dequeTakeLeft(JSGlobalObject* globalObject, PyRecord* deque)
{
    VM& vm = getVM(globalObject);
    JSValue item = dequeAt(deque, 0);
    dequePut(vm, deque, 0, jsUndefined());
    dequeSetHead(vm, deque, (dequeHead(deque) + 1) & (dequeBuffer(deque)->length() - 1));
    dequeSetSize(vm, deque, dequeSize(deque) - 1);
    ++deque->number;
    dequeShrinkIfSparse(globalObject, deque);
    return item;
}

static bool dequePushRight(JSGlobalObject* globalObject, PyRecord* deque, JSValue item, int64_t maxLen)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, dequeMakeRoom(globalObject, deque)))
        return false;
    unsigned size = dequeSize(deque);
    dequePut(vm, deque, size, item);
    dequeSetSize(vm, deque, size + 1);
    if (maxLen >= 0 && size + 1 > maxLen)
        dequeTakeLeft(globalObject, deque);
    else
        ++deque->number;
    return true;
}

static bool dequePushLeft(JSGlobalObject* globalObject, PyRecord* deque, JSValue item, int64_t maxLen)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, dequeMakeRoom(globalObject, deque)))
        return false;
    unsigned size = dequeSize(deque);
    dequeSetHead(vm, deque, (dequeHead(deque) - 1) & (dequeBuffer(deque)->length() - 1));
    dequePut(vm, deque, 0, item);
    dequeSetSize(vm, deque, size + 1);
    if (maxLen >= 0 && size + 1 > maxLen)
        dequeTakeRight(globalObject, deque);
    else
        ++deque->number;
    return true;
}

static void dequeClear(JSGlobalObject* globalObject, PyRecord* deque)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    if (!dequeSize(deque))
        return;
    deque->set(vm, DequeBuffer, rt.emptyTuple());
    dequeSetHead(vm, deque, 0);
    dequeSetSize(vm, deque, 0);
    ++deque->number;
}

static void dequeRotate(JSGlobalObject* globalObject, PyRecord* deque, int64_t steps)
{
    VM& vm = getVM(globalObject);
    int64_t size = dequeSize(deque);
    if (size <= 1)
        return;
    int64_t half = size >> 1;
    if (steps > half || steps < -half) {
        steps %= size;
        if (steps > half)
            steps -= size;
        else if (steps < -half)
            steps += size;
    }
    ++deque->number;
    PyTuple* buffer = dequeBuffer(deque);
    unsigned mask = buffer->length() - 1;
    unsigned head = dequeHead(deque);
    // From one end to the other, one at a time; nothing moves if the ring is full.
    bool isFull = static_cast<unsigned>(size) == buffer->length();
    for (; steps > 0; --steps) {
        head = (head - 1) & mask;
        if (!isFull) {
            unsigned last = (head + size) & mask;
            buffer->initializeAt(vm, head, buffer->at(last));
            buffer->initializeAt(vm, last, jsUndefined());
        }
    }
    for (; steps < 0; ++steps) {
        if (!isFull) {
            unsigned next = (head + size) & mask;
            buffer->initializeAt(vm, next, buffer->at(head));
            buffer->initializeAt(vm, head, jsUndefined());
        }
        head = (head + 1) & mask;
    }
    dequeSetHead(vm, deque, head);
    UNUSED_PARAM(globalObject);
}

static void dequeRemoveAt(JSGlobalObject* globalObject, PyRecord* deque, unsigned index)
{
    dequeRotate(globalObject, deque, -static_cast<int64_t>(index));
    dequeTakeLeft(globalObject, deque);
    dequeRotate(globalObject, deque, index);
}

static NEVER_INLINE JSValue dequeRaiseMutated(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, type, "deque mutated during iteration"_s);
}

template<bool toTheLeft>
static bool dequeExtendWith(JSGlobalObject* globalObject, PyRecord* deque, JSValue iterable)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // deque.extend(deque)
    if (iterable.isCell() && iterable.asCell() == deque) {
        iterable = listFromIterable(globalObject, iterable);
        RETURN_IF_EXCEPTION(scope, false);
    }
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, false);
    int64_t maxLen = dequeMaxLen(deque);
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item)
            return true;
        // A deque that holds nothing still uses the iterator up.
        if (!maxLen)
            continue;
        bool pushed = toTheLeft ? dequePushLeft(globalObject, deque, item, maxLen) : dequePushRight(globalObject, deque, item, maxLen);
        RETURN_IF_EXCEPTION(scope, false);
        if (!pushed)
            return false;
    }
}

static JSValue dequeNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&)
{
    return dequeCreate(globalObject, PyType::from(cls));
}

static JSValue dequeInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "iterable"_s, "maxlen"_s };
    JSValue values[2];
    if (failed(scope, unpackKeywordArguments(globalObject, "deque"_s, keywords, 0, 2, false, args, values)))
        return { };
    PyRecord* deque = dequeOf(self);
    JSValue maxLen = jsUndefined();
    if (values[1] && !isNone(values[1])) {
        if (!isInt(values[1]))
            return raiseTypeError(globalObject, scope, "an integer is required"_s);
        maxLen = intValue(values[1]);
        if (!tryInt64(maxLen))
            return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
        if (intSign(maxLen) < 0)
            return raiseValueError(globalObject, scope, "maxlen must be non-negative"_s);
    }
    deque->set(vm, DequeMaxLen, maxLen);
    dequeClear(globalObject, deque);
    if (values[0]) {
        dequeExtendWith<false>(globalObject, deque, values[0]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue dequeAppend(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    if (failed(scope, dequePushRight(globalObject, deque, item, dequeMaxLen(deque))))
        return { };
    return jsUndefined();
}

static JSValue dequeAppendLeft(JSGlobalObject* globalObject, JSValue self, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    if (failed(scope, dequePushLeft(globalObject, deque, item, dequeMaxLen(deque))))
        return { };
    return jsUndefined();
}

static JSValue dequeExtend(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    if (!dequeExtendWith<false>(globalObject, dequeOf(self), iterable))
        return { };
    return jsUndefined();
}

static JSValue dequeExtendLeft(JSGlobalObject* globalObject, JSValue self, JSValue iterable)
{
    if (!dequeExtendWith<true>(globalObject, dequeOf(self), iterable))
        return { };
    return jsUndefined();
}

static JSValue dequePop(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    if (!dequeSize(deque))
        return raiseIndexError(globalObject, scope, "pop from an empty deque"_s);
    RELEASE_AND_RETURN(scope, dequeTakeRight(globalObject, deque));
}

static JSValue dequePopLeft(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    if (!dequeSize(deque))
        return raiseIndexError(globalObject, scope, "pop from an empty deque"_s);
    RELEASE_AND_RETURN(scope, dequeTakeLeft(globalObject, deque));
}

static JSValue dequeClearMethod(JSGlobalObject* globalObject, JSValue self)
{
    dequeClear(globalObject, dequeOf(self));
    return jsUndefined();
}

static JSValue dequeCopy(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    PyType* exactType = dequeType(globalObject);
    if (deque->type() == exactType) {
        PyRecord* copy = dequeCreate(globalObject, exactType);
        copy->set(vm, DequeMaxLen, deque->get(DequeMaxLen));
        unsigned size = dequeSize(deque);
        if (size) {
            unsigned capacity = dequeMinCapacity;
            while (capacity < size)
                capacity *= 2;
            PyTuple* buffer = PyTuple::create(globalObject, capacity);
            for (unsigned i = 0; i < size; ++i)
                buffer->initializeAt(vm, i, dequeAt(deque, i));
            copy->set(vm, DequeBuffer, buffer);
            dequeSetSize(vm, copy, size);
        }
        return copy;
    }
    JSValue maxLen = deque->get(DequeMaxLen);
    JSValue result = isNone(maxLen) ? call(globalObject, deque->type(), self) : call(globalObject, deque->type(), self, maxLen);
    RETURN_IF_EXCEPTION(scope, { });
    if (!PyRecord::tryFrom(result, exactType))
        return raiseTypeError(globalObject, scope, deque->type()->name(), "() must return a deque, not "_s, typeName(globalObject, result));
    return result;
}

static JSValue dequeAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isDeque = isInstance(globalObject, other, dequeType(globalObject));
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDeque)
        return raiseTypeError(globalObject, scope, "can only concatenate deque (not \""_s, typeName(globalObject, other), "\") to deque"_s);
    JSValue result = dequeCopy(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    dequeExtendWith<false>(globalObject, dequeOf(result), other);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue dequeIAdd(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!dequeExtendWith<false>(globalObject, dequeOf(self), other))
        return { };
    return self;
}

static JSValue dequeRepeatInPlace(JSGlobalObject* globalObject, PyRecord* deque, int64_t count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t size = dequeSize(deque);
    if (!size || count == 1)
        return deque;
    if (count <= 0) {
        dequeClear(globalObject, deque);
        return deque;
    }
    int64_t maxLen = dequeMaxLen(deque);
    if (size > std::numeric_limits<int64_t>::max() / count)
        return raiseMemoryError(globalObject, scope, String());
    // No more repetitions than there is room for.
    if (maxLen >= 0 && count * size > maxLen)
        count = (maxLen + size - 1) / size;
    if (count * size > dequeMaxCapacity)
        return raiseMemoryError(globalObject, scope, String());
    JSArray* items = listFromIterable(globalObject, deque);
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 1; i < count; ++i) {
        dequeExtendWith<false>(globalObject, deque, items);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return deque;
}

static JSValue dequeMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue copy = dequeCopy(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, dequeRepeatInPlace(globalObject, dequeOf(copy), count));
}

static JSValue dequeIMul(JSGlobalObject* globalObject, JSValue self, JSValue countValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = toSsize(globalObject, countValue, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, dequeRepeatInPlace(globalObject, dequeOf(self), count));
}

static JSValue dequeRotateMethod(JSGlobalObject* globalObject, JSValue self, JSValue stepsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t steps = 1;
    if (stepsValue && failed(scope, ssizeArgument(globalObject, stepsValue, steps)))
        return { };
    dequeRotate(globalObject, dequeOf(self), steps);
    return jsUndefined();
}

static JSValue dequeReverse(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    PyRecord* deque = dequeOf(self);
    unsigned size = dequeSize(deque);
    for (unsigned i = 0; i < size / 2; ++i) {
        JSValue left = dequeAt(deque, i);
        dequePut(vm, deque, i, dequeAt(deque, size - 1 - i));
        dequePut(vm, deque, size - 1 - i, left);
    }
    return jsUndefined();
}

static JSValue dequeCount(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    unsigned size = dequeSize(deque);
    int64_t state = deque->number;
    unsigned count = 0;
    for (unsigned i = 0; i < size; ++i) {
        bool equal = isEqual(globalObject, dequeAt(deque, i), value);
        RETURN_IF_EXCEPTION(scope, { });
        count += equal;
        if (deque->number != state)
            RELEASE_AND_RETURN(scope, dequeRaiseMutated(globalObject, runtime(globalObject).RuntimeError()));
    }
    return jsNumber(count);
}

static JSValue dequeContains(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    unsigned size = dequeSize(deque);
    int64_t state = deque->number;
    for (unsigned i = 0; i < size; ++i) {
        bool equal = isEqual(globalObject, dequeAt(deque, i), value);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            return jsBoolean(true);
        if (deque->number != state)
            RELEASE_AND_RETURN(scope, dequeRaiseMutated(globalObject, runtime(globalObject).RuntimeError()));
    }
    return jsBoolean(false);
}

static JSValue dequeLen(JSGlobalObject*, JSValue self) { return jsNumber(dequeSize(dequeOf(self))); }

static JSValue dequeIndex(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "deque.index"_s, args)) || failed(scope, checkPositional(globalObject, "index"_s, args, 1, 3)))
        return { };
    JSValue value = args[0];
    JSValue startValue = args.get(1);
    JSValue stopValue = args.get(2);
    PyRecord* deque = dequeOf(self);
    int64_t start = 0;
    int64_t stop = std::numeric_limits<int64_t>::max();
    if (startValue && failed(scope, sliceIndexArgument(globalObject, startValue, start)))
        return { };
    if (stopValue && failed(scope, sliceIndexArgument(globalObject, stopValue, stop)))
        return { };
    int64_t size = dequeSize(deque);
    int64_t state = deque->number;
    if (start < 0)
        start = std::max<int64_t>(start + size, 0);
    if (stop < 0)
        stop = std::max<int64_t>(stop + size, 0);
    stop = std::min(stop, size);
    start = std::min(start, stop);
    for (int64_t i = start; i < stop; ++i) {
        bool equal = isEqual(globalObject, dequeAt(deque, i), value);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            return jsNumber(static_cast<int32_t>(i));
        if (deque->number != state)
            RELEASE_AND_RETURN(scope, dequeRaiseMutated(globalObject, runtime(globalObject).RuntimeError()));
    }
    return raiseValueError(globalObject, scope, "deque.index(x): x not in deque"_s);
}

static JSValue dequeInsert(JSGlobalObject* globalObject, JSValue self, JSValue indexValue, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    int64_t index;
    if (failed(scope, ssizeArgument(globalObject, indexValue, index)))
        return { };
    int64_t size = dequeSize(deque);
    int64_t maxLen = dequeMaxLen(deque);
    if (maxLen == size)
        return raiseIndexError(globalObject, scope, "deque already at its maximum size"_s);
    bool pushed;
    if (index >= size)
        pushed = dequePushRight(globalObject, deque, value, maxLen);
    else if (index <= -size || !index)
        pushed = dequePushLeft(globalObject, deque, value, maxLen);
    else {
        dequeRotate(globalObject, deque, -index);
        pushed = index < 0 ? dequePushRight(globalObject, deque, value, maxLen) : dequePushLeft(globalObject, deque, value, maxLen);
        if (pushed)
            dequeRotate(globalObject, deque, index);
    }
    if (failed(scope, pushed))
        return { };
    return jsUndefined();
}

static JSValue dequeRemove(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    unsigned size = dequeSize(deque);
    int64_t state = deque->number;
    for (unsigned i = 0; i < size; ++i) {
        bool equal = isEqual(globalObject, dequeAt(deque, i), value);
        RETURN_IF_EXCEPTION(scope, { });
        if (deque->number != state)
            RELEASE_AND_RETURN(scope, dequeRaiseMutated(globalObject, runtime(globalObject).IndexError()));
        if (equal) {
            dequeRemoveAt(globalObject, deque, i);
            return jsUndefined();
        }
    }
    return raiseValueError(globalObject, scope, "deque.remove(x): x not in deque"_s);
}

// The index of an item, from what is between the brackets. False: an exception.
static bool dequeSubscript(JSGlobalObject* globalObject, PyRecord* deque, JSValue key, unsigned& index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t result = toSsize(globalObject, key, runtime(globalObject).OverflowError());
    RETURN_IF_EXCEPTION(scope, false);
    // __index__ may have done anything to the deque.
    int64_t size = dequeSize(deque);
    if (result < 0)
        result += size;
    if (result < 0 || result >= size) {
        raiseIndexError(globalObject, scope, "deque index out of range"_s);
        return false;
    }
    index = static_cast<unsigned>(result);
    return true;
}

static JSValue dequeGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned index;
    if (failed(scope, dequeSubscript(globalObject, dequeOf(self), key, index)))
        return { };
    return dequeAt(dequeOf(self), index);
}

static JSValue dequeSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned index;
    if (failed(scope, dequeSubscript(globalObject, dequeOf(self), key, index)))
        return { };
    dequePut(getVM(globalObject), dequeOf(self), index, value);
    return jsUndefined();
}

static JSValue dequeDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned index;
    if (failed(scope, dequeSubscript(globalObject, dequeOf(self), key, index)))
        return { };
    dequeRemoveAt(globalObject, dequeOf(self), index);
    return jsUndefined();
}

static JSValue dequeReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* deque = dequeOf(self);
    JSValue state = getObjectState(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIter(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue maxLen = deque->get(DequeMaxLen);
    JSValue arguments = isNone(maxLen) ? rt.emptyTuple() : PyTuple::create(globalObject, { JSValue(rt.emptyTuple()), maxLen });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { JSValue(deque->type()), arguments, state, iterator }));
}

static JSValue dequeRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ReprGuard guard(globalObject, self);
    if (guard.isRecursive())
        return makeStr(vm, "[...]"_s);
    PyRecord* deque = dequeOf(self);
    JSArray* items = listFromIterable(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSString* itemsRepr = repr(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    String text = itemsRepr->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue maxLen = deque->get(DequeMaxLen);
    if (isNone(maxLen))
        return makeStr(vm, makeString(deque->type()->name(), '(', text, ')'));
    String maxLenText = intToString(globalObject, maxLen);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(deque->type()->name(), '(', text, ", maxlen="_s, maxLenText, ')'));
}

template<CompareOperator op>
static JSValue dequeCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* right = PyRecord::tryFrom(other, dequeType(globalObject));
    if (!right)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    PyRecord* left = dequeOf(self);
    if constexpr (op == CompareOperator::Eq || op == CompareOperator::NotEq) {
        if (left == right)
            return jsBoolean(op == CompareOperator::Eq);
        if (dequeSize(left) != dequeSize(right))
            return jsBoolean(op == CompareOperator::NotEq);
    }

    // The first place where the items differ decides.
    JSValue leftIterator = getIter(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue rightIterator = getIter(globalObject, other);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue x;
    JSValue y;
    while (true) {
        x = iterNext(globalObject, leftIterator);
        RETURN_IF_EXCEPTION(scope, { });
        y = iterNext(globalObject, rightIterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!x || !y)
            break;
        bool equal = isEqual(globalObject, x, y);
        RETURN_IF_EXCEPTION(scope, { });
        if (!equal) {
            bool result = richCompareBool(globalObject, x, y, op);
            RETURN_IF_EXCEPTION(scope, { });
            return jsBoolean(result);
        }
    }
    switch (op) {
    case CompareOperator::Lt:
        return jsBoolean(!!y);
    case CompareOperator::LtE:
        return jsBoolean(!x);
    case CompareOperator::Eq:
        return jsBoolean(!x && !y);
    case CompareOperator::NotEq:
        return jsBoolean(!!x || !!y);
    case CompareOperator::Gt:
        return jsBoolean(!!x);
    default:
        return jsBoolean(!y);
    }
}

static JSValue dequeSizeOf(JSGlobalObject*, JSValue self)
{
    // As if the items were in CPython's blocks of 64.
    uint64_t blocks = (static_cast<uint64_t>(dequeSize(dequeOf(self))) + 64) / 64;
    return jsNumber(static_cast<double>(224 + blocks * 520));
}

static JSValue dequeGetMaxLen(JSGlobalObject*, JSValue self) { return dequeOf(self)->get(DequeMaxLen); }

// ---- The iterators of deque ----

enum DequeIteratorField : unsigned {
    DequeIteratorDeque,
    DequeIteratorState, // What the state of the deque was when the iterator was made.
    DequeIteratorFieldCount,
};
// PyRecord::number is how many items are left.

static PyRecord* dequeMakeIterator(JSGlobalObject* globalObject, const TypeDef& typeDef, PyRecord* deque)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyRecord* iterator = PyRecord::create(globalObject, rt.typeFor(typeDef), DequeIteratorFieldCount);
    iterator->set(vm, DequeIteratorDeque, deque);
    iterator->set(vm, DequeIteratorState, jsNumber(static_cast<double>(deque->number)));
    iterator->number = dequeSize(deque);
    return iterator;
}

static JSValue dequeIter(JSGlobalObject* globalObject, JSValue self) { return dequeMakeIterator(globalObject, dequeIteratorTypeDef, dequeOf(self)); }
static JSValue dequeReversed(JSGlobalObject* globalObject, JSValue self) { return dequeMakeIterator(globalObject, dequeReverseIteratorTypeDef, dequeOf(self)); }

// Empty, with no exception: there are no more.
template<bool isReverse>
static JSValue dequeIteratorStep(JSGlobalObject* globalObject, PyRecord* iterator)
{
    PyRecord* deque = dequeOf(iterator->get(DequeIteratorDeque));
    if (isReverse && !iterator->number)
        return { };
    if (static_cast<double>(deque->number) != iterator->get(DequeIteratorState).asNumber()) {
        iterator->number = 0;
        return dequeRaiseMutated(globalObject, runtime(globalObject).RuntimeError());
    }
    if (!iterator->number)
        return { };
    unsigned index = isReverse ? iterator->number - 1 : dequeSize(deque) - iterator->number;
    --iterator->number;
    return dequeAt(deque, index);
}

template<bool isReverse>
static JSValue dequeIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue item = dequeIteratorStep<isReverse>(globalObject, PyRecord::from(self));
    RETURN_IF_EXCEPTION(scope, { });
    if (!item)
        return raiseStopIteration(globalObject, scope);
    return item;
}

template<bool isReverse>
static JSValue dequeIteratorNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!args.size() || args.size() > 2)
        return raiseTypeError(globalObject, scope, "function takes at "_s, args.size() ? "most 2 arguments ("_s : "least 1 argument ("_s, args.size(), " given)"_s);
    PyRecord* deque = PyRecord::tryFrom(args[0], dequeType(globalObject));
    if (!deque)
        return raiseTypeError(globalObject, scope, "argument 1 must be collections.deque, not "_s, typeNameOfArgument(globalObject, args[0]));
    int64_t index = 0;
    if (args.size() > 1 && failed(scope, ssizeArgument(globalObject, args[1], index)))
        return { };
    PyRecord* iterator = dequeMakeIterator(globalObject, isReverse ? dequeReverseIteratorTypeDef : dequeIteratorTypeDef, deque);
    for (int64_t i = 0; i < index && iterator->number; ++i)
        --iterator->number;
    return iterator;
}

static JSValue dequeIteratorSelf(JSGlobalObject*, JSValue self) { return self; }
static JSValue dequeIteratorLengthHint(JSGlobalObject*, JSValue self) { return jsNumber(static_cast<double>(PyRecord::from(self)->number)); }

static JSValue dequeIteratorReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* iterator = PyRecord::from(self);
    JSValue deque = iterator->get(DequeIteratorDeque);
    JSValue position = jsNumber(static_cast<double>(dequeSize(dequeOf(deque)) - iterator->number));
    return PyTuple::create(globalObject, { JSValue(iterator->type()), PyTuple::create(globalObject, { deque, position }) });
}

static constexpr NativeDef dequeMethods[] = {
    PY_NEW(dequeNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", dequeInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", dequeRepr, "($self, /)"),
    PY_SLOT("__eq__", dequeCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", dequeCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_SLOT("__lt__", dequeCompare<CompareOperator::Lt>, "($self, value, /)"),
    PY_SLOT("__le__", dequeCompare<CompareOperator::LtE>, "($self, value, /)"),
    PY_SLOT("__gt__", dequeCompare<CompareOperator::Gt>, "($self, value, /)"),
    PY_SLOT("__ge__", dequeCompare<CompareOperator::GtE>, "($self, value, /)"),
    PY_SLOT("__iter__", dequeIter, "($self, /)"),
    PY_SLOT("__len__", dequeLen, "($self, /)"),
    PY_SEQUENCE_SLOT("__getitem__", dequeGetItem, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__setitem__", dequeSetItem, "($self, key, value, /)"),
    PY_SEQUENCE_SLOT("__delitem__", dequeDelItem, "($self, key, /)"),
    PY_SLOT("__contains__", dequeContains, "($self, key, /)"),
    PY_SEQUENCE_SLOT("__add__", dequeAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__mul__", dequeMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__rmul__", dequeMul, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__iadd__", dequeIAdd, "($self, value, /)"),
    PY_SEQUENCE_SLOT("__imul__", dequeIMul, "($self, value, /)"),
    PY_METHOD("append", dequeAppend, "($self, item, /)"),
    PY_METHOD("appendleft", dequeAppendLeft, "($self, item, /)"),
    PY_METHOD("clear", dequeClearMethod, "($self, /)"),
    PY_METHOD("__copy__", dequeCopy, "($self, /)"),
    PY_METHOD("copy", dequeCopy, "($self, /)"),
    PY_METHOD("count", dequeCount, "($self, value, /)"),
    PY_METHOD("extend", dequeExtend, "($self, iterable, /)"),
    PY_METHOD("extendleft", dequeExtendLeft, "($self, iterable, /)"),
    PY_METHOD("index", dequeIndex, "!($self, value, [start, [stop]])"),
    PY_METHOD("insert", dequeInsert, "($self, index, value, /)"),
    PY_METHOD("pop", dequePop, "($self, /)"),
    PY_METHOD("popleft", dequePopLeft, "($self, /)"),
    PY_METHOD("__reduce__", dequeReduce, "($self, /)"),
    PY_METHOD("remove", dequeRemove, "($self, value, /)"),
    PY_METHOD("__reversed__", dequeReversed, "($self, /)"),
    PY_METHOD("reverse", dequeReverse, "($self, /)"),
    PY_METHOD("rotate", dequeRotateMethod, "($self, n=1, /)"),
    PY_METHOD("__sizeof__", dequeSizeOf, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet dequeGetSets[] = {
    { "maxlen"_s, dequeGetMaxLen },
};

constexpr TypeDef dequeTypeDef { "collections.deque"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Sequence, TypeFlag::HasWeakref, TypeFlag::OwnsContainers }, dequeMethods, dequeGetSets, "A list-like sequence optimized for data accesses near its endpoints."_s };

static constexpr NativeDef dequeIteratorMethods[] = {
    PY_NEW(dequeIteratorNew<false>, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", dequeIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", dequeIteratorNext<false>, "($self, /)"),
    PY_METHOD("__length_hint__", dequeIteratorLengthHint, "($self, /)"),
    PY_METHOD("__reduce__", dequeIteratorReduce, "($self, /)"),
};

static constexpr NativeDef dequeReverseIteratorMethods[] = {
    PY_NEW(dequeIteratorNew<true>, "($type, *args, **kwargs)"),
    PY_SLOT("__iter__", dequeIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", dequeIteratorNext<true>, "($self, /)"),
    PY_METHOD("__length_hint__", dequeIteratorLengthHint, "($self, /)"),
    PY_METHOD("__reduce__", dequeIteratorReduce, "($self, /)"),
};

constexpr TypeDef dequeIteratorTypeDef { "collections._deque_iterator"_s, { }, dequeIteratorMethods, { }, { } };
constexpr TypeDef dequeReverseIteratorTypeDef { "collections._deque_reverse_iterator"_s, { }, dequeReverseIteratorMethods, { }, { } };

// ---- defaultdict ----

// The hidden field of a defaultdict that holds default_factory. Empty: None.
static constexpr unsigned defaultDictFactoryField = 0;

extern const TypeDef defaultDictTypeDef;

static PyType* defaultDictType(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    return rt.typeFor(defaultDictTypeDef, rt.dictType());
}

static JSValue defaultDictFactory(JSGlobalObject* globalObject, JSValue self)
{
    JSValue factory = hiddenField(globalObject, self, defaultDictFactoryField);
    return factory ? factory : jsUndefined();
}

static JSValue defaultDictInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue factory = jsUndefined();
    if (args.size()) {
        factory = args[0];
        if (!isNone(factory) && !isCallable(globalObject, factory))
            return raiseTypeError(globalObject, scope, "first argument must be callable or None"_s);
    }
    setHiddenField(globalObject, self, defaultDictFactoryField, factory);
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, rt.dictType()->lookup(Special::Init), self, args.size() ? args.withoutFirst() : args));
}

static JSValue defaultDictMissing(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue factory = defaultDictFactory(globalObject, self);
    if (isNone(factory))
        return raiseKeyError(globalObject, scope, key);
    JSValue value = call(globalObject, factory);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyDict::from(self)->setDefault(globalObject, key, value));
}

// type(self)(self.default_factory, items)
static JSValue defaultDictMakeAnother(JSGlobalObject* globalObject, JSValue self, JSValue items)
{
    return call(globalObject, PyDict::from(self)->type(), defaultDictFactory(globalObject, self), items);
}

static JSValue defaultDictCopy(JSGlobalObject* globalObject, JSValue self) { return defaultDictMakeAnother(globalObject, self, self); }

static JSValue defaultDictReduce(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue factory = defaultDictFactory(globalObject, self);
    JSValue arguments = isNone(factory) ? rt.emptyTuple() : PyTuple::create(globalObject, { factory });
    JSValue items = callMethod(globalObject, self, rt.names.items());
    RETURN_IF_EXCEPTION(scope, { });
    JSValue iterator = getIter(globalObject, items);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { JSValue(PyDict::from(self)->type()), arguments, jsUndefined(), jsUndefined(), iterator }));
}

static JSValue defaultDictRepr(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue base = callSpecial(globalObject, rt.dictType()->lookup(Special::Repr), self);
    RETURN_IF_EXCEPTION(scope, { });
    String baseText = asString(base)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue factory = defaultDictFactory(globalObject, self);
    String factoryText = "None"_s;
    if (factory.isCell()) {
        // The factory may be a method of the dict.
        ReprGuard guard(globalObject, factory);
        if (guard.isRecursive())
            factoryText = "..."_s;
        else {
            JSString* text = repr(globalObject, factory);
            RETURN_IF_EXCEPTION(scope, { });
            factoryText = text->value(globalObject);
            RETURN_IF_EXCEPTION(scope, { });
        }
    } else if (!isNone(factory)) {
        JSString* text = repr(globalObject, factory);
        RETURN_IF_EXCEPTION(scope, { });
        factoryText = text->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return makeStr(vm, makeString(PyDict::from(self)->type()->name(), '(', factoryText, ", "_s, baseText, ')'));
}

static JSValue defaultDictUnion(JSGlobalObject* globalObject, JSValue left, JSValue right)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool leftIsDefaultDict = PyDict::is(left) && PyDict::from(left)->type()->isSubtypeOf(defaultDictType(globalObject));
    JSValue self = leftIsDefaultDict ? left : right;
    JSValue other = leftIsDefaultDict ? right : left;
    if (!PyDict::is(other))
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    JSValue result = defaultDictMakeAnother(globalObject, self, left);
    RETURN_IF_EXCEPTION(scope, { });
    if (!isDict(result))
        return raiseBadInternalCall(globalObject, scope);
    dictUpdate(globalObject, PyDict::from(result), right);
    RETURN_IF_EXCEPTION(scope, { });
    return result;
}

static JSValue defaultDictOr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return defaultDictUnion(globalObject, self, other); }
static JSValue defaultDictROr(JSGlobalObject* globalObject, JSValue self, JSValue other) { return defaultDictUnion(globalObject, other, self); }

static void defaultDictSetFactory(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    setHiddenField(globalObject, self, defaultDictFactoryField, value ? value : jsUndefined());
}

static constexpr NativeDef defaultDictMethods[] = {
    PY_SLOT("__init__", defaultDictInit, "($self, /, *args, **kwargs)"),
    PY_SLOT("__repr__", defaultDictRepr, "($self, /)"),
    PY_SLOT("__or__", defaultDictOr, "($self, value, /)"),
    PY_SLOT("__ror__", defaultDictROr, "($self, value, /)"),
    PY_METHOD("__missing__", defaultDictMissing, "($self, object, /)"),
    PY_METHOD("copy", defaultDictCopy, "($self, /)"),
    PY_METHOD("__copy__", defaultDictCopy, "($self, /)"),
    PY_METHOD("__reduce__", defaultDictReduce, "($self, /)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet defaultDictGetSets[] = {
    PY_WRITABLE_MEMBER("default_factory", defaultDictFactory, defaultDictSetFactory),
};

constexpr TypeDef defaultDictTypeDef { "collections.defaultdict"_s, { TypeFlag::Base, TypeFlag::OwnLayout }, defaultDictMethods, defaultDictGetSets,
    "defaultdict(default_factory=None, /, [...]) --> dict with default factory\n\n"
    "The default factory is called without arguments to produce\n"
    "a new value when a key is not present, in __getitem__ only.\n"
    "A defaultdict compares equal to a dict with the same items.\n"
    "All remaining arguments are treated the same as if they were\n"
    "passed to the dict constructor, including keyword arguments.\n"_s };

// ---- _tuplegetter ----
//
// What the fields of a named tuple are: a descriptor for the item of a tuple at an index, which is PyRecord::number.

enum TupleGetterField : unsigned { TupleGetterDoc, TupleGetterFieldCount };

static JSValue tupleGetterNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "_tuplegetter"_s, args)) || failed(scope, checkPositional(globalObject, "_tuplegetter"_s, args, 2, 2)))
        return { };
    int64_t index;
    if (failed(scope, ssizeArgument(globalObject, args[0], index)))
        return { };
    PyRecord* getter = PyRecord::create(vm, rt.structure<PyRecord>(), PyType::from(cls), TupleGetterFieldCount);
    getter->number = index;
    getter->set(vm, TupleGetterDoc, args[1]);
    return getter;
}

static JSValue tupleGetterGet(JSGlobalObject* globalObject, JSValue self, JSValue instance, JSValue owner)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isNone(instance) && (!owner || isNone(owner)))
        return raiseTypeError(globalObject, scope, "__get__(None, None) is invalid"_s);
    int64_t index = PyRecord::from(self)->number;
    auto* tuple = PyTuple::tryFrom(instance);
    if (!tuple) {
        if (isNone(instance))
            return self;
        return raiseTypeError(globalObject, scope, "descriptor for index '"_s, index, "' for tuple subclasses doesn't apply to '"_s, typeName(globalObject, instance), "' object"_s);
    }
    if (index < 0 || index >= tuple->length())
        return raiseIndexError(globalObject, scope, "tuple index out of range"_s);
    return tuple->at(static_cast<unsigned>(index));
}

static JSValue tupleGetterSet(JSGlobalObject* globalObject, JSValue, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "can't set attribute"_s);
}

static JSValue tupleGetterDelete(JSGlobalObject* globalObject, JSValue, JSValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), "can't delete attribute"_s);
}

static JSValue tupleGetterDocOf(JSGlobalObject*, JSValue self)
{
    JSValue doc = PyRecord::from(self)->get(TupleGetterDoc);
    return doc ? doc : jsUndefined();
}

static void tupleGetterSetDoc(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    PyRecord::from(self)->set(getVM(globalObject), TupleGetterDoc, value);
}

static JSValue tupleGetterReduce(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* getter = PyRecord::from(self);
    return PyTuple::create(globalObject, { JSValue(getter->type()), PyTuple::create(globalObject, { makeInt(globalObject, getter->number), tupleGetterDocOf(globalObject, self) }) });
}

static JSValue tupleGetterRepr(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* getter = PyRecord::from(self);
    JSString* doc = repr(globalObject, tupleGetterDocOf(globalObject, self));
    RETURN_IF_EXCEPTION(scope, { });
    String text = doc->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, makeString(getter->type()->name(), '(', getter->number, ", "_s, text, ')'));
}

static constexpr NativeDef tupleGetterMethods[] = {
    PY_NEW(tupleGetterNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", tupleGetterRepr, "($self, /)"),
    PY_SLOT("__get__", tupleGetterGet, "($self, instance, owner=None, /)"),
    PY_SLOT("__set__", tupleGetterSet, "($self, instance, value, /)"),
    PY_SLOT("__delete__", tupleGetterDelete, "($self, instance, /)"),
    PY_METHOD("__reduce__", tupleGetterReduce, "($self, /)"),
};

static constexpr NativeGetSet tupleGetterGetSets[] = {
    PY_WRITABLE_MEMBER("__doc__", tupleGetterDocOf, tupleGetterSetDoc),
};

static constexpr TypeDef tupleGetterTypeDef { "collections._tuplegetter"_s, { }, tupleGetterMethods, tupleGetterGetSets, { } };

int64_t indexOfTupleGetter(JSGlobalObject* globalObject, JSValue value)
{
    if (!PyRecord::is(value))
        return -1;
    auto* record = uncheckedDowncast<PyRecord>(value.asCell());
    return record->type() == runtime(globalObject).typeIfMadeFor(tupleGetterTypeDef) ? record->number : -1;
}

// ---- _count_elements ----

static JSValue collectionsCountElements(JSGlobalObject* globalObject, JSValue mapping, JSValue iterable)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, iterable);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue one = jsNumber(1);

    // Straight into the table, unless the mapping has ideas of its own about get() and __setitem__().
    PyType* type = rt.typeOf(mapping);
    JSValue mappingGet = type->lookup(rt.names.get());
    JSValue mappingSetItem = type->lookup(Special::SetItem);
    if (isDict(mapping) && mappingGet && mappingGet == rt.dictType()->lookup(rt.names.get()) && mappingSetItem && mappingSetItem == rt.dictType()->lookup(Special::SetItem)) {
        PyDict* dict = PyDict::from(mapping);
        while (true) {
            JSValue key = iterNext(globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, { });
            if (!key)
                return jsUndefined();
            int64_t keyHash = PyDict::hashIfNeeded(globalObject, key);
            RETURN_IF_EXCEPTION(scope, { });
            JSValue count = dict->get(globalObject, key, keyHash);
            RETURN_IF_EXCEPTION(scope, { });
            if (count) {
                count = binaryOperation(globalObject, BinaryOperator::Add, count, one);
                RETURN_IF_EXCEPTION(scope, { });
            } else
                count = one;
            dict->set(globalObject, key, keyHash, count);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }

    JSValue get = getAttr(globalObject, mapping, rt.names.get());
    RETURN_IF_EXCEPTION(scope, { });
    while (true) {
        JSValue key = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!key)
            return jsUndefined();
        JSValue count = call(globalObject, get, key, jsNumber(0));
        RETURN_IF_EXCEPTION(scope, { });
        count = binaryOperation(globalObject, BinaryOperator::Add, count, one);
        RETURN_IF_EXCEPTION(scope, { });
        setItem(globalObject, mapping, key, count);
        RETURN_IF_EXCEPTION(scope, { });
    }
}

static constexpr NativeDef collectionsFunctions[] = {
    PY_FUNCTION("_count_elements", collectionsCountElements, "(mapping, iterable, /)"),
};

void initializeCollectionsModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    module->add(globalObject, "__doc__"_s, makeStr(vm,
        "High performance data structures.\n"
        "- deque:        ordered collection accessible from endpoints only\n"
        "- defaultdict:  dict subclass with a default value factory\n"_s));
    module->addFunctions(globalObject, collectionsFunctions);

    PyType* deque = dequeType(globalObject);
    if (!deque->dict()->get(rt.name(Special::Hash))) {
        deque->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        deque->didChange(vm);
    }
    module->add(globalObject, "deque"_s, deque);
    module->add(globalObject, "defaultdict"_s, defaultDictType(globalObject));
    module->add(globalObject, "OrderedDict"_s, orderedDictType(globalObject));
    module->add(globalObject, "_deque_iterator"_s, rt.typeFor(dequeIteratorTypeDef));
    module->add(globalObject, "_deque_reverse_iterator"_s, rt.typeFor(dequeReverseIteratorTypeDef));
    module->add(globalObject, "_tuplegetter"_s, rt.typeFor(tupleGetterTypeDef));
}

} // namespace Python
} // namespace JSC
