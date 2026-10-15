/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
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

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace B3 {

class Arena {
    WTF_MAKE_NONCOPYABLE(Arena);
public:
    Arena() = default;
    JS_EXPORT_PRIVATE ~Arena();

    ALWAYS_INLINE void* allocate(size_t size, size_t alignment = sizeof(void*))
    {
        uintptr_t result = (m_head + (alignment - 1)) & ~(alignment - 1);
        if (result + size <= m_end) [[likely]] {
            m_head = result + size;
            return reinterpret_cast<void*>(result);
        }
        return allocateSlow(size, alignment);
    }

    void setKeepsFirstChunkForThread(bool value) { m_keepsFirstChunkForThread = value; }

private:
    struct Chunk {
        Chunk* next;
        size_t size;
    };

    static constexpr size_t firstChunkSize = 64 * KB;
    static constexpr size_t maxChunkSize = 4 * MB;

    JS_EXPORT_PRIVATE void* allocateSlow(size_t, size_t alignment);

    struct ChunkOfThread {
        ~ChunkOfThread();

        Chunk* chunk { nullptr };
    };
    static ChunkOfThread& chunkOfThread();

    uintptr_t m_head { 0 };
    uintptr_t m_end { 0 };
    Chunk* m_chunks { nullptr };
    bool m_keepsFirstChunkForThread { false };
};

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
