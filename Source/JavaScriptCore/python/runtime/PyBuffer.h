/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

class ArrayBuffer;

namespace Python {

// The buffer protocol: how an object lets others at the memory it keeps its data in. (PEP 3118, PEP 688.)
//
// The memory always belongs to a cell, the owner: a bytes, a bytearray, a PyByteStorage, or a typed array or an ArrayBuffer of
// JS. What is remembered is the owner and an offset into its memory, not an address.
// - Cells do not move, so the memory of a bytes or of a PyByteStorage stays where it is for as long as the cell lives.
// - That of a bytearray stays where it is while the bytearray has exports, which keep it from being resized.
// - JS may detach, shrink or move the memory of a typed array whenever it runs. Nothing stops it: where the memory is, and
//   whether there is still enough of it, is looked up again after anything that may have run code.
//   With threads, JS may run while another thread, which has let go of the GIL, is in a system call that uses the memory. So
//   while there are threads, a Buffer (not a memoryview, which may live long) pins the ArrayBuffer: until it is released, a
//   transfer copies the memory instead of detaching it, and it cannot shrink. A thread that holds a Buffer of memory that is
//   not pinned does not let go of the GIL in blocking calls.

// What the consumer of a buffer can cope with. The values are those of inspect.BufferFlags.
enum BufferFlag : unsigned {
    BufferSimple = 0,
    BufferWritable = 0x1,
    BufferFormat = 0x4,
    BufferND = 0x8,
    BufferStrides = 0x10 | BufferND,
    BufferCContiguous = 0x20 | BufferStrides,
    BufferFContiguous = 0x40 | BufferStrides,
    BufferAnyContiguous = 0x80 | BufferStrides,
    BufferIndirect = 0x100 | BufferStrides,
    BufferContig = BufferND | BufferWritable,
    BufferContigRO = BufferND,
    BufferStrided = BufferStrides | BufferWritable,
    BufferStridedRO = BufferStrides,
    BufferRecords = BufferStrides | BufferWritable | BufferFormat,
    BufferRecordsRO = BufferStrides | BufferFormat,
    BufferFull = BufferIndirect | BufferWritable | BufferFormat,
    BufferFullRO = BufferIndirect | BufferFormat,
};

static constexpr unsigned maxBufferDimensions = 64;

// ---- For code written in C++ that wants the memory of an object ----

// Py_buffer. Keep it on the stack: that is what keeps what it refers to alive.
struct Buffer {
    // Tells the exporter that the buffer is no longer in use. It must not run code of the program, nor allocate cells.
    using Release = void (*)(JSGlobalObject*, JSValue object);

    JSValue object; // What exported it, and is told when it is released. Empty: the buffer holds nothing.
    uint8_t* data { nullptr }; // The first item. See refreshBuffer().
    int64_t length { 0 }; // In bytes.
    int64_t itemSize { 1 };
    bool isReadOnly { true };
    String format; // In the syntax of the module struct. Null: "B".
    unsigned dimensions { 1 };
    Vector<int64_t, 4> shape; // Empty if not asked for: one dimension, of length / itemSize items.
    Vector<int64_t, 4> strides; // Empty if not asked for: the items follow one another, the last dimension varying fastest.

    std::span<const uint8_t> span() const { return { data, static_cast<size_t>(length) }; }
    std::span<uint8_t> mutableSpan() const { return { data, static_cast<size_t>(length) }; }

