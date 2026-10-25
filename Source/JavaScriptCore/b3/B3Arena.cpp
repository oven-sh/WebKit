/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3Arena.h"

#if ENABLE(B3_JIT)

#include "B3CompilationScratch.h"
#include <wtf/FastMalloc.h>

namespace JSC { namespace B3 {

Arena::Arena(CompilationScratch* scratch)
    : m_scratch(scratch)
{
    if (m_scratch)
        ++m_scratch->m_numberOfUsers;
}

Arena::~Arena()
{
    for (Chunk* chunk = m_chunks; chunk;) {
        Chunk* next = chunk->next;
        if (!next && m_scratch && chunk->size == firstChunkSize)
            m_scratch->giveBackFirstChunk(chunk);
        else
            fastFree(chunk);
        chunk = next;
    }
    if (m_scratch)
        --m_scratch->m_numberOfUsers;
}

void* Arena::allocateSlow(size_t size, size_t alignment)
{
    size_t needed = sizeof(Chunk) + alignment + size;
    Chunk* chunk = nullptr;
    if (!m_chunks && m_scratch && needed <= firstChunkSize)
        chunk = m_scratch->takeFirstChunk();
    if (!chunk) {
        size_t chunkSize = std::max(m_chunks ? std::min(m_chunks->size * 2, maxChunkSize) : firstChunkSize, needed);
        chunk = static_cast<Chunk*>(fastMalloc(chunkSize));
        chunk->size = chunkSize;
    }
    chunk->next = m_chunks;
    m_chunks = chunk;
    m_head = reinterpret_cast<uintptr_t>(chunk + 1);
    m_end = reinterpret_cast<uintptr_t>(chunk) + chunk->size;
    return allocate(size, alignment);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
