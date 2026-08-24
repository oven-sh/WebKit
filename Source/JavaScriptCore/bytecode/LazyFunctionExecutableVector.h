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
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "WriteBarrier.h"
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>

namespace JSC {

class Decoder;
class UnlinkedFunctionExecutable;

// UnlinkedCodeBlock::m_functionExprs: the block's function-expression executables, which for a block decoded from the
// bytecode cache are created on first use (op_new_func_exp and friends) rather than when the block is decoded. One
// pointer in the cell like FixedVector; what is needed to decode an entry later (the Decoder and where the slots are in
// its payload) lives in the out-of-line header, so only blocks that have function expressions pay for it.
class LazyFunctionExecutableVector {
    WTF_MAKE_NONCOPYABLE(LazyFunctionExecutableVector);
public:
    LazyFunctionExecutableVector() = default;
    explicit LazyFunctionExecutableVector(size_t size)
    {
        if (size)
            m_storage = Storage::create(size);
    }
    LazyFunctionExecutableVector(LazyFunctionExecutableVector&& other)
        : m_storage(std::exchange(other.m_storage, nullptr)) { }
    LazyFunctionExecutableVector& operator=(LazyFunctionExecutableVector&& other)
    {
        Storage::destroy(std::exchange(m_storage, std::exchange(other.m_storage, nullptr)));
        return *this;
    }
    ~LazyFunctionExecutableVector() { Storage::destroy(m_storage); }

    size_t size() const { return m_storage ? m_storage->size : 0; }
    size_t byteSize() const { return m_storage ? Storage::allocationSize(m_storage->size) : 0; }
    bool isEmpty() const { return !size(); }

    // Entries not yet materialized are null; see UnlinkedCodeBlock::functionExpr().
    WriteBarrier<UnlinkedFunctionExecutable>& at(size_t i) LIFETIME_BOUND { RELEASE_ASSERT(i < size()); return m_storage->entries()[i]; }
    const WriteBarrier<UnlinkedFunctionExecutable>& at(size_t i) const LIFETIME_BOUND { RELEASE_ASSERT(i < size()); return m_storage->entries()[i]; }
    WriteBarrier<UnlinkedFunctionExecutable>& operator[](size_t i) LIFETIME_BOUND { return at(i); }
    std::span<WriteBarrier<UnlinkedFunctionExecutable>> span() LIFETIME_BOUND { return m_storage ? std::span { m_storage->entries(), m_storage->size } : std::span<WriteBarrier<UnlinkedFunctionExecutable>> { }; }
    std::span<const WriteBarrier<UnlinkedFunctionExecutable>> span() const LIFETIME_BOUND { return m_storage ? std::span { m_storage->entries(), m_storage->size } : std::span<const WriteBarrier<UnlinkedFunctionExecutable>> { }; }

    // Set by the cache decoder: entries stay null and are decoded from `slots` (an array of the cache's per-entry
    // records inside decoder's payload) on demand. `remaining` counts null entries; the Decoder is dropped at zero.
    void setPending(RefPtr<Decoder>&&, const void* slots);
    bool hasPending() const { return m_storage && m_storage->remaining; }
    Decoder* decoder() const { return m_storage ? m_storage->decoder.get() : nullptr; }
    const void* slots() const { return m_storage ? m_storage->slots : nullptr; }
    void didMaterialize()
    {
        ASSERT(m_storage && m_storage->remaining);
        if (!--m_storage->remaining)
            m_storage->decoder = nullptr;
    }

private:
    struct Storage {
        static size_t allocationSize(size_t size) { return sizeof(Storage) + size * sizeof(WriteBarrier<UnlinkedFunctionExecutable>); }
        static Storage* create(size_t size)
        {
            static_assert(alignof(WriteBarrier<UnlinkedFunctionExecutable>) <= alignof(Storage));
            Storage* storage = new (NotNull, fastMalloc(allocationSize(size))) Storage;
            storage->size = size;
            for (size_t i = 0; i < size; ++i)
                new (NotNull, storage->entries() + i) WriteBarrier<UnlinkedFunctionExecutable>;
            return storage;
        }
        static void destroy(Storage*);
        WriteBarrier<UnlinkedFunctionExecutable>* entries() { return std::bit_cast<WriteBarrier<UnlinkedFunctionExecutable>*>(this + 1); }

        RefPtr<Decoder> decoder;
        const void* slots { nullptr };
        size_t size { 0 };
        size_t remaining { 0 };
    };
    Storage* m_storage { nullptr };
};

} // namespace JSC
