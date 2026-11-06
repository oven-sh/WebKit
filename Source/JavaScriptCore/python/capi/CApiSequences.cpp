/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CApi.h"

#include "JSCInlines.h"
#include "PyList.h"
#include "PyTuple.h"

#if PYTHON_HAS_CAPI

// PyTuple_*, PyList_*
//
// An item that C is lent is the value that the container holds: it is good for as long as the container holds it, which is CPython's rule.
// A reference that C hands over to a container is given up: from then on the container keeps the object alive, as it does for anybody.

namespace JSC {
namespace Python {
namespace CApi {

CAPI_FILE(Sequences)

// ---- tuple ----

// The items are None, where CPython has NULL, until they are set.
PY_CAPI Handle PyTuple_New(ssize_t length)
{
    CAPI_ENTER(nullptr);
    if (length < 0) {
        entry.raiseBadInternalCall();
        return nullptr;
    }
    if (static_cast<size_t>(length) > std::numeric_limits<int32_t>::max()) {
        entry.raise(entry.runtime().MemoryError(), String());
        return nullptr;
    }
    // (There is one empty tuple.)
    PyTuple* tuple = PyTuple::create(entry.globalObject, static_cast<unsigned>(length));
    if (length)
        tuple->setIsFilledByC();
    return entry.newReference(tuple);
}

PY_CAPI ssize_t PyTuple_Size(Handle object)
{
    CAPI_NOISY(-1);
    if (PyTuple* tuple = PyTuple::tryFrom(toJS(object))) [[likely]]
        return tuple->length();
    CAPI_ENTER(-1);
    entry.raiseBadInternalCall();
    return -1;
}

PY_CAPI Handle PyTuple_GetItem(Handle object, ssize_t index)
{
    CAPI_NOISY(nullptr);
    PyTuple* tuple = PyTuple::tryFrom(toJS(object));
    if (tuple && static_cast<size_t>(index) < tuple->length()) [[likely]]
        return toC(tuple->at(static_cast<unsigned>(index)));
    CAPI_ENTER(nullptr);
    if (!tuple)
        entry.raiseBadInternalCall();
    else
        entry.raise(entry.runtime().IndexError(), "tuple index out of range"_s);
    return nullptr;
}

// For a tuple that nobody else has seen yet. CPython goes by the count of references, and refuses if there is more than one. Nobody counts the program's here. What is known is
// asked: that PyTuple_New() has made the tuple, so what the program has made (a constant of its code) is as it was made, and that C owns one reference, so it has not given the
// tuple away. Takes over the reference to the item.
PY_CAPI int PyTuple_SetItem(Handle object, ssize_t index, Handle item)
{
    CAPI_NOISY(-1);
    TakenOver takenOver(item);
    PyTuple* tuple = PyTuple::tryFrom(toJS(object));
    bool isBeingFilled = tuple && tuple->isFilledByC() && interpreter().counts.get(tuple) == 1;
    if (isBeingFilled && static_cast<size_t>(index) < tuple->length()) [[likely]] {
        tuple->initializeAt(interpreter().vm, static_cast<unsigned>(index), item ? toJS(item) : jsUndefined());
        // Somebody may have asked before it was full.
        tuple->setCachedHash(-1);
        return 0;
    }
    CAPI_ENTER(-1);
    if (!isBeingFilled)
        entry.raiseBadInternalCall();
    else
        entry.raise(entry.runtime().IndexError(), "tuple assignment index out of range"_s);
    return -1;
}

// ---- list ----

// The items are None, where CPython has NULL, until they are set.
PY_CAPI Handle PyList_New(ssize_t length)
{
    CAPI_ENTER(nullptr);
    if (length < 0) {
        entry.raiseBadInternalCall();
        return nullptr;
    }
    if (static_cast<size_t>(length) > std::numeric_limits<int32_t>::max()) {
        entry.raise(entry.runtime().MemoryError(), String());
        return nullptr;
    }
    JSArray* list = newList(entry.globalObject, static_cast<unsigned>(length));
    if (entry.failed())
        return nullptr;
    listInsertGap(entry.globalObject, list, 0, static_cast<unsigned>(length));
    return entry.newReference(list);
}

PY_CAPI ssize_t PyList_Size(Handle object)
{
    CAPI_NOISY(-1);
    if (JSArray* list = tryAsList(toJS(object))) [[likely]]
        return listLength(list);
    CAPI_ENTER(-1);
    entry.raiseBadInternalCall();
    return -1;
}

PY_CAPI Handle PyList_GetItem(Handle object, ssize_t index)
{
    CAPI_NOISY(nullptr);
    JSArray* list = tryAsList(toJS(object));
    if (list && static_cast<size_t>(index) < listLength(list)) [[likely]]
        return toC(listGet(list, static_cast<unsigned>(index)));
    CAPI_ENTER(nullptr);
    if (!list)
        entry.raiseBadInternalCall();
    else
        entry.raise(entry.runtime().IndexError(), "list index out of range"_s);
    return nullptr;
}

// Takes over the reference to the item.
PY_CAPI int PyList_SetItem(Handle object, ssize_t index, Handle item)
{
    CAPI_NOISY(-1);
    TakenOver takenOver(item);
    CAPI_ENTER(-1);
    JSArray* list = tryAsList(toJS(object));
    if (!list) {
        entry.raiseBadInternalCall();
        return -1;
    }
    if (static_cast<size_t>(index) >= listLength(list)) {
        entry.raise(entry.runtime().IndexError(), "list assignment index out of range"_s);
        return -1;
    }
    listSet(entry.globalObject, list, static_cast<unsigned>(index), item ? toJS(item) : jsUndefined());
    return entry.status();
}

PY_CAPI int PyList_Append(Handle object, Handle item)
{
    CAPI_ENTER(-1);
    JSArray* list = tryAsList(toJS(object));
    if (!list || !item) {
        entry.raiseBadInternalCall();
        return -1;
    }
    listAppend(entry.globalObject, list, toJS(item));
    return entry.status();
}

} // namespace CApi
} // namespace Python
} // namespace JSC

#endif // PYTHON_HAS_CAPI
