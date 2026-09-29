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

#include "JSCInlines.h"
#include "PyRealm.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"
#include "PythonSignatures.h"

// The module _heapq: Modules/_heapqmodule.c of CPython. Which things are compared with which, and in what order, is something that a program can see, and so is what is made of a list that is changed by what its elements are
// compared by. So it is gone about just as it is there: what is in the list is looked at again after each comparison, and how long it is.

namespace JSC { namespace Python {

namespace {

enum class Heap : bool { Min, Max };

// PyObject_RichCompareBool(a, b, Py_LT). Nothing if it raised.
std::optional<bool> isLessThan(JSGlobalObject* globalObject, JSValue a, JSValue b)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = compare(globalObject, ComparisonOperator::Lt, a, b);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    bool truth = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return truth;
}

void swap(JSGlobalObject* globalObject, JSArray* heap, unsigned a, unsigned b)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue first = listGet(globalObject, heap, a);
    RETURN_IF_EXCEPTION(scope, void());
    JSValue second = listGet(globalObject, heap, b);
    RETURN_IF_EXCEPTION(scope, void());
    listSet(globalObject, heap, a, second);
    RETURN_IF_EXCEPTION(scope, void());
    scope.release();
    listSet(globalObject, heap, b, first);
}

// siftdown() and siftdown_max(): follows the path to the root, moving parents down until there is a place that what was at `position` fits. It may raise.
template<Heap kind>
void siftDown(JSGlobalObject* globalObject, JSArray* heap, unsigned start, unsigned position)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    unsigned size = heap->length();
    if (position >= size) {
        raise(globalObject, scope, BuiltinType::IndexError, "index out of range"_s);
        return;
    }
    JSValue newItem = listGet(globalObject, heap, position);
    RETURN_IF_EXCEPTION(scope, void());
    while (position > start) {
        unsigned parentPosition = (position - 1) >> 1;
        JSValue parent = listGet(globalObject, heap, parentPosition);
        RETURN_IF_EXCEPTION(scope, void());
        auto isLess = kind == Heap::Min ? isLessThan(globalObject, newItem, parent) : isLessThan(globalObject, parent, newItem);
        RETURN_IF_EXCEPTION(scope, void());
        if (size != heap->length()) {
            raise(globalObject, scope, BuiltinType::RuntimeError, "list changed size during iteration"_s);
            return;
        }
        if (!*isLess)
            break;
        swap(globalObject, heap, parentPosition, position);
        RETURN_IF_EXCEPTION(scope, void());
        // What has been moved is what was there by then, which need not be what was compared.
        newItem = listGet(globalObject, heap, parentPosition);
        RETURN_IF_EXCEPTION(scope, void());
        position = parentPosition;
    }
}

// siftup() and siftup_max(): brings the smaller child up, or the larger, until there is a leaf, and then what was moved to there goes to where it belongs. It may raise.
template<Heap kind>
void siftUp(JSGlobalObject* globalObject, JSArray* heap, unsigned position)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    unsigned end = heap->length();
    unsigned start = position;
    if (position >= end) {
        raise(globalObject, scope, BuiltinType::IndexError, "index out of range"_s);
        return;
    }
    unsigned limit = end >> 1; // The first that has no child
    while (position < limit) {
        unsigned child = 2 * position + 1;
        if (child + 1 < end) {
            JSValue left = listGet(globalObject, heap, child);
            RETURN_IF_EXCEPTION(scope, void());
            JSValue right = listGet(globalObject, heap, child + 1);
            RETURN_IF_EXCEPTION(scope, void());
            auto isLess = kind == Heap::Min ? isLessThan(globalObject, left, right) : isLessThan(globalObject, right, left);
            RETURN_IF_EXCEPTION(scope, void());
            child += !*isLess;
            if (end != heap->length()) {
                raise(globalObject, scope, BuiltinType::RuntimeError, "list changed size during iteration"_s);
                return;
            }
        }
        swap(globalObject, heap, child, position);
        RETURN_IF_EXCEPTION(scope, void());
        position = child;
    }
    scope.release();
    siftDown<kind>(globalObject, heap, start, position);
}

void siftUp(JSGlobalObject* globalObject, Heap kind, JSArray* heap, unsigned position)
{
    if (kind == Heap::Min)
        siftUp<Heap::Min>(globalObject, heap, position);
    else
        siftUp<Heap::Max>(globalObject, heap, position);
}

// The list that all of them take first. Null if it raised.
JSArray* heapArgument(JSGlobalObject* globalObject, ThrowScope& scope, const NativeArguments& args, ASCIILiteral function, bool isOnlyArgument)
{
    if (JSArray* list = tryList(args[0]))
        return list;
    raiseTypeError(globalObject, scope, concatenate(function, isOnlyArgument ? "() argument must be list, not "_s : "() argument 1 must be list, not "_s, typeNameOfArgument(globalObject, args[0])));
    return nullptr;
}

unsigned keepTopBit(unsigned n)
{
    unsigned i = 0;
    while (n > 1) {
        n >>= 1;
        ++i;
    }
    return n << i;
}

} // anonymous namespace

