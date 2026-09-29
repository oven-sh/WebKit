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
static uint32_t* s_sizes; // See sizeOfImmutable().
static size_t s_capacityOfSizes;

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
    ::free(std::exchange(s_sizes, nullptr));
    s_capacityOfSizes = 0;
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

// In Arena::MutableMalloc, what is asked for is preceded by how much was asked for: it may be given back, or be found too small,
// when the program runs.
static constexpr size_t sizeOfHeader = 8;

// What is in Arena::Malloc stays as it is once the region is built, so how big it is only matters until then, and is not in the
// file: it is kept here, by where the thing is. And it is only kept clear of what comes next by as much as it has to be. Everything is
// a multiple of this.
static constexpr size_t unit = 8;

static bool isInImmutableMalloc(const void* pointer)
{
    return reinterpret_cast<uintptr_t>(pointer) - StaticRegion::startOf(StaticRegion::Arena::Malloc) < StaticRegion::arenaReservation;
}

static uint32_t& sizeOfImmutable(const void* pointer)
{
    size_t index = (reinterpret_cast<uintptr_t>(pointer) - StaticRegion::startOf(StaticRegion::Arena::Malloc)) / unit;
    if (index >= s_capacityOfSizes) {
        size_t capacity = s_capacityOfSizes ? s_capacityOfSizes : 1 << 20;
        while (capacity <= index)
            capacity *= 2;
        s_sizes = static_cast<uint32_t*>(::realloc(s_sizes, capacity * sizeof(uint32_t)));
        RELEASE_BASSERT(s_sizes);
        memset(s_sizes + s_capacityOfSizes, 0, (capacity - s_capacityOfSizes) * sizeof(uint32_t));
        s_capacityOfSizes = capacity;
    }
    return s_sizes[index];
}

static size_t unitsFor(size_t size) { return size ? (size + unit - 1) / unit : 1; }

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
    fprintf(stderr, "MALLOCAUDIT freed, whether or not it was used again: %llu bytes in %llu allocations\n", static_cast<unsigned long long>(s_auditFreedBytes), static_cast<unsigned long long>(s_auditFreedCount));
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
uint64_t s_freeListsInUse[2][numberOfFreeLists / 64];
}
// Those are: of Arena::MutableMalloc, as said; and of Arena::Malloc, by how many units, those that are at a multiple of 16. These are
// the rest of Arena::Malloc's.
static constexpr size_t immutableAtMultipleOf16 = 0;
static constexpr size_t mutableOnes = 1;
static constexpr size_t immutableOthers = 2;
namespace {
FreeBlock* s_moreFreeLists[numberOfFreeLists];
uint64_t s_moreFreeListsInUse[numberOfFreeLists / 64];
}
static FreeBlock** freeLists(size_t which) { return which == immutableOthers ? s_moreFreeLists : s_freeLists[which]; }
static uint64_t* freeListsInUse(size_t which) { return which == immutableOthers ? s_moreFreeListsInUse : s_freeListsInUse[which]; }

static size_t freeListFor(size_t size) { return (size + sizeOfHeader + 15) / 16; }

static void pushFree(size_t which, size_t index, void* pointer)
{
    auto* block = static_cast<FreeBlock*>(pointer);
    block->next = std::exchange(freeLists(which)[index], block);
    freeListsInUse(which)[index / 64] |= 1ull << (index % 64);
}

static void* popFree(size_t which, size_t index)
{
    FreeBlock*& list = freeLists(which)[index];
    FreeBlock* block = list;
    list = std::exchange(block->next, nullptr);
    if (!list)
        freeListsInUse(which)[index / 64] &= ~(1ull << (index % 64));
    return block;
}

// So many units of Arena::Malloc, all zero. (What is too big for the lists is not used again.)
static void pushFreeImmutable(void* pointer, size_t units)
{
    if (units && units < numberOfFreeLists)
        pushFree(reinterpret_cast<uintptr_t>(pointer) & 15 ? immutableOthers : immutableAtMultipleOf16, units, pointer);
}

