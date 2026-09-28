/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <bmalloc/StaticRegion.h>
#include <span>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace WTF {
class SymbolRegistry;
}

namespace JSC {

class CachedBytecode;
class DecoderStringTable;
class JSCell;
class JSString;
class PreciseAllocation;
class SourceCodeKey;
class TDZEnvironmentLink;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

// Cells in bmalloc::StaticRegion: made when a program is built, there from the start when it runs, and there for good.
//
// To the collector they look like precise allocations, by their addresses, that share one PreciseAllocation, which says of all of
// them that they are marked. So it never visits them, and never writes to them. What one of them refers to is another of them,
// unless it has been stored to since the program started: the write barrier says which those are (Heap::addToRememberedSet()).
//
// They belong to one VM of the process.
class StaticHeap {
public:
    static ALWAYS_INLINE bool contains(const void* pointer) { return bmalloc::StaticRegion::contains(pointer); }

    // ---- When a program is built.

    // Everything that can be made ahead of time, as a file. `strings` is what EncoderStringTable::serialize() returned; the payload
    // and the entries of its modules are BytecodeLinkEncoder::finish()'s. Empty if it cannot be done here.
    JS_EXPORT_PRIVATE static Vector<uint8_t> build(VM&, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules);
    static bool isBuilding() { return s_isBuilding; }
    static JSString* emptyStringWhileBuilding(VM&); // Not the VM's own.
    static WTF::SymbolRegistry& symbolRegistryWhileBuilding(bool isPrivate); // Likewise.
    // The executable is being decoded from that record, which has some.
    static void noteParentScopeTDZVariables(const UnlinkedFunctionExecutable&, const void* record);

    // ---- When it runs.

    // `image` is what build() returned, and is at `offsetInFile`, a multiple of the size of a page, of the file. On the thread that
    // is going to have the VM, before that thread has made an atom. False if it is not for this engine, or there is no room.
    JS_EXPORT_PRIVATE static bool map(std::span<const uint8_t> image, int fileDescriptor, off_t offsetInFile);
    // From now on the static cells are this VM's: the first of the process, on the thread that called map().
    JS_EXPORT_PRIVATE static void install(VM&);
    static VM* vm() { return s_vm; }
    // Of a lock that is for keeping the compiler's threads from what the mutator is changing: taking it would be writing to it, and
    // there are no such threads.
    static ALWAYS_INLINE bool needsNoLocking(const void* lock) { return contains(lock) && s_hasNoCompilerThreads; }
    // The table for those strings, all of them made already, if they are the ones build() was given and the VM is the one.
    JS_EXPORT_PRIVATE static std::unique_ptr<DecoderStringTable> tryCreateStringTable(VM&, std::span<const uint8_t> strings);
    // What decoding that would give, if it is a module of the payload that build() was given, and is the code for that key.
    static UnlinkedCodeBlock* codeFor(VM&, const SourceCodeKey&, const CachedBytecode&);
    JS_EXPORT_PRIVATE static RefPtr<TDZEnvironmentLink> parentScopeTDZVariablesOf(const UnlinkedFunctionExecutable&);

    // A cell says what it is by the place of its Structure among all Structures. The first VM of a process makes the ones it starts
    // with in an order that does not change, in a block that is where this says.
    static constexpr uint32_t offsetOfFirstStructureBlock = 16 * 1024;

    // A cell is preceded by its size, and is halfway between two multiples of 16.
    static constexpr size_t sizeOfCellHeader = 8;
    static size_t cellSize(const void* cell) { return *reinterpret_cast<const size_t*>(static_cast<const char*>(cell) - sizeOfCellHeader); }
    template<typename Functor> static void forEachCell(bmalloc::StaticRegion::Arena arena, size_t bytesUsed, const Functor& functor)
    {
        uintptr_t start = bmalloc::StaticRegion::startOf(arena);
        for (size_t offset = 0; offset < bytesUsed;) {
            void* cell = reinterpret_cast<void*>(start + offset + sizeOfCellHeader);
            functor(cell, cellSize(cell));
            offset += (sizeOfCellHeader + cellSize(cell) + 15) & ~static_cast<size_t>(15);
        }
    }

    // While the region is being built, on a thread that has a bmalloc::StaticRegion::AllocationScope. Null otherwise.
    static ALWAYS_INLINE void* tryAllocateCell(size_t size)
    {
        if (s_isBuilding) [[unlikely]]
            return tryAllocateCellSlow(size);
        return nullptr;
    }
    static void setIsBuilding(bool isBuilding) { s_isBuilding = isBuilding; }

private:
    JS_EXPORT_PRIVATE static void* tryAllocateCellSlow(size_t);

    struct Header;
    static void makeContainer(VM&);

    JS_EXPORT_PRIVATE static bool s_isBuilding;
    JS_EXPORT_PRIVATE static VM* s_vm;
    JS_EXPORT_PRIVATE static bool s_hasNoCompilerThreads;
    static const Header* s_header; // Of what is mapped.
};

} // namespace JSC
