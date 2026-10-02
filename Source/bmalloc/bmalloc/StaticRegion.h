/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BExport.h"
#include "BInline.h"
#include "BPlatform.h"
#include <cstddef>
#include <cstdint>

namespace bmalloc {

// Memory that is at the same address in every process. Data that a program needs regardless of how it is run is created once, when
// the program is built, by the same code that would otherwise create it at every start. The memory is written to the program's
// file, and mapped from there when the program runs. Pointers within the region stay valid, and a page that is never written to
// stays file-backed and shared, and does not count as the process's private memory.
//
// Nothing in it is ever freed. It may be written to: the mapping is private.
//
// Without BENABLE(STATIC_REGION) there is no such memory: nothing is in it, and nothing can be built or mapped.
class StaticRegion {
public:
    enum class Arena : uint8_t {
        Data, // Raw bytes placed by the code that builds the region.
        Malloc, // What malloc returns while the region is being built.
        Cells, // For the garbage collector's clients. See JSC::StaticHeap.
        // Like the previous two, but for objects that a running program is likely to write to. Keeping them apart avoids dirtying
        // the other arenas' pages.
        MutableCells,
        MutableMalloc,
        // Not in the file. Zero-filled at process start, in every process, whether or not the program uses the other arenas. It is
        // for objects that have to be created at run time but are referred to by objects created at build time.
        Bss,
        // The program's machine code and what goes with it (JSC::AOT::Image), from the file as well, but not built here.
        Image,
        // Exists only while the region is being built, and is not written to any file. It is for temporary objects.
        Scratch,
    };
    static constexpr unsigned numberOfArenas = 8;
    static constexpr unsigned numberOfArenasInFile = 5;

#if BOS(DARWIN)
    // Beyond where mimalloc asks for memory, and beyond ASAN's shadow memory, which goes into the first gap that is large enough.
    static constexpr uintptr_t base = 0x200000000000ULL;
#else
    // 128 GB: within a 39-bit address space, which is the least that a kernel for ARM64 gives a process, below where mimalloc asks
    // for memory (2 TB and up), and far from where the kernel puts executables, heaps, stacks and mappings.
    static constexpr uintptr_t base = 0x2000000000ULL;
#endif
    static constexpr size_t arenaReservation = 4ULL << 30;
    static constexpr size_t reservation = arenaReservation * numberOfArenas;

#if BENABLE(STATIC_REGION)
    static BINLINE bool contains(const void* pointer) { return reinterpret_cast<uintptr_t>(pointer) - base < reservation; }
#else
    static BINLINE bool contains(const void*) { return false; }
#endif
    static constexpr uintptr_t startOf(Arena arena) { return base + static_cast<size_t>(arena) * arenaReservation; }

    // ---- When the program is built. One thread does it.

    BEXPORT static bool beginBuilding(); // False if the addresses are taken.
    BEXPORT static void endBuilding();
    // The result is `misalignment` past a multiple of `alignment`. Zeroed.
    BEXPORT static void* allocate(Arena, size_t, size_t alignment, size_t misalignment = 0);
    BEXPORT static size_t used(Arena);

    // While there is one, what this thread mallocs is in the region.
    class AllocationScope {
    public:
        BEXPORT explicit AllocationScope(bool inRegion = true); // (False: not, for a while, inside of one.)
        BEXPORT ~AllocationScope();
        AllocationScope(const AllocationScope&) = delete;

    private:
        bool m_previous;
    };
    BEXPORT static bool isAllocatingOnThisThread();
    // While there is one as well, it is in Arena::MutableMalloc.
    class MutableScope {
    public:
        BEXPORT MutableScope();
        BEXPORT ~MutableScope();
        MutableScope(const MutableScope&) = delete;

    private:
        bool m_previous;
    };
    BEXPORT static bool isAllocatingMutable();

    // ---- When it runs.

    // What every process has, whether or not it uses the other arenas: the part of Arena::Bss before sizeOfBssOfEveryProcess.
    BEXPORT static void mapBss(); // Before anything else here. The process does not start if the addresses are taken.
    // The rest of Arena::Bss, for a process that builds the other arenas or runs with them. False if the addresses are taken.
    BEXPORT static bool mapRestOfBss();
#if BENABLE(STATIC_REGION)
    static BINLINE uintptr_t addressInBss(size_t offset) { return startOf(Arena::Bss) + offset; }
#else
    // Nothing in a file refers to it, so it is ordinary data.
    static BINLINE uintptr_t addressInBss(size_t offset) { return reinterpret_cast<uintptr_t>(s_bss) + offset; }
#endif
    // The layout of Arena::Bss.
    static constexpr size_t offsetOfEmptyStringInBss = 0; // WTF::StringImpl::empty()
    static constexpr size_t offsetOfSymbolsInBss = 64; // JSC::Symbols
    static constexpr size_t offsetOfEmbedderSymbolsInBss = 128 * 1024; // JSC::StaticHeap::embedderSymbols()
    // The rest belongs to JSC::StaticHeap: mapRestOfBss(). Only pages that are touched are committed.
    static constexpr size_t sizeOfBssOfEveryProcess = 256 * 1024;
    static constexpr size_t offsetOfDecodersInBss = 16 << 20; // One per module, like the next few.
    static constexpr size_t offsetOfSourceProvidersInBss = 64 << 20;
    static constexpr size_t offsetOfTopLevelExecutablesInBss = 128 << 20;
    static constexpr size_t offsetOfBlocksInBss = 256 << 20; // JSC::StaticHeap::allocateBlock()

    enum class Access : uint8_t { Read, ReadAndWrite };
    // False if the addresses are taken. `offsetInArena` and the rest are multiples of the size of a page.
    BEXPORT static bool map(Arena, Access, int fileDescriptor, int64_t offsetInFile, size_t, size_t offsetInArena = 0);

    // ---- For malloc.

#if BENABLE(STATIC_REGION)
    static BINLINE void* tryMalloc(size_t size, size_t alignment = 16)
    {
        if (s_isBuilding) [[unlikely]]
            return tryMallocSlow(size, alignment);
        return nullptr;
    }
#else
    static BINLINE void* tryMalloc(size_t, size_t = 16) { return nullptr; }
#endif
    BEXPORT static size_t mallocSize(const void*);
    BEXPORT static void* reallocate(void*, size_t); // For a pointer that tryMalloc() returned.
    // Likewise. At run time the memory is not reused. While building, freed memory is zeroed, so that the output is deterministic.
#if BENABLE(STATIC_REGION)
    static BINLINE void didFree(void* pointer)
    {
        if (s_isBuilding) [[unlikely]]
            didFreeSlow(pointer);
    }
#else
    static BINLINE void didFree(void*) { }
#endif

private:
    BEXPORT static void didFreeSlow(void*);
    BEXPORT static void* tryMallocSlow(size_t, size_t alignment);
public:
    BEXPORT static void clearFreeLists(); // Call this once the region is complete.
    BEXPORT static size_t bytesThatAreFree(); // Before that: memory that was freed but still takes up space in the file.
private:

    BEXPORT static bool s_isBuilding;
#if !BENABLE(STATIC_REGION)
    alignas(16) BEXPORT static char s_bss[sizeOfBssOfEveryProcess];
#endif
};

} // namespace bmalloc
