/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "B3CompilationScratch.h"

#if ENABLE(B3_JIT)

#include "B3AbstractHeapRepository.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace B3 {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CompilationScratch);

CompilationScratch::CompilationScratch(HeapRepositoryFactory createHeaps)
    : m_createHeaps(createHeaps)
{
}

CompilationScratch::~CompilationScratch()
{
    RELEASE_ASSERT(!m_numberOfUsers);
    for (Arena::Chunk* chunk : m_firstChunks)
        fastFree(chunk);
}

void CompilationScratch::setHeapRepositoryFactory(HeapRepositoryFactory createHeaps)
{
    if (m_createHeaps == createHeaps)
        return;
    RELEASE_ASSERT(!m_numberOfUsers && m_heaps.isEmpty());
    m_createHeaps = createHeaps;
}

std::unique_ptr<AbstractHeapRepository> CompilationScratch::createHeaps()
{
    return m_createHeaps ? m_createHeaps() : makeUnique<AbstractHeapRepository>();
}

std::unique_ptr<AbstractHeapRepository> CompilationScratch::takeHeaps()
{
    return m_heaps.isEmpty() ? createHeaps() : m_heaps.takeLast();
}

void CompilationScratch::giveBackHeaps(std::unique_ptr<AbstractHeapRepository>&& heaps)
{
    heaps->detachProcedure();
    if (m_heaps.size() < maxRetainedRepositories)
        m_heaps.append(WTF::move(heaps));
}

Arena::Chunk* CompilationScratch::takeFirstChunk()
{
    return m_firstChunks.isEmpty() ? nullptr : m_firstChunks.takeLast();
}

void CompilationScratch::giveBackFirstChunk(Arena::Chunk* chunk)
{
    ASSERT(chunk->size == Arena::firstChunkSize);
    if (m_firstChunks.size() < maxRetainedChunks)
        m_firstChunks.append(chunk);
    else
        fastFree(chunk);
}

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT)
