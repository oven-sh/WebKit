/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CodeSpecializationKind.h"
#include "LineColumn.h"
#include <bmalloc/StaticRegion.h>
#include <span>
#include <wtf/Forward.h>
#include <wtf/Function.h>
#include <wtf/Vector.h>

namespace WTF {
class SymbolRegistry;
}

namespace JSC {

class CachedBytecode;
class Decoder;
class DecoderStringTable;
class JSCell;
class JSString;
class PreciseAllocation;
class FunctionExecutable;
class Identifier;
namespace AOT {
struct FunctionInfo;
struct ImageFunction;
struct ImageKey;
}
class ScriptExecutable;
class SourceCodeKey;
class SourceOrigin;
class SourceProvider;
struct ReportableSitesOfFunction;
class TDZEnvironmentLink;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

// GC cells that live in bmalloc::StaticRegion. They are created when a program is built, are present from startup, and are never
// freed.
//
// To the GC they look like precise allocations (recognized by address) that all share one PreciseAllocation header, which reports
// them as marked. The GC therefore never visits or writes to them. A static cell only refers to other static cells unless it has
// been stored to since startup; the write barrier records those (Heap::addToRememberedSet()).
//
// Static cells are built for the first VM of the process: their headers carry StructureIDs of that VM's Structures. Since nothing
// writes to them, other VMs may also refer to them for as long as the first VM is alive. The exception is the mutable arenas, which
// only the first VM may use (isOnlyForFirstVM()).
class StaticHeap {
public:
    static ALWAYS_INLINE bool contains(const void* pointer) { return bmalloc::StaticRegion::contains(pointer); }

    // ---- Build time.

    // Serializes everything that can be created ahead of time. `strings` is the result of EncoderStringTable::serialize(); the
    // payload and its module entry offsets come from BytecodeLinkEncoder::finish(). Returns an empty vector if the static region is
    // unavailable.
    //
    // PositionsToKeep replaces per-instruction expression info. For each bytecode offset a frame can report, it stores the position
    // in the original sources. `find` maps a module (by its entry offset in the payload) and a position in the bundled text to a
    // source name and position. If it returns false, the position in the bundled text is kept. With PositionsToKeep, none of the
    // payload is retained.
    struct PositionsToKeep {
        const Vector<ReportableSitesOfFunction>& sites; // Indexed by function index in the code image.
        Function<bool(uint32_t entryOffsetOfModule, LineColumn inModule, CString& nameOfSource, LineColumn& inSource)> find;
    };
    // If keptPayloadStart is nonzero, the payload before it is omitted. It must be the start of
    // BytecodeLinkRegions::ExpressionInfo. The program can then no longer be interpreted or decoded again.
    JS_EXPORT_PRIVATE static Vector<uint8_t> build(VM&, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules, std::span<const uint8_t> imageOfCode = { }, size_t keptPayloadStart = 0, const PositionsToKeep* = nullptr, std::span<const ReportableSitesOfFunction> reportableSites = { }, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules = { });
    static bool isBuilding() { return s_isBuilding; }
    static JSString* emptyStringWhileBuilding(VM&); // Distinct from the VM's own empty string.
    static WTF::SymbolRegistry& symbolRegistryWhileBuilding(bool isPrivate); // Distinct from the VM's own registries.
    // The executable is being decoded from `record`, which has parent scope TDZ variables.
    static void noteParentScopeTDZVariables(const UnlinkedFunctionExecutable&, const void* record);

    // ---- Run time.

