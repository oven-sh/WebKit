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

namespace JSC {
namespace Python {

// The modules _heapq and _bisect. (Modules/_heapqmodule.c and Modules/_bisectmodule.c in CPython are the reference.)

// ---- _heapq ----
//
// Comparisons run Python code, which may do anything to the list: its length is looked at again after each.

static NEVER_INLINE bool heapqRaiseOutOfRange(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseIndexError(globalObject, scope, "index out of range"_s);
    return false;
}

static NEVER_INLINE bool heapqRaiseChangedSize(JSGlobalObject* globalObject)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseRuntimeError(globalObject, scope, "list changed size during iteration"_s);
    return false;
}

// Whether `a` belongs nearer the top than `b`.
template<bool isMax>
static ALWAYS_INLINE bool heapqComesFirst(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    return isMax ? richCompareBool(globalObject, b, a, CompareOperator::Lt) : richCompareBool(globalObject, a, b, CompareOperator::Lt);
}

// Moves the item at `position` towards the top, no further than `start`, to where it fits. False: an exception.
template<bool isMax>
static bool heapqSiftDown(JSGlobalObject* globalObject, JSArray* heap, unsigned start, unsigned position)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned size = listLength(heap);
    if (position >= size)
        RELEASE_AND_RETURN(scope, heapqRaiseOutOfRange(globalObject));
    while (position > start) {
        unsigned parentPosition = (position - 1) >> 1;
        bool comesFirst = heapqComesFirst<isMax>(globalObject, listGet(heap, position), listGet(heap, parentPosition));
        RETURN_IF_EXCEPTION(scope, false);
        if (size != listLength(heap))
            RELEASE_AND_RETURN(scope, heapqRaiseChangedSize(globalObject));
        if (!comesFirst)
            break;
        JSValue parent = listGet(heap, parentPosition);
        listSet(globalObject, heap, parentPosition, listGet(heap, position));
        RETURN_IF_EXCEPTION(scope, false);
        listSet(globalObject, heap, position, parent);
        RETURN_IF_EXCEPTION(scope, false);
        position = parentPosition;
    }
    return true;
}

// Moves the item at `position` away from the top, to a leaf, then back up to where it fits.
template<bool isMax>
static bool heapqSiftUp(JSGlobalObject* globalObject, JSArray* heap, unsigned position)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned end = listLength(heap);
    unsigned start = position;
    if (position >= end)
        RELEASE_AND_RETURN(scope, heapqRaiseOutOfRange(globalObject));
    unsigned limit = end >> 1; // The first position without a child.
    while (position < limit) {
        unsigned child = 2 * position + 1;
        if (child + 1 < end) {
            bool leftComesFirst = heapqComesFirst<isMax>(globalObject, listGet(heap, child), listGet(heap, child + 1));
            RETURN_IF_EXCEPTION(scope, false);
            if (!leftComesFirst)
                ++child;
            if (end != listLength(heap))
                RELEASE_AND_RETURN(scope, heapqRaiseChangedSize(globalObject));
        }
        JSValue item = listGet(heap, position);
        listSet(globalObject, heap, position, listGet(heap, child));
        RETURN_IF_EXCEPTION(scope, false);
        listSet(globalObject, heap, child, item);
        RETURN_IF_EXCEPTION(scope, false);
        position = child;
    }
    RELEASE_AND_RETURN(scope, heapqSiftDown<isMax>(globalObject, heap, start, position));
}

// Null: a TypeError. `position` is 0 for a function of one argument.
static JSArray* heapqListArgument(JSGlobalObject* globalObject, ASCIILiteral function, unsigned position, JSValue value)
{
    if (isList(value)) [[likely]]
        return asList(value);
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (position)
        raiseTypeError(globalObject, scope, function, "() argument "_s, position, " must be list, not "_s, typeNameOfArgument(globalObject, value));
    else
        raiseTypeError(globalObject, scope, function, "() argument must be list, not "_s, typeNameOfArgument(globalObject, value));
    return nullptr;
}

template<bool isMax>
static JSValue heapqPush(JSGlobalObject* globalObject, JSValue heapValue, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* heap = heapqListArgument(globalObject, isMax ? "heappush_max"_s : "heappush"_s, 1, heapValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!heap)
        return { };
    listAppend(globalObject, heap, item);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, heapqSiftDown<isMax>(globalObject, heap, 0, listLength(heap) - 1)))
        return { };
    return jsUndefined();
}

