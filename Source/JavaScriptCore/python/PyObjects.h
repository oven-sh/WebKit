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
#include "JSInternalFieldObjectImpl.h"
#include "JSObject.h"
#include "JSTypedArrays.h"
#include "PyInstance.h"
#include "PyRealm.h"
#include "PythonSignatures.h"

namespace JSC {

class PyDict;

// The small built-in kinds of object. Each is a cell with a few fields, and its class is its prototype, as for any other.

#define PYTHON_CELL_BOILERPLATE(ClassName, spaceName) \
    using Base = JSNonFinalObject; \
    template<typename CellType, SubspaceAccess mode> \
    static GCClient::IsoSubspace* subspaceFor(VM& vm) { return vm.spaceName<mode>(); } \
    PYTHON_OVERLOADS_OPERATORS \
    DECLARE_EXPORT_INFO; \
    DECLARE_VISIT_CHILDREN; \
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

// instance.method, when it is not called at once. It is a `method` if that is a function written in Python. If it is written in C++ it is a
// `builtin_function_or_method`, as a function that is no method is, or a `method-wrapper`.
class PyBoundMethod final : public JSNonFinalObject {
public:
    static constexpr unsigned StructureFlags = JSNonFinalObject::StructureFlags | OverridesGetCallData;
    PYTHON_CELL_BOILERPLATE(PyBoundMethod, pyBoundMethodSpace)

    static PyBoundMethod* create(JSGlobalObject*, JSValue function, JSValue self);
    JSValue function() const { return m_function.get(); }
    JSValue self() const { return m_self.get(); }
    static CallData getCallData(JSCell*);
    // What calling it does, for what has the arguments somewhere other than on the stack.
    JSValue call(JSGlobalObject*, const ArgList&, JSCellButterfly* keywordNames);

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

// Its ends and its step are ints of any size, and what it does is defined by arithmetic on those: see PythonRange.cpp. When they and its length all fit
// an int64, which is nearly always, they are here as that too, for what is done a great deal.
class PyRange final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyRange, pyRangeSpace)

    // They are ints and nothing else, and the step is not zero. Null if it raised.
    static PyRange* create(JSGlobalObject*, JSValue start, JSValue stop, JSValue step);
    JSValue start() const { return m_start.get(); }
    JSValue stop() const { return m_stop.get(); }
    JSValue step() const { return m_step.get(); }
    JSValue length() const { return m_length.get(); }
    bool isEmpty() const { return m_length.get().isInt32() && !m_length.get().asInt32(); }

    bool isSmall() const { return m_isSmall; }
    int64_t smallStart() const { ASSERT(m_isSmall); return m_smallStart; }
    int64_t smallStop() const { ASSERT(m_isSmall); return m_smallStop; }
    int64_t smallStep() const { ASSERT(m_isSmall); return m_smallStep; }
    int64_t smallLength() const { ASSERT(m_isSmall); return m_smallLength; }

private:
    PyRange(VM&, Structure*, JSValue start, JSValue stop, JSValue step, JSValue length);

    WriteBarrier<Unknown> m_start;
    WriteBarrier<Unknown> m_stop;
    WriteBarrier<Unknown> m_step;
    WriteBarrier<Unknown> m_length;
    int64_t m_smallStart { 0 };
    int64_t m_smallStop { 0 };
    int64_t m_smallStep { 0 };
    int64_t m_smallLength { 0 };
    bool m_isSmall { false };
};

