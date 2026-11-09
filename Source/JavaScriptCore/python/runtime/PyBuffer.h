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
// - JS may detach or shrink the memory of a typed array whenever it runs: in a callback, in the handler of a signal, in another
//   thread while this one is in a system call. So a Buffer pins the ArrayBuffer: until it is released, a transfer copies the
//   memory instead of detaching it, and it cannot shrink. (Not a memoryview, which may live long; but for the one that ctypes
//   keeps, PyMemoryView::pinMemoryOfJS().)
//   Whoever has no Buffer, a memoryview among them, looks up where the memory is, and whether there is still enough of it,
//   after anything that may have run code.
//   (The memory of a typed array of up to 1000 items moves once, when the array is given its ArrayBuffer. That is when Python
//   first asks for its memory, whoever asks, before it is told where the memory is.)
//   Memory of WebAssembly cannot be pinned: it may move when it grows. Whoever holds a Buffer of it has to look it up again as
//   if he had none (refreshBuffer()), and does not let go of the GIL in blocking calls.

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
    // A Buffer is on the stack of a thread for as long as a system call takes, where the collector takes what looks like an address for
    // one. A store narrower than a word, alone in its word, leaves the rest of what was there: with its last byte set to 0, what is
    // left of the address of a cell is the address of a neighbour of the cell, which this thread may never have seen. So these
    // make up a word, all of which is written. (The room inside the vectors is not: to write it takes 10 ns, for every Buffer.)
    unsigned dimensions { 1 };
    bool isReadOnly { true };
    bool keepsGIL { false }; // For getBuffer() and releaseBuffer(), if the memory is that of WebAssembly.
    uint16_t unused { 0 };
    String format; // In the syntax of the module struct. Null: "B".
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
};
static_assert(OBJECT_OFFSETOF(Buffer, format) - OBJECT_OFFSETOF(Buffer, dimensions) == sizeof(uint64_t));

// Whether the object exports its memory at all.
bool hasBuffer(JSValue);
// Whether getting a buffer from it, or releasing that, runs code of the program: a method written in Python; a function written in C++ that is not the __buffer__() of
// an exporter, and may call anything. For a PickleBuffer: whether to get the buffer of its object does. To ask runs nothing.
JS_EXPORT_PRIVATE bool getBufferRunsCode(JSValue);
// Whether two buffers got from it one after the other, with nothing done between them, are of the same bytes: to get them runs no code, and it is not memory of JS that
// another thread can write to or make longer, a SharedArrayBuffer. (What ctypes has made of such memory with from_buffer() is not told from other instances: its length does not
// change, and a BytesHolder copies it each time.)
bool canGetBufferTwice(JSValue);
// False: a TypeError ("a bytes-like object is required, not 'x'"), a BufferError, or whatever __buffer__() raised.
// With BufferSimple, the memory is `length` bytes from `data`. Until the buffer is released, a bytearray, an array and the like
// cannot be resized, whatever runs.
bool getBuffer(JSGlobalObject*, JSValue object, Buffer&, unsigned flags = BufferSimple);
// May be called with an exception pending, and on a buffer that holds nothing. It raises nothing.
void releaseBuffer(JSGlobalObject*, Buffer&);
// After code of the program may have run, for memory of JS's that is not pinned: sets `data` again. False: a BufferError.
// A system call that is made again when a signal has interrupted it comes behind such code, the handler: this goes into the loop, right before the call.
bool refreshBuffer(JSGlobalObject*, Buffer&);
// The same for whoever has noted the owner and not a Buffer: where `length` bytes of its memory are, from `offset` on.
bool locateMemory(JSGlobalObject*, JSCell* owner, size_t offset, size_t length, uint8_t*& data);
// Copies the items, `length` bytes in all, in the order of C ('C', or 'A' unless they are in that of Fortran) or Fortran ('F').
void copyBufferToContiguous(uint8_t* destination, const Buffer&, char order);

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

