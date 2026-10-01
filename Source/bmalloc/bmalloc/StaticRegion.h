/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "BExport.h"
#include "BInline.h"
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace bmalloc {

// Memory that is at the same address in every process. What a program is going to need that does not depend on how it is run is
// made once, when the program is built, by the code that would otherwise make it each time the program starts; the memory it was
// made in is written to the program's file, and mapped from there when the program runs. Pointers from one part of it to another
// are good as they are, and a page of it that nobody writes to is the file's, not the process's.
//
// Nothing in it is ever freed. It may be written to: the mapping is private.
class StaticRegion {
public:
    enum class Arena : uint8_t {
        Data, // Bytes that whoever builds the region puts there.
        Malloc, // What malloc returns while the region is being built.
        Cells, // For the garbage collector's clients. See JSC::StaticHeap.
        // Like the two before, for what running the program is likely to write to: kept from making the others' pages dirty.
        MutableCells,
        MutableMalloc,
        // Not in the file. Zero when the process starts, in every process, whether or not the program has anything in the others:
        // for what has to be made when the program runs, but is referred to by what is made when it is built.
        Bss,
        // The program's machine code and what goes with it (JSC::AOT::Image), from the file as well, but not built here.
        Image,
        // Only there while the region is built, and in no file: for what is made on the way to what is kept.
        Scratch,
    };
    static constexpr unsigned numberOfArenas = 8;
    static constexpr unsigned numberOfArenasInFile = 5;

    static constexpr uintptr_t base = 0x200000000000ULL; // Beyond where mimalloc asks for memory, and far from where the kernel puts things.
    static constexpr size_t arenaReservation = 4ULL << 30;
    static constexpr size_t reservation = arenaReservation * numberOfArenas;

    static BINLINE bool contains(const void* pointer) { return reinterpret_cast<uintptr_t>(pointer) - base < reservation; }
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

    BEXPORT static void mapBss(); // Before anything else here. Crashes if the addresses are taken.
    // What is where in Arena::Bss.
    static constexpr size_t offsetOfEmptyStringInBss = 0; // WTF::StringImpl::empty()
    static constexpr size_t offsetOfSymbolsInBss = 64; // JSC::Symbols
    static constexpr size_t offsetOfEmbedderSymbolsInBss = 128 * 1024; // JSC::StaticHeap::embedderSymbols()
    // The rest is JSC::StaticHeap's. Addresses cost nothing: only what is touched is there.
    static constexpr size_t offsetOfVTablesInBss = 256 * 1024;
    static constexpr size_t offsetOfVMInBss = 1 << 20;
    static constexpr size_t offsetOfGlobalObjectInBss = 2 << 20;
    static constexpr size_t offsetOfDecodersInBss = 16 << 20; // One for each module, as of the next few.
    static constexpr size_t offsetOfSourceProvidersInBss = 64 << 20;
    static constexpr size_t offsetOfTopLevelExecutablesInBss = 128 << 20;
    static constexpr size_t offsetOfBlocksInBss = 256 << 20; // JSC::StaticHeap::allocateBlock()

    // False if the addresses are taken. `offsetInArena` and the rest are multiples of the size of a page.
    BEXPORT static bool map(Arena, int fileDescriptor, off_t offsetInFile, size_t, size_t offsetInArena = 0, bool isCode = false);

    // ---- For malloc.

    static BINLINE void* tryMalloc(size_t size, size_t alignment = 16)
    {
        if (s_isBuilding) [[unlikely]]
            return tryMallocSlow(size, alignment);
        return nullptr;
    }
    BEXPORT static size_t mallocSize(const void*);
    BEXPORT static void* reallocate(void*, size_t); // Of what tryMalloc returned.
    // Likewise. Nothing is used again; while building, what is freed becomes zero, so that it says nothing.
    static BINLINE void didFree(void* pointer)
    {
        if (s_isBuilding) [[unlikely]]
            didFreeSlow(pointer);
    }

private:
    BEXPORT static void didFreeSlow(void*);
    BEXPORT static void* tryMallocSlow(size_t, size_t alignment);
public:
    BEXPORT static void clearFreeLists(); // When all is built.
    BEXPORT static size_t bytesThatAreFree(); // Before that: what was freed and is in the file all the same.
private:

    BEXPORT static bool s_isBuilding;
};

} // namespace bmalloc