class PyComplex final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyComplex, pyComplexSpace)

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
    using Base = JSNonFinalObject;

    // How many there are along a dimension, and how far it is in bytes from one to the next, which can be backwards.
    struct Dimension {
        int64_t length;
        int64_t stride;
    };
    static constexpr unsigned maxDimensionCount = 64; // PyBUF_MAX_NDIM

    // All that there is to say of a view but what it is of and its dimensions.
    struct Layout {
        char format { 'B' }; // What an item is: one of the struct module's characters.
        bool formatHasAtSign { false }; // It means the same with one as without, but is given back as it was given.
        bool isReadOnly { false };
        unsigned itemSize { 1 }; // It goes with the format, but for a view that was asked for without its format.
        int64_t offset { 0 }; // In bytes, of the first item.
    };

    // The dimensions come after it, as the items of a tuple come after the tuple.
    static size_t allocationSize(Checked<size_t> dimensionCount)
    {
        return sizeof(PyMemoryView) + dimensionCount * sizeof(Dimension);
    }

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm)
    {
        return &vm.cellSpace();
    }

    PYTHON_OVERLOADS_OPERATORS
    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    // `exporter` is for what a class of a program's gave with __buffer__(). See Python::newBufferWrapper().
    static PyMemoryView* create(JSGlobalObject*, JSValue object, const Layout&, std::span<const Dimension>, JSValue exporter = { });
    // Another of the same bytes, or of some of them.
    PyMemoryView* derive(JSGlobalObject*, const Layout&, std::span<const Dimension>) const;

    JSValue object() const { return m_object.get(); }
    const Layout& layout() const { return m_layout; }
    char format() const { return m_layout.format; }
    unsigned itemSize() const { return m_layout.itemSize; }
    bool isReadOnly() const { return m_layout.isReadOnly; }
    // None at all is one item, which is not in a row of anything.
    std::span<const Dimension> dimensions() const { return { std::bit_cast<const Dimension*>(this + 1), m_dimensionCount }; }
    int64_t byteLength() const { return m_byteLength; } // Of all the items, and not of what is between them.
    JSValue exporter() const { return m_exporter.get(); }
    bool isReleased() const { return !m_object; }
    // It can run __release_buffer__().
    void release(JSGlobalObject*);

    // Whether the items are one after another with nothing between: with the last dimension going round fastest, as C has arrays, or with the first, as Fortran has them.
    bool isCContiguous() const { return m_isCContiguous; }
    bool isFortranContiguous() const { return m_isFortranContiguous; }

    // All of it as it lies, for a view that is contiguous one way or the other. Empty if it is no longer all there, and nothing if it has been released.
    std::optional<std::span<const uint8_t>> span() const;
    // The bytes of the item that is that many bytes on from the first. Empty if it is no longer there.
    std::span<uint8_t> itemAt(int64_t distance) const;

    // Once worked out it is kept, and is still to be had when the view has been released.
    std::optional<int64_t> hash() const { return m_hash; }
    void setHash(int64_t hash) { m_hash = hash; }

private:
    PyMemoryView(VM&, Structure*, JSValue object, const Layout&, std::span<const Dimension>, JSValue exporter);

    WriteBarrier<Unknown> m_object;
    WriteBarrier<Unknown> m_exporter;
    Layout m_layout;
    int64_t m_byteLength;
    std::optional<int64_t> m_hash;
    unsigned m_dimensionCount;
    bool m_isCContiguous;
    bool m_isFortranContiguous;
};

static_assert(!(sizeof(PyMemoryView) % alignof(PyMemoryView::Dimension)), "the dimensions come straight after it");