#define HEAP_PROLOGUE(minimumName, maximumName, isOnlyArgument) \
    auto kind = unpack<Heap>(callFrame, 0); \
    NATIVE_PROLOGUE(); \
    JSArray* heap = heapArgument(globalObject, scope, args, kind == Heap::Min ? minimumName ""_s : maximumName ""_s, isOnlyArgument); \
    RETURN_IF_EXCEPTION(scope, { });

PYTHON_NATIVE(heapPush)
{
    HEAP_PROLOGUE("heappush", "heappush_max", false);
    listAppend(globalObject, heap, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (kind == Heap::Min)
        siftDown<Heap::Min>(globalObject, heap, 0, heap->length() - 1);
    else
        siftDown<Heap::Max>(globalObject, heap, 0, heap->length() - 1);
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

PYTHON_NATIVE(heapPop)
{
    HEAP_PROLOGUE("heappop", "heappop_max", true);
    unsigned size = heap->length();
    if (!size)
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "index out of range"_s));
    JSValue last = listGet(globalObject, heap, size - 1);
    RETURN_IF_EXCEPTION(scope, { });
    listRemoveRange(globalObject, heap, size - 1, 1);
    RETURN_IF_EXCEPTION(scope, { });
    if (size == 1)
        return JSValue::encode(last);
    JSValue result = listGet(globalObject, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    listSet(globalObject, heap, 0, last);
    RETURN_IF_EXCEPTION(scope, { });
    siftUp(globalObject, kind, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(heapReplace)
{
    HEAP_PROLOGUE("heapreplace", "heapreplace_max", false);
    if (!heap->length())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "index out of range"_s));
    JSValue result = listGet(globalObject, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    listSet(globalObject, heap, 0, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    siftUp(globalObject, kind, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(heapPushPop)
{
    HEAP_PROLOGUE("heappushpop", "heappushpop_max", false);
    JSValue item = args[1];
    if (!heap->length())
        return JSValue::encode(item);
    JSValue top = listGet(globalObject, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    auto isLess = kind == Heap::Min ? isLessThan(globalObject, top, item) : isLessThan(globalObject, item, top);
    RETURN_IF_EXCEPTION(scope, { });
    if (!*isLess)
        return JSValue::encode(item);
    if (!heap->length())
        return JSValue::encode(raise(globalObject, scope, BuiltinType::IndexError, "index out of range"_s));
    JSValue result = listGet(globalObject, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    listSet(globalObject, heap, 0, item);
    RETURN_IF_EXCEPTION(scope, { });
    siftUp(globalObject, kind, heap, 0);
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(result);
}

PYTHON_NATIVE(heapify)
{
    HEAP_PROLOGUE("heapify", "heapify_max", true);
    int64_t size = heap->length();
    // A heap that is likely to be too large for the processor to have all of at hand is gone through in another order, that comes to each parent while its children are still at hand. It comes to the same heap by the same
    // number of comparisons, but they are not made in the same order.
    constexpr int64_t largestForSimpleOrder = 2500;
    if (size <= largestForSimpleOrder) {
        for (int64_t i = (size >> 1) - 1; i >= 0; --i) {
            siftUp(globalObject, kind, heap, i);
            RETURN_IF_EXCEPTION(scope, { });
        }
        RETURN_NONE();
    }
    int64_t firstChildless = size >> 1;
    int64_t leftmost = static_cast<int64_t>(keepTopBit(firstChildless + 1)) - 1; // The leftmost in the row that that one is in
    int64_t half = firstChildless >> 1;
    auto siftFrom = [&] (int64_t i) {
        for (int64_t j = i;; j >>= 1) {
            siftUp(globalObject, kind, heap, j);
            RETURN_IF_EXCEPTION(scope, false);
            if (!(j & 1))
                return true;
        }
    };
    for (int64_t i = leftmost - 1; i >= half; --i) {
        if (!siftFrom(i))
            return { };
    }
    for (int64_t i = firstChildless - 1; i >= leftmost; --i) {
        if (!siftFrom(i))
            return { };
    }
    RETURN_NONE();
}

#undef HEAP_PROLOGUE

JSObject* createHeapqModule(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    JSObject* module = newBuiltinModule(globalObject, "_heapq"_s);
    module->putDirect(vm, Identifier::fromString(vm, "__about__"_s), jsString(vm, findModuleText("_heapq:__about__"_s)));
    addFunction(globalObject, module, "heappush"_s, heapPush, pack(Heap::Min));
    addFunction(globalObject, module, "heappushpop"_s, heapPushPop, pack(Heap::Min));
    addFunction(globalObject, module, "heappop"_s, heapPop, pack(Heap::Min));
    addFunction(globalObject, module, "heapreplace"_s, heapReplace, pack(Heap::Min));
    addFunction(globalObject, module, "heapify"_s, heapify, pack(Heap::Min));
    addFunction(globalObject, module, "heappush_max"_s, heapPush, pack(Heap::Max));
    addFunction(globalObject, module, "heappushpop_max"_s, heapPushPop, pack(Heap::Max));
    addFunction(globalObject, module, "heappop_max"_s, heapPop, pack(Heap::Max));
    addFunction(globalObject, module, "heapreplace_max"_s, heapReplace, pack(Heap::Max));
    addFunction(globalObject, module, "heapify_max"_s, heapify, pack(Heap::Max));
    return module;
}

} } // namespace JSC::Python
