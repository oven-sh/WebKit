/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyThreads.h"
#include "TypedArrayType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The typed arrays of JS (Uint8Array, Float64Array, Buffer, ...) in Python: _js.JSTypedArray, which derives from JSObject.
//
// They are objects of JS like any other, with the properties and methods they have there, and they export their memory
// (PyBuffer.h). Besides, they are sequences whose length is fixed, as an array.array or a bytearray with an export is:
//
//   a[-1], a[1:3], a[::-1]     An index counts from the end if it is negative. A slice is a copy, as of any sequence that can be
//                              changed, and what %TypedArray%.prototype.slice() makes: of a Buffer, a Buffer.
//   a[i] = x, a[1:3] = items   JS converts what is stored. As many items as the slice has.
//   len(a), bool(a), iter(a)   By what an index is counted from and checked against, and not by what the object says its `length` or its
//                              Symbol.iterator is.
//   x in a, a.index(x), a.count(x)
//   a == b                     As for memoryview(a): the same items in the same shape, whatever exports them. So they have no hash.
//
// A key that is neither an index nor a slice names a property, as for any object of JS: a["length"].
// An ArrayBuffer and a DataView have no items.

static ALWAYS_INLINE JSArrayBufferView* jsTypedArrayOf(JSValue self) { return uncheckedDowncast<JSArrayBufferView>(self.asCell()); }

// An index into the array as it is now. -1: an exception.
static int64_t jsTypedArrayIndex(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t index = toSsize(globalObject, key, rt.IndexError());
    RETURN_IF_EXCEPTION(scope, -1);
    int64_t length = jsTypedArrayOf(self)->length();
    if (index < 0)
        index += length;
    if (index < 0 || index >= length) {
        raiseIndexError(globalObject, scope, "typed array index out of range"_s);
        return -1;
    }
    return index;
}

static JSValue jsTypedArrayPropertyMethod(JSGlobalObject* globalObject, Special special)
{
    return runtime(globalObject).jsObjectType()->lookup(special);
}

// %TypedArray%.prototype.slice.call(array, start, end)
static JSArrayBufferView* jsTypedArrayCopyPart(JSGlobalObject* globalObject, JSArrayBufferView* array, int64_t start, int64_t end)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* prototype = globalObject->typedArrayStructure(TypeUint8, false)->storedPrototypeObject();
    JSValue function = asObject(prototype->getPrototypeDirect())->get(globalObject, Identifier::fromString(vm, "slice"_s));
    RETURN_IF_EXCEPTION(scope, nullptr);
    MarkedArgumentBuffer arguments;
    arguments.append(jsNumber(static_cast<double>(start)));
    arguments.append(jsNumber(static_cast<double>(end)));
    JSValue result = JSC::call(globalObject, function, array, arguments, "%TypedArray%.prototype.slice is not a function"_s);
    RETURN_IF_EXCEPTION(scope, nullptr);
    // A constructor that the program gave as the species may return anything.
    if (!result.isCell() || result.asCell()->type() != array->type()) {
        raiseTypeError(globalObject, scope, "the slice of a typed array is not a typed array of the same kind"_s);
        return nullptr;
    }
    return uncheckedDowncast<JSArrayBufferView>(result.asCell());
}

