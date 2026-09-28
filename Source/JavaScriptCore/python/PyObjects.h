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

#include "ErrorInstance.h"
#include "JSArray.h"
#include "JSObject.h"
#include "JSTypedArrays.h"
#include "PyInstance.h"
#include "PyRealm.h"

namespace JSC {

class PyDict;

// The small built-in kinds of object. Each is a cell with a few fields, and its class is its prototype, as for any other.

#define PYTHON_CELL_BOILERPLATE(ClassName) \
    using Base = JSNonFinalObject; \
    template<typename CellType, SubspaceAccess> \
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); } \
    DECLARE_EXPORT_INFO; \
    DECLARE_VISIT_CHILDREN; \
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

// instance.method, when it is not called at once.
class PyBoundMethod final : public JSNonFinalObject {
public:
    static constexpr unsigned StructureFlags = JSNonFinalObject::StructureFlags | OverridesGetCallData;
    PYTHON_CELL_BOILERPLATE(PyBoundMethod)

    static PyBoundMethod* create(JSGlobalObject*, JSValue function, JSValue self);
    JSValue function() const { return m_function.get(); }
    JSValue self() const { return m_self.get(); }
    static CallData getCallData(JSCell*);

private:
    PyBoundMethod(VM& vm, Structure* structure, JSValue function, JSValue self)
        : Base(vm, structure)
        , m_function(function, WriteBarrierEarlyInit)
        , m_self(self, WriteBarrierEarlyInit)
    {
    }

    WriteBarrier<Unknown> m_function;
    WriteBarrier<Unknown> m_self;
};

class PyRange final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyRange)

    // FIXME: A range whose ends do not fit an int64.
    static PyRange* create(JSGlobalObject*, int64_t start, int64_t stop, int64_t step);
    int64_t start() const { return m_start; }
    int64_t stop() const { return m_stop; }
    int64_t step() const { return m_step; }
    int64_t length() const { return m_length; }

private:
    PyRange(VM& vm, Structure* structure, int64_t start, int64_t stop, int64_t step);

    int64_t m_start;
    int64_t m_stop;
    int64_t m_step;
    int64_t m_length;
};

class PyComplex final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyComplex)

    static PyComplex* create(VM&, Structure*, double real, double imaginary);
    static PyComplex* create(JSGlobalObject*, double real, double imaginary);
    double real() const { return m_real; }
    double imaginary() const { return m_imaginary; }

private:
    PyComplex(VM& vm, Structure* structure, double real, double imaginary)
        : Base(vm, structure)
        , m_real(real)
        , m_imaginary(imaginary)
    {
    }

    double m_real;
    double m_imaginary;
};

// A window on the bytes of something else. It holds no pointer into them, only where they are in it, and looks again each time, so what it
// is a view of can be resized or detached under it and the worst that comes of it is an error.
class PyMemoryView final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyMemoryView)

    static PyMemoryView* create(JSGlobalObject*, JSValue object, char format, unsigned itemSize, int64_t offset, int64_t length, int64_t stride, bool isReadOnly);

    JSValue object() const { return m_object.get(); }
    char format() const { return m_format; }
    unsigned itemSize() const { return m_itemSize; }
    int64_t offset() const { return m_offset; } // In bytes, of the first item.
    int64_t length() const { return m_length; } // In items.
    int64_t stride() const { return m_stride; } // In bytes, from one item to the next. It can be negative.
    bool isReadOnly() const { return m_isReadOnly; }
    bool isReleased() const { return !m_object; }
    void release() { m_object.clear(); }
    bool isContiguous() const { return m_stride == static_cast<int64_t>(m_itemSize) || m_length <= 1; }

    // All of it, if its items are one after another and are still there.
    std::optional<std::span<const uint8_t>> contiguousSpan() const;
    // The bytes of one item. Empty if it is no longer there.
    std::span<uint8_t> item(int64_t index) const;

private:
    PyMemoryView(VM& vm, Structure* structure, JSValue object, char format, unsigned itemSize, int64_t offset, int64_t length, int64_t stride, bool isReadOnly)
        : Base(vm, structure)
        , m_object(object, WriteBarrierEarlyInit)
        , m_offset(offset)
        , m_length(length)
        , m_stride(stride)
        , m_itemSize(itemSize)
        , m_format(format)
        , m_isReadOnly(isReadOnly)
    {
    }

    WriteBarrier<Unknown> m_object;
    int64_t m_offset;
    int64_t m_length;
    int64_t m_stride;
    unsigned m_itemSize;
    char m_format;
    bool m_isReadOnly;
};

class PySlice final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PySlice)

    static PySlice* create(JSGlobalObject*, JSValue start, JSValue stop, JSValue step);
    JSValue start() const { return m_start.get(); }
    JSValue stop() const { return m_stop.get(); }
    JSValue step() const { return m_step.get(); }

    struct Indices {
        int64_t start;
        int64_t stop;
        int64_t step;
        int64_t length; // How many it selects.
    };
    // What it selects of a sequence of the length. Nothing if it raised.
    std::optional<Indices> indices(JSGlobalObject*, int64_t length) const;

