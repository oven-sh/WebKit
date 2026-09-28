/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StaticRegion.h"

#include "BAssert.h"
#include "BPlatform.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <utility>
#include <execinfo.h>
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

bool StaticRegion::map(Arena arena, int fileDescriptor, off_t offsetInFile, size_t size, size_t offsetInArena, bool isCode)
{
    RELEASE_BASSERT(offsetInArena <= arenaReservation && size <= arenaReservation - offsetInArena);
    if (!size)
        return true;
    void* wanted = reinterpret_cast<void*>(startOf(arena) + offsetInArena);
    if (arena == Arena::Image) {
        void* result = mmap(wanted, size, isCode ? PROT_READ | PROT_EXEC : PROT_READ, MAP_PRIVATE, fileDescriptor, offsetInFile);
        if (result == MAP_FAILED)
            return false;
        if (result != wanted) {
            munmap(result, size);
            return false;
        }
        return true;
    }
    // For finding out who writes to it, which is allowed, and costs a page each time.
    static const bool findWriters = !!getenv("BUN_STATIC_HEAP_READONLY");
    // (=logall: of what is expected to be written to as well.)
    static const bool ofAll = findWriters && !strcmp(getenv("BUN_STATIC_HEAP_READONLY"), "logall");
    bool isReadOnly = findWriters && (ofAll || (arena != Arena::MutableCells && arena != Arena::MutableMalloc));
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

// TEMPORARY-MALLOC-AUDIT. BUN_STATIC_HEAP_MALLOC_AUDIT=1: who asks for how much.
namespace {
struct AuditEntry {
    static constexpr unsigned depth = 7;
    void* frames[depth];
    uint64_t count;
    uint64_t bytes;
    uint64_t freed;
};
constexpr size_t auditCapacity = 1 << 16;
AuditEntry* s_audit;
uint64_t s_auditFreedBytes;
uint64_t s_auditFreedCount;
struct AuditOwner { const void* pointer; uint32_t entry; };
}

static AuditEntry* auditEntryForCaller()
{
    void* stack[AuditEntry::depth + 3] = { };
    backtrace(stack, AuditEntry::depth + 3);
    uint64_t hash = 1469598103934665603ull;
    for (unsigned i = 0; i < AuditEntry::depth; ++i)
        hash = (hash ^ reinterpret_cast<uintptr_t>(stack[i + 3])) * 1099511628211ull;
    for (size_t probe = 0; probe < auditCapacity; ++probe) {
        auto& entry = s_audit[(hash + probe) & (auditCapacity - 1)];
        if (!entry.count)
            memcpy(entry.frames, stack + 3, sizeof(entry.frames));
        if (!memcmp(entry.frames, stack + 3, sizeof(entry.frames)))
            return &entry;
    }
    return nullptr;
}

void StaticRegion::dumpMallocAudit()
{
    if (!s_audit)
        return;
    fprintf(stderr, "MALLOCAUDIT freed and not used again: %llu bytes in %llu allocations\n", static_cast<unsigned long long>(s_auditFreedBytes), static_cast<unsigned long long>(s_auditFreedCount));
    for (unsigned round = 0; round < 60; ++round) {
        AuditEntry* best = nullptr;
        for (size_t i = 0; i < auditCapacity; ++i) {
            if (s_audit[i].count && (!best || s_audit[i].bytes > best->bytes))
                best = &s_audit[i];
        }
        if (!best)
            break;
        fprintf(stderr, "MALLOCAUDIT %llu bytes in %llu allocations\n", static_cast<unsigned long long>(best->bytes), static_cast<unsigned long long>(best->count));
        backtrace_symbols_fd(best->frames, AuditEntry::depth, 2);
        best->count = 0;
    }
}

// What is freed while the region is being built is for the next one who asks for as much: what is not used again is in the file
// all the same. By how many times 16 bytes it takes up, with what precedes it.
namespace {
struct FreeBlock {
    FreeBlock* next;
};
constexpr size_t numberOfFreeLists = 4096;
FreeBlock* s_freeLists[2][numberOfFreeLists];
}

static size_t freeListFor(size_t size) { return (size + sizeOfHeader + 15) / 16; }

void StaticRegion::forgetWhatIsFree()
{
    for (auto& lists : s_freeLists) {
        for (auto*& list : lists) {
            while (list)
                list = std::exchange(list->next, nullptr);
        }
    }
}

void* StaticRegion::tryMallocSlow(size_t size, size_t alignment)
{
    if (!t_isAllocating)
        return nullptr;
    if (alignment < 16)
        alignment = 16;
    if (size_t index = freeListFor(size); alignment == 16 && index < numberOfFreeLists) {
        if (FreeBlock*& list = s_freeLists[t_isAllocatingWhatIsMutable][index]) {
            FreeBlock* block = list;
            list = std::exchange(block->next, nullptr);
            memcpy(reinterpret_cast<char*>(block) - sizeOfHeader, &size, sizeof(size));
            return block;
        }
    }
    static const bool audit = !!getenv("BUN_STATIC_HEAP_MALLOC_AUDIT");
    if (audit) {
        if (!s_audit)
            s_audit = static_cast<AuditEntry*>(calloc(auditCapacity, sizeof(AuditEntry)));
        if (auto* entry = auditEntryForCaller()) {
            entry->count++;
            entry->bytes += (size + sizeOfHeader + 15) & ~static_cast<size_t>(15);
        }
    }
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
    // The last there is has room after it.
    if (s_isBuilding && t_isAllocating) {
        uintptr_t end = reinterpret_cast<uintptr_t>(pointer) + mallocSize(pointer);
        for (Arena arena : { Arena::Malloc, Arena::MutableMalloc }) {
            size_t& used = s_used[static_cast<size_t>(arena)];
            if (end == startOf(arena) + used && newSize >= mallocSize(pointer) && newSize - mallocSize(pointer) <= arenaReservation - used) {
                used += newSize - mallocSize(pointer);
                memcpy(static_cast<char*>(pointer) - sizeOfHeader, &newSize, sizeof(newSize));
                return pointer;
            }
        }
    }
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
            s_auditFreedBytes += (mallocSize(pointer) + sizeOfHeader + 15) & ~static_cast<size_t>(15);
            s_auditFreedCount++;
            memset(pointer, 0, mallocSize(pointer));
            if (size_t index = freeListFor(mallocSize(pointer)); index < numberOfFreeLists && !(address & 15)) {
                auto* block = static_cast<FreeBlock*>(pointer);
                block->next = std::exchange(s_freeLists[arena == Arena::MutableMalloc][index], block);
            }
            return;
        }
    }
}

} // namespace bmalloc