static JSValue jsTypedArrayGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArrayBufferView* array = jsTypedArrayOf(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = jsTypedArrayIndex(globalObject, self, key);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, array->getIndex(globalObject, static_cast<uint64_t>(index)));
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, jsTypedArrayPropertyMethod(globalObject, Special::GetItem), self, key));
    PySlice::Indices indices;
    if (failed(scope, slice->unpack(globalObject, indices)))
        return { };
    PySlice::adjust(array->length(), indices);
    if (indices.length <= 0)
        RELEASE_AND_RETURN(scope, jsTypedArrayCopyPart(globalObject, array, 0, 0));
    if (indices.step == 1)
        RELEASE_AND_RETURN(scope, jsTypedArrayCopyPart(globalObject, array, indices.start, indices.start + indices.length));
    // An array of the right class and length, of which every item is then replaced.
    JSArrayBufferView* result = jsTypedArrayCopyPart(globalObject, array, 0, indices.length);
    RETURN_IF_EXCEPTION(scope, { });
    size_t itemSize = elementSize(typedArrayType(array->type()));
    int64_t last = indices.start + (indices.length - 1) * indices.step;
    // That may have run code of the program.
    if (static_cast<int64_t>(result->length()) < indices.length || std::max(indices.start, last) >= static_cast<int64_t>(array->length()))
        return raiseBufferError(globalObject, scope, "the ArrayBuffer was detached or shrunk"_s);
    auto* source = static_cast<const uint8_t*>(array->vector());
    auto* destination = static_cast<uint8_t*>(result->vector());
    for (int64_t i = 0; i < indices.length; ++i)
        memcpy(destination + i * itemSize, source + (indices.start + i * indices.step) * itemSize, itemSize);
    return result;
}

// What JS stores: a BigInt in an array of those, which takes nothing else.
static JSValue jsTypedArrayItemForJS(JSGlobalObject* globalObject, JSArrayBufferView* array, JSValue value)
{
    if (isBigInt(typedArrayType(array->type())) && isInt(value))
        return toBigInt(globalObject, intValue(value));
    return valueForJS(value);
}

static JSValue jsTypedArraySetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArrayBufferView* array = jsTypedArrayOf(self);
    if (hasIndex(globalObject, key)) {
        int64_t index = jsTypedArrayIndex(globalObject, self, key);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue item = jsTypedArrayItemForJS(globalObject, array, value);
        RETURN_IF_EXCEPTION(scope, { });
        array->putByIndexInline(globalObject, static_cast<uint64_t>(index), item, true);
        RETURN_IF_EXCEPTION(scope, { });
        return jsUndefined();
    }
    auto* slice = PySlice::tryFrom(key);
    if (!slice)
        RELEASE_AND_RETURN(scope, callSpecial(globalObject, jsTypedArrayPropertyMethod(globalObject, Special::SetItem), self, key, value));
    // A list of its own, so that it does not change as the array does: a[1:] = a[:-1], a[::-1] = a.
    JSArray* items = listFromIterable(globalObject, value);
    RETURN_IF_EXCEPTION(scope, { });
    PySlice::Indices indices;
    if (failed(scope, slice->unpack(globalObject, indices)))
        return { };
    PySlice::adjust(array->length(), indices);
    int64_t count = std::max<int64_t>(indices.length, 0);
    if (static_cast<int64_t>(listLength(items)) != count)
        return raiseValueError(globalObject, scope, "a typed array cannot be resized: attempt to assign a sequence of size "_s, listLength(items), " to a slice of size "_s, count);
    for (int64_t i = 0; i < count; ++i) {
        JSValue item = jsTypedArrayItemForJS(globalObject, array, items->getIndex(globalObject, static_cast<unsigned>(i)));
        RETURN_IF_EXCEPTION(scope, { });
        // Beyond the end of an array that has shrunk meanwhile, JS stores nothing.
        array->putByIndexInline(globalObject, static_cast<uint64_t>(indices.start + i * indices.step), item, true);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return jsUndefined();
}

static JSValue jsTypedArrayDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (hasIndex(globalObject, key) || PySlice::is(key))
        return raiseTypeError(globalObject, scope, "a typed array cannot be resized: its items cannot be deleted"_s);
    RELEASE_AND_RETURN(scope, callSpecial(globalObject, jsTypedArrayPropertyMethod(globalObject, Special::DelItem), self, key));
}

