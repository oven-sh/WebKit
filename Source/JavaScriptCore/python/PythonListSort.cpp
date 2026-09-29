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
#include "PyTuple.h"
#include "PythonIO.h"
#include "PythonListSortKernel.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonOperators.h"
#include "PythonSequences.h"

// list.sort(): the part of Objects/listobject.c of CPython that has to do with what is sorted. The part that has to do with the order of things is PythonListSortKernel.h.

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace Python {

namespace {

using namespace ListSortKernel;

struct SortContext {
    JSGlobalObject* globalObject;
    // What stands for ms->key_richcompare: see richCompareOf().
    const void* keyRichCompare { nullptr };
};

SortContext& contextOf(MergeState* ms) { return *static_cast<SortContext*>(ms->context); }

// What stands for tp_richcompare, which is only ever asked whether it is the same as another. A class of a program's has one function for that whatever the class, which looks for the method. What is written in C++ has its own.
const void* richCompareOf(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = globalObject->vm();
    static const char ofAProgram = 0;
    auto* native = dynamicDowncast<PyNativeFunction>(typeOf(globalObject, value)->lookup(vm, vm.pythonNames().dunder_lt));
    return native ? std::bit_cast<const void*>(native->nativeFunction().taggedPtr()) : &ofAProgram;
}

// PyObject_RichCompareBool(v, w, Py_LT): 1 if it is less, 0 if it is not, and -1 if it raised.
int isLess(JSGlobalObject* globalObject, JSValue v, JSValue w)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    JSValue result = compare(globalObject, ComparisonOperator::Lt, v, w);
    RETURN_IF_EXCEPTION(scope, -1);
    if (result.isBoolean())
        return result.asBoolean();
    bool truth = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, -1);
    return truth;
}

// What will do for any two things
int safeObjectCompare(Item v, Item w, MergeState* ms)
{
    return isLess(contextOf(ms).globalObject, JSValue::decode(v), JSValue::decode(w));
}

// For things that are all of one class. The class is asked outright, and nothing is asked of the other one unless it has nothing to say. Then it is asked again, in the ordinary way: a __lt__() that gives NotImplemented is
// called twice.
int unsafeObjectCompare(Item encodedV, Item encodedW, MergeState* ms)
{
    JSGlobalObject* globalObject = contextOf(ms).globalObject;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue v = JSValue::decode(encodedV);
    JSValue w = JSValue::decode(encodedW);
    // What it is compared by can have been changed by what has been run since it was looked at.
    if (richCompareOf(globalObject, v) != contextOf(ms).keyRichCompare)
        RELEASE_AND_RETURN(scope, isLess(globalObject, v, w));
    JSValue self;
    JSValue method = lookupSpecial(globalObject, v, vm.pythonNames().dunder_lt, self);
    RETURN_IF_EXCEPTION(scope, -1);
    JSValue result = globalObject->pyRealm()->notImplemented();
    if (method) {
        result = callMethod(globalObject, method, self, w);
        RETURN_IF_EXCEPTION(scope, -1);
    }
    if (result == globalObject->pyRealm()->notImplemented())
        RELEASE_AND_RETURN(scope, isLess(globalObject, v, w));
    if (result.isBoolean())
        return result.asBoolean();
    bool truth = isTrue(globalObject, result);
    RETURN_IF_EXCEPTION(scope, -1);
    return truth;
}

// For strs that have nothing in them that does not fit in a byte. None of them is a rope: see the check below.
int unsafeLatinCompare(Item v, Item w, MergeState*)
{
    auto a = asString(JSValue::decode(v))->tryGetValue().data.span8();
    auto b = asString(JSValue::decode(w))->tryGetValue().data.span8();
    int result = memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
    return result ? result < 0 : a.size() < b.size();
}

// For ints that are not large
int unsafeLongCompare(Item v, Item w, MergeState*)
{
    return classify(JSValue::decode(v)).small < classify(JSValue::decode(w)).small;
}

int unsafeFloatCompare(Item v, Item w, MergeState*)
{
    return classify(JSValue::decode(v)).real < classify(JSValue::decode(w)).real;
}

// For tuples, none of which is empty. The first of each is compared as the check below found that the first of all of them can be, since it is seldom that any more of a tuple has to be looked at.
int unsafeTupleCompare(Item encodedV, Item encodedW, MergeState* ms)
{
    JSGlobalObject* globalObject = contextOf(ms).globalObject;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyTuple* v = asTuple(JSValue::decode(encodedV));
    PyTuple* w = asTuple(JSValue::decode(encodedW));
    unsigned vLength = v->length();
    unsigned wLength = w->length();
    unsigned i = 0;
    for (; i < vLength && i < wLength; ++i) {
        bool isSame = isEqual(globalObject, v->at(i), w->at(i));
        RETURN_IF_EXCEPTION(scope, -1);
        if (!isSame)
            break;
    }
    if (i >= vLength || i >= wLength)
        return vLength < wLength;
    if (!i)
        RELEASE_AND_RETURN(scope, ms->tuple_elem_compare(JSValue::encode(v->at(i)), JSValue::encode(w->at(i)), ms));
    RELEASE_AND_RETURN(scope, isLess(globalObject, v->at(i), w->at(i)));
}