template<bool isMax>
static JSValue heapqPop(JSGlobalObject* globalObject, JSValue heapValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* heap = heapqListArgument(globalObject, isMax ? "heappop_max"_s : "heappop"_s, 0, heapValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!heap)
        return { };
    unsigned size = listLength(heap);
    if (!size) {
        scope.release();
        heapqRaiseOutOfRange(globalObject);
        return { };
    }
    JSValue last = listGet(heap, size - 1);
    listTruncate(globalObject, heap, size - 1);
    RETURN_IF_EXCEPTION(scope, { });
    if (size == 1)
        return last;
    JSValue top = listGet(heap, 0);
    listSet(globalObject, heap, 0, last);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, heapqSiftUp<isMax>(globalObject, heap, 0)))
        return { };
    return top;
}

template<bool isMax>
static JSValue heapqReplace(JSGlobalObject* globalObject, JSValue heapValue, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* heap = heapqListArgument(globalObject, isMax ? "heapreplace_max"_s : "heapreplace"_s, 1, heapValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!heap)
        return { };
    if (!listLength(heap)) {
        scope.release();
        heapqRaiseOutOfRange(globalObject);
        return { };
    }
    JSValue top = listGet(heap, 0);
    listSet(globalObject, heap, 0, item);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, heapqSiftUp<isMax>(globalObject, heap, 0)))
        return { };
    return top;
}

template<bool isMax>
static JSValue heapqPushPop(JSGlobalObject* globalObject, JSValue heapValue, JSValue item)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* heap = heapqListArgument(globalObject, isMax ? "heappushpop_max"_s : "heappushpop"_s, 1, heapValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!heap)
        return { };
    if (!listLength(heap))
        return item;
    bool topComesFirst = heapqComesFirst<isMax>(globalObject, listGet(heap, 0), item);
    RETURN_IF_EXCEPTION(scope, { });
    if (!topComesFirst)
        return item;
    if (!listLength(heap)) {
        scope.release();
        heapqRaiseOutOfRange(globalObject);
        return { };
    }
    JSValue top = listGet(heap, 0);
    listSet(globalObject, heap, 0, item);
    RETURN_IF_EXCEPTION(scope, { });
    if (failed(scope, heapqSiftUp<isMax>(globalObject, heap, 0)))
        return { };
    return top;
}

template<bool isMax>
static JSValue heapqHeapify(JSGlobalObject* globalObject, JSValue heapValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* heap = heapqListArgument(globalObject, isMax ? "heapify_max"_s : "heapify"_s, 0, heapValue);
    RETURN_IF_EXCEPTION(scope, { });
    if (!heap)
        return { };
    // From the last item that has a child to the top: what is below each is a heap by then.
    for (unsigned i = listLength(heap) >> 1; i--;) {
        if (failed(scope, heapqSiftUp<isMax>(globalObject, heap, i)))
            return { };
    }
    return jsUndefined();
}

static constexpr NativeDef heapqFunctions[] = {
    PY_FUNCTION("heappush", heapqPush<false>, "(heap, item, /)"),
    PY_FUNCTION("heappushpop", heapqPushPop<false>, "(heap, item, /)"),
    PY_FUNCTION("heappop", heapqPop<false>, "(heap, /)"),
    PY_FUNCTION("heapreplace", heapqReplace<false>, "(heap, item, /)"),
    PY_FUNCTION("heapify", heapqHeapify<false>, "(heap, /)"),
    PY_FUNCTION("heappush_max", heapqPush<true>, "(heap, item, /)"),
    PY_FUNCTION("heappushpop_max", heapqPushPop<true>, "(heap, item, /)"),
    PY_FUNCTION("heappop_max", heapqPop<true>, "(heap, /)"),
    PY_FUNCTION("heapreplace_max", heapqReplace<true>, "(heap, item, /)"),
    PY_FUNCTION("heapify_max", heapqHeapify<true>, "(heap, /)"),
};

void initializeHeapqModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "__doc__"_s, makeStr(getVM(globalObject), "Heap queue algorithm (a.k.a. priority queue)."_s));
    module->addFunctions(globalObject, heapqFunctions);
    module->add(globalObject, "__about__"_s, makeStr(getVM(globalObject), "Heap queues\n"_s));
}

// ---- _bisect ----

struct BisectBounds {
    int64_t low { 0 };
    int64_t high { -1 }; // -1: the length of the sequence.
};