class PySlice final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PySlice, pySliceSpace)

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
    // What it says, as numbers: PySlice_Unpack(). This is the part that can run a program's code, which can change how long the sequence is. Nothing if it raised.
    struct Bounds {
        int64_t start;
        int64_t stop;
        int64_t step;
    };
    std::optional<Bounds> unpack(JSGlobalObject*) const;
    // What that selects of a sequence of the length: PySlice_AdjustIndices().
    static Indices adjust(Bounds, int64_t length);
    // The two together, for a sequence that cannot change.
    std::optional<Indices> indices(JSGlobalObject* globalObject, int64_t length) const { return indices(globalObject, [length] { return length; }); }
    // And for one that can, which is asked how long it is when it can change no more.
    template<typename Length> requires std::is_invocable_v<Length>
    std::optional<Indices> indices(JSGlobalObject* globalObject, const Length& length) const
    {
        auto bounds = unpack(globalObject);
        if (!bounds)
            return std::nullopt;
        return adjust(*bounds, length());
    }
    // The same, of something whose length is an int of any size, as ints of any size. False if it raised.
    bool indices(JSGlobalObject*, JSValue length, JSValue& start, JSValue& stop, JSValue& step) const;

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
    PYTHON_CELL_BOILERPLATE(PyIterator, pyIteratorSpace)

    enum class Kind : uint8_t {
        List, // a: the list. index
        ListReverse,
        Tuple,
        Range, // index: the next value. stop: how many are left. step
        LongRange, // a: the range, which is not a small one. b: which of its items is next, an int
        AsciiStr, // a: the string. index, in code units
        Str, // The same, of a string that is not all ASCII. They are two classes in CPython.
        Bytes,
        ByteArray,
        Memory, // a: the memoryview. index. stop: how long it was when this began
        DictKeys, // a: the dict. index: the entry. stop: its size when this began. step: how many it has given
        DictValues,
        DictItems,
        DictReverseKeys,
        DictReverseValues,
        DictReverseItems,
        Set, // The same
        Sequence, // a: what has __getitem__. index
        Callable, // a: the callable. b: the sentinel
        Enumerate, // a: an iterator. index: the count, or b if it is one that does not fit
        Zip, // a: a tuple of iterators. index: whether it is strict
        Map, // a: the function. b: a tuple of iterators. index: whether it is strict
        Filter, // a: the function or None. b: an iterator
        Reversed, // a: a sequence. index
        JavaScript, // a: a JavaScript iterator. b: its next
        CodeLines, // a: a tuple of what code.co_lines() gives. index
        CodePositions, // The same, for code.co_positions()
        ContextKeys, // The same, for what is in a contextvars.Context
        ContextValues,
        ContextItems,
    };

    static PyIterator* create(JSGlobalObject*, Kind, JSValue a = JSValue(), JSValue b = JSValue(), int64_t index = 0, int64_t stop = 0, int64_t step = 0);
    // Of a class derived from the one that goes with the kind.
    static PyIterator* create(JSGlobalObject*, Structure*, Kind, JSValue a = JSValue(), JSValue b = JSValue(), int64_t index = 0, int64_t stop = 0, int64_t step = 0);
    // One of the kinds that go through a dict.
    static PyIterator* create(JSGlobalObject*, Kind, PyDict*);
    static BuiltinType typeFor(Kind);

    Kind kind() const { return m_kind; }
    // Of a class that a program derived from enumerate or the like, which may have a __next__() of its own.
    bool isOfDerivedClass() const { return m_isOfDerivedClass; }
    // Empty when there is no more. This is the __next__() of the built-in class.
    JSValue next(JSGlobalObject*);

    JSValue a() const { return m_a.get(); }
    JSValue b() const { return m_b.get(); }
    int64_t index() const { return m_index; }
    int64_t stop() const { return m_stop; }
    int64_t step() const { return m_step; }
    bool isReverse() const { return m_kind == Kind::DictReverseKeys || m_kind == Kind::DictReverseValues || m_kind == Kind::DictReverseItems; }

    // For __setstate__().
    void setIndex(int64_t index) { m_index = index; }
    void setStop(int64_t stop) { m_stop = stop; }
    void setB(VM& vm, JSValue b) { m_b.set(vm, this, b); }
    // Another that is where this one is, and goes on from there by itself.
    PyIterator* copy(JSGlobalObject*) const;

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
    bool m_isOfDerivedClass { false };
    WriteBarrier<Unknown> m_a;
    WriteBarrier<Unknown> m_b;
    int64_t m_index;
    int64_t m_stop;
    int64_t m_step;
};

// Objects with up to four values in them and nothing else, told apart by their class: property, staticmethod, classmethod, super, the
// views of a dict, NotImplemented and Ellipsis.
class PyNativeObject final : public JSInternalFieldObjectImpl<4> {
public:
    using Base = JSInternalFieldObjectImpl<4>;
    static constexpr unsigned StructureFlags = Base::StructureFlags | OverridesGetCallData;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyNativeObjectSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;
    PYTHON_OVERLOADS_OPERATORS
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    static PyNativeObject* create(VM&, Structure*);
    static PyNativeObject* create(JSGlobalObject*, BuiltinType, JSValue = JSValue(), JSValue = JSValue(), JSValue = JSValue(), JSValue = JSValue());

    JSValue field(unsigned index) const { return internalField(index).get(); }
    void setField(VM& vm, unsigned index, JSValue value) { internalField(index).set(vm, this, value); }

    // It can be called if its class has __call__: a staticmethod, list[int].
    static CallData getCallData(JSCell*);

private:
    PyNativeObject(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

// The same, for what has more to keep than that has room for: a type variable, and what a `type` statement makes.
class PyTypingObject final : public JSInternalFieldObjectImpl<8> {
public:
    using Base = JSInternalFieldObjectImpl<8>;

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.pyTypingObjectSpace<mode>();
    }

    DECLARE_EXPORT_INFO;
    DECLARE_VISIT_CHILDREN;
    PYTHON_OVERLOADS_OPERATORS
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);

