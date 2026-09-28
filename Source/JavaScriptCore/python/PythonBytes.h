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

#include "JSTypedArrays.h"
#include "PythonCodecs.h"

namespace JSC {

class PyMemoryView;

namespace Python {

// bytes and bytearray are both Uint8Arrays. One whose class is bytes, or derived from it, is a bytes, and nothing in Python changes what
// is in it. Any other is a bytearray, and that includes every Uint8Array that JavaScript makes.
enum class BytesKind : uint8_t { None, Bytes, ByteArray };
BytesKind bytesKindOf(JSValue);
inline bool isBytes(JSValue value) { return bytesKindOf(value) == BytesKind::Bytes; }
inline bool isByteArray(JSValue value) { return bytesKindOf(value) == BytesKind::ByteArray; }

JSUint8Array* newBytes(JSGlobalObject*, std::span<const uint8_t>);
JSUint8Array* newByteArray(JSGlobalObject*, std::span<const uint8_t>);
// Of what was put together. Null, with MemoryError raised, if there was no room for it.
JSUint8Array* newBytes(JSGlobalObject*, const ByteVector&);
JSUint8Array* newByteArray(JSGlobalObject*, const ByteVector&);

// What is in anything of a built-in kind that has bytes in it, one after another: bytes, bytearray, a memoryview that skips none, and any typed array or
// ArrayBuffer of JavaScript's. Nothing if it is no such thing. It runs no code of a program's.
//
// It is good only until something runs that could be a program's, in either language, since that can resize a bytearray or give an ArrayBuffer away. So it is not
// kept: see Buffer.
std::optional<std::span<const uint8_t>> builtinBufferOf(JSValue);

// What PyObject_GetBuffer() is given. A class that has a __buffer__() sees them.
enum BufferFlags : int {
    SimpleBuffer = 0,
    WritableBuffer = 1,
    FormatBuffer = 4,
    ShapedBuffer = 8,
    StridedBuffer = 0x18, // It need not be one after another.
    CContiguousBuffer = 0x38,
    FortranContiguousBuffer = 0x58,
    AnyContiguousBuffer = 0x98,
    FullReadOnlyBuffer = 0x11C,
};

// The bytes that something has to show, for as long as this lasts: what is between PyObject_GetBuffer() and PyBuffer_Release().
//
// It keeps the object and not where its bytes are, which it finds out each time it is asked. So there is nothing in it to be left pointing at what has been freed,
// whatever is run while it lasts.
class Buffer {
    WTF_MAKE_NONCOPYABLE(Buffer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    Buffer() = default;
    // Of something of a built-in kind
    explicit Buffer(JSValue object)
        : m_object(object)
    {
    }
    Buffer(Buffer&& other) { *this = WTF::move(other); }
    Buffer& operator=(Buffer&& other)
    {
        release();
        m_object = std::exchange(other.m_object, JSValue());
        m_view = std::exchange(other.m_view, nullptr);
        m_globalObject = other.m_globalObject;
        return *this;
    }
    ~Buffer() { release(); }

    explicit operator bool() const { return !!m_object; }

    // What CPython has as the `obj` of a Py_buffer: whose bytes they are.
    JS_EXPORT_PRIVATE JSValue object() const;
    // All of it, one after another, even if it is a memoryview that skips some. That takes flags that allow for one.
    JS_EXPORT_PRIVATE void appendTo(ByteVector&) const;

    std::span<const uint8_t> span() const
    {
        auto now = builtinBufferOf(m_object);
        return now ? *now : std::span<const uint8_t>();
    }
    operator std::span<const uint8_t>() const { return span(); }
    std::span<const uint8_t> operator*() const { return span(); }
    struct Arrow {
        std::span<const uint8_t> span;
        const std::span<const uint8_t>* operator->() const { return &span; }
    };
    Arrow operator->() const { return { span() }; }

    size_t size() const { return span().size(); }
    bool empty() const { return span().empty(); }
    const uint8_t* data() const { return span().data(); }
    uint8_t operator[](size_t index) const { return span()[index]; }
    auto begin() const { return span().begin(); }
    auto end() const { return span().end(); }
    std::span<const uint8_t> subspan(size_t offset, size_t count = std::dynamic_extent) const { return span().subspan(offset, count); }
    std::span<const uint8_t> first(size_t count) const { return span().first(count); }
    std::span<const uint8_t> last(size_t count) const { return span().last(count); }

private:
    friend Buffer tryBufferOf(JSGlobalObject*, JSValue, int);
    friend class Buffers;
    JS_EXPORT_PRIVATE void release();

    JSValue m_object;
    // For what a class of a program's gave with __buffer__(): a memoryview of it, made for this, which is m_object too. It is released when this is done with.
    PyMemoryView* m_view { nullptr };
    JSGlobalObject* m_globalObject { nullptr };
};

// Any number of them, all kept until the last has been got, and then let go of in the order they were got in.
class Buffers {
    WTF_MAKE_NONCOPYABLE(Buffers);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit Buffers(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }
    JS_EXPORT_PRIVATE ~Buffers();

    void append(Buffer&& buffer)
    {
        m_objects.append(buffer.m_object);
        if (auto* view = std::exchange(buffer.m_view, nullptr))
            m_views.append(std::bit_cast<JSCell*>(view));
    }
    size_t size() const { return m_objects.size(); }
    std::span<const uint8_t> at(size_t index) const
    {
        auto now = builtinBufferOf(m_objects.at(index));
        return now ? *now : std::span<const uint8_t>();
    }

private:
    JSGlobalObject* m_globalObject;
    MarkedArgumentBuffer m_objects;
    MarkedArgumentBuffer m_views;
};

// exporter.__release_buffer__(view), if the class of the exporter is a program's and has one.
void releaseBufferOfProgram(JSGlobalObject*, JSValue exporter, JSValue view);
// Whether it has any to show: PyObject_CheckBuffer().
bool hasBuffer(JSGlobalObject*, JSValue);
// Empty if it has none, in which case nothing has been raised, or if something has been.
Buffer tryBufferOf(JSGlobalObject*, JSValue, int flags = SimpleBuffer);
// The same, but that whatever is raised in getting at them is lost, whatever it is. It is for where CPython goes on to say in its own words that what it was given will not do, and so writes over what had
// been raised: `if (PyObject_GetBuffer(...) == 0) { ... } PyErr_Format(...)`.
Buffer bufferOrNothing(JSGlobalObject*, JSValue);
// The same, raising TypeError if it has none: a bytes-like object is required, not 'str'.
Buffer bufferOf(JSGlobalObject*, JSValue);

String reprOfBytes(std::span<const uint8_t>);
int64_t hashOfBytes(std::span<const uint8_t>);

void initializeBytesTypes(JSGlobalObject*);

} } // namespace JSC::Python