static bool bisectParseBounds(JSGlobalObject* globalObject, JSValue low, JSValue high, BisectBounds& bounds)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (low) {
        if (failed(scope, ssizeArgument(globalObject, low, bounds.low)))
            return false;
        if (bounds.low < 0) {
            raiseValueError(globalObject, scope, "lo cannot be negative"_s);
            return false;
        }
    }
    if (high && !isNone(high)) {
        if (!hasIndex(globalObject, high)) {
            raiseTypeError(globalObject, scope, "argument should be integer or None, not '"_s, typeName(globalObject, high), '\'');
            return false;
        }
        bounds.high = toSsize(globalObject, high, runtime(globalObject).OverflowError());
        RETURN_IF_EXCEPTION(scope, false);
    }
    return true;
}

// Where to insert the item. -1: an exception. `key` is empty for none.
template<bool toTheRight>
static int64_t bisectSearch(JSGlobalObject* globalObject, JSValue sequence, JSValue item, BisectBounds bounds, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t low = bounds.low;
    int64_t high = bounds.high;
    if (high == -1) {
        high = length(globalObject, sequence);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    PyType* type = rt.typeOf(sequence);
    if (PyDict::is(sequence) && !type->isHeapType()) {
        raiseTypeError(globalObject, scope, type->name(), " is not a sequence"_s);
        return -1;
    }
    if (!type->lookup(Special::GetItem)) {
        raiseTypeError(globalObject, scope, '\'', type->name(), "' object does not support indexing"_s);
        return -1;
    }
    while (low < high) {
        int64_t middle = static_cast<int64_t>((static_cast<uint64_t>(low) + static_cast<uint64_t>(high)) / 2);
        JSValue candidate = getItem(globalObject, sequence, makeInt(globalObject, middle));
        RETURN_IF_EXCEPTION(scope, -1);
        if (key) {
            candidate = call(globalObject, key, candidate);
            RETURN_IF_EXCEPTION(scope, -1);
        }
        if (toTheRight) {
            bool isBefore = richCompareBool(globalObject, item, candidate, CompareOperator::Lt);
            RETURN_IF_EXCEPTION(scope, -1);
            if (isBefore)
                high = middle;
            else
                low = middle + 1;
        } else {
            bool isAfter = richCompareBool(globalObject, candidate, item, CompareOperator::Lt);
            RETURN_IF_EXCEPTION(scope, -1);
            if (isAfter)
                low = middle + 1;
            else
                high = middle;
        }
    }
    return low;
}

template<bool toTheRight>
static JSValue bisectFind(JSGlobalObject* globalObject, JSValue sequence, JSValue item, JSValue low, JSValue high, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BisectBounds bounds;
    if (failed(scope, bisectParseBounds(globalObject, low, high, bounds)))
        return { };
    int64_t index = bisectSearch<toTheRight>(globalObject, sequence, item, bounds, key && !isNone(key) ? key : JSValue());
    if (failed(scope, index >= 0))
        return { };
    RELEASE_AND_RETURN(scope, makeInt(globalObject, index));
}

template<bool toTheRight>
static JSValue bisectInsort(JSGlobalObject* globalObject, JSValue sequence, JSValue item, JSValue low, JSValue high, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    BisectBounds bounds;
    if (failed(scope, bisectParseBounds(globalObject, low, high, bounds)))
        return { };
    JSValue sought = item;
    if (key && isNone(key))
        key = JSValue();
    if (key) {
        sought = call(globalObject, key, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    int64_t index = bisectSearch<toTheRight>(globalObject, sequence, sought, bounds, key);
    if (failed(scope, index >= 0))
        return { };
    if (isExactList(sequence)) {
        JSArray* list = asList(sequence);
        listInsert(globalObject, list, static_cast<unsigned>(std::min<int64_t>(index, listLength(list))), item);
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    callMethod(globalObject, sequence, internString(vm, "insert"_s), makeInt(globalObject, index), item);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static constexpr NativeDef bisectFunctions[] = {
    PY_FUNCTION("bisect_right", bisectFind<true>, "(a, x, lo=0, hi=None, *, key=None)"),
    PY_FUNCTION("insort_right", bisectInsort<true>, "(a, x, lo=0, hi=None, *, key=None)"),
    PY_FUNCTION("bisect_left", bisectFind<false>, "(a, x, lo=0, hi=None, *, key=None)"),
    PY_FUNCTION("insort_left", bisectInsort<false>, "(a, x, lo=0, hi=None, *, key=None)"),
};

void initializeBisectModule(JSGlobalObject* globalObject, PyModule* module)
{
    module->add(globalObject, "__doc__"_s, makeStr(getVM(globalObject), "Bisection algorithms."_s));
    module->addFunctions(globalObject, bisectFunctions);
}

} // namespace Python
} // namespace JSC
