/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyHash.h"
#include "PyObject.h"

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
    PyByteStorage* storage() const { return m_storage.get(); }

    unsigned exportCount() const { return m_exports; }
    void addExport() { ++m_exports; }
    void removeExport() { --m_exports; }

private:
    PyByteArray(VM&, Structure*, PyType*);

    WriteBarrier<PyByteStorage> m_storage;
    size_t m_start { 0 }; // Where in the storage the first byte is.
    size_t m_length { 0 };
    unsigned m_exports { 0 };
};

// The bytes of anything that has them to show: bytes, bytearray, memoryview, array.array, whatever has __buffer__...
// They stay put only as long as nothing else runs, and the object has to be kept alive. To hold on to them, or to write, use
// getBuffer() in PyBuffer.h.
bool isBytesLike(JSValue);
// False: a TypeError, "a bytes-like object is required, not 'x'"; or, though it is bytes-like, a ValueError (a memoryview
// that was released), a BufferError (one whose items do not follow one another), or what __buffer__() raised. That may run
// code, so the bytes of a bytearray taken earlier are not to be relied on.
bool getBytesLike(JSGlobalObject*, JSValue, std::span<const uint8_t>&);
// The same for int() and float(), which read bytes and a bytearray where they are: a class derived from them is not asked for its __buffer__().
// False, and no exception: the object would not give them, whatever it raised. To the two it is then no text.
bool getBytesOfNumber(JSGlobalObject*, JSValue, std::span<const uint8_t>&);
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
