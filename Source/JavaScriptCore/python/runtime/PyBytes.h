/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyObject.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

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
    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(this + 1); }
    uint8_t* mutableData() { return reinterpret_cast<uint8_t*>(this + 1); }

private:
    PyBytes(VM&, Structure*, PyType*, size_t length);

    size_t m_length;
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
    uint8_t* data() { return reinterpret_cast<uint8_t*>(this + 1); }

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
    std::span<const uint8_t> span() const { return { m_storage ? m_storage->data() : nullptr, m_length }; }
    std::span<uint8_t> mutableSpan() { return { m_storage ? m_storage->data() : nullptr, m_length }; }
    // New bytes are zero. False, with a BufferError, if a memoryview is looking, or a MemoryError.
    bool resize(JSGlobalObject*, size_t length);
    void append(JSGlobalObject*, std::span<const uint8_t>);
    // How many bytes there is room for, counting the NUL that is kept after the last. 0: nothing is allocated.
    size_t capacity() const { return m_storage ? m_storage->capacity() : 0; }
    PyByteStorage* storage() const { return m_storage.get(); }

    unsigned exportCount() const { return m_exports; }
    void addExport() { ++m_exports; }
    void removeExport() { --m_exports; }

private:
    PyByteArray(VM&, Structure*, PyType*);

    WriteBarrier<PyByteStorage> m_storage;
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
// bytes.hex()
JSValue hexOfBytes(JSGlobalObject*, JSValue bytesOrByteArray, JSValue separator, JSValue bytesPerSeparator);

String bytesRepr(std::span<const uint8_t>);
int64_t hashOfBytes(std::span<const uint8_t>);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