private:
    PySlice(VM& vm, Structure* structure, JSValue start, JSValue stop, JSValue step)
        : Base(vm, structure)
        , m_start(start, WriteBarrierEarlyInit)
        , m_stop(stop, WriteBarrierEarlyInit)
        , m_step(step, WriteBarrierEarlyInit)
    {
    }

    WriteBarrier<Unknown> m_start;
    WriteBarrier<Unknown> m_stop;
    WriteBarrier<Unknown> m_step;
};

// Everything that is iterated by C++. What the fields are for depends on the kind.
class PyIterator final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyIterator)

    enum class Kind : uint8_t {
        List, // a: the list. index
        ListReverse,
        Tuple,
        Range, // index: the next value. stop: how many are left. step
        Str, // a: the string. index, in code units
        Bytes,
        DictKeys, // a: the dict. index: the entry. stop: its size when this began
        DictValues,
        DictItems,
        DictReverseKeys,
        Set,
        Sequence, // a: what has __getitem__. index
        Callable, // a: the callable. b: the sentinel
        Enumerate, // a: an iterator. index
        Zip, // a: a tuple of iterators. index: whether it is strict
        Map, // a: the function. b: a tuple of iterators
        Filter, // a: the function or None. b: an iterator
        Reversed, // a: a sequence. index
        JavaScript, // a: a JavaScript iterator. b: its next
    };

    static PyIterator* create(JSGlobalObject*, Kind, JSValue a = JSValue(), JSValue b = JSValue(), int64_t index = 0, int64_t stop = 0, int64_t step = 0);
    // One of the kinds that go through a dict.
    static PyIterator* create(JSGlobalObject*, Kind, PyDict*);
    static BuiltinType typeFor(Kind);

    Kind kind() const { return m_kind; }
    // Empty when there is no more.
    JSValue next(JSGlobalObject*);

    JSValue a() const { return m_a.get(); }
    JSValue b() const { return m_b.get(); }
    int64_t index() const { return m_index; }
    int64_t remaining() const { return m_stop; }

private:
    PyIterator(VM& vm, Structure* structure, Kind kind, JSValue a, JSValue b, int64_t index, int64_t stop, int64_t step)
        : Base(vm, structure)
        , m_kind(kind)
        , m_a(a, WriteBarrierEarlyInit)
        , m_b(b, WriteBarrierEarlyInit)
        , m_index(index)
        , m_stop(stop)
        , m_step(step)
    {
    }

    void finish() { m_a.clear(); }

    Kind m_kind;
    WriteBarrier<Unknown> m_a;
    WriteBarrier<Unknown> m_b;
    int64_t m_index;
    int64_t m_stop;
    int64_t m_step;
};

// Objects with up to four values in them and nothing else, told apart by their class: property, staticmethod, classmethod, super, the
// views of a dict, NotImplemented and Ellipsis.
class PyNativeObject final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyNativeObject)
    static constexpr unsigned numberOfFields = 4;

    static PyNativeObject* create(VM&, Structure*);
    static PyNativeObject* create(JSGlobalObject*, BuiltinType, JSValue = JSValue(), JSValue = JSValue(), JSValue = JSValue(), JSValue = JSValue());

    JSValue field(unsigned index) const { return m_fields[index].get(); }
    void setField(VM& vm, unsigned index, JSValue value) { m_fields[index].set(vm, this, value); }

private:
    PyNativeObject(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }

    WriteBarrier<Unknown> m_fields[numberOfFields];
};

// An attribute of a built-in type that is worked out by C++: function.__name__.
class PyGetSetDescriptor final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyGetSetDescriptor)

    using Getter = JSValue (*)(JSGlobalObject*, JSValue self);
    using Setter = void (*)(JSGlobalObject*, JSValue self, JSValue value); // The value is empty to delete.

    // A member is what in CPython is a field of a C struct. It differs in what it is called and in what it says when it cannot be set.
    static PyGetSetDescriptor* create(JSGlobalObject*, PyType* owner, const String& name, Getter, Setter, bool isMember = false);
    bool isMember() const { return m_isMember; }
    Getter getter() const { return m_getter; }
    Setter setter() const { return m_setter; }
    PyType* owner() const { return m_owner.get(); }
    JSString* name() const { return m_name.get(); }

private:
    PyGetSetDescriptor(VM& vm, Structure* structure, Getter getter, Setter setter)
        : Base(vm, structure)
        , m_getter(getter)
        , m_setter(setter)
    {
    }

    Getter m_getter;
    Setter m_setter;
    bool m_isMember { false };
    WriteBarrier<PyType> m_owner;
    WriteBarrier<JSString> m_name;
};

// Where the global variables of a module are: they are its properties. Its prototype is the builtins' namespace, and that one's is
// one that raises NameError, so that looking up a global is get_by_id. No Python code ever sees one.
class PyNamespace final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyNamespace)

    static PyNamespace* create(VM&, Structure*);

