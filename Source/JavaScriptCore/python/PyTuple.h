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

#pragma once

#include "JSObject.h"
#include "PyInstance.h"

namespace JSC {

// A tuple, or an instance of a class derived from it.
class PyTuple final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut | InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero | OverridesGetOwnPropertyNames;

    static size_t allocationSize(Checked<size_t> length)
    {
        return sizeof(PyTuple) + length * sizeof(WriteBarrier<Unknown>);
    }

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    // Filled with None. Whoever makes it fills it in before anything else sees it.
    static PyTuple* create(VM&, Structure*, unsigned length);
    static PyTuple* create(JSGlobalObject*, unsigned length);
    static PyTuple* create(JSGlobalObject*, std::span<const JSValue>);
    static PyTuple* create(JSGlobalObject*, std::initializer_list<JSValue>);
    static PyTuple* createFromArguments(JSGlobalObject*, const ArgList&);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    unsigned length() const { return m_length; }
    JSValue at(unsigned index) const
    {
        ASSERT(index < m_length);
        return values()[index].get();
    }
    void initializeAt(VM& vm, unsigned index, JSValue value)
    {
        ASSERT(index < m_length);
        values()[index].set(vm, this, value);
    }

    std::span<WriteBarrier<Unknown>> span() { return { values(), m_length }; }

    // To JavaScript it is like an array that cannot be changed.
    PYTHON_DECLARE_EXOTIC_METHODS
    static bool getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

private:
    PyTuple(VM& vm, Structure* structure, unsigned length)
        : Base(vm, structure)
        , m_length(length)
    {
    }

    WriteBarrier<Unknown>* values() { return std::bit_cast<WriteBarrier<Unknown>*>(this + 1); }
    const WriteBarrier<Unknown>* values() const { return std::bit_cast<const WriteBarrier<Unknown>*>(this + 1); }

    unsigned m_length;
};

inline bool isTuple(JSValue value) { return value.isCell() && value.asCell()->type() == PyTupleType; }

} // namespace JSC