    // For the exporter.
    JSCell* owner { nullptr };
    int64_t offset { 0 }; // Of the first item, from the start of the owner's memory.
    int64_t requiredSize { 0 }; // How much memory the owner had, and still has to have.
    Release release { nullptr };
    JSValue view; // A memoryview made for this buffer alone, which is released with it.
    // For getBuffer() and releaseBuffer(), if the memory is that of JS.
    ArrayBuffer* pinned { nullptr };
    bool keepsGIL { false };
};

// Whether the object exports its memory at all.
bool hasBuffer(JSValue);
// Whether getting a buffer from it, or releasing that, calls a method written in Python.
bool getBufferRunsCode(JSValue);
// False: a TypeError ("a bytes-like object is required, not 'x'"), a BufferError, or whatever __buffer__() raised.
// With BufferSimple, the memory is `length` bytes from `data`. Until the buffer is released, a bytearray, an array and the like
// cannot be resized, whatever runs.
bool getBuffer(JSGlobalObject*, JSValue object, Buffer&, unsigned flags = BufferSimple);
// May be called with an exception pending, and on a buffer that holds nothing. It raises nothing.
void releaseBuffer(JSGlobalObject*, Buffer&);
// After code of the program may have run, if the memory may be that of JS: sets `data` again. False: a BufferError.
bool refreshBuffer(JSGlobalObject*, Buffer&);
// Copies the items, `length` bytes in all, in the order of C ('C', or 'A' unless they are in that of Fortran) or Fortran ('F').
void copyBufferToContiguous(uint8_t* destination, const Buffer&, char order);

// All of getBytesLike() but the common case.
bool getBytesLikeSlow(JSGlobalObject*, JSValue, std::span<const uint8_t>&);

class BufferHolder {
    WTF_MAKE_NONCOPYABLE(BufferHolder);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit BufferHolder(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }
    ~BufferHolder() { releaseBuffer(m_globalObject, buffer); }

    bool get(JSValue object, unsigned flags = BufferSimple) { return getBuffer(m_globalObject, object, buffer, flags); }