// ---- For the host ----
//
// Code of the host that takes a typed array or an ArrayBuffer of JS takes what exports its memory in Python as well: bytes, a
// bytearray, a memoryview, an array, an mmap, an instance of a class with __buffer__(). Such code keeps no Buffer on its stack.
// It has a value, which it roots for as long as it uses the memory, and that may be long after the call has returned.
//
// memoryForHost() makes that value, the holder, of the object:
// - Of a bytes, the bytes itself: what it holds does not move, and is not to change (but see DEFERRED.md, "What a raw stream is lent").
// - Of a view of what JS exported (memoryview(uint8Array)), a Uint8Array of JS on the same memory. The host has its ways with those.
// - Of anything else, a memoryview that the program cannot get at. Like any view, it is an export for as long as it can be
//   reached from a root that has a name: the bytearray cannot be resized, the mmap cannot be closed (BufferError). One that only
//   the stack of the host refers to lasts until code of the program runs, as the memory of a typed array does that JS may detach.
//   To hold on to it across that, or with no root, the host pins it.
struct HostMemory {
    uint8_t* data { nullptr };
    size_t length { 0 }; // In bytes.
    bool isReadOnly { true }; // It must not be written to: it is a bytes, or a page that cannot be.
};
// An object of Python's that has a buffer to give. (False for the buffers of JS.)
JS_EXPORT_PRIVATE bool exportsMemoryToHost(JSValue);
// Empty: an exception, as where Python itself asks for bytes that follow one another (a view that was released, or has gaps).
// May run code of the program, a __buffer__() written in Python above all (getBufferRunsCode()). So the host calls this where it converts its
// arguments, as it would call toString() there. Where it has begun its work and merely asks a value for its bytes, an object
// like that is no buffer.
JS_EXPORT_PRIVATE JSValue memoryForHost(JSGlobalObject*, JSValue object);
// Where the memory of a holder that is an object of Python's is now. False: it is no holder, or there is no memory any more.
// Raises nothing, runs nothing, allocates nothing.
JS_EXPORT_PRIVATE bool locateMemoryForHost(JSCell* holder, HostMemory&);
// False: there is nothing to pin, and nothing to unpin, because nothing can take the memory away.
JS_EXPORT_PRIVATE bool pinMemoryForHost(JSCell* holder);
// Of a holder for which pinMemoryForHost() returned true, once for each time it did. May be called while the collector sweeps.
JS_EXPORT_PRIVATE void unpinMemoryForHost(JSCell* holder);

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
// (With a frame on the stack of which the registers in use cannot be told, every word counts: one of the optimizing compilers, of WebAssembly, of the callee of an
// inline cache; and in a VM that has code compiled ahead of time.)
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
    FinalizerState& finalizerState() { return m_finalizerState; }
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
    FinalizerState m_finalizerState;
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
    // A view that has exports stays as it is, and goes when the collector finds that nothing refers to it. (Who asks on behalf of the program has
    // looked before. The view that __release_buffer__() is lent may have been handed to another thread, which is using it.)
    void release(JSGlobalObject*);
    // For who keeps addresses of the memory for as long as he keeps the view (ctypes). An export holds memory of Python's. Memory of
    // JS is held by a pin, which a view does not have: from now on this one pins it, until it is collected, released or not.
    // (Not memory of WebAssembly: see above.)
    void pinMemoryOfJS(JSGlobalObject*);
    // What it pins, as that is now, released or not. Empty: it pins nothing, or the memory has gone in spite of the pin. (A pin holds against JS. An mmap has its memory in an ArrayBuffer
    // as well, and what holds that is the export: close() and resize() do not ask about pins.) It looks through all the views that pin: for a view that has been released.
    std::span<uint8_t> memoryOfJSThatIsPinned(JSGlobalObject*) const;

    // Where the first item is now. Null with an exception if JS took the memory away; null without if there is no memory.
    uint8_t* data(JSGlobalObject*) const;

    unsigned dimensions() const { return m_dimensions; }
    int64_t* shape() { return storageBehind<int64_t>(this); }
    int64_t* strides() { return shape() + m_dimensions; }
    const int64_t* shape() const { return storageBehind<const int64_t>(this); }
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
    bool isForHost { false }; // Made by memoryForHost().
    // Code written in C++ has made it for itself, or is not through with making it, and counts on it that nothing of the program's has it. So the
    // module gc, which finds what nothing refers to, does not hand it out (PyGC.cpp).
    bool isInternal { false };
    bool isCContiguous { true };
    bool isFContiguous { true };

private:
    PyMemoryView(VM&, Structure*, PyType*, unsigned dimensions);

    WriteBarrier<PyManagedBuffer> m_managedBuffer;
    WriteBarrier<JSString> m_format;
    unsigned m_dimensions;
    char m_formatCode { 'B' };
    char m_formatSubcode { 0 };
    bool m_isPinningMemoryOfJS { false };
};

// memoryview(object)
JSValue makeMemoryView(JSGlobalObject*, JSValue object, unsigned flags = BufferFullRO);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