    // `image` is the result of build(), located at the page-aligned `offsetInFile`. Call on the thread that will own the VM, before
    // that thread creates any atom string. Returns false if the image was built by a different engine build or the address range is
    // unavailable.
    JS_EXPORT_PRIVATE static bool map(std::span<const uint8_t> image, int fileDescriptor, int64_t offsetInFile);
    // The image contains its own copies of the string table and the bytecode payload, which its contents refer to. Reports where
    // they are, so that a container file does not need to store them twice.
    struct Copies {
        size_t offsetOfStrings;
        size_t sizeOfStrings;
        size_t offsetOfPayload;
        size_t sizeOfPayload;
        bool payloadIsOmitted; // Only part of the payload was kept, so it is not stored at all.
        uintptr_t addressOfPayload; // omittedPayload().data() at run time.
        bool hasPositionsOfCallSites; // See PositionsToKeep.
    };
    JS_EXPORT_PRIVATE static std::optional<Copies> copiesIn(std::span<const uint8_t> image);
    // Call on a thread that will own a VM other than the first, before it creates any atom string. Does for that thread what map()
    // does for its own.
    JS_EXPORT_PRIVATE static void prepareThread();
    // Attaches the static cells to the VM. For the first VM, call on the thread that called map(). For any other VM, call on a
    // thread that called prepareThread(), which must be the only thread that runs the VM.
    JS_EXPORT_PRIVATE static void install(VM&);
    static void willDestroy(VM&);
    static bool isUsedBy(VM&);
    static bool isFirst(VM& vm) { return s_vm == &vm; }
    static ALWAYS_INLINE bool isOnlyForFirstVM(const void* pointer)
    {
        return std::bit_cast<uintptr_t>(pointer) - bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::MutableCells) < 2 * bmalloc::StaticRegion::arenaReservation;
    }
    // Once more than one VM uses the heap, the PreciseAllocation for a static cell is that of the VM that placed it, or else that
    // of the calling thread's VM.
    static ALWAYS_INLINE bool isShared() { return s_isShared; }
    JS_EXPORT_PRIVATE static PreciseAllocation* containerOfSlow(const void* cell);
    // For locks that protect the mutator's data from compiler threads. Taking such a lock would write to a static cell, and with no
    // JIT there are no compiler threads.
    static ALWAYS_INLINE bool needsNoLocking(const void* lock) { return contains(lock) && s_hasNoCompilerThreads; }
    // Returns a table in which all strings already exist, if `strings` is what build() was given and the VM uses this heap.
    JS_EXPORT_PRIVATE static std::unique_ptr<DecoderStringTable> tryCreateStringTable(VM&, std::span<const uint8_t> strings);
    // Returns what decoding would produce, if the bytecode is a module of the payload given to build() and matches the key.
    static UnlinkedCodeBlock* codeFor(VM&, const SourceCodeKey&, const CachedBytecode&);
    // True if the provider was created at the address returned by takePlaceForSourceProvider().
    static bool isProviderOfModule(const SourceProvider& provider) { return bmalloc::StaticRegion::contains(&provider); }
    JS_EXPORT_PRIVATE static bool payloadIsOmitted();
    // See PositionsToKeep and AOT::FunctionRef::reportedPositionFor().
    static bool hasPositionsOfCallSites();
    static bool hasIdentifiersOfProgram();
    static WTF::UniquedStringImpl* const* identifiersOfProgram(); // Indexed by identifier number. Null if there are none.
    JS_EXPORT_PRIVATE static String nameOfSource(uint32_t); // Source numbers start at one.
    // True if `bytes` claims to be a payload that cannot be read: the omitted payload, or one inside a static heap that was never
    // mapped.
    static bool isNoPayloadToRead(std::span<const uint8_t> bytes) { return contains(bytes.data()) && (!isMapped() || payloadIsOmitted()); }
    // The address range the omitted payload would occupy. It identifies the payload; most of it is not readable. See build().
    JS_EXPORT_PRIVATE static std::span<const uint8_t> omittedPayload();
    // Returns what linking the result of decodeBuiltinFunction() would produce, for a builtin with this payload entry offset and
    // source text. Its source() equals what makeSource() would have returned. Only valid in the realm that runs the program.
    JS_EXPORT_PRIVATE static FunctionExecutable* builtinFunctionFor(JSGlobalObject*, uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin&, const String& sourceURL);
    // The same for one of JSC's own builtins (BuiltinExecutables::stampOf()). May be called while the realm is being initialized.
    static FunctionExecutable* engineBuiltinFor(JSGlobalObject*, unsigned index, std::span<const Latin1Character> text);
    JS_EXPORT_PRIVATE static RefPtr<TDZEnvironmentLink> parentScopeTDZVariablesOf(const UnlinkedFunctionExecutable&);

    // A cell's header identifies its Structure by StructureID, which is an offset into the structure heap. The first VM of a
    // process creates its initial Structures in a fixed order, in a block at this offset.
    static constexpr uint32_t offsetOfFirstStructureBlock = 16 * 1024;

    // Per-function data for FunctionExecutables in the short form. Indexed by AOT::CodeHeader::index.
    struct RowOfFunction {
        static constexpr unsigned bitsOfModule = 17;
        static constexpr unsigned bitsOfParameterCount = 12;
        const Identifier& name() const LIFETIME_BOUND { return *reinterpret_cast<const Identifier*>(&nameImpl); }

