/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyBuffer.h"
#include "PyHash.h"
#include "PyObject.h"
#include "PyType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// To ask for more is an OverflowError, "byte string is too large", and not a MemoryError: what a Py_ssize_t holds, less the rest of
// the object in CPython.
constexpr size_t maxLengthOfBytes = static_cast<size_t>(std::numeric_limits<int64_t>::max()) - 33;

class PyBytes final : public PyObject {
public:
    PY_DECLARE_CELL(PyBytes, PyObject, PyBytesType);

    static size_t allocationSize(size_t length) { return sizeof(PyBytes) + length + 1; }

    // To be filled before anything else sees it. A NUL follows the last byte.
    static PyBytes* createUninitialized(VM&, Structure*, PyType*, size_t length);
    static PyBytes* tryCreateUninitialized(VM&, Structure*, PyType*, size_t length); // Null: there is no room for it.
    static PyBytes* createUninitialized(JSGlobalObject*, size_t length); // Of type bytes.
    static PyBytes* create(JSGlobalObject*, std::span<const uint8_t>);
    static PyBytes* create(JSGlobalObject* globalObject, std::span<const char> span) { return create(globalObject, byteCast<uint8_t>(span)); }

    size_t length() const { return m_length; }
    std::span<const uint8_t> span() const { return { data(), m_length }; }
    const uint8_t* data() const { return storageBehind<const uint8_t>(this); }
    uint8_t* mutableData() { return storageBehind<uint8_t>(this); }

    // hash(bytes), once it has been worked out; -1 until then.
    int64_t cachedHash() const { return m_hash; }
    void setCachedHash(int64_t hash) { m_hash = hash; }

private:
    PyBytes(VM&, Structure*, PyType*, size_t length);

    size_t m_length;
    int64_t m_hash { -1 };
};

// The bytes of a bytearray.
class PyByteStorage final : public JSCell {
public:
    using Base = JSCell;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr DestructionMode needsDestruction = DoesNotNeedDestruction;

    template<typename CellType, SubspaceAccess>
    static CompleteSubspace* subspaceFor(VM& vm) { return &vm.cellSpace(); }

    DECLARE_INFO;

    static Structure* createStructure(VM& vm, JSGlobalObject* globalObject)
    {
        return Structure::create(vm, globalObject, jsNull(), TypeInfo(CellType, StructureFlags), info());
    }
    static PyByteStorage* create(VM&, Structure*, size_t capacity);
    static PyByteStorage* tryCreate(VM&, Structure*, size_t capacity); // Null: there is no room for it.

    size_t capacity() const { return m_capacity; }
    uint8_t* data() { return storageBehind<uint8_t>(this); }

private:
    PyByteStorage(VM&, Structure*, size_t capacity);

    size_t m_capacity;
};

class PyByteArray final : public PyObject {
public:
    PY_DECLARE_CELL(PyByteArray, PyObject, PyByteArrayType);
    DECLARE_VISIT_CHILDREN;

    static PyByteArray* create(VM&, Structure*, PyType*);
    static PyByteArray* create(JSGlobalObject*, std::span<const uint8_t>);

    size_t length() const { return m_length; }
    std::span<const uint8_t> span() const { return { m_storage ? m_storage->data() + m_start : nullptr, m_length }; }
    std::span<uint8_t> mutableSpan() { return { m_storage ? m_storage->data() + m_start : nullptr, m_length }; }
    // New bytes are zero. False, with a BufferError, if a memoryview is looking, or a MemoryError.
    bool resize(JSGlobalObject*, size_t length);
    // del array[:count]. The rest stays where it is: what reads a stream takes bytes off the front of its buffer again and again.
    // Fails as resize() does.
    bool removeFront(JSGlobalObject*, size_t count);
    void append(JSGlobalObject*, std::span<const uint8_t>);
    // How many bytes there is room for, counting the NUL that is kept after the last. 0: nothing is allocated.
    size_t capacity() const { return m_storage ? m_storage->capacity() - m_start : 0; }

