/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StaticRegion.h"

#include "BAssert.h"
#include "BPlatform.h"

#if BENABLE(STATIC_REGION)

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <utility>
#include <sys/mman.h>

#if BOS(DARWIN)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#if BUSE(MIMALLOC)
#include "mimalloc.h"
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace bmalloc {

bool StaticRegion::s_isBuilding = false;
static size_t s_used[StaticRegion::numberOfArenas];
static thread_local bool t_isAllocating = false;
static thread_local bool t_isAllocatingMutable = false;
static uint32_t* s_sizes; // See sizeOfImmutable().
static size_t s_capacityOfSizes;

bool StaticRegion::beginBuilding()
{
    RELEASE_BASSERT(!s_isBuilding);
    if (!mapRestOfBss())
        return false;
    constexpr size_t size = numberOfArenasInFile * arenaReservation;
    void* result = mmap(reinterpret_cast<void*>(base), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result == MAP_FAILED)
        return false;
    if (result != reinterpret_cast<void*>(base)) {
        munmap(result, size);
        return false;
    }
    void* scratch = mmap(reinterpret_cast<void*>(startOf(Arena::Scratch)), arenaReservation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (scratch != reinterpret_cast<void*>(startOf(Arena::Scratch))) {
        if (scratch != MAP_FAILED)
            munmap(scratch, arenaReservation);
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
    munmap(reinterpret_cast<void*>(startOf(Arena::Scratch)), arenaReservation);
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
    : m_previous(t_isAllocatingMutable)
{
    t_isAllocatingMutable = true;
}

StaticRegion::MutableScope::~MutableScope()
{
    t_isAllocatingMutable = m_previous;
}

bool StaticRegion::isAllocatingMutable()
{
    return t_isAllocatingMutable;
}

void StaticRegion::mapBss()
{
    static bool isMapped = false;
    if (isMapped)
        return;
    isMapped = true;
    void* wanted = reinterpret_cast<void*>(startOf(Arena::Bss));
    void* result = mmap(wanted, sizeOfBssOfEveryProcess, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (result != wanted) {
        fprintf(stderr, "fatal: cannot map memory at %p: %s\n", wanted, result == MAP_FAILED ? strerror(errno) : "the address is taken");
        BCRASH();
    }
}

bool StaticRegion::mapRestOfBss()
{
    static std::once_flag once;
    static bool isMapped = false;
    std::call_once(once, [] {
        void* wanted = reinterpret_cast<void*>(startOf(Arena::Bss) + sizeOfBssOfEveryProcess);
        constexpr size_t size = arenaReservation - sizeOfBssOfEveryProcess;
        void* result = mmap(wanted, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        isMapped = result == wanted;
        if (!isMapped && result != MAP_FAILED)
            munmap(result, size);
    });
    return isMapped;
}

bool StaticRegion::map(Arena arena, Access access, int fileDescriptor, int64_t offsetInFile, size_t size, size_t offsetInArena)
{
    RELEASE_BASSERT(offsetInArena <= arenaReservation && size <= arenaReservation - offsetInArena);
    if (!size)
        return true;
    void* wanted = reinterpret_cast<void*>(startOf(arena) + offsetInArena);
    int protection = access == Access::ReadAndWrite ? PROT_READ | PROT_WRITE : access == Access::ReadAndExecute ? PROT_READ | PROT_EXEC : PROT_READ;
    void* result = mmap(wanted, size, protection, MAP_PRIVATE, fileDescriptor, offsetInFile);
    // Darwin refuses an executable mapping of a file that is not signed (EPERM), but allows a mapping of one to be made executable.
    bool needsExecutePermission = false;
    if (result == MAP_FAILED && access == Access::ReadAndExecute && errno == EPERM) {
        result = mmap(wanted, size, PROT_READ, MAP_PRIVATE, fileDescriptor, offsetInFile);
        needsExecutePermission = true;
    }
    if (result == MAP_FAILED)
        return false;
    if (result != wanted || (needsExecutePermission && mprotect(result, size, protection))) {
        munmap(result, size);
        return false;
    }
#if BOS(DARWIN)
    // It cannot be made writable again either.
    if (access == Access::Read)
        mach_vm_protect(mach_task_self(), reinterpret_cast<mach_vm_address_t>(result), size, true, VM_PROT_READ);
#endif
    return true;
}

// In Arena::MutableMalloc, each allocation is preceded by its requested size, because it may be freed or reallocated at run time.
static constexpr size_t sizeOfHeader = 8;

// Allocations in Arena::Malloc are immutable once the region is built. Their sizes only matter until then, so they are kept in a
// side table, indexed by address, and are not written to the file. Allocations are packed as tightly as alignment allows. Every
// size is a multiple of this unit.
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

// Memory that is freed while the region is being built is reused for later allocations, because unused memory would still end up in
// the file. The free lists are indexed by size in units of 16 bytes, including the header.
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

// Frees that many units of Arena::Malloc, which must already be zeroed. (A block that is too large for the free lists is not
// reused.)
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

void StaticRegion::clearFreeLists()
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

static void* mallocImmutable(size_t size, size_t alignment)
{
    RELEASE_BASSERT(size <= UINT32_MAX);
    // An object whose size is not a multiple of 16 cannot require, or contain anything that requires, 16-byte alignment.
    if (alignment <= 16)
        alignment = size % 16 || !size ? unit : 16;
    size_t units = unitsFor(size);
    if (alignment <= 16 && units + 1 < numberOfFreeLists) {
        // Best fit. The remainder of the block goes back on a free list.
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
    if (!t_isAllocatingMutable)
        return mallocImmutable(size, alignment);
    if (alignment < 16)
        alignment = 16;
    if (size_t index = freeListFor(size); alignment == 16 && index < numberOfFreeLists) {
        // Best fit. The remainder of the block goes back on a free list.
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
    auto* header = static_cast<char*>(allocate(Arena::MutableMalloc, size + sizeOfHeader, alignment, alignment - sizeOfHeader));
    memcpy(header, &size, sizeof(size));
    return header + sizeOfHeader;
}

size_t StaticRegion::mallocSize(const void* pointer)
{
    if (isInImmutableMalloc(pointer)) {
        // (The size of an immutable allocation is only needed while building.)
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
            // (The current address also has to satisfy the alignment of the new size.)
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
        memset(pointer, 0, units * unit);
        sizeOfImmutable(pointer) = 0;
        pushFreeImmutable(pointer, units);
        return;
    }
    if (address - startOf(Arena::MutableMalloc) < used(Arena::MutableMalloc)) {
        memset(pointer, 0, mallocSize(pointer));
        if (size_t index = freeListFor(mallocSize(pointer)); index < numberOfFreeLists && !(address & 15))
            pushFree(mutableOnes, index, pointer);
    }
}

} // namespace bmalloc

#else // BENABLE(STATIC_REGION)

namespace bmalloc {

bool StaticRegion::s_isBuilding = false;
alignas(16) char StaticRegion::s_bss[sizeOfBssOfEveryProcess];

bool StaticRegion::beginBuilding() { return false; }
void StaticRegion::endBuilding() { }
void* StaticRegion::allocate(Arena, size_t, size_t, size_t) { RELEASE_BASSERT_NOT_REACHED(); return nullptr; }
size_t StaticRegion::used(Arena) { return 0; }
StaticRegion::AllocationScope::AllocationScope(bool) : m_previous(false) { }
StaticRegion::AllocationScope::~AllocationScope() { static_cast<void>(m_previous); }
bool StaticRegion::isAllocatingOnThisThread() { return false; }
StaticRegion::MutableScope::MutableScope() : m_previous(false) { }
StaticRegion::MutableScope::~MutableScope() { static_cast<void>(m_previous); }
bool StaticRegion::isAllocatingMutable() { return false; }
void StaticRegion::mapBss() { }
bool StaticRegion::mapRestOfBss() { return false; }
bool StaticRegion::map(Arena, Access, int, int64_t, size_t, size_t) { return false; }
size_t StaticRegion::mallocSize(const void*) { return 0; }
void* StaticRegion::reallocate(void*, size_t) { return nullptr; }
void StaticRegion::didFreeSlow(void*) { }
void* StaticRegion::tryMallocSlow(size_t, size_t) { return nullptr; }
void StaticRegion::clearFreeLists() { }
size_t StaticRegion::bytesThatAreFree() { return 0; }

} // namespace bmalloc

#endif // BENABLE(STATIC_REGION)
