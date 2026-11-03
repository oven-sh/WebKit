/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

namespace JSC {
namespace Python {

class PyRange final : public PyObject {
public:
    PY_DECLARE_CELL(PyRange, PyObject, PyRangeType);
    DECLARE_VISIT_CHILDREN;

    // All exact ints. `length` is what they imply.
    static PyRange* create(JSGlobalObject*, JSValue start, JSValue stop, JSValue step, JSValue length);

    JSValue start() const { return m_start.get(); }
    JSValue stop() const { return m_stop.get(); }
    JSValue step() const { return m_step.get(); }
    JSValue length() const { return m_length.get(); }

private:
    PyRange(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_start;
    WriteBarrier<Unknown> m_stop;
    WriteBarrier<Unknown> m_step;
    WriteBarrier<Unknown> m_length;
};

class PySlice final : public PyObject {
public:
    PY_DECLARE_CELL(PySlice, PyObject, PySliceType);
    DECLARE_VISIT_CHILDREN;

    static PySlice* create(JSGlobalObject*, JSValue start, JSValue stop, JSValue step);

    JSValue start() const { return m_start.get(); }
    JSValue stop() const { return m_stop.get(); }
    JSValue step() const { return m_step.get(); }

    struct Indices {
        int64_t start;
        int64_t stop;
        int64_t step;
        int64_t length; // How many items the slice selects.
    };
    // For a sequence of that length, which nothing can change. False: an exception.
    bool indices(JSGlobalObject* globalObject, int64_t sequenceLength, Indices& result) const
    {
        if (!unpack(globalObject, result))
            return false;
        adjust(sequenceLength, result);
        return true;
    }
    // The same in two steps. The first may call __index__ methods, which may do anything to the sequence the slice is for. So
    // its length is asked for after that, for the second, and whatever else was known about it is found out again.
    bool unpack(JSGlobalObject*, Indices&) const;
    static void adjust(int64_t sequenceLength, Indices&);

private:
    PySlice(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_start;
    WriteBarrier<Unknown> m_stop;
    WriteBarrier<Unknown> m_step;
};

} // namespace Python
} // namespace JSC