void raiseNoMemory(MergeState* ms)
{
    JSGlobalObject* globalObject = contextOf(ms).globalObject;
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    raiseMemoryError(globalObject, scope);
}

// The middle of list_sort_impl(): from when there are keys, if there are to be any, to when it is all in order. `keys` is null if things are sorted by what they are. Everything in either is kept from being collected by whoever
// calls this. Whatever comes of it, what is in `values` afterwards is what was in it before, in some order. False if it raised.
bool sortItems(JSGlobalObject* globalObject, Item* values, Item* keys, ptrdiff_t size, bool reverse)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRealm* realm = globalObject->pyRealm();
    SortContext context { globalObject };
    MergeState ms;
    ms.context = &context;
    ms.no_memory = raiseNoMemory;
    sortslice lo { keys ? keys : values, keys ? values : nullptr };
    auto keyAt = [&] (ptrdiff_t i) { return JSValue::decode(lo.keys[i]); };
    auto isNonEmptyTuple = [&] (JSValue value) { return isTuple(value) && typeOf(globalObject, value) == realm->typeTuple() && asTuple(value)->length(); };

    // Which way of comparing will do is found by looking at every key first, for what would take long to look for every time that two are compared.
    if (size > 1) {
        // The first is taken to be like the rest, and then that is looked into.
        bool keysAreInTuples = isNonEmptyTuple(keyAt(0));
        PyType* keyType = typeOf(globalObject, keysAreInTuples ? asTuple(keyAt(0))->at(0) : keyAt(0));
        JSValue firstKey = keysAreInTuples ? asTuple(keyAt(0))->at(0) : keyAt(0);
        bool keysAreAllSameType = true;
        bool stringsAreLatin = true;
        bool intsAreBounded = true;
        for (ptrdiff_t i = 0; i < size; ++i) {
            if (keysAreInTuples && !isNonEmptyTuple(keyAt(i))) {
                keysAreInTuples = false;
                keysAreAllSameType = false;
                break;
            }
            JSValue key = keysAreInTuples ? asTuple(keyAt(i))->at(0) : keyAt(i);
            if (typeOf(globalObject, key) != keyType) {
                keysAreAllSameType = false;
                // The rest have still to be found to be tuples.
                if (!keysAreInTuples)
                    break;
            }
            if (keysAreAllSameType) {
                if (keyType == realm->typeInt() && intsAreBounded && classify(key).kind != Number::Kind::Small)
                    intsAreBounded = false;
                else if (keyType == realm->typeStr() && stringsAreLatin) {
                    // What it is made of has to be all in one place to be compared a byte at a time.
                    auto text = asString(key)->value(globalObject);
                    RETURN_IF_EXCEPTION(scope, false);
                    if (!text->is8Bit())
                        stringsAreLatin = false;
                }
            }
        }
        if (keysAreAllSameType) {
            if (keyType == realm->typeStr() && stringsAreLatin)
                ms.key_compare = unsafeLatinCompare;
            else if (keyType == realm->typeInt() && intsAreBounded)
                ms.key_compare = unsafeLongCompare;
            else if (keyType == realm->typeFloat())
                ms.key_compare = unsafeFloatCompare;
            else {
                context.keyRichCompare = richCompareOf(globalObject, firstKey);
                ms.key_compare = unsafeObjectCompare;
            }
        } else
            ms.key_compare = safeObjectCompare;
        if (keysAreInTuples) {
            // Not tuples in tuples
            ms.tuple_elem_compare = keyType == realm->typeTuple() ? safeObjectCompare : ms.key_compare;
            ms.key_compare = unsafeTupleCompare;
        }
    }

    merge_init(&ms, size, !!keys, &lo);
    bool succeeded = [&] {
        ptrdiff_t remaining = size;
        if (remaining < 2)
            return true;
        // Those that are equal stay in the order that they were in, backwards as well: it is turned round first, sorted forwards, and turned round again.
        if (reverse) {
            if (keys)
                reverse_slice(keys, keys + size);
            reverse_slice(values, values + size);
        }
        // Once along it from left to right, finding what is in order already, and making up what is too short of that to the least that is worth having.
        ptrdiff_t minrun = merge_compute_minrun(remaining);
        do {
            ptrdiff_t n = count_run(&ms, &lo, remaining);
            if (n < 0)
                return false;
            if (n < minrun) {
                ptrdiff_t force = remaining <= minrun ? remaining : minrun;
                if (binarysort(&ms, &lo, force, n) < 0)
                    return false;
                n = force;
            }
            if (found_new_run(&ms, n) < 0)
                return false;
            ms.pending[ms.n].base = lo;
            ms.pending[ms.n].len = n;
            ++ms.n;
            sortslice_advance(&lo, n);
            remaining -= n;
        } while (remaining);
        return merge_force_collapse(&ms) >= 0;
    }();
    if (reverse && size > 1)
        reverse_slice(values, values + size);
    merge_freemem(&ms);
    ASSERT(succeeded == !scope.exception());
    return succeeded;
}