// The first index from `start` on and before `end` at which the item equals the value. -1: there is none, or an exception.
static int64_t jsTypedArrayFind(JSGlobalObject* globalObject, JSArrayBufferView* array, JSValue value, int64_t start, int64_t end)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // __eq__() may do anything to the array.
    for (int64_t i = start; i < end && i < static_cast<int64_t>(array->length()); ++i) {
        JSValue item = array->getIndex(globalObject, static_cast<uint64_t>(i));
        RETURN_IF_EXCEPTION(scope, -1);
        bool isFound = isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, -1);
        if (isFound)
            return i;
    }
    return -1;
}

static JSValue jsTypedArrayContains(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t index = jsTypedArrayFind(globalObject, jsTypedArrayOf(self), value, 0, std::numeric_limits<int64_t>::max());
    RETURN_IF_EXCEPTION(scope, { });
    return jsBoolean(index >= 0);
}

static JSValue jsTypedArrayIndexMethod(JSGlobalObject* globalObject, JSValue self, JSValue value, JSValue startValue, JSValue endValue)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t start, end;
    if (failed(scope, unpackSliceBounds(globalObject, startValue, endValue, start, end)))
        return { };
    adjustSliceBounds(jsTypedArrayOf(self)->length(), start, end);
    int64_t index = jsTypedArrayFind(globalObject, jsTypedArrayOf(self), value, start, end);
    RETURN_IF_EXCEPTION(scope, { });
    if (index < 0)
        return raiseValueError(globalObject, scope, "the value is not in the typed array"_s);
    return makeInt(globalObject, index);
}

static JSValue jsTypedArrayCount(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = 0;
    for (int64_t from = 0;; ++count) {
        int64_t index = jsTypedArrayFind(globalObject, jsTypedArrayOf(self), value, from, std::numeric_limits<int64_t>::max());
        RETURN_IF_EXCEPTION(scope, { });
        if (index < 0)
            break;
        from = index + 1;
    }
    return makeInt(globalObject, count);
}

template<CompareOperator op>
static JSValue jsTypedArrayCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!hasBuffer(other))
        return runtime(globalObject).notImplemented();
    JSValue view = makeMemoryView(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue result = richCompare(globalObject, view, other, op);
    {
        ExceptionSetAside suspended(globalObject);
        PyMemoryView::from(view)->release(globalObject);
    }
    EXCEPTION_ASSERT(!!scope.exception() == !result);
    return result;
}

// What an index is counted from and checked against. (It takes no JS to tell, so any thread is told, as by memoryview(a).)
static JSValue jsTypedArrayLen(JSGlobalObject*, JSValue self)
{
    return smallInt(jsTypedArrayOf(self)->length());
}

static JSValue jsTypedArrayBool(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // A class of Python's that derives from one of these may have a __len__() of its own.
    if (auto* native = PyNativeFunction::tryFrom(typeOf(globalObject, self)->lookup(Special::Len)); !native || native->def().pointer.f1 != jsTypedArrayLen) [[unlikely]] {
        int64_t count = length(globalObject, self);
        RETURN_IF_EXCEPTION(scope, { });
        return jsBoolean(count);
    }
    return jsBoolean(jsTypedArrayOf(self)->length());
}

// iter(a): what is at 0, 1 and so on in the array itself, while there is anything, as `in` and index() look. Not a[0], a[1]: a class of Python's
// that derives from one of these may have a __getitem__() of its own, which iteration passes by, as that of a list or a bytearray does.
enum JSTypedArrayIteratorField : unsigned { JSTypedArrayIteratorArray, JSTypedArrayIteratorPosition, JSTypedArrayIteratorFieldCount };

static JSValue jsTypedArrayIteratorSelf(JSGlobalObject*, JSValue self) { return self; }

