/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StaticRegion.h"

#include "BAssert.h"
#include "BPlatform.h"
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

#if BUSE(MIMALLOC)
#include "mimalloc.h"
#endif

namespace bmalloc {

bool StaticRegion::s_isBuilding = false;
static size_t s_used[StaticRegion::numberOfArenas];
static thread_local bool t_isAllocating = false;
static thread_local bool t_isAllocatingWhatIsMutable = false;

bool StaticRegion::beginBuilding()
{
    RELEASE_BASSERT(!s_isBuilding);
    constexpr size_t size = numberOfArenasInFile * arenaReservation;
    void* result = mmap(reinterpret_cast<void*>(base), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result == MAP_FAILED)
        return false;
    if (result != reinterpret_cast<void*>(base)) {
        munmap(result, size);
        return false;
    }
    memset(s_used, 0, sizeof(s_used));
    s_isBuilding = true;
    return true;
}

void StaticRegion::endBuilding()
{
    RELEASE_BASSERT(s_isBuilding && !t_isAllocating);
    s_isBuilding = false;
    munmap(reinterpret_cast<void*>(base), numberOfArenasInFile * arenaReservation);
}

void* StaticRegion::allocate(Arena arena, size_t size, size_t alignment, size_t misalignment)
{
    RELEASE_BASSERT(s_isBuilding && alignment && !(alignment & (alignment - 1)) && misalignment < alignment);
    size_t& used = s_used[static_cast<size_t>(arena)];
    size_t offset = ((used + alignment - 1 - misalignment) & ~(alignment - 1)) + misalignment;
    RELEASE_BASSERT(offset >= used && size <= arenaReservation && offset <= arenaReservation - size);
    used = offset + size;
    return reinterpret_cast<void*>(startOf(arena) + offset);
}

size_t StaticRegion::used(Arena arena)
{
    return s_used[static_cast<size_t>(arena)];
}

StaticRegion::AllocationScope::AllocationScope(bool inRegion)
    : m_previous(t_isAllocating)
{
    RELEASE_BASSERT(s_isBuilding);
    t_isAllocating = inRegion;
}

StaticRegion::AllocationScope::~AllocationScope()
{
    t_isAllocating = m_previous;
}

bool StaticRegion::isAllocatingOnThisThread()
{
    return t_isAllocating;
}

StaticRegion::MutableScope::MutableScope()
    : m_previous(t_isAllocatingWhatIsMutable)
{
    t_isAllocatingWhatIsMutable = true;
}

StaticRegion::MutableScope::~MutableScope()
{
    t_isAllocatingWhatIsMutable = m_previous;
}

bool StaticRegion::isAllocatingWhatIsMutable()
{
    return t_isAllocatingWhatIsMutable;
}

void StaticRegion::mapBss()
{
    static bool isMapped = false;
    if (isMapped)
        return;
    isMapped = true;
    void* wanted = reinterpret_cast<void*>(startOf(Arena::Bss));
    void* result = mmap(wanted, arenaReservation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    RELEASE_BASSERT(result == wanted);
}

bool StaticRegion::map(Arena arena, int fileDescriptor, off_t offsetInFile, size_t size)
{
    RELEASE_BASSERT(size <= arenaReservation);
    if (!size)
        return true;
    void* wanted = reinterpret_cast<void*>(startOf(arena));
    // For finding out who writes to it, which is allowed, and costs a page each time.
    static const bool findWriters = !!getenv("BUN_STATIC_HEAP_READONLY");
    bool isReadOnly = findWriters && arena != Arena::MutableCells && arena != Arena::MutableMalloc;
    void* result = mmap(wanted, size, isReadOnly ? PROT_READ : PROT_READ | PROT_WRITE, MAP_PRIVATE, fileDescriptor, offsetInFile);
    if (result == MAP_FAILED)
        return false;
    if (result != wanted) {
        munmap(result, size);
        return false;
    }
    return true;
}

// What is asked for is preceded by how much was asked for.
static constexpr size_t sizeOfHeader = 8;

void* StaticRegion::tryMallocSlow(size_t size, size_t alignment)
{
    if (!t_isAllocating)
        return nullptr;
    if (alignment < 16)
        alignment = 16;
    auto* header = static_cast<char*>(allocate(t_isAllocatingWhatIsMutable ? Arena::MutableMalloc : Arena::Malloc, size + sizeOfHeader, alignment, alignment - sizeOfHeader));
    memcpy(header, &size, sizeof(size));
    return header + sizeOfHeader;
}

size_t StaticRegion::mallocSize(const void* pointer)
{
    size_t size;
    memcpy(&size, static_cast<const char*>(pointer) - sizeOfHeader, sizeof(size));
    return size;
}

void* StaticRegion::reallocate(void* pointer, size_t newSize)
{
    void* result = tryMalloc(newSize);
    if (!result) {
#if BUSE(MIMALLOC)
        result = mi_malloc(newSize);
#else
        result = ::malloc(newSize);
#endif
    }
    if (!result)
        return nullptr;
    size_t oldSize = mallocSize(pointer);
    memcpy(result, pointer, oldSize < newSize ? oldSize : newSize);
    didFree(pointer);
    return result;
}

void StaticRegion::didFreeSlow(void* pointer)
{
    uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    for (Arena arena : { Arena::Malloc, Arena::MutableMalloc }) {
        if (address - startOf(arena) < used(arena)) {
            memset(pointer, 0, mallocSize(pointer));
            return;
        }
    }
}

} // namespace bmalloc
