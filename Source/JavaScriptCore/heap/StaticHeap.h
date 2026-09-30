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

// Cells in bmalloc::StaticRegion: made when a program is built, there from the start when it runs, and there for good.
//
// To the collector they look like precise allocations, by their addresses, that share one PreciseAllocation, which says of all of
// them that they are marked. So it never visits them, and never writes to them. What one of them refers to is another of them,
// unless it has been stored to since the program started: the write barrier says which those are (Heap::addToRememberedSet()).
//
// They are made for the first VM of the process, and say so: what a cell says it is, it says with a Structure of that VM's. But
// nothing writes to them, so any other VM can refer to them as well (Options::useStaticHeapInEveryVM()), for as long as the first
// is there. The exception is what is in the arenas for what is written to, which is for the first VM alone (isOnlyForFirstVM()).
class StaticHeap {
public:
    static ALWAYS_INLINE bool contains(const void* pointer) { return bmalloc::StaticRegion::contains(pointer); }

    // ---- When a program is built.

    // Everything that can be made ahead of time, as a file. `strings` is what EncoderStringTable::serialize() returned; the payload
    // and the entries of its modules are BytecodeLinkEncoder::finish()'s. Empty if it cannot be done here.
    // Instead of what says where in the text of a module every instruction came from: for each place that a frame of a function can
    // say it is at, where that is in the sources that the program was made from, as far as `find` knows. It is given a module (where
    // its entry is in the payload) and a place in its text, and says which source and where in it; or false, and then it is the
    // place in the text of the module that is kept. Nothing at all is kept of the payload then.
    struct PositionsToKeep {
        const Vector<ReportableSitesOfFunction>& sites; // By the numbers that the functions have in the image of the code.
        Function<bool(uint32_t entryOffsetOfModule, LineColumn inModule, CString& nameOfSource, LineColumn& inSource)> find;
    };
    // What comes before `whatIsKeptOfPayloadStartsAt` in the payload is left out, if that is not zero: it had better be where
    // BytecodeLinkRegions::ExpressionInfo starts. Then nothing of the program can be interpreted, or decoded again.
    JS_EXPORT_PRIVATE static Vector<uint8_t> build(VM&, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules, std::span<const uint8_t> imageOfCode = { }, size_t whatIsKeptOfPayloadStartsAt = 0, const PositionsToKeep* = nullptr, std::span<const ReportableSitesOfFunction> whatTheCompilerSaysOfFunctions = { });
    static bool isBuilding() { return s_isBuilding; }
    static JSString* emptyStringWhileBuilding(VM&); // Not the VM's own.
    static WTF::SymbolRegistry& symbolRegistryWhileBuilding(bool isPrivate); // Likewise.
    // The executable is being decoded from that record, which has some.
    static void noteParentScopeTDZVariables(const UnlinkedFunctionExecutable&, const void* record);

    // ---- When it runs.