static JSValue jsTypedArrayIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    if (!ensureMainThreadForJS(globalObject))
        return { };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* record = uncheckedDowncast<PyRecord>(self.asCell());
    if (JSValue array = record->get(JSTypedArrayIteratorArray)) {
        int64_t position = smallIntValue(record->get(JSTypedArrayIteratorPosition));
        if (position < static_cast<int64_t>(jsTypedArrayOf(array)->length())) {
            record->set(vm, JSTypedArrayIteratorPosition, smallInt(position + 1));
            RELEASE_AND_RETURN(scope, jsTypedArrayOf(array)->getIndex(globalObject, static_cast<uint64_t>(position)));
        }
        // At its end it stays there, whatever the array grows to.
        record->set(vm, JSTypedArrayIteratorArray, { });
    }
    return raiseStopIteration(globalObject, scope);
}

static JSValue jsTypedArrayIteratorLengthHint(JSGlobalObject*, JSValue self)
{
    auto* record = uncheckedDowncast<PyRecord>(self.asCell());
    JSValue array = record->get(JSTypedArrayIteratorArray);
    return smallInt(array ? std::max<int64_t>(static_cast<int64_t>(jsTypedArrayOf(array)->length()) - smallIntValue(record->get(JSTypedArrayIteratorPosition)), 0) : 0);
}

static constexpr NativeDef jsTypedArrayIteratorMethods[] = {
    PY_SLOT("__iter__", jsTypedArrayIteratorSelf, "($self, /)"),
    PY_SLOT("__next__", jsTypedArrayIteratorNext, "($self, /)"),
    PY_METHOD("__length_hint__", jsTypedArrayIteratorLengthHint, "($self, /)"),
};
static constexpr TypeDef jsTypedArrayIteratorTypeDef { "_js.typed_array_iterator"_s, { TypeFlag::OwnLayout, TypeFlag::DisallowInstantiation }, jsTypedArrayIteratorMethods, { }, { } };

static JSValue jsTypedArrayIter(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = PyRecord::create(globalObject, runtime(globalObject).typeFor(jsTypedArrayIteratorTypeDef), JSTypedArrayIteratorFieldCount);
    record->set(getVM(globalObject), JSTypedArrayIteratorArray, self);
    record->set(getVM(globalObject), JSTypedArrayIteratorPosition, jsNumber(0));
    return record;
}

static constexpr NativeDef jsTypedArrayMethods[] = {
    PY_SLOT("__len__", jsTypedArrayLen, "($self, /)"),
    PY_SLOT("__bool__", jsTypedArrayBool, "($self, /)"),
    PY_SLOT("__iter__", jsTypedArrayIter, "($self, /)"),
    PY_SLOT("__getitem__", jsTypedArrayGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", jsTypedArraySetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", jsTypedArrayDelItem, "($self, key, /)"),
    PY_SLOT("__contains__", jsTypedArrayContains, "($self, value, /)"),
    PY_SLOT("__eq__", jsTypedArrayCompare<CompareOperator::Eq>, "($self, value, /)"),
    PY_SLOT("__ne__", jsTypedArrayCompare<CompareOperator::NotEq>, "($self, value, /)"),
    PY_METHOD("index", jsTypedArrayIndexMethod, "($self, value, start=0, stop=sys.maxsize, /)"),
    PY_METHOD("count", jsTypedArrayCount, "($self, value, /)"),
};
static constexpr TypeDef jsTypedArrayTypeDef { "_js.JSTypedArray"_s, { TypeFlag::DisallowInstantiation, TypeFlag::HasWeakref, TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Sequence }, jsTypedArrayMethods, { }, { } };
PyType* jsTypedArrayType(Runtime& rt)
{
    // (typeOf() comes by here for every typed array.)
    if (PyType* type = rt.typeIfMadeFor(jsTypedArrayTypeDef)) [[likely]]
        return type;
    PyType* type = rt.typeFor(jsTypedArrayTypeDef, rt.jsObjectType());
    type->dict()->set(rt.vm(), rt.name(Special::Hash), jsUndefined());
    type->didChange(rt.vm());
    return type;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