private:
    PyNamespace(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

// The last in that chain. It has every property, and to get any of them raises NameError.
class PyNameErrorRaiser final : public JSNonFinalObject {
public:
    static constexpr unsigned StructureFlags = JSNonFinalObject::StructureFlags | OverridesGetOwnPropertySlot | GetOwnPropertySlotMayBeWrongAboutDontEnum | GetOwnPropertySlotIsImpureForPropertyAbsence;
    PYTHON_CELL_BOILERPLATE(PyNameErrorRaiser)

    static PyNameErrorRaiser* create(VM&, JSGlobalObject*);
    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);

private:
    PyNameErrorRaiser(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

// To JavaScript, the properties of a module are its global variables: those of its namespace, and not what the namespace inherits, which is
// there for Python's sake.
class PyModule final : public JSNonFinalObject {
public:
    static constexpr unsigned StructureFlags = JSNonFinalObject::StructureFlags | OverridesGetOwnPropertySlot | OverridesGetOwnPropertyNames | OverridesPut | GetOwnPropertySlotIsImpureForPropertyAbsence;
    PYTHON_CELL_BOILERPLATE(PyModule)

    static PyModule* create(JSGlobalObject*, const String& name);
    JSObject* namespaceObject() const { return m_namespace.get(); }

    static bool getOwnPropertySlot(JSObject*, JSGlobalObject*, PropertyName, PropertySlot&);
    static bool put(JSCell*, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&);
    static bool deleteProperty(JSCell*, JSGlobalObject*, PropertyName, DeletePropertySlot&);
    static void getOwnPropertyNames(JSObject*, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode);

private:
    PyModule(VM& vm, Structure* structure, JSObject* namespaceObject)
        : Base(vm, structure)
        , m_namespace(namespaceObject, WriteBarrierEarlyInit)
    {
    }

    WriteBarrier<JSObject> m_namespace;
};

// An instance of a class derived from list. A list is an Array, and so is this, made and worked on as one. It differs only in what its Structure
// says are its methods, since unlike a list it can have attributes, about which its class may have something to say.
class PyDerivedList final : public JSArray {
public:
    using Base = JSArray;
    DECLARE_EXPORT_INFO;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    PYTHON_DECLARE_EXOTIC_METHODS
};

// The same, for a class derived from bytes or bytearray.
class PyDerivedBytes final : public JSUint8Array {
public:
    using Base = JSUint8Array;
    DECLARE_EXPORT_INFO;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    PYTHON_DECLARE_EXOTIC_METHODS
};

// An exception. It is an Error, as a list is an Array: JavaScript can tell by Error.isError(), it has a stack that was noted when it was made, with
// the frames of both languages in it, and whatever knows what to do with an Error knows what to do with it.
//
// What an Error has that is JavaScript's own way of putting things is JavaScript's, as it is for any Error, and is kept from Python by not being
// enumerable: name, message, cause, stack, line, column and sourceURL. Until they are set, the name is that of the class, the message is str(),
// and the cause is __cause__. An attribute that Python calls by one of those names, as AttributeError.name, is another thing.
class PyException final : public ErrorInstance {
public:
    using Base = ErrorInstance;
    DECLARE_EXPORT_INFO;
    static PyException* create(VM&, PyType*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    PYTHON_DECLARE_EXOTIC_METHODS

private:
    using Base::Base;
};

// An instance of a class derived from int, float or str. Those types' own instances are not cells, so it holds one.
class PyBoxedValue final : public JSNonFinalObject {
public:
    static constexpr unsigned StructureFlags = JSNonFinalObject::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut;
    PYTHON_CELL_BOILERPLATE(PyBoxedValue)
    PYTHON_DECLARE_EXOTIC_METHODS

    static PyBoxedValue* create(VM&, Structure*, JSValue);
    JSValue value() const { return m_value.get(); }

private:
    PyBoxedValue(VM& vm, Structure* structure, JSValue value)
        : Base(vm, structure)
        , m_value(value, WriteBarrierEarlyInit)
    {
    }

    WriteBarrier<Unknown> m_value;
};

template<typename T, JSType type>
inline T* tryCell(JSValue value) { return value.isCell() && value.asCell()->type() == type ? uncheckedDowncast<T>(value.asCell()) : nullptr; }

inline PyBoundMethod* tryBoundMethod(JSValue value) { return tryCell<PyBoundMethod, PyBoundMethodType>(value); }
inline PyRange* tryRange(JSValue value) { return tryCell<PyRange, PyRangeType>(value); }
inline PySlice* trySlice(JSValue value) { return tryCell<PySlice, PySliceType>(value); }
inline PyIterator* tryIterator(JSValue value) { return tryCell<PyIterator, PyIteratorType>(value); }
inline PyModule* tryModule(JSValue value) { return tryCell<PyModule, PyModuleType>(value); }
inline PyBoxedValue* tryBoxedValue(JSValue value) { return tryCell<PyBoxedValue, PyBoxedValueType>(value); }
inline PyNativeObject* tryNativeObject(JSValue value) { return tryCell<PyNativeObject, PyNativeObjectType>(value); }

} // namespace JSC
