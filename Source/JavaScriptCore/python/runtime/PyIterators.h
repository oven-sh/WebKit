/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyDict.h"

namespace JSC {
namespace Python {

// The iterators of the built-in containers.
class PyIterator final : public PyObject {
public:
    PY_DECLARE_CELL(PyIterator, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    enum class Kind : uint8_t {
        List,
        ListReverse,
        Tuple,
        Str,
        Bytes,
        ByteArray,
        Range, // position: the next value; step; remaining
        LongRange, // target: the next value; extra: a tuple (step, remaining)
        DictKeys,
        DictValues,
        DictItems,
        DictReverseKeys,
        DictReverseValues,
        DictReverseItems,
        Set,
        Sequence, // Anything with __getitem__.
        Callable, // iter(callable, sentinel)
    };

    static PyIterator* create(JSGlobalObject*, Kind, JSValue target);

    Kind kind() const { return m_kind; }
    JSValue target() const { return m_target.get(); } // Empty once exhausted.
    void setTarget(VM& vm, JSValue value) { m_target.set(vm, this, value); }
    JSValue extra() const { return m_extra.get(); }
    void setExtra(VM& vm, JSValue value) { m_extra.set(vm, this, value); }

    // Empty, with no exception: exhausted.
    JSValue next(JSGlobalObject*);

    int64_t position { 0 };
    int64_t step { 0 }; // For a dict: how many items are still to come.
    int64_t remaining { 0 }; // For a dict or a set: the size it had, which must not change.

    static constexpr ptrdiff_t offsetOfPosition() { return OBJECT_OFFSETOF(PyIterator, position); }
    static constexpr ptrdiff_t offsetOfStep() { return OBJECT_OFFSETOF(PyIterator, step); }
    static constexpr ptrdiff_t offsetOfRemaining() { return OBJECT_OFFSETOF(PyIterator, remaining); }
    static constexpr ptrdiff_t offsetOfTarget() { return OBJECT_OFFSETOF(PyIterator, m_target); }
    static constexpr ptrdiff_t offsetOfKind() { return OBJECT_OFFSETOF(PyIterator, m_kind); }

private:
    PyIterator(VM&, Structure*, PyType*, Kind);

    WriteBarrier<Unknown> m_target;
    WriteBarrier<Unknown> m_extra;
    Kind m_kind;
};

// dict.keys(), dict.values(), dict.items()
class PyDictView final : public PyObject {
public:
    PY_DECLARE_CELL(PyDictView, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    enum class Kind : uint8_t { Keys, Values, Items };

    static PyDictView* create(JSGlobalObject*, Kind, PyDict*);

    Kind kind() const { return m_kind; }
    PyDict* dict() const { return m_dict.get(); }

private:
    PyDictView(VM&, Structure*, PyType*, Kind);

    WriteBarrier<PyDict> m_dict;
    Kind m_kind;
};

// The iterators that are made from others: enumerate, zip, map, filter, reversed.
class PyMultiIterator final : public PyObject {
public:
    PY_DECLARE_CELL(PyMultiIterator, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    enum class Kind : uint8_t {
        Enumerate, // first: the iterator; second: the next count
        Zip, // first: a tuple of iterators; flag: strict
        Map, // first: a tuple of iterators; second: the function; flag: strict
        Filter, // first: the iterator; second: the function, or None
        Reversed, // first: the sequence; position: the next index
    };

    static PyMultiIterator* create(VM&, Structure*, PyType*, Kind);

    Kind kind() const { return m_kind; }
    JSValue next(JSGlobalObject*);

    WriteBarrier<Unknown> first;
    WriteBarrier<Unknown> second;
    int64_t position { 0 };
    bool flag { false };

private:
    PyMultiIterator(VM&, Structure*, PyType*, Kind);

    Kind m_kind;
};

} // namespace Python
} // namespace JSC