    void addExport() { ++m_exports; }
    void removeExport() { --m_exports; }
    // An export that a function written in C++ has for as long as it runs, and that nothing takes over. A view that the program has dropped goes on counting until a
    // collection finds it (isStillExported() in PyBuffer.h); these are not worth asking the collector about.
    void addHold()
    {
        ++m_exports;
        ++m_holds;
    }
    void removeHold()
    {
        --m_exports;
        --m_holds;
    }
    // Whether it cannot be resized. May collect.
    bool isStillExported(JSGlobalObject*);
    // Whether the bytes may change while this thread has the GIL and runs no code of the program. Who writes to them then (a thread that waits in readinto() or recv_into(), the
    // host, a foreign function) has asked for a buffer, which is an export (but see DEFERRED.md, "What a raw stream is lent"); a hold only reads. A view that is dead and not yet
    // found counts too, which costs a copy and no more.
    bool mayBeWrittenMeanwhile() const { return m_exports != m_holds; }

private:
    PyByteArray(VM&, Structure*, PyType*);

    WriteBarrier<PyByteStorage> m_storage;
    size_t m_start { 0 }; // Where in the storage the first byte is.
    size_t m_length { 0 };
    unsigned m_exports { 0 };
    unsigned m_holds { 0 }; // How many of them are holds.
};

// Whether it has bytes to show: bytes, bytearray, memoryview, array.array, whatever has __buffer__...
bool isBytesLike(JSValue);

// For a function that reads the bytes of an argument, which may be anything that has bytes to show: bytes, a bytearray, a memoryview, an
// array.array, a typed array of JS, whatever has __buffer__(). They are held for as long as this lives, as by a Buffer, whatever runs: the
// conversion of the next argument, its __buffer__(), the __release_buffer__() of this one, which is called when this goes.
// Of memory that another thread of JS can write to, a SharedArrayBuffer, this has a copy, so that the bytes are not seen to change. (The buffer is then released as soon as the
// copy is made.) That is not to be counted on, though: an mmap of a file that another process writes to is read where it is, and so is a bytearray that another thread of Python
// is having filled. What looks at the bytes twice, and takes from the first look how much the second writes, calls settle() first.
// Mostly the argument is a bytes. That does not move, and is not to change (but see settle()), so there is nothing to hold and nothing is done. Something has
// to keep it alive, as an argument is kept.
// CPython releases the buffers of a call in the order of the arguments, and C++ lets go first of what was declared last: the holder of the last argument is declared first.
class BytesHolder {
    WTF_MAKE_NONCOPYABLE(BytesHolder);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit BytesHolder(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
    {
    }
    ~BytesHolder()
    {
        if (m_held) [[unlikely]]
            release();
    }

    // One of these, once. False: a TypeError, "a bytes-like object is required, not 'x'"; or, though it is bytes-like, a ValueError (a memoryview
    // that was released), a BufferError (one whose items do not follow one another), or what __buffer__() raised.
    ALWAYS_INLINE bool get(JSValue value) { return getPlainBytes(value) || getSlow(value, false); }
    // For when nothing of the program's runs from here until the bytes have been read for the last time. All the difference is in memory of WebAssembly, which moves when code
    // makes it grow, and cannot be held where it is: get() makes a copy of it, as of shared memory, which this saves.
    ALWAYS_INLINE bool getLast(JSValue value) { return getPlainBytes(value) || getSlow(value, true); }
    // As getLast(), for int(), float() and compile(), which read bytes and a bytearray where they are: a class derived from them is not asked for its __buffer__().
    // False, and no exception but a termination: the object would not give them, whatever it raised. To the three it is then no text.
    bool getText(JSValue);

    // After this the bytes do not change until this goes: unless they are a bytes, a view that a bytes has given of itself, or a bytearray that nobody else has a buffer of, span()
    // is that of a copy from now on, which the caller keeps alive. What was held stays held. To be called when the last code of the program has run. False: a MemoryError.
    // There are writers that nobody counts (DEFERRED.md, "What a raw stream is lent"): whoever writes by what it has counted looks at the room all the same.
    ALWAYS_INLINE bool settle(JSValue& copy) { return !m_held || settleSlow(copy); }

    std::span<const uint8_t> span() const { return m_span; }

private:
    ALWAYS_INLINE bool getPlainBytes(JSValue value)
    {
        if (!value || !value.isCell() || value.asCell()->type() != PyBytesType)
            return false;
        // A class derived from bytes may have a __buffer__() of its own.
        auto* bytes = uncheckedDowncast<PyBytes>(value.asCell());
        if (bytes->type()->has(TypeFlag::Heap)) [[unlikely]]
            return false;
        m_span = bytes->span();
        return true;
    }
    bool getSlow(JSValue, bool isLast);
    bool settleSlow(JSValue& copy);
    void hold(PyByteArray*);
    void release();

    std::span<const uint8_t> m_span;
    // What is left of a Buffer once the bytes are known: see bufferReduce() in PyMemoryView.cpp. (A Buffer has sixteen words to set up, which shows in b.find(x).) Or the copy.
    JSCell* m_held { nullptr };
    void (*m_release)(JSGlobalObject*, JSValue held) { nullptr };
    ArrayBuffer* m_pinned { nullptr };
    JSGlobalObject* m_globalObject;
};

// bytes.hex()
JSValue hexOfBytes(JSGlobalObject*, JSValue bytesOrByteArray, JSValue separator, JSValue bytesPerSeparator);

// Bytes that are put together piece by piece. What a program asks for may be more than there is room for: the builder then takes
// no more, and makeBytes() raises a MemoryError. (A Vector ends the process, and holds 2**31 - 1 bytes at most.)
class ByteBuilder {
    WTF_MAKE_NONCOPYABLE(ByteBuilder);
public:
    ByteBuilder() = default;
    ~ByteBuilder()
    {
        if (m_bytes != m_ownRoom)
            fastFree(m_bytes);
    }

