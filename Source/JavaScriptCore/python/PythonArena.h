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

#include "ParserArena.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace Python {

// Where the syntax tree and what hangs off it live. Nothing in it is destroyed one by one: it all goes when the arena goes,
// so what is put here has to be trivially destructible. Identifiers are the exception, and have an arena of their own.
class Arena {
    WTF_MAKE_NONCOPYABLE(Arena);
public:
    Arena() = default;

    ~Arena()
    {
        for (void* chunk : m_chunks)
            fastFree(chunk);
    }

    void* allocate(size_t size)
    {
        size = roundUpToMultipleOf<alignment>(size);
        if (size > static_cast<size_t>(m_end - m_next)) [[unlikely]]
            return allocateSlow(size);
        void* result = m_next;
        m_next += size;
        return result;
    }

    template<typename T, typename... Arguments>
    T* create(Arguments&&... arguments)
    {
        static_assert(std::is_trivially_destructible_v<T>);
        static_assert(alignof(T) <= alignment);
        return new (NotNull, allocate(sizeof(T))) T(std::forward<Arguments>(arguments)...);
    }

    template<typename T, size_t inlineCapacity>
    std::span<T> copy(const Vector<T, inlineCapacity>& vector)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (vector.isEmpty())
            return { };
        T* result = static_cast<T*>(allocate(vector.size() * sizeof(T)));
        memcpySpan(std::span<T> { result, vector.size() }, vector.span());
        return { result, vector.size() };
    }

    IdentifierArena& identifiers() { return m_identifiers; }

    // How long an int can be that is written in decimal, or 0 for any length: sys.set_int_max_str_digits(). It is looked at when the source is first read.
    unsigned maximumDigitsOfIntLiteral { 0 };

private:
    static constexpr size_t alignment = 8;
    static constexpr size_t chunkSize = 16 * KB;

    void* allocateSlow(size_t size)
    {
        // What does not fit a chunk gets one of its own, and the current one goes on being filled.
        if (size > chunkSize / 4) {
            void* chunk = fastMalloc(size);
            m_chunks.append(chunk);
            return chunk;
        }
        char* chunk = static_cast<char*>(fastMalloc(chunkSize));
        m_chunks.append(chunk);
        m_next = chunk + size;
        m_end = chunk + chunkSize;
        return chunk;
    }

    char* m_next { nullptr };
    char* m_end { nullptr };
    Vector<void*> m_chunks;
    IdentifierArena m_identifiers;
};

} } // namespace JSC::Python
