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

namespace JSC {

// What makes something of Python's an exotic object to JavaScript: see PyType.h. Every kind of cell that can have attributes of its own has these.
#define PYTHON_DECLARE_EXOTIC_METHODS \
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&); \
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&); \
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&); \
    static bool defineOwnProperty(JSObject*, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool shouldThrow); \
    static bool preventExtensions(JSObject*, JSGlobalObject*);

// `isOrdinary(vm, name)` says which properties are the cell's own business and no attribute: the items of a tuple, the length of an array.
#define PYTHON_DEFINE_EXOTIC_METHODS(ClassName, isOrdinary) \
    bool ClassName::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot) \
    { \
        if (name.isSymbol() || isOrdinary(globalObject->vm(), name)) \
            return Base::getOwnPropertySlot(object, globalObject, name, slot); \
        return Python::getOwnPropertySlotFromJavaScript(object, globalObject, name, slot, Base::getOwnPropertySlot); \
    } \
    bool ClassName::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot) \
    { \
        if (name.isSymbol() || isOrdinary(globalObject->vm(), name) || slot.thisValue() != JSValue(cell)) \
            return Base::put(cell, globalObject, name, value, slot); \
        return Python::setPropertyFromJavaScript(globalObject, cell, name, value, slot); \
    } \
    bool ClassName::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot) \
    { \
        if (name.isSymbol() || isOrdinary(globalObject->vm(), name)) \
            return Base::deleteProperty(cell, globalObject, name, slot); \
        return Python::deletePropertyFromJavaScript(globalObject, cell, name); \
    } \
    bool ClassName::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow) \
    { \
        if (name.isSymbol() || isOrdinary(globalObject->vm(), name)) \
            return Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow); \
        return Python::definePropertyFromJavaScript(globalObject, object, name, descriptor, shouldThrow); \
    } \
    bool ClassName::preventExtensions(JSObject*, JSGlobalObject*) \
    { \
        /* Whether attributes can be set is for Python to say, one at a time. */ \
        return false; \
    }

// An instance of a class that is written in Python, or of `object`. Its attributes are its properties, and its class is its prototype.
// It is a JSFinalObject in all but name: it has inline storage, and nothing else of its own.
class PyInstance final : public JSObjectWithButterfly {
public:
    using Base = JSObjectWithButterfly;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData | OverridesGetOwnPropertySlot | OverridesPut;
    static constexpr unsigned defaultInlineCapacity = 6;

    static size_t allocationSize(Checked<size_t> inlineCapacity)
    {
        return sizeof(JSObjectWithButterfly) + inlineCapacity * sizeof(WriteBarrierBase<Unknown>);
    }

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;

    static PyInstance* create(VM&, Structure*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype, unsigned inlineCapacity = defaultInlineCapacity);

    // It can be called if its class has __call__.
    static CallData getCallData(JSCell*);

    PYTHON_DECLARE_EXOTIC_METHODS

private:
    PyInstance(VM& vm, Structure* structure, size_t inlineCapacity)
        : Base(vm, structure, nullptr)
    {
        memset(inlineStorageUnsafe(), 0, inlineCapacity * sizeof(EncodedJSValue));
    }
};

} // namespace JSC