    void reserveInitialCapacity(size_t capacity) { makeRoomFor(capacity); }
    void append(uint8_t byte)
    {
        if (makeRoomFor(1)) [[likely]]
            m_bytes[m_size++] = byte;
    }
    void append(std::span<const uint8_t> bytes)
    {
        if (auto room = appendUninitialized(bytes.size()); room.data()) [[likely]]
            memcpy(room.data(), bytes.data(), bytes.size());
    }
    void appendRepeated(uint8_t byte, size_t count)
    {
        if (auto room = appendUninitialized(count); room.data()) [[likely]]
            memset(room.data(), byte, count);
    }
    // For the caller to write to. Nothing: there is no room, or none was asked for.
    std::span<uint8_t> appendUninitialized(size_t count)
    {
        if (!count || !makeRoomFor(count)) [[unlikely]]
            return { };
        m_size += count;
        return { m_bytes + m_size - count, count };
    }
    void shrink(size_t size)
    {
        ASSERT(size <= m_size);
        m_size = size;
    }
    void clear()
    {
        m_size = 0;
        m_hasOverflowed = false;
    }
    size_t size() const { return m_size; }
    uint8_t& operator[](size_t index)
    {
        ASSERT(index < m_size);
        return m_bytes[index];
    }
    bool hasOverflowed() const { return m_hasOverflowed; }
    std::span<const uint8_t> span() const { return { m_bytes, m_size }; }
    std::span<uint8_t> mutableSpan() { return { m_bytes, m_size }; }

protected:
    explicit ByteBuilder(std::span<uint8_t> ownRoom)
        : m_bytes(ownRoom.data())
        , m_ownRoom(ownRoom.data())
        , m_capacity(ownRoom.size())
    {
    }

private:
    bool makeRoomFor(size_t count)
    {
        if (count <= m_capacity - m_size) [[likely]]
            return !m_hasOverflowed;
        return makeRoomSlow(count);
    }
    bool makeRoomSlow(size_t count);

    uint8_t* m_bytes { nullptr };
    uint8_t* m_ownRoom { nullptr };
    size_t m_size { 0 };
    size_t m_capacity { 0 };
    bool m_hasOverflowed { false };
};
// With room in itself for what is mostly short.
template<size_t room>
class ByteBuilderWithRoom : public ByteBuilder {
public:
    ByteBuilderWithRoom()
        : ByteBuilder(std::span { m_room })
    {
    }

private:
    std::array<uint8_t, room> m_room;
};
// Null, with a MemoryError: there are too many.
PyBytes* makeBytes(JSGlobalObject*, const ByteBuilder&);
void appendUTF8(ByteBuilder&, char32_t);
// With the handler "surrogatepass" if `allowSurrogates`; otherwise false if there is a lone surrogate.
bool appendUTF8(ByteBuilder&, StringView, bool allowSurrogates = false);
// False: an exception, that of str.encode() for a lone surrogate or a MemoryError.
bool appendUTF8(JSGlobalObject*, ByteBuilder&, JSString*);

String bytesRepr(std::span<const uint8_t>);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