        WTF::UniquedStringImpl* nameImpl; // UnlinkedFunctionExecutable::ecmaName()
        uint32_t unlinkedFunction; // Offset into Arena::Cells. See unlinkedFunctionOf().
        uint32_t module : bitsOfModule; // See sourceProviderOfModule().
        uint32_t parameterCount : bitsOfParameterCount;
        uint32_t isArrowFunctionContext : 1;
        uint32_t isInsideOrdinaryFunction : 1;
    };
    static_assert(sizeof(RowOfFunction) == 16);
    static const RowOfFunction& rowOf(uint32_t indexOfFunction) { return s_rowsOfFunctions[indexOfFunction]; }
    static UnlinkedFunctionExecutable* unlinkedFunctionOf(const RowOfFunction& row) { return reinterpret_cast<UnlinkedFunctionExecutable*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Cells) + row.unlinkedFunction); }
    static SourceProvider* sourceProviderOfModule(size_t index) { return reinterpret_cast<SourceProvider*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss) + bmalloc::StaticRegion::offsetOfSourceProvidersInBss + index * sizeOfPlaceForSourceProvider); }
    JS_EXPORT_PRIVATE static LineColumn whereFunctionStarts(uint32_t indexOfFunction);
    // The next cell allocated will be an UnlinkedFunctionExecutable. See retainNeededFunctionData().
    static void willAllocateUnlinkedFunction()
    {
        if (s_isBuilding) [[unlikely]]
            willAllocateUnlinkedFunctionSlow();
    }

    // Static cells are placed 8 bytes past a 16-byte boundary.
    static constexpr size_t sizeOfCellHeader = 8;

    // Build time only. The program will run without bytecode, so no function's bytecode will ever be generated.
    JS_EXPORT_PRIVATE static bool keepsNothingForGeneratingCode();

    // Returns memory for a cell while the region is being built, on a thread with a bmalloc::StaticRegion::AllocationScope; or the
    // address passed to placeNextCell(). Null otherwise.
    JS_EXPORT_PRIVATE static void* tryAllocateCellSlow(VM&, size_t); // Called when Heap::m_placeOfNextCell is set.

    // ---- Objects created at run time, at fixed addresses so that build-time objects can refer to them.

    static VM* addressOfVM() { return reinterpret_cast<VM*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss) + bmalloc::StaticRegion::offsetOfVMInBss); }
    static void* addressOfGlobalObject() { return reinterpret_cast<void*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss) + bmalloc::StaticRegion::offsetOfGlobalObjectInBss + sizeOfCellHeader); }
    // Space for embedder symbols that are identical in every VM, like JSC::Symbols. They are created here in a fixed order so that
    // build-time objects can refer to them.
    static void* addressOfEmbedderSymbols() { return reinterpret_cast<void*>(bmalloc::StaticRegion::addressInBss(bmalloc::StaticRegion::offsetOfEmbedderSymbolsInBss)); }
    static constexpr size_t sizeForEmbedderSymbols = bmalloc::StaticRegion::offsetOfVTablesInBss - bmalloc::StaticRegion::offsetOfEmbedderSymbolsInBss;
    static bool isMapped() { return !!s_header; }
    // The next cell allocated in the VM is placed at `address`. It is never collected or destroyed. After didPlaceCell(), every GC
    // visits it.
    JS_EXPORT_PRIVATE static void placeNextCell(VM&, void* address);
    JS_EXPORT_PRIVATE static void didPlaceCell(VM&, JSCell*);
    // ---- FunctionExecutables created at build time. Each points at its module's SourceProvider, which is created at run time and
    // must therefore be created at that address.

    static constexpr size_t sizeOfPlaceForSourceProvider = 256;
    // Returns the address reserved for the provider of the module with this payload entry offset. Returns null if there is none, if
    // it has already been taken, or if the VM does not use this heap. If another VM has already created the provider, it is
    // returned in `made`; it is shared by all VMs and must be SourceProvider::becomeShareableBetweenThreads(). The provider is
    // never destroyed. Call didMakeSourceProvider() once it has been constructed.
    JS_EXPORT_PRIVATE static void* takePlaceForSourceProvider(VM&, size_t entryOffsetOfModule, size_t sizeOfProvider, SourceProvider*& made);
    JS_EXPORT_PRIVATE static void didMakeSourceProvider(void* place);
    // The function with this index in the code image (AOT::CodeHeader::index), and which of its two code kinds the index refers to.
    static std::pair<FunctionExecutable*, CodeSpecializationKind> executableOfFunction(uint32_t index);
    static bool hasExecutablesOfFunctions(VM&);
    // FunctionInfo for each function in the image, by index, as far as it was known at build time. Null if none was.
    static AOT::FunctionInfo* infosOfFunctions(VM&);
    static const void* constantsOfProgram(VM&); // AOT::Instance::constantsOfProgram
    // The image's key table, if the image omits it: the keys of functions whose executables are created at run time.
    static std::span<const AOT::ImageKey> keysOfImage();
    static const AOT::ImageFunction* imageFunctionOfFunction(uint32_t index);
    // AOT::Instance::functionMetadataOffsets. The offsets in AOT::FunctionMetadata are relative to an arena (inData(), inMalloc()).
    static const uint32_t* functionMetadataOffsets(VM&);
    template<typename T> static const T* inData(uint32_t offset) { return reinterpret_cast<const T*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Data) + offset); }
    template<typename T> static const T* inMalloc(uint32_t offset) { return reinterpret_cast<const T*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Malloc) + offset); }
    // For decoding a function's code from the kept payload. `placed` is the decoder passed to
    // UnlinkedFunctionExecutable::leaveCodeInPayload().
    static Ref<Decoder> decoderForKeptPayload(VM&, Decoder& placed);
    static void ensureDecoder(VM&, size_t indexOfModule, SourceProvider&);
    // An ordinary executable owned by this VM for the same function as a static executable. Used where the static executable's code
    // cannot run and it has no room to hold other code.
    static FunctionExecutable* standInFor(VM&, FunctionExecutable*);
    // Code that this VM generated for a static executable. See UnlinkedFunctionExecutable::unlinkedCodeBlockFor().
    static UnlinkedFunctionCodeBlock* codeOf(VM&, const UnlinkedFunctionExecutable&, CodeSpecializationKind);
    static void setCodeOf(VM&, const UnlinkedFunctionExecutable&, CodeSpecializationKind, UnlinkedFunctionCodeBlock*);
    static bool isPlaceOfSourceProvider(const void* pointer)
    {
        uintptr_t start = bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss);
        uintptr_t address = std::bit_cast<uintptr_t>(pointer);
        return address >= start + bmalloc::StaticRegion::offsetOfSourceProvidersInBss && address < start + bmalloc::StaticRegion::offsetOfTopLevelExecutablesInBss;
    }
    // The executable for the module's top-level code, in the realm that runs the program.
    static ScriptExecutable*& topLevelExecutableOfModuleWithProvider(VM& vm, const void* provider)
    {
        uintptr_t start = bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss);
        size_t index = (std::bit_cast<uintptr_t>(provider) - start - bmalloc::StaticRegion::offsetOfSourceProvidersInBss) / sizeOfPlaceForSourceProvider;
        if (!isFirst(vm)) [[unlikely]]
            return topLevelExecutableOfModuleInOtherVM(vm, index);
        return reinterpret_cast<ScriptExecutable**>(start + bmalloc::StaticRegion::offsetOfTopLevelExecutablesInBss)[index];
    }
    JS_EXPORT_PRIVATE static ScriptExecutable*& topLevelExecutableOfModuleInOtherVM(VM&, size_t index);

    // All cells placed outside the GC's own memory belong to a single VM. Returns whether that is this VM. Without a static heap,
    // the first VM to ask claims the role.
    JS_EXPORT_PRIVATE static bool canPlaceCellsOf(VM&);
    // Page-aligned, zeroed memory for placing cells. Any thread.
    JS_EXPORT_PRIVATE static void* allocateBlock(VM&, size_t);
    JS_EXPORT_PRIVATE static void freeBlock(void*, size_t);

private:
    struct Header;
    static void makeContainer(VM&);
    static inline void* const placeOfEveryCellWhileBuilding = reinterpret_cast<void*>(1); // Value of Heap::m_placeOfNextCell meaning "allocate anywhere in the region".

    JS_EXPORT_PRIVATE static bool s_isBuilding;
    JS_EXPORT_PRIVATE static void willAllocateUnlinkedFunctionSlow();
    static void retainNeededFunctionData(VM&, Header&);
    JS_EXPORT_PRIVATE static const RowOfFunction* s_rowsOfFunctions;
    JS_EXPORT_PRIVATE static VM* s_vm;
    JS_EXPORT_PRIVATE static bool s_isShared;
    JS_EXPORT_PRIVATE static bool s_hasNoCompilerThreads;
    static const Header* s_header; // Header of the mapped image.
};

} // namespace JSC