// The first list, from that one on, that has something in it.
static size_t firstFreeListInUse(size_t which, size_t index)
{
    for (size_t word = index / 64; word < numberOfFreeLists / 64; ++word) {
        uint64_t bits = freeListsInUse(which)[word];
        if (word == index / 64)
            bits &= ~0ull << (index % 64);
        if (bits)
            return word * 64 + __builtin_ctzll(bits);
    }
    return numberOfFreeLists;
}

size_t StaticRegion::bytesThatAreFree()
{
    size_t bytes = 0;
    for (size_t which : { immutableAtMultipleOf16, mutableOnes, immutableOthers }) {
        for (size_t index = 0; index < numberOfFreeLists; ++index) {
            for (FreeBlock* block = freeLists(which)[index]; block; block = block->next)
                bytes += index * (which == mutableOnes ? 16 : unit);
        }
    }
    return bytes;
}

void StaticRegion::forgetWhatIsFree()
{
    for (size_t which : { immutableAtMultipleOf16, mutableOnes, immutableOthers }) {
        for (size_t index = 0; index < numberOfFreeLists; ++index) {
            FreeBlock*& list = freeLists(which)[index];
            while (list)
                list = std::exchange(list->next, nullptr);
        }
    }
    memset(s_freeListsInUse, 0, sizeof(s_freeListsInUse));
    memset(s_moreFreeListsInUse, 0, sizeof(s_moreFreeListsInUse));
}

static void noteForAudit(size_t bytes)
{
    static const bool audit = !!getenv("BUN_STATIC_HEAP_MALLOC_AUDIT");
    if (!audit)
        return;
    if (!s_audit)
        s_audit = static_cast<AuditEntry*>(calloc(auditCapacity, sizeof(AuditEntry)));
    if (auto* entry = auditEntryForCaller()) {
        entry->count++;
        entry->bytes += bytes;
    }
}

static void* mallocImmutable(size_t size, size_t alignment)
{
    RELEASE_BASSERT(size <= UINT32_MAX);
    // Nothing whose size is not a multiple of 16 is, or is made of, what has to be at one.
    if (alignment <= 16)
        alignment = size % 16 || !size ? unit : 16;
    size_t units = unitsFor(size);
    if (alignment <= 16 && units + 1 < numberOfFreeLists) {
        // The smallest that will do. What is left over of it is for somebody else.
        size_t atMultiple = firstFreeListInUse(immutableAtMultipleOf16, units);
        size_t other = firstFreeListInUse(immutableOthers, alignment == 16 ? units + 1 : units);
        if (atMultiple < numberOfFreeLists || other < numberOfFreeLists) {
            bool takesOther = other < atMultiple || (other == atMultiple && alignment == unit);
            size_t found = takesOther ? other : atMultiple;
            auto* block = static_cast<char*>(popFree(takesOther ? immutableOthers : immutableAtMultipleOf16, found));
            if (takesOther && alignment == 16) {
                pushFreeImmutable(block, 1);
                block += unit;
                --found;
            }
            pushFreeImmutable(block + units * unit, found - units);
            sizeOfImmutable(block) = static_cast<uint32_t>(size);
            return block;
        }
    }
    noteForAudit(units * unit);
    uintptr_t endOfLast = StaticRegion::startOf(StaticRegion::Arena::Malloc) + StaticRegion::used(StaticRegion::Arena::Malloc);
    auto* block = static_cast<char*>(StaticRegion::allocate(StaticRegion::Arena::Malloc, units * unit, alignment));
    pushFreeImmutable(reinterpret_cast<void*>(endOfLast), (reinterpret_cast<uintptr_t>(block) - endOfLast) / unit);
    sizeOfImmutable(block) = static_cast<uint32_t>(size);
    return block;
}