    Buffer buffer;

private:
    JSGlobalObject* m_globalObject;
};

// ---- For an exporter that cannot be resized while it has exports ----
//
// A view that the program drops without releasing it goes on counting as an export until the collector notices. In CPython it
// does not, and programs rely on that: bytes(memoryview(b)[:n]); del b[:n]. So before it raises a BufferError, the exporter
// asks this, with a function that returns how many exports it counts.
// - If a variable, an argument or an operand still to be read of a frame is a view of the exporter, there is no more to ask.
// - Otherwise a collection tells: of what is young, which will do for a view just made and dropped; then of everything.
// In these collections, what counts is what the program can reach: from the roots that have a name and from the frames as
// above. A word of the stack that merely looks like a reference keeps the cell it names, as ever, but a view that nothing else
// refers to is released. So code written in C++ that relies on the memory of an object across calls that may run anything
// takes a buffer, which counts whatever refers to it; having a view in a variable is not enough.
// (With a frame of the optimizing compilers on the stack, of which the registers in use cannot be told, every word counts.)
// A __release_buffer__() written in Python is not called from here, but at the next safe point.
bool isHeldByFrames(JSGlobalObject*, JSValue exporter);
void releaseUnreachableBuffers(JSGlobalObject*, bool thorough);

template<typename Function>
bool isStillExported(JSGlobalObject* globalObject, JSValue exporter, const Function& exportCount)
{
    if (!exportCount()) [[likely]]
        return false;
    if (isHeldByFrames(globalObject, exporter))
        return true;
    releaseUnreachableBuffers(globalObject, false);
    if (!exportCount())
        return false;
    releaseUnreachableBuffers(globalObject, true);
    return exportCount();
}

// ---- For a type written in C++ that exports its memory ----
//
// It has these two in its table of methods:
//     PY_SLOT("__buffer__", myBuffer, "($self, flags, /)"),
//     PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
// where myBuffer() returns exportBuffer(), having counted the export if that returned a view. `release` uncounts it.

struct BufferExport {
    JSCell* owner; // May be null if there is no memory.
    size_t offset;
    size_t length; // In bytes.
    bool isReadOnly;
    size_t itemSize { 1 };
    ASCIILiteral format { }; // Null: "B".
    Buffer::Release release { nullptr };
};
// A memoryview of a one-dimensional array. `flags` is the argument of __buffer__().
JSValue exportBuffer(JSGlobalObject*, JSValue self, JSValue flags, const BufferExport&);
// The same for an exporter whose items have a format that is not a literal, or that has more or fewer dimensions than one. The
// buffer says everything but `data`, whatever the flags ask for, and is taken over.
JSValue exportBuffer(JSGlobalObject*, JSValue flags, Buffer&);
JSValue releaseBufferMethod(JSGlobalObject*, JSValue self, JSValue view);
// __buffer__ of bytes, bytearray and memoryview.
JSValue bufferMethod(JSGlobalObject*, JSValue self, JSValue flags);

// ---- memoryview ----

class PyMemoryView;

// One export of an object's memory, which any number of memoryviews share: the object is told that it is over when the last
// of them is released, or when the collector finds that nothing can reach those that are not.
class PyManagedBuffer final : public PyObject {
public:
    PY_DECLARE_CELL(PyManagedBuffer, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    // Takes the buffer over: it is for this to release.
    static PyManagedBuffer* create(JSGlobalObject*, Buffer&);

    JSValue object() const { return m_object.get(); } // memoryview.obj. Empty: nothing is to be told.
    JSCell* owner() const { return m_owner.get(); }
    // The first view, which is of all that was exported.
    PyMemoryView* master() const { return m_master.get(); }
    void setMaster(VM&, PyMemoryView*);

    bool isReleased() const { return m_isReleased; }
    void release(JSGlobalObject*);
    // Whether that calls a __release_buffer__() written in Python.
    bool releaseRunsCode() const;

    int64_t requiredSize { 0 };
    unsigned viewCount { 0 }; // The memoryviews that have not been released.
    unsigned bufferCount { 0 }; // The buffers taken from those, and not released.

private:
    PyManagedBuffer(VM&, Structure*, PyType*);

    WriteBarrier<Unknown> m_object;
    WriteBarrier<JSCell> m_owner;
    WriteBarrier<PyMemoryView> m_master;
    Buffer::Release m_release { nullptr };
    bool m_isReleased { false };
    bool m_isRegistered { false };
};

class PyMemoryView final : public PyObject {
public:
    PY_DECLARE_CELL(PyMemoryView, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static size_t allocationSize(unsigned dimensions) { return sizeof(PyMemoryView) + 2 * dimensions * sizeof(int64_t); }

    // Registered with the managed buffer, and with nothing set but the number of dimensions.
    static PyMemoryView* createUninitialized(JSGlobalObject*, PyManagedBuffer*, unsigned dimensions);
    // Of memory that nothing exported: `length` bytes.
    static PyMemoryView* createForMemory(JSGlobalObject*, JSCell* owner, size_t offset, size_t length, bool isReadOnly);

    // Null once the view is released, so that a view that is kept does not make those that are dropped look reachable.
    PyManagedBuffer* managedBuffer() const { return m_managedBuffer.get(); }
    bool isReleased() const { return !m_managedBuffer || m_managedBuffer->isReleased(); }
    bool wasReleasedItself() const { return !m_managedBuffer; }
    // There must be no exports.
    void release(JSGlobalObject*);

    // Where the first item is now. Null with an exception if JS took the memory away; null without if there is no memory.
    uint8_t* data(JSGlobalObject*) const;

    unsigned dimensions() const { return m_dimensions; }
    int64_t* shape() { return reinterpret_cast<int64_t*>(this + 1); }
    int64_t* strides() { return shape() + m_dimensions; }
    const int64_t* shape() const { return reinterpret_cast<const int64_t*>(this + 1); }
    const int64_t* strides() const { return shape() + m_dimensions; }

    JSString* format() const { return m_format.get(); } // Null: "B".
    String formatString() const;
    void setFormat(VM&, JSString*);
    // The format, without the @ it may begin with, if it is that of a single item: a character, or Z and a character.
    // Otherwise 0.
    char formatCode() const { return m_formatCode; }
    char formatSubcode() const { return m_formatSubcode; }

    // From the shape, the strides and the size of an item.
    void updateLength();
    void updateContiguity();

    int64_t offset { 0 }; // Of the first item, from the start of the owner's memory.
    int64_t length { 0 }; // In bytes: the product of the shape and the size of an item.
    int64_t itemSize { 1 };
    int64_t hash { 0 };
    unsigned exports { 0 }; // Buffers taken from the view, which keep it from being released.
    bool hasHash { false };
    bool isReadOnly { true };
    bool isRestricted { false }; // Lent to __release_buffer__(): nothing may hold on to the memory.
    bool isCContiguous { true };
    bool isFContiguous { true };

private:
    PyMemoryView(VM&, Structure*, PyType*, unsigned dimensions);

    WriteBarrier<PyManagedBuffer> m_managedBuffer;
    WriteBarrier<JSString> m_format;
    unsigned m_dimensions;
    char m_formatCode { 'B' };
    char m_formatSubcode { 0 };
};

// memoryview(object)
JSValue makeMemoryView(JSGlobalObject*, JSValue object, unsigned flags = BufferFullRO);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