    static PyTypingObject* create(VM&, Structure*);

    JSValue field(unsigned index) const { return internalField(index).get(); }
    void setField(VM& vm, unsigned index, JSValue value) { internalField(index).set(vm, this, value); }

private:
    PyTypingObject(VM& vm, Structure* structure)
        : Base(vm, structure)
    {
    }
};

// An attribute of a built-in type that is worked out by C++: function.__name__.
class PyGetSetDescriptor final : public JSNonFinalObject {
public:
    PYTHON_CELL_BOILERPLATE(PyGetSetDescriptor, pyGetSetDescriptorSpace)

    using Getter = JSValue (*)(JSGlobalObject*, JSValue self);
    using Setter = void (*)(JSGlobalObject*, JSValue self, JSValue value); // The value is empty to delete.

    // A member is what in CPython is a field of a C struct. It differs in what it is called and in what it says when it cannot be set.
    // A member for which the instance has a slot: one of __slots__, or a field of a built-in exception. What is in the slot is a property of the
    // instance under `storage`, which is a private name. `initialValue` is what it has until it is set, or empty if until then there is no such attribute.
    static PyGetSetDescriptor* createForSlot(JSGlobalObject*, PyType* owner, JSString* name, Symbol* storage, JSValue initialValue);
    // Null unless it is such a one.
    Symbol* storage() const { return m_storage.get(); }
    JSValue initialValue() const { return m_initialValue.get(); }

    // `doc` is for one that CPython has in no built-in class.
    static PyGetSetDescriptor* create(JSGlobalObject*, PyType* owner, const String& name, Getter, Setter, bool isMember = false, ASCIILiteral doc = { });
    bool isMember() const { return m_isMember; }
    Getter getter() const { return m_getter; }
    Setter setter() const { return m_setter; }
    PyType* owner() const { return m_owner.get(); }
    JSString* name() const { return m_name.get(); }
    // descriptor.__doc__. Null if it has none.
    ASCIILiteral doc() const { return m_doc; }

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
    ASCIILiteral m_doc;
    WriteBarrier<PyType> m_owner;
    WriteBarrier<JSString> m_name;
    WriteBarrier<Symbol> m_storage;
    WriteBarrier<Unknown> m_initialValue;
};

// An instance of a class derived from list. A list is an Array, and so is this, made and worked on as one. It differs only in what its Structure
// says are its methods, since unlike a list it can have attributes, about which its class may have something to say.
class PyDerivedList final : public JSArray {
public:
    using Base = JSArray;
    DECLARE_EXPORT_INFO;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    PYTHON_DECLARE_EXOTIC_METHODS
    PYTHON_OVERLOADS_OPERATORS
};

// The same, for a class derived from bytes or bytearray.
class PyDerivedBytes final : public JSUint8Array {
public:
    using Base = JSUint8Array;
    DECLARE_EXPORT_INFO;
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue prototype);
    PYTHON_DECLARE_EXOTIC_METHODS
    PYTHON_OVERLOADS_OPERATORS
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
    PYTHON_OVERLOADS_OPERATORS

private:
    using Base::Base;
};

// An instance of a class derived from int, float or str. Those types' own instances are not cells, so it holds one.
class PyBoxedValue final : public JSNonFinalObject {
public:
    static constexpr unsigned StructureFlags = JSNonFinalObject::StructureFlags | OverridesGetOwnPropertySlot | OverridesPut;
    PYTHON_CELL_BOILERPLATE(PyBoxedValue, pyBoxedValueSpace)
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
inline PyBoxedValue* tryBoxedValue(JSValue value) { return tryCell<PyBoxedValue, PyBoxedValueType>(value); }

// The string that a str is, or that an instance of a class derived from str holds. Null for anything else.
inline JSString* stringIn(JSValue value)
{
    if (value.isString())
        return asString(value);
    if (auto* boxed = tryBoxedValue(value); boxed && boxed->value().isString())
        return asString(boxed->value());
    return nullptr;
}
inline PyNativeObject* tryNativeObject(JSValue value) { return tryCell<PyNativeObject, PyNativeObjectType>(value); }
inline PyNativeObject* asNativeObject(JSValue value) { return uncheckedDowncast<PyNativeObject>(value.asCell()); }

} // namespace JSC