// What is to be sorted, and what it is to be sorted by, where the kernel can move them about. What is here is not looked at by the collector, which is told of it all by `kept`. It does not move things, so that is enough.
struct Sorting {
    Vector<Item> values;
    Vector<Item> keys;
    MarkedArgumentBuffer kept;
};

// The keys, by calling the function for each. False if it raised.
bool computeKeys(JSGlobalObject* globalObject, Sorting& sorting, JSValue keyFunction)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (!sorting.keys.tryReserveCapacity(sorting.values.size())) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    for (Item value : sorting.values) {
        JSValue key = call(globalObject, keyFunction, JSValue::decode(value));
        RETURN_IF_EXCEPTION(scope, false);
        sorting.kept.append(key);
        sorting.keys.append(JSValue::encode(key));
    }
    if (sorting.kept.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    return true;
}

} // anonymous namespace

// PyList_Sort(), of what is not yet a list
bool sortValues(JSGlobalObject* globalObject, MarkedArgumentBuffer& values, MarkedArgumentBuffer& sorted)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Vector<Item> items;
    if (!items.tryReserveCapacity(values.size())) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    for (size_t i = 0; i < values.size(); ++i)
        items.append(JSValue::encode(values.at(i)));
    sortItems(globalObject, items.mutableSpan().data(), nullptr, static_cast<ptrdiff_t>(items.size()), false);
    RETURN_IF_EXCEPTION(scope, false);
    for (Item item : items)
        sorted.append(JSValue::decode(item));
    return true;
}

// list_sort_impl(). False if it raised.
bool sortList(JSGlobalObject* globalObject, JSArray* self, JSValue keyFunction, bool reverse)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (keyFunction && isNone(keyFunction))
        keyFunction = { };

    Sorting sorting;
    unsigned size = self->length();
    if (!sorting.values.tryReserveCapacity(size)) {
        raiseMemoryError(globalObject, scope);
        return false;
    }
    for (unsigned i = 0; i < size; ++i) {
        JSValue value = listGet(globalObject, self, i);
        RETURN_IF_EXCEPTION(scope, false);
        sorting.kept.append(value);
        sorting.values.append(JSValue::encode(value));
    }
    if (sorting.kept.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return false;
    }

    // There is nothing in the list meanwhile, so that what is run to compare things can do nothing to what is being sorted. It has not even room for anything, which is how it is told afterwards whether it has been given any.
    // That is of a list such as Python makes. JavaScript can have made one that cannot be emptied, or cannot be filled again, or has an element that is not to be written to, which taking it out and putting another in would get
    // round. What is in such a one stays where it is, and what it comes to is written over it, if it can be.
    bool canTellIfChanged = false;
    if (self->isStructureExtensible() && self->hasVectorOfElements()) {
        self->setLength(globalObject, 0, true);
        RETURN_IF_EXCEPTION(scope, false);
        canTellIfChanged = self->releaseVector(vm);
    }

    bool hasKeys = false;
    bool succeeded = true;
    if (keyFunction) {
        succeeded = computeKeys(globalObject, sorting, keyFunction);
        hasKeys = succeeded;
    }
    bool reachedSorting = succeeded;
    if (succeeded)
        succeeded = sortItems(globalObject, sorting.values.mutableSpan().data(), hasKeys ? sorting.keys.mutableSpan().data() : nullptr, size, reverse);

    // What was in it goes back, in whatever order it is now in, in place of anything that has been put there since, whether or not anything has gone wrong.
    Exception* raised = succeeded ? nullptr : takeRaisedException(vm);
    bool wasChanged = canTellIfChanged && (self->length() || self->hasVector());
    MarkedArgumentBuffer inOrder;
    for (Item item : sorting.values)
        inOrder.append(JSValue::decode(item));
    listReplaceRange(globalObject, self, 0, self->length(), inOrder);
    if (raised) {
        if (scope.exception() && !scope.tryClearException())
            return false;
        restoreRaisedException(globalObject, raised);
        return false;
    }
    RETURN_IF_EXCEPTION(scope, false);
    if (wasChanged && reachedSorting) {
        raiseValueError(globalObject, scope, "list modified during sort"_s);
        return false;
    }
    return true;
}

} } // namespace JSC::Python

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