void* StaticRegion::tryMallocSlow(size_t size, size_t alignment)
{
    if (!t_isAllocating)
        return nullptr;
    if (!t_isAllocatingWhatIsMutable)
        return mallocImmutable(size, alignment);
    if (alignment < 16)
        alignment = 16;
    if (size_t index = freeListFor(size); alignment == 16 && index < numberOfFreeLists) {
        // The smallest that will do. What is left over of it is for somebody else.
        if (size_t found = firstFreeListInUse(mutableOnes, index); found < numberOfFreeLists) {
            auto* block = static_cast<char*>(popFree(mutableOnes, found));
            memcpy(block - sizeOfHeader, &size, sizeof(size));
            if (found > index) {
                char* rest = block + index * 16;
                size_t sizeOfRest = (found - index) * 16 - sizeOfHeader;
                memcpy(rest - sizeOfHeader, &sizeOfRest, sizeof(sizeOfRest));
                pushFree(mutableOnes, found - index, rest);
            }
            return block;
        }
    }
    noteForAudit((size + sizeOfHeader + 15) & ~static_cast<size_t>(15));
    auto* header = static_cast<char*>(allocate(Arena::MutableMalloc, size + sizeOfHeader, alignment, alignment - sizeOfHeader));
    memcpy(header, &size, sizeof(size));
    return header + sizeOfHeader;
}

size_t StaticRegion::mallocSize(const void* pointer)
{
    if (isInImmutableMalloc(pointer)) {
        // (Whoever wants to know that of what cannot change?)
        RELEASE_BASSERT(s_isBuilding);
        return sizeOfImmutable(pointer);
    }
    size_t size;
    memcpy(&size, static_cast<const char*>(pointer) - sizeOfHeader, sizeof(size));
    return size;
}

void* StaticRegion::reallocate(void* pointer, size_t newSize)
{
    // The last there is has room after it.
    if (s_isBuilding && t_isAllocating) {
        if (isInImmutableMalloc(pointer)) {
            size_t& used = s_used[static_cast<size_t>(Arena::Malloc)];
            size_t units = unitsFor(mallocSize(pointer));
            size_t newUnits = unitsFor(newSize);
            // (Where it is has to do for what it is going to be, too.)
            bool isWhereItMayBe = newSize % 16 || !(reinterpret_cast<uintptr_t>(pointer) & 15);
            if (reinterpret_cast<uintptr_t>(pointer) + units * unit == startOf(Arena::Malloc) + used && newUnits >= units && isWhereItMayBe && newSize <= UINT32_MAX && (newUnits - units) * unit <= arenaReservation - used) {
                used += (newUnits - units) * unit;
                sizeOfImmutable(pointer) = static_cast<uint32_t>(newSize);
                return pointer;
            }
        } else {
            uintptr_t end = reinterpret_cast<uintptr_t>(pointer) + mallocSize(pointer);
            size_t& used = s_used[static_cast<size_t>(Arena::MutableMalloc)];
            if (end == startOf(Arena::MutableMalloc) + used && newSize >= mallocSize(pointer) && newSize - mallocSize(pointer) <= arenaReservation - used) {
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
    if (address - startOf(Arena::Malloc) < used(Arena::Malloc)) {
        size_t units = unitsFor(mallocSize(pointer));
        s_auditFreedBytes += units * unit;
        s_auditFreedCount++;
        memset(pointer, 0, units * unit);
        sizeOfImmutable(pointer) = 0;
        pushFreeImmutable(pointer, units);
        return;
    }
    if (address - startOf(Arena::MutableMalloc) < used(Arena::MutableMalloc)) {
        s_auditFreedBytes += (mallocSize(pointer) + sizeOfHeader + 15) & ~static_cast<size_t>(15);
        s_auditFreedCount++;
        memset(pointer, 0, mallocSize(pointer));
        if (size_t index = freeListFor(mallocSize(pointer)); index < numberOfFreeLists && !(address & 15))
            pushFree(mutableOnes, index, pointer);
    }
}

} // namespace bmalloc