    // `image` is what build() returned, and is at `offsetInFile`, a multiple of the size of a page, of the file. On the thread that
    // is going to have the VM, before that thread has made an atom. False if it is not for this engine, or there is no room.
    JS_EXPORT_PRIVATE static bool map(std::span<const uint8_t> image, int fileDescriptor, off_t offsetInFile);
    // An image has copies of the table of strings and of the bytecode it was built from, which what is in it refers to. Where they
    // are in it, for whoever puts it in a file that would otherwise have them twice.
    struct Copies {
        size_t offsetOfStrings;
        size_t sizeOfStrings;
        size_t offsetOfPayload;
        size_t sizeOfPayload;
        bool payloadIsLeftOut; // Not all of it is there, so it is nowhere.
        uintptr_t addressOfPayload; // payloadThatIsLeftOut().data(), when the program runs.
        bool hasPositionsOfCallSites; // See PositionsToKeep.
    };
    JS_EXPORT_PRIVATE static std::optional<Copies> copiesIn(std::span<const uint8_t> image);
    // On a thread that is going to have a VM other than the first, before it has made an atom: as map() does for its own thread.
    JS_EXPORT_PRIVATE static void prepareThread();
    // From now on the VM has the static cells. The first of the process, on the thread that called map(); any other, on a thread
    // that called prepareThread(), which is the only thread that is going to run it.
    JS_EXPORT_PRIVATE static void install(VM&);
    static void willDestroy(VM&);
    static bool isUsedBy(VM&);
    static bool isFirst(VM& vm) { return s_vm == &vm; }
    static ALWAYS_INLINE bool isOnlyForFirstVM(const void* pointer)
    {
        return std::bit_cast<uintptr_t>(pointer) - bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::MutableCells) < 2 * bmalloc::StaticRegion::arenaReservation;
    }
    // What a static cell has for a PreciseAllocation, once there is more than one VM to be asked about: that of the VM that placed
    // it, or else of the VM of the thread that asks.
    static ALWAYS_INLINE bool isShared() { return s_isShared; }
    JS_EXPORT_PRIVATE static PreciseAllocation* containerOfSlow(const void* cell);
    // Of a lock that is for keeping the compiler's threads from what the mutator is changing: taking it would be writing to it, and
    // there are no such threads.
    static ALWAYS_INLINE bool needsNoLocking(const void* lock) { return contains(lock) && s_hasNoCompilerThreads; }
    // The table for those strings, all of them made already, if they are the ones build() was given and the VM is the one.
    JS_EXPORT_PRIVATE static std::unique_ptr<DecoderStringTable> tryCreateStringTable(VM&, std::span<const uint8_t> strings);
    // What decoding that would give, if it is a module of the payload that build() was given, and is the code for that key.
    static UnlinkedCodeBlock* codeFor(VM&, const SourceCodeKey&, const CachedBytecode&);
    // See build(). Then this is where the payload would be and how long it is, for whoever has to say which payload they mean:
    // most of it is not there to be read.
    // One that is where takePlaceForSourceProvider() said.
    static bool isProviderOfModule(const SourceProvider& provider) { return bmalloc::StaticRegion::contains(&provider); }
    JS_EXPORT_PRIVATE static bool payloadIsLeftOut();
    // See PositionsToKeep, and AOT::FunctionRef::reportedPositionFor().
    static bool hasPositionsOfCallSites();
    static bool hasIdentifiersOfProgram();
    static WTF::UniquedStringImpl* const* identifiersOfProgram(); // By number. Null if there are none.
    JS_EXPORT_PRIVATE static String nameOfSource(uint32_t); // From one.
    // Of what is said to be a payload: it is that one, or the static heap it would be in has not been mapped at all.
    static bool isNoPayloadToRead(std::span<const uint8_t> bytes) { return contains(bytes.data()) && (!isMapped() || payloadIsLeftOut()); }
    JS_EXPORT_PRIVATE static std::span<const uint8_t> payloadThatIsLeftOut();
    // Likewise what linking the result of decodeBuiltinFunction() would give, for a builtin whose entry in the payload is there,
    // and whose source is that. Its source() is what makeSource() would have returned. Only in the realm that the program is run in.
    JS_EXPORT_PRIVATE static FunctionExecutable* builtinFunctionFor(JSGlobalObject*, uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin&, const String& sourceURL);
    // Likewise for one of the engine's own (BuiltinExecutables::stampOf()). It may be asked for while the realm is being made.
    static FunctionExecutable* builtinOfEngineFor(JSGlobalObject*, unsigned index, std::span<const Latin1Character> text);
    JS_EXPORT_PRIVATE static RefPtr<TDZEnvironmentLink> parentScopeTDZVariablesOf(const UnlinkedFunctionExecutable&);

    // A cell says what it is by the place of its Structure among all Structures. The first VM of a process makes the ones it starts
    // with in an order that does not change, in a block that is where this says.
    static constexpr uint32_t offsetOfFirstStructureBlock = 16 * 1024;

    // What there is to say about a function whose FunctionExecutable is in the short form, which see. By AOT::CodeHeader::index.
    struct RowOfFunction {
        static constexpr unsigned bitsOfModule = 17;
        static constexpr unsigned bitsOfParameterCount = 12;
        const Identifier& name() const LIFETIME_BOUND { return *reinterpret_cast<const Identifier*>(&nameImpl); }

        WTF::UniquedStringImpl* nameImpl; // UnlinkedFunctionExecutable::ecmaName()
        uint32_t unlinkedFunction; // How far into Arena::Cells. See unlinkedFunctionOf().
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
    // The next cell is an UnlinkedFunctionExecutable. See keepWhatIsWantedOfFunctions().
    static void willAllocateUnlinkedFunction()
    {
        if (s_isBuilding) [[unlikely]]
            willAllocateUnlinkedFunctionSlow();
    }

    // A cell is halfway between two multiples of 16: this far past one.
    static constexpr size_t sizeOfCellHeader = 8;

    // While it is being built: the program is going to run without its bytecode, and no function's code is ever going to be generated.
    JS_EXPORT_PRIVATE static bool keepsNothingForGeneratingCode();

    // While the region is being built, on a thread that has a bmalloc::StaticRegion::AllocationScope; or the place that
    // placeNextCell() said. Null otherwise.
    JS_EXPORT_PRIVATE static void* tryAllocateCellSlow(VM&, size_t); // If Heap::m_placeOfNextCell.

    // ---- What is made when the program runs, at an address that what is made when it is built can refer to it by.

    static VM* addressOfVM() { return reinterpret_cast<VM*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss) + bmalloc::StaticRegion::offsetOfVMInBss); }
    static void* addressOfGlobalObject() { return reinterpret_cast<void*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss) + bmalloc::StaticRegion::offsetOfGlobalObjectInBss + sizeOfCellHeader); }
    // For symbols of the embedder's that every VM has the same ones of, as it has JSC::Symbols: made there, in an order that does
    // not change, what is made when a program is built can refer to them.
    static void* addressOfEmbedderSymbols() { return reinterpret_cast<void*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss) + bmalloc::StaticRegion::offsetOfEmbedderSymbolsInBss); }
    static constexpr size_t sizeForEmbedderSymbols = bmalloc::StaticRegion::offsetOfVTablesInBss - bmalloc::StaticRegion::offsetOfEmbedderSymbolsInBss;
    static bool isMapped() { return !!s_header; }
    // The next cell that is allocated in the VM is there. It is not collected, nor destroyed; didPlaceCell(), once it is made, has
    // every collection look at it.
    JS_EXPORT_PRIVATE static void placeNextCell(VM&, void* address);
    JS_EXPORT_PRIVATE static void didPlaceCell(VM&, JSCell*);
    // ---- The FunctionExecutables that are made when the program is built. They say what their source is by pointing at the
    // module's SourceProvider, which is made when the program runs: so that has to be made where they point.

    static constexpr size_t sizeOfPlaceForSourceProvider = 256;
    // For the provider of the module whose bytecode is there in the payload. Null if there is none, or it has been taken, or the
    // VM is not the one that the static heap is for. What is made there stays.
    // Or, if some other VM has made one there: `made`. There is one for all of them, so it had better be
    // SourceProvider::becomeShareableBetweenThreads(). Whoever is given the place says when the provider is made.
    JS_EXPORT_PRIVATE static void* takePlaceForSourceProvider(VM&, size_t entryOffsetOfModule, size_t sizeOfProvider, SourceProvider*& made);
    JS_EXPORT_PRIVATE static void didMakeSourceProvider(void* place);
    // The function that has this number in the image of code (AOT::CodeHeader::index), and which of its two kinds of code has it.
    static std::pair<FunctionExecutable*, CodeSpecializationKind> executableOfFunction(uint32_t index);
    static bool hasExecutablesOfFunctions(VM&);
    // What the image's functions are, by that number, as far as that could be told when the program was built. Null: nothing was.
    static AOT::FunctionInfo* infosOfFunctions(VM&);
    static const void* constantsOfProgram(VM&); // AOT::Instance::constantsOfProgram
    // The image's table of keys, if it goes without: those of the functions whose executables are made when the program runs.
    static std::span<const AOT::ImageKey> keysOfImage();
    static const AOT::ImageFunction* imageFunctionOfFunction(uint32_t index);
    // AOT::Instance::factsOfFunctions, and what the numbers in AOT::FunctionFacts are: how far into an arena.
    static const uint32_t* factsOfFunctions(VM&);
    template<typename T> static const T* inData(uint32_t offset) { return reinterpret_cast<const T*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Data) + offset); }
    template<typename T> static const T* inMalloc(uint32_t offset) { return reinterpret_cast<const T*>(bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Malloc) + offset); }
    // For the code of a function, which was left in the payload. `placed` is what UnlinkedFunctionExecutable::leaveCodeInPayload() was given.
    static Ref<Decoder> decoderOfWhatWasLeftInPayload(VM&, Decoder& placed);
    static void ensureDecoder(VM&, size_t indexOfModule, SourceProvider&);
    // What the VM has got for an executable of the static heap, if it has had to: see UnlinkedFunctionExecutable::unlinkedCodeBlockFor().
    // An executable like any other, of the VM's own, for the same function as one of the static heap's: for where that one's code
    // is no good, and there is nowhere in it to put any other.
    static FunctionExecutable* standInFor(VM&, FunctionExecutable*);
    static UnlinkedFunctionCodeBlock* codeOf(VM&, const UnlinkedFunctionExecutable&, CodeSpecializationKind);
    static void setCodeOf(VM&, const UnlinkedFunctionExecutable&, CodeSpecializationKind, UnlinkedFunctionCodeBlock*);
    static bool isPlaceOfSourceProvider(const void* pointer)
    {
        uintptr_t start = bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss);
        uintptr_t address = std::bit_cast<uintptr_t>(pointer);
        return address >= start + bmalloc::StaticRegion::offsetOfSourceProvidersInBss && address < start + bmalloc::StaticRegion::offsetOfTopLevelExecutablesInBss;
    }
    // The executable of the code of the module itself, in the realm that runs the program.
    static ScriptExecutable*& topLevelExecutableOfModuleWithProvider(VM& vm, const void* provider)
    {
        uintptr_t start = bmalloc::StaticRegion::startOf(bmalloc::StaticRegion::Arena::Bss);
        size_t index = (std::bit_cast<uintptr_t>(provider) - start - bmalloc::StaticRegion::offsetOfSourceProvidersInBss) / sizeOfPlaceForSourceProvider;
        if (!isFirst(vm)) [[unlikely]]
            return topLevelExecutableOfModuleInOtherVM(vm, index);
        return reinterpret_cast<ScriptExecutable**>(start + bmalloc::StaticRegion::offsetOfTopLevelExecutablesInBss)[index];
    }
    JS_EXPORT_PRIVATE static ScriptExecutable*& topLevelExecutableOfModuleInOtherVM(VM&, size_t index);

    // Cells that are not in the collector's own memory all say that they are of one VM. Whether it is this one: the first to ask,
    // if there is no static heap to have settled it.
    JS_EXPORT_PRIVATE static bool canPlaceCellsOf(VM&);
    // Zeroed memory to place cells in, a multiple of the size of a page. Any thread.
    JS_EXPORT_PRIVATE static void* allocateBlock(VM&, size_t);
    JS_EXPORT_PRIVATE static void freeBlock(void*, size_t);

private:
    struct Header;
    static void makeContainer(VM&);
    static inline void* const placeOfEveryCellWhileBuilding = reinterpret_cast<void*>(1); // Heap::m_placeOfNextCell: wherever there is room.

    JS_EXPORT_PRIVATE static bool s_isBuilding;
    JS_EXPORT_PRIVATE static void willAllocateUnlinkedFunctionSlow();
    static void keepWhatIsWantedOfFunctions(VM&, Header&);
    JS_EXPORT_PRIVATE static const RowOfFunction* s_rowsOfFunctions;
    JS_EXPORT_PRIVATE static VM* s_vm;
    JS_EXPORT_PRIVATE static bool s_isShared;
    JS_EXPORT_PRIVATE static bool s_hasNoCompilerThreads;
    static const Header* s_header; // Of what is mapped.
};

} // namespace JSC
