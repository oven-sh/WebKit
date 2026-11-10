/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineArena.h"

#if ENABLE(AOT)

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <wtf/FastMalloc.h>

#if ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

// Every allocate() therefore falls through to allocateSlow(), which unpoisons the bytes it hands out.
static ALWAYS_INLINE uintptr_t poisonFreeMachineArenaBytes([[maybe_unused]] uintptr_t begin, uintptr_t end)
{
#if ASAN_ENABLED
    __asan_poison_memory_region(reinterpret_cast<void*>(begin), end - begin);
    return begin;
#else
    return end;
#endif
}

static ALWAYS_INLINE void freeMachineArenaChunk(void* chunk, [[maybe_unused]] size_t size)
{
#if ASAN_ENABLED
    __asan_unpoison_memory_region(chunk, size);
#endif
    fastFree(chunk);
}

Arena::~Arena()
{
    reset();
    if (m_chunk)
        freeMachineArenaChunk(m_chunk, m_chunk->size);
    while (KeptObject* kept = m_keptObjects) {
        m_keptObjects = kept->next;
        kept->destroy(kept->object);
        fastFree(kept->object);
        fastFree(kept);
    }
}

void* Arena::allocateSlow(size_t bytes, size_t alignment)
{
#if ASAN_ENABLED
    if (m_chunk) {
        uintptr_t endOfChunk = reinterpret_cast<uintptr_t>(m_chunk) + m_chunk->size;
        uintptr_t start = (m_cursor + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
        if (start >= m_cursor && start <= endOfChunk && bytes <= endOfChunk - start && bytesTakenBy(bytes) <= endOfChunk - start) {
            __asan_unpoison_memory_region(reinterpret_cast<void*>(m_cursor), start + bytes - m_cursor);
            m_cursor = start + bytesTakenBy(bytes);
            m_end = m_cursor;
            return reinterpret_cast<void*>(start);
        }
    }
#endif
    if (m_endOfChunk)
        return allocateOutsideRoomLetGo(bytes, alignment);

    static_assert(!(sizeof(Chunk) % alignmentOfCursor));
    size_t overhead = sizeof(Chunk) + alignment - 1 + alignmentOfCursor - 1;
    RELEASE_ASSERT(bytes <= std::numeric_limits<size_t>::max() - overhead);
    size_t size = std::max(m_chunk ? m_chunk->size * 2 : smallestChunkSize, bytes + overhead);
    Chunk* chunk = new (NotNull, fastMalloc(size)) Chunk { m_chunk, size };

    m_bytesInFullChunks += m_cursor - m_begin;
    m_chunk = chunk;
    m_begin = reinterpret_cast<uintptr_t>(chunk + 1);
    m_cursor = m_begin;
    m_end = poisonFreeMachineArenaBytes(m_begin, reinterpret_cast<uintptr_t>(chunk) + size);
    return allocate(bytes, alignment);
}

bool Arena::holds(uintptr_t begin, uintptr_t end) const
{
    for (const Chunk* chunk = m_chunk; chunk; chunk = chunk->previous) {
        if (begin >= reinterpret_cast<uintptr_t>(chunk + 1) && end <= reinterpret_cast<uintptr_t>(chunk) + chunk->size)
            return true;
    }
    return false;
}

void Arena::letGoOfLargeRoom(uintptr_t begin, size_t bytes)
{
    constexpr uintptr_t maskOfCursor = ~(static_cast<uintptr_t>(alignmentOfCursor) - 1);
    uintptr_t end = (begin + bytes) & maskOfCursor;
    begin = (begin + alignmentOfCursor - 1) & maskOfCursor;
    if (end <= begin || end - begin < smallestRoomKept || !holds(begin, end))
        return;

#if ASAN_ENABLED
    __asan_poison_memory_region(reinterpret_cast<void*>(begin), end - begin);
#else
#if ASSERT_ENABLED
    std::memset(reinterpret_cast<void*>(begin), 0xab, end - begin);
#endif
    if (!m_forgetsRoomsLetGoAtReset) {
        m_forgetsRoomsLetGoAtReset = true;
        addFinalizer(this, [](void* arena) { static_cast<Arena*>(arena)->forgetRoomsLetGo(); });
    }
    keepRoomLetGo(begin, end);
    if (!m_endOfChunk)
        standInRoomLetGo();
#endif
}

void Arena::keepRoomLetGo(uintptr_t begin, uintptr_t end)
{
    RoomLetGo** linkToRoomInFront = nullptr;
    RoomLetGo** link = &m_roomsLetGo;
    while (*link && reinterpret_cast<uintptr_t>(*link) < begin) {
        linkToRoomInFront = link;
        link = &(*link)->next;
    }

    RoomLetGo* behind = *link;
    if (behind && reinterpret_cast<uintptr_t>(behind) == end) {
        end = behind->end;
        behind = behind->next;
    }
    if (linkToRoomInFront && (*linkToRoomInFront)->end == begin) {
        begin = reinterpret_cast<uintptr_t>(*linkToRoomInFront);
        link = linkToRoomInFront;
    }

    uintptr_t& cursorInChunk = m_endOfChunk ? m_cursorInChunk : m_cursor;
    if (end == cursorInChunk) {
        cursorInChunk = begin;
        *link = behind;
        return;
    }
    if (end - begin < smallestRoomKept)
        return;
    *link = new (NotNull, reinterpret_cast<void*>(begin)) RoomLetGo { behind, end };
}

void Arena::standInRoomLetGo()
{
    ASSERT(!m_endOfChunk);
    RoomLetGo* room = m_roomsLetGo;
    if (!room)
        return;
    m_roomsLetGo = room->next;
    m_cursorInChunk = m_cursor;
    m_endOfChunk = m_end;
    m_cursor = reinterpret_cast<uintptr_t>(room);
    m_end = room->end;
}

void* Arena::allocateOutsideRoomLetGo(size_t bytes, size_t alignment)
{
    keepRoomLetGo(m_cursor, m_end);
    m_cursor = m_cursorInChunk;
    m_end = m_endOfChunk;
    m_endOfChunk = 0;

    size_t bytesTaken = bytesTakenBy(bytes);
    RELEASE_ASSERT(bytesTaken >= bytes);
    for (RoomLetGo** link = &m_roomsLetGo; RoomLetGo* room = *link; link = &room->next) {
        uintptr_t start = (reinterpret_cast<uintptr_t>(room) + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
        if (start > room->end || room->end - start < bytesTaken)
            continue;
        *link = room->next;
        m_cursorInChunk = m_cursor;
        m_endOfChunk = m_end;
        m_cursor = reinterpret_cast<uintptr_t>(room);
        m_end = room->end;
        return allocate(bytes, alignment);
    }

    void* result = allocate(bytes, alignment);
    standInRoomLetGo();
    return result;
}

void Arena::forgetRoomsLetGo()
{
    if (m_endOfChunk) {
        m_cursor = m_cursorInChunk;
        m_end = m_endOfChunk;
        m_endOfChunk = 0;
    }
    m_roomsLetGo = nullptr;
    m_forgetsRoomsLetGoAtReset = false;
}

void Arena::addFinalizer(void* object, void (*destroy)(void*))
{
    m_finalizers = new (NotNull, allocate(sizeof(Finalizer), alignof(Finalizer))) Finalizer { m_finalizers, object, destroy };
}

void* Arena::allocateKeptObject(size_t bytes, const char* keyOfType, void (*destroy)(void*))
{
    void* object = fastMalloc(bytes);
    m_keptObjects = new (NotNull, fastMalloc(sizeof(KeptObject))) KeptObject { m_keptObjects, keyOfType, object, destroy };
    return object;
}

void Arena::reset()
{
    while (Finalizer* finalizer = m_finalizers) {
        m_finalizers = finalizer->next;
        finalizer->destroy(finalizer->object);
    }

    if (!m_chunk)
        return;

    for (Chunk* chunk = std::exchange(m_chunk->previous, nullptr); chunk;) {
        ASSERT(chunk->size < m_chunk->size);
        Chunk* previous = chunk->previous;
        freeMachineArenaChunk(chunk, chunk->size);
        chunk = previous;
    }

    if (m_chunk->size > largestChunkKept) {
        freeMachineArenaChunk(m_chunk, m_chunk->size);
        m_chunk = nullptr;
        m_bytesInFullChunks = 0;
        m_begin = 0;
        m_cursor = 0;
        m_end = 0;
        return;
    }

#if ASSERT_ENABLED
#if ASAN_ENABLED
    __asan_unpoison_memory_region(reinterpret_cast<void*>(m_begin), m_cursor - m_begin);
#endif
    std::memset(reinterpret_cast<void*>(m_begin), 0xab, m_cursor - m_begin);
#endif
    m_bytesInFullChunks = 0;
    m_end = poisonFreeMachineArenaBytes(m_begin, m_end);
    m_cursor = m_begin;
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
