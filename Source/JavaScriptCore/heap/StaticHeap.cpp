/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "StaticHeap.h"

#if ENABLE(AOT)

#include "BuiltinExecutables.h"
#include "AOTImage.h"
#include "AOTRuntime.h"
#include "AbstractSlotVisitorInlines.h"
#include "AOTProgram.h"
#include "BuiltinNames.h"
#include "JSBigInt.h"
#include "BytecodeStructs.h"
#include "CachedTypes.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "PreciseAllocation.h"
#include "SourceCodeKey.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include <wtf/BitVector.h>
#include <wtf/NeverDestroyed.h>
#include <sys/mman.h>
#include <wtf/text/AtomStringTable.h>
#include <wtf/text/SymbolRegistry.h>

#if OS(DARWIN)
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

namespace JSC {

using Region = bmalloc::StaticRegion;

bool StaticHeap::s_isBuilding = false;
VM* StaticHeap::s_vm = nullptr;
bool StaticHeap::s_isShared = false;
bool StaticHeap::s_hasNoCompilerThreads = false;
const StaticHeap::Header* StaticHeap::s_header = nullptr;
static constexpr size_t pageSizeOfImage = 16 * KB;

struct StaticHeapModule {
    uint32_t entryOffset; // The array is sorted by this.
    // The parts of the module's SourceCodeKey that differ between modules, other than the source text itself.
    uint32_t keyHash;
    uint32_t keyLength;
    uint32_t keyFlags;
    // For a builtin function this is its UnlinkedFunctionExecutable instead. keyHash is then the embedder's stamp, and keyFlags is
    // unused.
    uint64_t codeBlock;
    uint64_t isBuiltinFunction;
};

struct StaticHeapTDZ {
    uint64_t executable; // The array is sorted by this.
    uint64_t record;
    uint64_t moduleIndex;
};
static Vector<StaticHeapTDZ>* s_tdzBeingBuilt;
static JSString* s_emptyStringBeingBuilt;
static SymbolRegistry* s_symbolRegistriesBeingBuilt[2];
static size_t s_moduleBeingBuilt;
static uint32_t s_entryOffsetOfModuleBeingBuilt;

// The layout of the file: this header, then each arena, starting on a page boundary.
struct StaticHeap::Header {
    static constexpr uint64_t expectedMagic = 0x3530504145485442ULL; // "BTHEAP05"
    static constexpr unsigned maxStructures = 32;

    uint64_t magic;
    uint64_t stamp; // Identifies the build of the engine. The arenas hold its objects, laid out as that build lays them out.
    uint64_t size; // Of the whole file.
    uint64_t arenaOffset[Region::numberOfArenasInFile];
    uint64_t arenaSize[Region::numberOfArenasInFile];
    // A range of Arena::Data that is not in the file and is left unmapped: the part of the bytecode payload that nothing needs at
    // run time. holeInData is its offset in the arena. In the file, the data after the hole directly follows the data before it.
    uint64_t holeInData;
    uint64_t sizeOfHoleInData;

    // The fields below are addresses, not offsets.
    uint64_t strings;
    uint64_t stringsSize;
    uint64_t stringSlots;
    uint64_t staticAtoms; // uint32_t[]. See AtomStringTable::StaticAtoms.
    uint64_t capacityOfStaticAtoms;
    uint64_t symbolRegistries[2]; // SymbolRegistry*: public, private.
    uint64_t payload;
    uint64_t payloadSize;
    uint64_t modules; // StaticHeapModule[]
    uint64_t numberOfModules;
    uint64_t tdz; // StaticHeapTDZ[]
    uint64_t numberOfTDZ;
    uint64_t infosOfFunctions; // AOT::FunctionInfo[], indexed by AOT::ImageFunction::index.
    uint64_t functionMetadataOffsets; // uint32_t[], indexed the same way. Zero if the unlinked code of functions is kept instead.
    uint64_t rowsOfFunctions; // RowOfFunction[], indexed the same way. See rowOf().
    // With Options::useGuardPagesForShortFunctionExecutables(), every other page of this range of Arena::Cells is left unmapped.
    uint64_t guardedFrom;
    uint64_t guardedTo;
    uint64_t numberOfFunctions;
    // See PositionsToKeep. With hasPositionsOfCallSites, FunctionMetadata::ExpressionInfo is the offset of the positions of a
    // function's call sites. An odd entry in functionMetadataOffsets is one more than the offset of the positions for code that has
    // no metadata.
    uint64_t namesOfSources; // uint32_t[numberOfSources + 1]: the offset of each name in the UTF-8 text that follows the array.
    uint64_t numberOfSources;
    uint64_t hasPositionsOfCallSites;
    uint64_t hasIdentifiersOfProgram; // See AOT::NumbersOfIdentifiers.
    uint64_t identifiersOfProgram; // UniquedStringImpl*[], indexed by identifier number.
    uint64_t constantsOfProgram; // EncodedJSValue[]. See AOT::NumbersOfConstants.
    uint64_t keysOfImage; // AOT::ImageKey[]. See keysOfImage().
    uint64_t capacityOfKeysOfImage;

    // The structures that cells in the heap use. Each is one of the VM's own structures, identified by its index in the VM,
    // together with the StructureID it must have.
    uint32_t numberOfStructures;
    struct {
        uint32_t indexInVM;
        uint32_t id;
    } structures[maxStructures];
};

// The structures that the VM creates first are consecutive members of VM.
static std::span<WriteBarrier<Structure>> structuresOf(VM& vm)
{
    return { &vm.structureStructure, static_cast<size_t>(&vm.bigIntStructure + 1 - &vm.structureStructure) };
}

// Arena::Bss has room for one Decoder per module.
static void* addressOfDecoder(size_t moduleIndex)
{
    constexpr size_t stride = roundUpToMultipleOf<64>(sizeof(Decoder));
    return reinterpret_cast<void*>(Region::startOf(Region::Arena::Bss) + Region::offsetOfDecodersInBss + moduleIndex * stride);
}

static VM* s_vmOfContainer = nullptr;

void StaticHeap::makeContainer(VM& vm)
{
    if (s_vmOfContainer == &vm)
        return;
    s_vmOfContainer = &vm;
    PreciseAllocation::setContainerOfStaticCells(PreciseAllocation::createForStaticCells(vm.heap, &vm.heap.cellSpace));
}

// The per-VM state of every VM other than the first. The first VM uses the process-wide state instead.
struct StaticHeapOfVM {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(StaticHeapOfVM);

    uint64_t number { 0 }; // Unique in the process, and never reused. (A VM's address can be reused by a later VM.)
    PreciseAllocation* container { nullptr };
    Vector<ScriptExecutable*> topLevelExecutables; // Indexed by module.
    UncheckedKeyHashMap<Decoder*, Ref<Decoder>> decoders;
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, std::array<Strong<UnlinkedFunctionCodeBlock>, 2>> code;
    UncheckedKeyHashMap<FunctionExecutable*, Strong<FunctionExecutable>> standIns;
};
static thread_local StaticHeapOfVM* t_ofVMOfThread = nullptr;
static thread_local bool t_threadIsPrepared = false;

static StaticHeapOfVM* ofVM(VM& vm) { return static_cast<StaticHeapOfVM*>(vm.m_staticHeapOfVM); }
static uint64_t numberOf(VM& vm) { return ofVM(vm) ? ofVM(vm)->number : 1; }

bool StaticHeap::isUsedBy(VM& vm)
{
    return s_vm == &vm || vm.m_staticHeapOfVM;
}

// Records which VM's container owns each block that cells are placed in.
struct OwnerOfBlock {
    std::atomic<uintptr_t> start { 0 };
    std::atomic<size_t> size { 0 };
    std::atomic<PreciseAllocation*> container { nullptr };
};
static constexpr unsigned maxBlocks = 256;
static OwnerOfBlock s_ownersOfBlocks[maxBlocks];
static std::atomic<unsigned> s_numberOfOwnersEverUsed { 0 }; // Only the first s_numberOfOwnersEverUsed entries have ever been used.

PreciseAllocation* StaticHeap::containerOfSlow(const void* cell)
{
    uintptr_t address = std::bit_cast<uintptr_t>(cell);
    if (address - (Region::startOf(Region::Arena::Bss) + Region::offsetOfBlocksInBss) < Region::arenaReservation - Region::offsetOfBlocksInBss) {
        for (unsigned i = 0, count = s_numberOfOwnersEverUsed.load(std::memory_order_acquire); i < count; ++i) {
            auto& owner = s_ownersOfBlocks[i];
            if (address - owner.start.load(std::memory_order_relaxed) < owner.size.load(std::memory_order_acquire))
                return owner.container.load(std::memory_order_relaxed);
        }
    }
    // (A GC helper thread has no VM. It only needs to know that the cell is marked, and every container reports that.)
    if (auto* ofVM = t_ofVMOfThread)
        return ofVM->container;
    return PreciseAllocation::containerOfStaticCells();
}

// See retainNeededFunctionData().
static bool s_allocatesFunctionsInScratch;
static bool s_nextCellIsOfAFunction;
// (At file scope these would have destructors to register whenever a process starts.)
static Vector<FunctionExecutable*>& executablesInScratch()
{
    static NeverDestroyed<Vector<FunctionExecutable*>> executables;
    return executables;
}
static UncheckedKeyHashSet<void*>& cellsInScratch()
{
    static NeverDestroyed<UncheckedKeyHashSet<void*>> cells;
    return cells;
}
static Vector<std::span<const WriteBarrier<UnlinkedFunctionExecutable>>>& listsOfFunctionsInFunctions()
{
    static NeverDestroyed<Vector<std::span<const WriteBarrier<UnlinkedFunctionExecutable>>>> lists;
    return lists;
}
const StaticHeap::RowOfFunction* StaticHeap::s_rowsOfFunctions;

bool StaticHeap::keepsNothingForGeneratingCode()
{
    return s_allocatesFunctionsInScratch;
}

void StaticHeap::willAllocateUnlinkedFunctionSlow()
{
    s_nextCellIsOfAFunction = true;
}

// The address and size of each cell, kept only while the heap is being built.
static std::array<Vector<std::pair<void*, size_t>>, 2>& cellsBeingBuilt() // For Arena::Cells and for Arena::MutableCells.
{
    static NeverDestroyed<std::array<Vector<std::pair<void*, size_t>>, 2>> cells;
    return cells;
}

template<typename Functor> static void forEachCell(Region::Arena arena, const Functor& functor)
{
    RELEASE_ASSERT(arena == Region::Arena::Cells || arena == Region::Arena::MutableCells);
    // (Iterates by index, because the functor may allocate another cell.)
    auto& cells = cellsBeingBuilt()[arena == Region::Arena::MutableCells];
    for (size_t i = 0; i < cells.size(); ++i)
        functor(cells[i].first, cells[i].second);
}

void StaticHeap::placeNextCell(VM& vm, void* address)
{
    RELEASE_ASSERT(!vm.heap.m_placeOfNextCell && !s_isBuilding && contains(address) && (std::bit_cast<uintptr_t>(address) & 15) == sizeOfCellHeader);
    vm.heap.m_placeOfNextCell = address;
}

bool StaticHeap::canPlaceCellsOf(VM& vm)
{
    static Lock lock;
    Locker locker { lock };
    if (!s_vmOfContainer && !s_isBuilding)
        makeContainer(vm);
    return s_vmOfContainer == &vm || vm.m_staticHeapOfVM;
}

static Lock s_blocksLock;
static size_t s_blocksUsed WTF_GUARDED_BY_LOCK(s_blocksLock) = 0;
static Vector<std::pair<void*, size_t>>& freeBlocks() WTF_REQUIRES_LOCK(s_blocksLock)
{
    static NeverDestroyed<Vector<std::pair<void*, size_t>>> blocks;
    return blocks;
}

void* StaticHeap::allocateBlock(VM& vm, size_t size)
{
    RELEASE_ASSERT(!(size % WTF::pageSize()));
    RELEASE_ASSERT(Region::mapRestOfBss());
    Locker locker { s_blocksLock };
    void* result = nullptr;
    auto& free = freeBlocks();
    for (unsigned i = 0; i < free.size(); ++i) {
        if (free[i].second == size) {
            result = free[i].first;
            free.removeAt(i);
            break;
        }
    }
    if (!result) {
        size_t offset = Region::offsetOfBlocksInBss + s_blocksUsed;
        RELEASE_ASSERT(size <= Region::arenaReservation - offset);
        s_blocksUsed += size;
        result = reinterpret_cast<void*>(Region::startOf(Region::Arena::Bss) + offset);
    }
    for (unsigned i = 0; i < maxBlocks; ++i) {
        auto& owner = s_ownersOfBlocks[i];
        if (owner.size.load(std::memory_order_relaxed))
            continue;
        if (i >= s_numberOfOwnersEverUsed.load(std::memory_order_relaxed))
            s_numberOfOwnersEverUsed.store(i + 1, std::memory_order_release);
        owner.container.store(ofVM(vm) ? ofVM(vm)->container : PreciseAllocation::containerOfStaticCells(), std::memory_order_relaxed);
        owner.start.store(std::bit_cast<uintptr_t>(result), std::memory_order_relaxed);
        owner.size.store(size, std::memory_order_release);
        return result;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void StaticHeap::freeBlock(void* block, size_t size)
{
    for (auto& owner : s_ownersOfBlocks) {
        if (owner.start.load(std::memory_order_relaxed) == std::bit_cast<uintptr_t>(block) && owner.size.load(std::memory_order_relaxed))
            owner.size.store(0, std::memory_order_release);
    }
    // Remapping zeroes the block and releases its physical pages until it is written again.
    void* result = mmap(block, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
    RELEASE_ASSERT(result == block);
    Locker locker { s_blocksLock };
    freeBlocks().append({ block, size });
}

void StaticHeap::didPlaceCell(VM& vm, JSCell* cell)
{
    RELEASE_ASSERT(contains(cell) && !vm.heap.m_placeOfNextCell);
    // Give the cell the state it would have after a collection that marked it. The write barrier makes the next collection visit
    // it, to find what has been stored in it since it was allocated.
    cell->setCellState(CellState::PossiblyBlack);
    vm.writeBarrier(cell);
}

void* StaticHeap::tryAllocateCellSlow(VM& vm, size_t size)
{
    if (vm.heap.m_placeOfNextCell != placeOfEveryCellWhileBuilding) {
        return std::exchange(vm.heap.m_placeOfNextCell, nullptr);
    }
    if (!Region::isAllocatingOnThisThread())
        return nullptr;
    if (std::exchange(s_nextCellIsOfAFunction, false) && s_allocatesFunctionsInScratch) {
        void* cell = Region::allocate(Region::Arena::Scratch, size, 16, sizeOfCellHeader);
        Region::AllocationScope notInRegion(false);
        cellsInScratch().add(cell);
        return cell;
    }
    bool isMutable = Region::isAllocatingMutable();
    void* cell = Region::allocate(isMutable ? Region::Arena::MutableCells : Region::Arena::Cells, size, 16, sizeOfCellHeader);
    Region::AllocationScope notInRegion(false);
    cellsBeingBuilt()[isMutable].append({ cell, size });
    return cell;
}

JSString* StaticHeap::emptyStringWhileBuilding(VM& vm)
{
    if (!s_emptyStringBeingBuilt)
        s_emptyStringBeingBuilt = JSString::createEmptyString(vm);
    return s_emptyStringBeingBuilt;
}

SymbolRegistry& StaticHeap::symbolRegistryWhileBuilding(bool isPrivate)
{
    return *s_symbolRegistriesBeingBuilt[isPrivate];
}

void StaticHeap::noteParentScopeTDZVariables(const UnlinkedFunctionExecutable& executable, const void* record)
{
    Region::AllocationScope notInRegion(false);
    s_tdzBeingBuilt->append({ std::bit_cast<uint64_t>(&executable), std::bit_cast<uint64_t>(record), s_moduleBeingBuilt });
}

namespace {

// Checks that everything the collector would visit from a cell is in the static heap too.
class ClosureChecker final : public AbstractSlotVisitor {
public:
    ClosureChecker(VM& vm)
        : AbstractSlotVisitor(vm.heap, "StaticHeap"_s, m_opaqueRootStorage)
    {
    }

    unsigned numberOfEscapes { 0 };
    JSCell* current { nullptr };

    void check(const void* pointer, const char* what)
    {
        bool isKept = std::bit_cast<uintptr_t>(pointer) - Region::startOf(Region::Arena::Scratch) >= Region::arenaReservation;
        if (!pointer || (StaticHeap::contains(pointer) && isKept))
            return;
        numberOfEscapes++;
        if (m_reported.add(pointer).isNewEntry && m_reported.size() <= 40) {
            auto* cell = static_cast<const JSCell*>(pointer);
            dataLogLn("StaticHeap: a ", current->classInfo()->className, " refers to ", what, " outside: ", RawPointer(pointer), " ", !strcmp(what, "a cell") ? cell->classInfo()->className : ""_s);
        }
    }

    void append(const ConservativeRoots&) final { }
    void appendUnbarriered(JSCell* cell) final
    {
        // (Strip the tag bit: see UnlinkedCodeBlock::executableIn().)
        cell = std::bit_cast<JSCell*>(std::bit_cast<uintptr_t>(cell) & ~UnlinkedCodeBlock::isExecutable);
        // (Structures are checked separately.)
        if (cell && cell->type() == StructureType)
            return;
        check(cell, "a cell");
    }
    void appendHiddenUnbarriered(JSCell* cell) final { appendUnbarriered(cell); }
    bool isFirstVisit() const final { return true; }
    bool isMarked(const void*) const final { return true; }
    bool isMarked(MarkedBlock&, HeapCell*) const final { return true; }
    bool isMarked(PreciseAllocation&, HeapCell*) const final { return true; }
    void markAuxiliary(const void* base) final { check(base, "storage"); }
    void reportExtraMemoryVisited(size_t) final { }
    void reportExternalMemoryVisited(size_t) final { }
    bool mutatorIsStopped() const final { return true; }
    void didRace(const VisitRaceKey&) final { }
    void visitAsConstraint(const JSCell*) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { }

private:
    UncheckedKeyHashSet<const void*> m_reported;
    ConcurrentPtrHashSet m_opaqueRootStorage;
};

} // anonymous namespace

static void* addressOfSourceProvider(size_t moduleIndex)
{
    return reinterpret_cast<void*>(Region::startOf(Region::Arena::Bss) + Region::offsetOfSourceProvidersInBss + moduleIndex * StaticHeap::sizeOfPlaceForSourceProvider);
}

// The offset of the AOT::FunctionMetadata of each function whose unlinked code is not kept. What the metadata refers to is kept
// (UnlinkedCodeBlock::leaveToStaticHeap()).
static std::span<uint32_t> s_functionMetadataOffsetsBeingBuilt;
static const StaticHeap::PositionsToKeep* s_positionsToKeep;
static UncheckedKeyHashMap<CString, uint32_t>* s_sourcesBeingBuilt;
static Vector<CString>* s_namesOfSourcesBeingBuilt;
static uint64_t s_bytesOfPositions;
static uint64_t s_numberOfPositions;

static void appendVarint(Vector<uint8_t>& bytes, uint64_t value)
{
    while (value >= 0x80) {
        bytes.append(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    bytes.append(static_cast<uint8_t>(value));
}

// The format of a function's positions, as a sequence of varints:
//   - The line and column where the function starts in the module's text (StaticHeap::whereFunctionStarts()).
//   - The number of entries.
//   - For each entry, in bytecode order: (delta of the bytecode offset from the previous entry) << 1 | isConstruction, then a
//     position. A construction has a second position, where its expression starts.
// A position on the same line of the same source as the previous one is zigzag(column delta) << 1 | 1. Any other position is
// zigzag(line delta) << 2 | sourceChanged << 1, then the number of the source if it changed (zero: no source, the position is in
// the module's text), then the column.
// (While a heap is being built its sources have no SourceProvider to ask. `lineStarts` came with the code of the module.)
static const uint8_t* makePositions(uint32_t index, UnlinkedCodeBlock* codeBlock, unsigned sourceOffset, LineStartTable& lineStarts, uint32_t entryOffsetOfModule)
{
    Vector<uint8_t> stream;
    {
        Region::AllocationScope notInRegion(false);
        auto& sites = s_positionsToKeep->sites[index];
        Vector<uint32_t> offsets = sites.offsets;
        // Also keep the resume points of an async function, which is where it reports being while it is suspended
        // (FunctionRef::resumePointOf()).
        if (size_t count = codeBlock->numberOfUnlinkedSwitchJumpTables(); count && isAsyncFunctionBodyParseMode(codeBlock->parseMode())) {
            auto& table = codeBlock->unlinkedSwitchJumpTable(count - 1);
            for (int32_t offset : table.m_branchOffsets)
                offsets.append(std::max(offset ? offset : table.m_defaultOffset, 0));
            std::ranges::sort(offsets);
            offsets.shrink(std::ranges::unique(offsets).begin() - offsets.begin());
        }
        if (!codeBlock->hasExpressionInfo())
            offsets.clear();
        auto positionOf = [&](unsigned offset) {
            LineColumn inText = lineStarts.lineColumnForOffset(StringView(), offset);
            return LineColumn { inText.line + 1, inText.column + 1 };
        };
        LineColumn start = positionOf(sourceOffset);
        appendVarint(stream, start.line);
        appendVarint(stream, start.column);
        appendVarint(stream, offsets.size());
        uint32_t previousOffset = 0;
        int64_t previousLine = 0;
        int64_t previousColumn = 0;
        uint32_t previousSource = 0;
        auto zigZagEncode = [](int64_t value) {
            return static_cast<uint64_t>(value << 1) ^ static_cast<uint64_t>(value >> 63);
        };
        auto appendPosition = [&](LineColumn inModule) {
            CString name;
            LineColumn position = inModule;
            uint32_t source = 0;
            if (s_positionsToKeep->find(entryOffsetOfModule, inModule, name, position)) {
                source = s_sourcesBeingBuilt->ensure(name, [&] {
                    s_namesOfSourcesBeingBuilt->append(name);
                    return static_cast<uint32_t>(s_namesOfSourcesBeingBuilt->size());
                }).iterator->value;
            } else
                position = inModule;
            if (position.line == previousLine && source == previousSource)
                appendVarint(stream, zigZagEncode(static_cast<int64_t>(position.column) - previousColumn) << 1 | 1);
            else {
                appendVarint(stream, zigZagEncode(static_cast<int64_t>(position.line) - previousLine) << 2 | (source != previousSource) << 1);
                if (source != previousSource)
                    appendVarint(stream, source);
                appendVarint(stream, position.column);
            }
            previousLine = position.line;
            previousColumn = position.column;
            previousSource = source;
            ++s_numberOfPositions;
        };
        size_t nextConstruction = 0;
        for (uint32_t offset : offsets) {
            if (offset >= codeBlock->instructions().size())
                offset = 0;
            while (nextConstruction < sites.constructions.size() && sites.constructions[nextConstruction].offset < offset)
                ++nextConstruction;
            const auto* construction = nextConstruction < sites.constructions.size() && sites.constructions[nextConstruction].offset == offset ? &sites.constructions[nextConstruction] : nullptr;
            LineColumn position = positionOf(sourceOffset + codeBlock->expressionInfoForBytecodeIndex(BytecodeIndex(offset)).divot);
            appendVarint(stream, static_cast<uint64_t>(offset - previousOffset) << 1 | !!construction);
            previousOffset = offset;
            appendPosition(position);
            if (construction) {
                LineColumn start = position;
                if (construction->linesUp) {
                    start.line = position.line > construction->linesUp ? position.line - construction->linesUp : 1;
                    start.column = construction->columnOrColumnsLeft;
                } else if (position.column > construction->columnOrColumnsLeft)
                    start.column = position.column - construction->columnOrColumnsLeft;
                appendPosition(start);
            }
        }
    }
    // (Allocated at an even address: see Header::hasPositionsOfCallSites.)
    auto* copy = static_cast<uint8_t*>(Region::allocate(Region::Arena::Data, stream.size(), 2));
    memcpySpan(std::span { copy, stream.size() }, stream.span());
    s_bytesOfPositions += stream.size();
    Region::AllocationScope notInRegion(false);
    stream = { };
    return copy;
}

static const uint8_t* deduplicatedCopy(std::span<const uint8_t>, size_t alignment); // Shares one copy between equal arrays, while there is an ArrayDeduplicator.

static void fillMetadata(uint32_t index, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, LineStartTable& lineStarts, uint32_t entryOffsetOfModule)
{
    if (s_functionMetadataOffsetsBeingBuilt.empty() || s_functionMetadataOffsetsBeingBuilt[index])
        return;
    if (codeBlock->codeType() != FunctionCode) {
        // The code of a module. Its UnlinkedCodeBlock is kept, so only the positions are needed.
        if (s_positionsToKeep)
            s_functionMetadataOffsetsBeingBuilt[index] = static_cast<uint32_t>(std::bit_cast<uintptr_t>(makePositions(index, codeBlock, 0, lineStarts, entryOffsetOfModule)) - Region::startOf(Region::Arena::Data)) | 1;
        return;
    }
    using Metadata = AOT::FunctionMetadata;
    auto in = [](Region::Arena arena, const void* pointer) {
        uintptr_t offset = std::bit_cast<uintptr_t>(pointer) - Region::startOf(arena);
        RELEASE_ASSERT(offset && offset < Region::used(arena));
        return static_cast<uint32_t>(offset);
    };
    RELEASE_ASSERT(codeBlock->instructions().size() < (1u << (32 - Metadata::shiftOfInstructionsSize)));
    Vector<uint32_t, 10> words { static_cast<uint32_t>(codeBlock->instructions().size()) << Metadata::shiftOfInstructionsSize | (codeBlock->isBuiltinFunction() ? Metadata::isBuiltinFunction : 0) };
    if (s_positionsToKeep) {
        // (A default class constructor gets its own executable in every realm, and has no position in any source.)
        if (executable) {
            words[0] |= Metadata::ExpressionInfo;
            words.append(in(Region::Arena::Data, makePositions(index, codeBlock, executable->source().startOffset(), lineStarts, entryOffsetOfModule)));
        }
    } else if (const void* record = codeBlock->cachedExpressionInfo()) {
        words[0] |= Metadata::ExpressionInfo;
        words.append(in(Region::Arena::Data, record));
    }
    if (size_t count = codeBlock->numberOfExceptionHandlers()) {
        auto* handlers = static_cast<UnlinkedHandlerInfo*>(Region::allocate(Region::Arena::Data, count * sizeof(UnlinkedHandlerInfo), alignof(UnlinkedHandlerInfo)));
        for (size_t i = 0; i < count; ++i)
            handlers[i] = codeBlock->exceptionHandler(i);
        words[0] |= Metadata::Handlers;
        words.append(in(Region::Arena::Data, handlers));
        words.append(count);
    }
    auto functions = [&](Metadata::Section section, std::span<const WriteBarrier<UnlinkedFunctionExecutable>> all) {
        if (all.empty())
            return;
        words[0] |= section;
        words.append(in(Region::Arena::Malloc, all.data()));
        words.append(all.size());
    };
    functions(Metadata::FunctionDecls, codeBlock->functionDecls());
    functions(Metadata::FunctionExprs, codeBlock->functionExprs());
    if (codeBlock->numberOfUnlinkedStringSwitchJumpTables()) {
        words[0] |= Metadata::StringSwitchJumpTables;
        words.append(in(Region::Arena::Malloc, &codeBlock->unlinkedStringSwitchJumpTable(0)));
    }
    if (!AOT::hasOnlyRealmIndependentConstants(codeBlock, AOT::SymbolTablesAreShared::Yes)) {
        auto& representations = codeBlock->constantsSourceCodeRepresentation();
        Vector<uint32_t, 16> list { static_cast<uint32_t>(representations.size()), 0 };
        for (unsigned i = 0; i < representations.size(); ++i) {
            if (representations[i] == SourceCodeRepresentation::LinkTimeConstant)
                list.append(i);
        }
        list[1] = list.size() - 2;
        auto* copy = static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, list.sizeInBytes(), alignof(uint32_t)));
        memcpySpan(std::span { copy, list.size() }, list.span());
        words[0] |= Metadata::RealmConstants;
        words.append(in(Region::Arena::Data, copy));
    }
    if (size_t count = codeBlock->numberOfUnlinkedSwitchJumpTables(); count && isAsyncFunctionBodyParseMode(codeBlock->parseMode())) {
        auto& table = codeBlock->unlinkedSwitchJumpTable(count - 1);
        Vector<int32_t, 16> list { table.m_min, static_cast<int32_t>(table.m_branchOffsets.size()) };
        for (int32_t offset : table.m_branchOffsets)
            list.append(offset ? offset : table.m_defaultOffset);
        auto* copy = static_cast<int32_t*>(Region::allocate(Region::Arena::Data, list.sizeInBytes(), alignof(int32_t)));
        memcpySpan(std::span { copy, list.size() }, list.span());
        words[0] |= Metadata::ResumePoints;
        words.append(in(Region::Arena::Data, copy));
    }
    if (codeBlock->numberOfConstantIdentifierSets()) {
        words[0] |= Metadata::ConstantIdentifierSets;
        words.append(in(Region::Arena::Malloc, &codeBlock->constantIdentifierSets()[0]));
    }
    {
        Vector<uint8_t> scalars;
        {
            Region::AllocationScope notInRegion(false);
            scalars = scalarsToMakeFunctionCodeFrom(*codeBlock);
        }
        words[0] |= Metadata::Scalars;
        words.append(in(Region::Arena::Data, deduplicatedCopy(scalars.span(), 1)));
        Region::AllocationScope notInRegion(false);
        scalars = { };
    }
    auto* metadata = static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, words.sizeInBytes(), alignof(uint32_t)));
    memcpySpan(std::span { metadata, words.size() }, words.span());
    s_functionMetadataOffsetsBeingBuilt[index] = in(Region::Arena::Data, metadata);
}

namespace {
class ArrayDeduplicator {
public:
    const uint8_t* copyOf(std::span<const uint8_t> content, size_t alignment = sizeof(void*))
    {
        if (content.empty())
            return nullptr;
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : content)
            hash = (hash ^ byte) * 1099511628211ull;
        Region::AllocationScope notInRegion(false);
        auto& withHash = m_copies.add(hash | 1, Vector<std::span<const uint8_t>, 1> { }).iterator->value;
        for (auto& copy : withHash) {
            if (equalSpans(copy, content) && !(std::bit_cast<uintptr_t>(copy.data()) % alignment))
                return copy.data();
        }
        auto* copy = static_cast<uint8_t*>(Region::allocate(Region::Arena::Data, content.size(), alignment));
        memcpySpan(std::span { copy, content.size() }, content);
        withHash.append(std::span<const uint8_t> { copy, content.size() });
        return copy;
    }
    void clear()
    {
        Region::AllocationScope notInRegion(false);
        m_copies.clear();
    }

private:
    UncheckedKeyHashMap<uint64_t, Vector<std::span<const uint8_t>, 1>> m_copies;
};
ArrayDeduplicator* s_arrayDeduplicator;
}

static const uint8_t* deduplicatedCopy(std::span<const uint8_t> content, size_t alignment)
{
    if (s_arrayDeduplicator)
        return s_arrayDeduplicator->copyOf(content, alignment);
    auto* copy = static_cast<uint8_t*>(Region::allocate(Region::Arena::Data, content.size(), alignment));
    memcpySpan(std::span { copy, content.size() }, content);
    return copy;
}

namespace {
std::span<const ReportableSitesOfFunction> s_reportableSites;
static thread_local bool s_realmIsProgramRealm;
std::span<UniquedStringImpl*> s_identifiersOfProgram; // See AOT::NumbersOfIdentifiers.
std::span<EncodedJSValue> s_constantsOfProgram; // See AOT::NumbersOfConstants.
}

static void fillInfo(AOT::FunctionInfo& info, const AOT::ImageView::Function& function, UnlinkedCodeBlock* codeBlock, ScriptExecutable* executable, CodeSpecializationKind kind, LineStartTable& lineStarts)
{
    // (Its SymbolTables are being created in the static heap right now, so they are shared.)
    bool constantsAreRealmIndependent = AOT::hasOnlyRealmIndependentConstants(codeBlock, AOT::SymbolTablesAreShared::Yes);
    RELEASE_ASSERT(constantsAreRealmIndependent || !function.startsCold);
    info.constants = codeBlock->constantRegisters().span().data();
    info.identifiers = codeBlock->identifiers().span().data();
    // When the unlinked code is not kept, these arrays are only ever read as plain arrays of words. So functions with equal arrays
    // share one copy, without a header.
    const Vector<uint32_t>* numbersOfConstants = s_constantsOfProgram.empty() ? nullptr : &s_reportableSites[function.index].numbersOfConstants;
    if (numbersOfConstants && !numbersOfConstants->isEmpty()) {
        RELEASE_ASSERT(constantsAreRealmIndependent && codeBlock->codeType() == FunctionCode && numbersOfConstants->size() == codeBlock->constantRegisters().size());
        for (unsigned i = 0; i < numbersOfConstants->size(); ++i) {
            JSValue value = codeBlock->constantRegisters()[i].get();
            uint32_t number = numbersOfConstants->at(i);
            // (A link-time constant is not read as a constant: see AOT::NodeKind::LinkTimeConstant.)
            if (codeBlock->constantsSourceCodeRepresentation()[i] == SourceCodeRepresentation::LinkTimeConstant)
                value = JSValue();
            RELEASE_ASSERT(!value == (number == AOT::notAConstantOfProgram));
            if (!value)
                continue;
            EncodedJSValue& inTable = s_constantsOfProgram[number];
            if (!inTable)
                inTable = JSValue::encode(value);
            else if (inTable != JSValue::encode(value)) {
                // Two JSStrings with equal contents. Either one can be used for both.
                JSValue other = JSValue::decode(inTable);
                RELEASE_ASSERT(value.isString() && other.isString());
                String said = asString(value)->tryGetValue();
                String saidByOther = asString(other)->tryGetValue();
                RELEASE_ASSERT(said == saidByOther);
            }
        }
        info.constants = nullptr;
    } else if (s_arrayDeduplicator && codeBlock->codeType() == FunctionCode)
        info.constants = std::bit_cast<decltype(info.constants)>(s_arrayDeduplicator->copyOf(asBytes(codeBlock->constantRegisters().span())));
    if (s_arrayDeduplicator && codeBlock->codeType() == FunctionCode) {
        // (With one identifier table for the whole program, the function has no table of its own.)
        if (s_identifiersOfProgram.empty())
            info.identifiers = std::bit_cast<decltype(info.identifiers)>(s_arrayDeduplicator->copyOf(asBytes(codeBlock->identifiers().span())));
    }
    if (!s_identifiersOfProgram.empty()) {
        static_assert(sizeof(Identifier) == sizeof(UniquedStringImpl*));
        auto& numbers = s_reportableSites[function.index].numbersOfIdentifiers;
        RELEASE_ASSERT(numbers.size() == codeBlock->numberOfIdentifiers());
        for (unsigned i = 0; i < numbers.size(); ++i) {
            UniquedStringImpl*& inTable = s_identifiersOfProgram[numbers[i]];
            UniquedStringImpl* name = codeBlock->identifier(i).impl();
            RELEASE_ASSERT(!inTable || inTable == name);
            inTable = name;
        }
        info.identifiers = s_identifiersOfProgram.data();
    }
    info.sites = function.sites;
    info.setExecutable(executable, kind, constantsAreRealmIndependent);
    info.flags = (function.hasSiteConstants ? AOT::FunctionInfo::hasSiteConstants : AOT::FunctionInfo::sitesHaveTheirConstants) | (function.startsCold && executable ? AOT::FunctionInfo::startsCold : 0) | AOT::FunctionInfo::slotsAmongFlags(function.numSlots);
    RELEASE_ASSERT(info.function() == function.function);
    fillMetadata(function.index, codeBlock, executable, lineStarts, s_entryOffsetOfModuleBeingBuilt);
}

// Creates the executables of the functions nested in `codeBlock`, whose source is `source`, recursively.
static void makeExecutables(VM& vm, UnlinkedCodeBlock* codeBlock, const SourceCode& source, bool isInsideOrdinaryFunction, uint32_t moduleID, const AOT::ImageView& image, std::span<AOT::FunctionInfo> byIndex, LineStartTable& lineStarts, UnlinkedFunctionExecutable* only = nullptr)
{
    auto make = [&](UnlinkedFunctionExecutable* unlinked) {
        if (unlinked->staticExecutable())
            return;
        // The source of a default class constructor belongs to the engine, so its executable is created at run time.
        bool isDefaultConstructor = unlinked->isBuiltinDefaultClassConstructor();
        auto functionKey = orderFunctionKey(*unlinked, isDefaultConstructor ? source : unlinked->linkedSourceCode(source));
        if (!functionKey)
            return;
        std::optional<AOT::ImageView::Function> code[2];
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (!unlinked->codeBlockIfExists(kind))
                continue;
            AOT::ImageKey key;
            key.module = moduleID;
            key.start = functionKey->start;
            key.kind = static_cast<uint32_t>(functionKey->kind) << 1 | (kind == CodeSpecializationKind::CodeForConstruct);
            code[static_cast<unsigned>(kind)] = image.find(key);
        }
        if (!code[0] && !code[1])
            return;
        if (isDefaultConstructor) {
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto& function = code[static_cast<unsigned>(kind)]; function && !byIndex[function->index].sites)
                    fillInfo(byIndex[function->index], *function, unlinked->codeBlockIfExists(kind), nullptr, kind, lineStarts);
            }
            return;
        }
        // A function compiled both for call and for construct has two copies of its nested functions. Each nested function still
        // has only one compiled function, which belongs to one executable (AOT::FunctionInfo).
        FunctionExecutable* executable = nullptr;
        for (auto& function : code) {
            if (function && byIndex[function->index].executable())
                executable = uncheckedDowncast<FunctionExecutable>(byIndex[function->index].executable());
        }
        if (executable) {
            bool isComplete = true;
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto& function = code[static_cast<unsigned>(kind)])
                    isComplete &= byIndex[function->index].executable() == executable && byIndex[function->index].kind() == kind;
            }
            if (!isComplete)
                return;
            unlinked->setStaticExecutable(executable);
            for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                if (auto* nested = unlinked->codeBlockIfExists(kind))
                    makeExecutables(vm, nested, executable->source(), executable->isInsideOrdinaryFunction(), moduleID, image, byIndex, lineStarts);
            }
            return;
        }
        s_nextCellIsOfAFunction = true;
        executable = unlinked->link(vm, nullptr, source, std::nullopt, NoIntrinsic, isInsideOrdinaryFunction);
        if (s_allocatesFunctionsInScratch) {
            Region::AllocationScope notInRegion(false);
            executablesInScratch().append(executable);
        }
        executable->becomeStatic(vm);
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (auto& function = code[static_cast<unsigned>(kind)]) {
                    executable->setAOTCode(kind, image.addressOfStub(isCall(kind) ? AOT::Stub::EnterStaticFunctionForCall : AOT::Stub::EnterStaticFunctionForConstruct), function->entry, function->index);
                fillInfo(byIndex[function->index], *function, unlinked->codeBlockIfExists(kind), executable, kind, lineStarts);
            }
        }
        // (Any other function either has code for construct or cannot be constructed.)
        if (code[0] && !code[1] && unlinked->constructAbility() == ConstructAbility::CanConstruct && !unlinked->isClassConstructorFunction()) {
            void* stub = image.addressOfStub(AOT::Stub::ConstructByCalling);
            executable->setAOTCode(CodeSpecializationKind::CodeForConstruct, stub, std::bit_cast<uintptr_t>(stub), FunctionExecutable::aotIndexOfWhatConstructsByCalling);
        }
        unlinked->setStaticExecutable(executable);
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (auto* nested = unlinked->codeBlockIfExists(kind))
                makeExecutables(vm, nested, executable->source(), executable->isInsideOrdinaryFunction(), moduleID, image, byIndex, lineStarts);
        }
    };
    if (only) {
        make(only);
        return;
    }
    for (unsigned i = 0; i < codeBlock->numberOfFunctionDecls(); ++i)
        make(codeBlock->functionDecl(i));
    for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
        make(codeBlock->functionExpr(i));
}

// The names in a SymbolTable are only needed to find a variable by name at run time. Compiled code refers to a variable by its
// offset, unless the bytecode generator could not tell which scope declares the name. Code that is evaluated at run time can name
// any variable in scope.
struct DynamicallyResolvedNames {
    bool mayBeAny { false };
    UncheckedKeyHashSet<UniquedStringImpl*> names;
};

static UncheckedKeyHashSet<SymbolTable*>* s_processedSymbolTables;

static bool isLookedUpByName(ResolveType type)
{
    return type != ResolvedClosureVar && type != ResolvedLazyClosureVar && type != ModuleVar && !isStaticClosureVarResolveType(type);
}

// Adds the names that the code and its nested functions look up to `lookedUp`, and drops the other names from the symbol tables of
// the scopes the code creates. No other code can see those scopes.
// exportedByModule: for the code of a module, the scope offsets of the variables it exports, if known.
static void dropUnreferencedVariableNames(UnlinkedCodeBlock* codeBlock, DynamicallyResolvedNames& lookedUp, const Vector<uint32_t>* exportedByModule = nullptr)
{
    DynamicallyResolvedNames own;
    {
        Region::AllocationScope notInRegion(false);
        for (const auto& instruction : codeBlock->instructions()) {
            switch (instruction->opcodeID()) {
            case op_call_direct_eval:
                own.mayBeAny = true;
                break;
            case op_resolve_scope:
                if (auto bytecode = instruction->as<OpResolveScope>(); isLookedUpByName(bytecode.m_resolveType))
                    own.names.add(codeBlock->identifier(bytecode.m_var).impl());
                break;
            case op_get_from_scope:
                if (auto bytecode = instruction->as<OpGetFromScope>(); isLookedUpByName(bytecode.m_getPutInfo.resolveType()))
                    own.names.add(codeBlock->identifier(bytecode.m_var).impl());
                break;
            case op_put_to_scope:
                if (auto bytecode = instruction->as<OpPutToScope>(); isLookedUpByName(bytecode.m_getPutInfo.resolveType()))
                    own.names.add(codeBlock->identifier(bytecode.m_var).impl());
                break;
            default:
                break;
            }
        }
    }
    auto inside = [&](UnlinkedFunctionExecutable* function) {
        if (function->features() & EvalFeature)
            own.mayBeAny = true;
        bool hasCode = false;
        for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
            if (auto* nested = function->codeBlockIfExists(kind)) {
                hasCode = true;
                dropUnreferencedVariableNames(nested, own);
            }
        }
        // (Without its code, assume that it may look up any name.)
        if (!hasCode)
            own.mayBeAny = true;
    };
    for (unsigned i = 0; i < codeBlock->numberOfFunctionDecls(); ++i)
        inside(codeBlock->functionDecl(i));
    for (unsigned i = 0; i < codeBlock->numberOfFunctionExprs(); ++i)
        inside(codeBlock->functionExpr(i));

    // A module's environment is also searched by name to resolve the module's exports, and by the engine for its private names.
    SymbolTable* ofModule = nullptr;
    if (auto* moduleCode = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
        ofModule = dynamicDowncast<SymbolTable>(moduleCode->constantRegister(VirtualRegister(moduleCode->moduleEnvironmentSymbolTableConstantRegisterOffset())).get());
    if (ofModule && exportedByModule && !own.mayBeAny) {
        UncheckedKeyHashSet<uint32_t, WTF::IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> exported;
        {
            Region::AllocationScope notInRegion(false);
            for (uint32_t offset : *exportedByModule)
                exported.add(offset);
        }
        ofModule->keepOnly([&](UniquedStringImpl* name, const SymbolTableEntry& entry) {
            return name->isSymbol() || own.names.contains(name) || (entry.varOffset().isScope() && exported.contains(entry.scopeOffset().offset()));
        });
        Region::AllocationScope notInRegion(false);
        exported.clear();
    }
    for (auto& constant : codeBlock->constantRegisters()) {
        auto* table = constant.get().isCell() ? dynamicDowncast<SymbolTable>(constant.get().asCell()) : nullptr;
        if (!table || table == ofModule)
            continue;
        {
            // Which names are kept depends on the code that can see the scope: the code that owns the table and the functions
            // nested in it. So each table must be processed only once.
            Region::AllocationScope notInRegion(false);
            RELEASE_ASSERT(s_processedSymbolTables->add(table).isNewEntry);
        }
        if (!own.mayBeAny)
            table->keepOnlyNames(own.names);
    }

    Region::AllocationScope notInRegion(false);
    lookedUp.mayBeAny |= own.mayBeAny;
    for (auto* name : own.names)
        lookedUp.names.add(name);
    own.names.clear();
}

std::pair<FunctionExecutable*, CodeSpecializationKind> StaticHeap::executableOfFunction(uint32_t index)
{
    RELEASE_ASSERT(s_header && index < s_header->numberOfFunctions);
    const AOT::FunctionInfo& info = std::bit_cast<const AOT::FunctionInfo*>(s_header->infosOfFunctions)[index];
    RELEASE_ASSERT(info.executable());
    return { uncheckedDowncast<FunctionExecutable>(info.executable()), info.kind() };
}

const uint32_t* StaticHeap::functionMetadataOffsets(VM& vm)
{
    return hasExecutablesOfFunctions(vm) ? std::bit_cast<const uint32_t*>(s_header->functionMetadataOffsets) : nullptr;
}

Ref<Decoder> StaticHeap::decoderForKeptPayload(VM& vm, Decoder& placed)
{
    RELEASE_ASSERT(isUsedBy(vm));
    static NeverDestroyed<UncheckedKeyHashMap<Decoder*, Ref<Decoder>>> decodersOfFirstVM;
    auto& decoders = ofVM(vm) ? ofVM(vm)->decoders : decodersOfFirstVM.get();
    return decoders.ensure(&placed, [&] {
        return Decoder::create(vm, placed.cachedBytecode(), placed.provider(), Decoder::RecoverableCode::No);
    }).iterator->value;
}

FunctionExecutable* StaticHeap::standInFor(VM& vm, FunctionExecutable* executable)
{
    RELEASE_ASSERT(isUsedBy(vm) && contains(executable) && !executable->isShortForm());
    static NeverDestroyed<decltype(StaticHeapOfVM::standIns)> standInsOfFirstVM;
    auto& standIns = ofVM(vm) ? ofVM(vm)->standIns : standInsOfFirstVM.get();
    if (auto it = standIns.find(executable); it != standIns.end())
        return it->value.get();
    FunctionExecutable* result = FunctionExecutable::create(vm, executable->topLevelExecutable(), executable->source(), executable->unlinkedExecutable(), NoIntrinsic, executable->isInsideOrdinaryFunction());
    standIns.add(executable, Strong<FunctionExecutable> { vm, result });
    return result;
}

static decltype(StaticHeapOfVM::code)& retainedCodeOf(VM& vm)
{
    RELEASE_ASSERT(StaticHeap::isUsedBy(vm));
    static NeverDestroyed<decltype(StaticHeapOfVM::code)> codeOfFirstVM;
    return ofVM(vm) ? ofVM(vm)->code : codeOfFirstVM.get();
}

UnlinkedFunctionCodeBlock* StaticHeap::codeOf(VM& vm, const UnlinkedFunctionExecutable& executable, CodeSpecializationKind kind)
{
    auto& code = retainedCodeOf(vm);
    auto it = code.find(&executable);
    return it == code.end() ? nullptr : it->value[static_cast<unsigned>(kind)].get();
}

void StaticHeap::setCodeOf(VM& vm, const UnlinkedFunctionExecutable& executable, CodeSpecializationKind kind, UnlinkedFunctionCodeBlock* codeBlock)
{
    retainedCodeOf(vm).add(&executable, std::array<Strong<UnlinkedFunctionCodeBlock>, 2> { }).iterator->value[static_cast<unsigned>(kind)].set(vm, codeBlock);
}

UniquedStringImpl* const* StaticHeap::identifiersOfProgram()
{
    return s_header ? std::bit_cast<UniquedStringImpl* const*>(s_header->identifiersOfProgram) : nullptr;
}

std::span<const AOT::ImageKey> StaticHeap::keysOfImage()
{
    if (!s_header)
        return { };
    return { std::bit_cast<const AOT::ImageKey*>(s_header->keysOfImage), static_cast<size_t>(s_header->capacityOfKeysOfImage) };
}

const AOT::ImageFunction* StaticHeap::imageFunctionOfFunction(uint32_t index)
{
    RELEASE_ASSERT(s_header && index < s_header->numberOfFunctions);
    return std::bit_cast<const AOT::FunctionInfo*>(s_header->infosOfFunctions)[index].function();
}

const void* StaticHeap::constantsOfProgram(VM& vm)
{
    return hasExecutablesOfFunctions(vm) ? std::bit_cast<const void*>(s_header->constantsOfProgram) : nullptr;
}

AOT::FunctionInfo* StaticHeap::infosOfFunctions(VM& vm)
{
    return hasExecutablesOfFunctions(vm) ? std::bit_cast<AOT::FunctionInfo*>(s_header->infosOfFunctions) : nullptr;
}

bool StaticHeap::hasExecutablesOfFunctions(VM& vm)
{
    return s_header && isUsedBy(vm) && s_header->numberOfFunctions;
}

ScriptExecutable*& StaticHeap::topLevelExecutableOfModuleInOtherVM(VM& vm, size_t index)
{
    return ofVM(vm)->topLevelExecutables[index];
}

// The state of each slot for a SourceProvider: free (maker is zero), being constructed by the VM whose number is `maker`, or
// constructed (isMade).
struct SourceProviderSlotState {
    std::atomic<uint64_t> maker { 0 }; // See numberOf().
    std::atomic<bool> isMade { false };
};
static SourceProviderSlotState* sourceProviderSlotStates(size_t numberOfModules)
{
    static SourceProviderSlotState* states;
    static std::once_flag once;
    std::call_once(once, [&] {
        states = new SourceProviderSlotState[numberOfModules];
    });
    return states;
}

void StaticHeap::didMakeSourceProvider(void* place)
{
    size_t index = (std::bit_cast<uintptr_t>(place) - std::bit_cast<uintptr_t>(addressOfSourceProvider(0))) / sizeOfPlaceForSourceProvider;
    sourceProviderSlotStates(s_header->numberOfModules)[index].isMade.store(true, std::memory_order_release);
}

void* StaticHeap::takePlaceForSourceProvider(VM& vm, size_t entryOffset, size_t sizeOfProvider, SourceProvider*& made)
{
    RELEASE_ASSERT(sizeOfProvider <= sizeOfPlaceForSourceProvider);
    made = nullptr;
    if (!s_header || !isUsedBy(vm))
        return nullptr;
    std::span<const StaticHeapModule> modules { std::bit_cast<const StaticHeapModule*>(s_header->modules), static_cast<size_t>(s_header->numberOfModules) };
    size_t index = std::ranges::lower_bound(modules, entryOffset, { }, &StaticHeapModule::entryOffset) - modules.begin();
    if (index == modules.size() || modules[index].entryOffset != entryOffset || !modules[index].codeBlock)
        return nullptr;
    void* place = addressOfSourceProvider(index);
    SourceProviderSlotState& state = sourceProviderSlotStates(modules.size())[index];
    uint64_t maker = 0;
    if (state.maker.compare_exchange_strong(maker, numberOf(vm)))
        return place;
    // (A VM that loads the same module a second time gets an ordinary heap-allocated provider, as it would without a static heap.)
    if (maker == numberOf(vm))
        return nullptr;
    while (!state.isMade.load(std::memory_order_acquire))
        Thread::yield();
    made = static_cast<SourceProvider*>(place);
    return nullptr;
}

// A FunctionExecutable and an UnlinkedFunctionExecutable are created for every function, as usual: they are how the function's code
// is found and described. But most of their fields are for code that has yet to be parsed, compiled or replaced, and a program that
// ships without its bytecode has none. So they are created in Arena::Scratch, and this function then copies what is needed to the
// arenas that are kept:
//   - For most functions, a FunctionExecutable in the short form (see FunctionExecutable::sizeOfShortForm) and a RowOfFunction. The
//     fields of the UnlinkedFunctionExecutable that are not in the row are equal for many functions, which share one copy.
//   - For a function that cannot use the short form, both objects in full.
//   - For a builtin, which is looked up by its UnlinkedFunctionExecutable, that object in full.
// The code that contains a function refers to its FunctionExecutable directly (UnlinkedCodeBlock::executableIn()).
void StaticHeap::retainNeededFunctionData(VM& vm, Header& header)
{
    auto isScratch = [](uintptr_t bits) { return bits - Region::startOf(Region::Arena::Scratch) < Region::used(Region::Arena::Scratch) && cellsInScratch().contains(std::bit_cast<void*>(bits)); };
    auto place = [&](const void* bytes, size_t size) {
        void* cell = Region::allocate(Region::Arena::Cells, size, 16, sizeOfCellHeader);
        memcpy(cell, bytes, size);
        cellsBeingBuilt()[0].append({ cell, size });
        return cell;
    };
    UncheckedKeyHashMap<UnlinkedFunctionExecutable*, UnlinkedFunctionExecutable*> kept;
    auto keep = [&](UnlinkedFunctionExecutable* function) {
        if (!isScratch(std::bit_cast<uintptr_t>(function)))
            return function;
        return kept.ensure(function, [&] { return static_cast<UnlinkedFunctionExecutable*>(place(function, sizeof(UnlinkedFunctionExecutable))); }).iterator->value;
    };
    std::span functionMetadataOffsets { std::bit_cast<const uint32_t*>(header.functionMetadataOffsets), static_cast<size_t>(header.numberOfFunctions) };

    // First the executables, contiguously, because they are what a call reads.
    std::span<RowOfFunction> rows { static_cast<RowOfFunction*>(Region::allocate(Region::Arena::Data, header.numberOfFunctions * sizeof(RowOfFunction), pageSizeOfImage)), static_cast<size_t>(header.numberOfFunctions) };
    zeroSpan(rows);
    header.rowsOfFunctions = std::bit_cast<uint64_t>(rows.data());
    UncheckedKeyHashMap<FunctionExecutable*, FunctionExecutable*> moved;
    Vector<std::pair<uint32_t, UnlinkedFunctionExecutable*>> inShortForm;
    Vector<FunctionExecutable*> inFull;
    Structure* structureOfShortForm = vm.shortFunctionExecutableStructure.get();
    for (auto* executable : executablesInScratch()) {
        UnlinkedFunctionExecutable* unlinked = executable->unlinkedExecutable();
        RELEASE_ASSERT(isScratch(std::bit_cast<uintptr_t>(executable)) && isScratch(std::bit_cast<uintptr_t>(unlinked)));
        bool hasCodeToCall = executable->aotEntryFor(CodeSpecializationKind::CodeForCall);
        bool hasCode = hasCodeToCall || (executable->aotEntryFor(CodeSpecializationKind::CodeForConstruct) && !executable->constructsByCalling());
        uint32_t index = executable->aotIndexFor(hasCodeToCall ? CodeSpecializationKind::CodeForCall : CodeSpecializationKind::CodeForConstruct);
        size_t module = (std::bit_cast<uintptr_t>(executable->source().provider()) - std::bit_cast<uintptr_t>(addressOfSourceProvider(0))) / sizeOfPlaceForSourceProvider;
        auto hasStartPosition = [&] {
            uint32_t at = functionMetadataOffsets[index];
            return at && !(at & 1) && inData<AOT::FunctionMetadata>(at)->find(AOT::FunctionMetadata::ExpressionInfo);
        };
        bool canBeShort = hasCode && unlinked->canBeSharedByStaticExecutables() && hasStartPosition()
            && isPlaceOfSourceProvider(executable->source().provider()) && module < (1u << RowOfFunction::bitsOfModule)
            && unlinked->parameterCount() < (1u << RowOfFunction::bitsOfParameterCount)
            && executable->intrinsic() == NoIntrinsic && executable->evalContextType() == EvalContextType::None && !executable->overrideLineNumber()
            && executable->derivedContextType() == unlinked->derivedContextType() && executable->lexicallyScopedFeatures() == unlinked->lexicallyScopedFeatures();
        if (!canBeShort) {
            inFull.append(executable);
            continue;
        }
        char* copy;
        if (Options::useGuardPagesForShortFunctionExecutables()) [[unlikely]] {
            // (Cells are at addresses that are 8 modulo 16, so a cell cannot end exactly at a page boundary. The bytes in between
            // are filled with a value that is not a valid address.)
            auto* pages = static_cast<char*>(Region::allocate(Region::Arena::Cells, 2 * pageSizeOfImage, pageSizeOfImage));
            if (!header.guardedFrom)
                header.guardedFrom = std::bit_cast<uintptr_t>(pages) - Region::startOf(Region::Arena::Cells);
            header.guardedTo = std::bit_cast<uintptr_t>(pages) + 2 * pageSizeOfImage - Region::startOf(Region::Arena::Cells);
            copy = pages + pageSizeOfImage - sizeOfCellHeader - FunctionExecutable::sizeOfShortForm;
            memcpy(copy, static_cast<const void*>(executable), FunctionExecutable::sizeOfShortForm);
            memset(copy + FunctionExecutable::sizeOfShortForm, 0xfb, sizeOfCellHeader);
            cellsBeingBuilt()[0].append({ copy, FunctionExecutable::sizeOfShortForm });
        } else
            copy = static_cast<char*>(place(executable, FunctionExecutable::sizeOfShortForm));
        *reinterpret_cast<uint32_t*>(copy + JSCell::structureIDOffset()) = structureOfShortForm->id().bits();
        *reinterpret_cast<uint8_t*>(copy + JSCell::typeInfoTypeOffset()) = ShortFunctionExecutableType;
        *reinterpret_cast<uint8_t*>(copy + JSCell::typeInfoFlagsOffset()) = structureOfShortForm->typeInfo().inlineTypeFlags();
        moved.add(executable, reinterpret_cast<FunctionExecutable*>(copy));
        RowOfFunction& row = rows[index];
        row.nameImpl = unlinked->ecmaName().impl();
        row.module = module;
        row.parameterCount = unlinked->parameterCount();
        row.isArrowFunctionContext = executable->isArrowFunctionContext();
        row.isInsideOrdinaryFunction = executable->isInsideOrdinaryFunction();
        inShortForm.append({ index, unlinked });
    }

    // Then the UnlinkedFunctionExecutables that they share. There are few.
    UncheckedKeyHashMap<String, UnlinkedFunctionExecutable*> shared;
    for (auto& [index, unlinked] : inShortForm) {
        auto bytes = unlinked->whatIsSharedByStaticExecutables();
        auto* one = shared.ensure(String { std::span { reinterpret_cast<const Latin1Character*>(bytes.data()), bytes.size() } }, [&] {
            return static_cast<UnlinkedFunctionExecutable*>(place(bytes.data(), bytes.size()));
        }).iterator->value;
        rows[index].unlinkedFunction = static_cast<uint32_t>(std::bit_cast<uintptr_t>(one) - Region::startOf(Region::Arena::Cells));
    }

    // Then the executables that are kept in full.
    for (auto* executable : inFull) {
        auto* copy = static_cast<FunctionExecutable*>(place(executable, sizeof(FunctionExecutable)));
        moved.add(executable, copy);
        copy->setUnlinkedExecutableWhileStaticHeapIsBuilt(keep(executable->unlinkedExecutable()));
    }
    size_t ofExecutablesInFull = kept.size();
    Vector<UnlinkedCodeBlock*> codeBlocks;
    for (auto arena : { Region::Arena::Cells, Region::Arena::MutableCells }) {
        forEachCell(arena, [&](void* pointer, size_t) {
            if (auto* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(static_cast<JSCell*>(pointer)))
                codeBlocks.append(codeBlock);
        });
    }
    for (auto* codeBlock : codeBlocks) {
        for (auto list : { codeBlock->functionDecls(), codeBlock->functionExprs() }) {
            for (auto& entry : list) {
                UnlinkedFunctionExecutable* unlinked = entry.get();
                if (!isScratch(std::bit_cast<uintptr_t>(unlinked)))
                    continue;
                FunctionExecutable* executable = unlinked->staticExecutable();
                reinterpret_cast<uintptr_t&>(const_cast<WriteBarrier<UnlinkedFunctionExecutable>&>(entry)) = executable ? std::bit_cast<uintptr_t>(moved.get(executable)) | UnlinkedCodeBlock::isExecutable : std::bit_cast<uintptr_t>(keep(unlinked));
            }
        }
    }
    for (auto& module : std::span { std::bit_cast<StaticHeapModule*>(header.modules), static_cast<size_t>(header.numberOfModules) }) {
        if (module.isBuiltinFunction && module.codeBlock)
            module.codeBlock = std::bit_cast<uint64_t>(keep(std::bit_cast<UnlinkedFunctionExecutable*>(module.codeBlock)));
    }
    size_t foundBy = kept.size() - ofExecutablesInFull;

    // Finally, update every reference to an executable to point to its copy.
    for (uint32_t at : functionMetadataOffsets) {
        if (!at || at & 1)
            continue;
        auto* metadata = inData<AOT::FunctionMetadata>(at);
        for (auto section : { AOT::FunctionMetadata::FunctionDecls, AOT::FunctionMetadata::FunctionExprs }) {
            const uint32_t* words = metadata->find(section);
            if (!words)
                continue;
            for (auto& entry : std::span { const_cast<uintptr_t*>(inMalloc<uintptr_t>(words[0])), static_cast<size_t>(words[1]) }) {
                if (!isScratch(entry))
                    continue;
                auto* unlinked = std::bit_cast<UnlinkedFunctionExecutable*>(entry);
                FunctionExecutable* executable = unlinked->staticExecutable();
                entry = executable ? std::bit_cast<uintptr_t>(moved.get(executable)) | UnlinkedCodeBlock::isExecutable : std::bit_cast<uintptr_t>(keep(unlinked));
            }
        }
    }
    for (auto& entry : kept) {
        if (FunctionExecutable* executable = entry.value->staticExecutable())
            entry.value->setStaticExecutable(moved.get(executable));
    }
    for (auto& info : std::span { std::bit_cast<AOT::FunctionInfo*>(header.infosOfFunctions), static_cast<size_t>(header.numberOfFunctions) }) {
        if (ScriptExecutable* executable = info.executable(); isScratch(std::bit_cast<uintptr_t>(executable)))
            info.setExecutable(moved.get(uncheckedDowncast<FunctionExecutable>(executable)), info.kind(), info.hasOnlyRealmIndependentConstants());
    }

    // What remains in Arena::Scratch is the functions nested in a function that has no compiled code. It has no executable, so they
    // are unreachable.
    // (ClosureChecker reports any other reference to Arena::Scratch.)
    size_t inFunctionsWithoutCode = 0;
    for (auto list : listsOfFunctionsInFunctions()) {
        for (auto& entry : list) {
            if (!isScratch(std::bit_cast<uintptr_t>(entry)))
                continue;
            const_cast<WriteBarrier<UnlinkedFunctionExecutable>&>(entry).clear();
            ++inFunctionsWithoutCode;
        }
    }
    s_rowsOfFunctions = rows.data();
    if (Options::verboseAOTCompilation()) [[unlikely]]
        dataLogLn("StaticHeap: of ", executablesInScratch().size(), " FunctionExecutables ", inShortForm.size(), " are in the short form, with ", shared.size(), " UnlinkedFunctionExecutables between them and ", rows.size_bytes(), " bytes of rows; ", inFull.size(), " are kept in full; ", foundBy, " more UnlinkedFunctionExecutables are kept for what finds a function by one; ", inFunctionsWithoutCode, " functions are in functions that there is no code for");
}

// SymbolTables are created in Arena::Scratch too (CachedSymbolTable::decode()). By now dropUnreferencedVariableNames() has removed
// the names that nothing looks up, if there is no bytecode, so most tables only record the number of variables and the kind of scope. Nothing writes to them
// or compares them by identity, so equal tables are merged. A reference to a table is any word that holds its address.
static void deduplicateSymbolTables()
{
    Region::AllocationScope notInRegion(false);
    UncheckedKeyHashMap<String, uintptr_t> kept;
    UncheckedKeyHashMap<uintptr_t, uintptr_t> moved;
    // (In address order, which is creation order, so that the output is deterministic.)
    Vector<uintptr_t> tables;
    for (void* cell : cellsInScratch()) {
        if (dynamicDowncast<SymbolTable>(static_cast<JSCell*>(cell)))
            tables.append(std::bit_cast<uintptr_t>(cell));
    }
    std::ranges::sort(tables);
    for (uintptr_t table : tables) {
        std::span bytes { std::bit_cast<const Latin1Character*>(table), sizeof(SymbolTable) };
        moved.add(table, kept.ensure(String { bytes }, [&] {
            void* cell = Region::allocate(Region::Arena::Cells, sizeof(SymbolTable), 16, StaticHeap::sizeOfCellHeader);
            memcpy(cell, bytes.data(), bytes.size());
            cellsBeingBuilt()[0].append({ cell, sizeof(SymbolTable) });
            return std::bit_cast<uintptr_t>(cell);
        }).iterator->value);
    }
    uintptr_t startOfScratch = Region::startOf(Region::Arena::Scratch);
    size_t usedOfScratch = Region::used(Region::Arena::Scratch);
    for (auto arena : { Region::Arena::Data, Region::Arena::Malloc, Region::Arena::Cells, Region::Arena::MutableCells, Region::Arena::MutableMalloc }) {
        for (auto& word : std::span { std::bit_cast<uintptr_t*>(Region::startOf(arena)), Region::used(arena) / sizeof(uintptr_t) }) {
            if (word - startOfScratch >= usedOfScratch)
                continue;
            if (auto found = moved.find(word); found != moved.end())
                word = found->value;
        }
    }
}

Vector<uint8_t> StaticHeap::build(VM& vm, std::span<const uint8_t> strings, std::span<const uint8_t> payload, std::span<const uint32_t> entryOffsetsOfModules, std::span<const uint8_t> imageOfCode, size_t keptPayloadStart, const PositionsToKeep* positionsToKeep, std::span<const ReportableSitesOfFunction> reportableSites, std::span<const std::optional<Vector<uint32_t>>> variablesExportedByModules)
{
    s_reportableSites = reportableSites;
    s_identifiersOfProgram = { };
    s_constantsOfProgram = { };
    auto forgetCells = makeScopeExit([] {
        for (auto& cells : cellsBeingBuilt())
            cells = { };
        s_allocatesFunctionsInScratch = false;
        s_nextCellIsOfAFunction = false;
        executablesInScratch() = { };
        cellsInScratch() = { };
        listsOfFunctionsInFunctions() = { };
        s_rowsOfFunctions = nullptr;
    });
    if (positionsToKeep)
        keptPayloadStart = payload.size();
    UncheckedKeyHashMap<CString, uint32_t> sources;
    Vector<CString> namesOfSources;
    s_positionsToKeep = positionsToKeep;
    s_sourcesBeingBuilt = &sources;
    s_namesOfSourcesBeingBuilt = &namesOfSources;
    s_bytesOfPositions = 0;
    s_numberOfPositions = 0;
    UncheckedKeyHashSet<SymbolTable*> processedSymbolTables;
    s_processedSymbolTables = &processedSymbolTables;
    ArrayDeduplicator arrayDeduplicator;
    s_arrayDeduplicator = nullptr;
    if (!Region::beginBuilding())
        return { };
    PreciseAllocation* containerBefore = PreciseAllocation::containerOfStaticCells();
    VM* vmOfContainerBefore = s_vmOfContainer;
    VM* vmBefore = s_vm;
    makeContainer(vm);
    s_vm = &vm;

    Header header { };
    header.magic = Header::expectedMagic;
    header.stamp = AOT::imageStamp();

    auto copy = [&](std::span<const uint8_t> bytes) {
        auto* result = static_cast<uint8_t*>(Region::allocate(Region::Arena::Data, bytes.size(), pageSizeOfImage));
        memcpy(result, bytes.data(), bytes.size());
        return std::span<uint8_t> { result, bytes.size() };
    };
    auto stringsCopy = copy(strings);
    header.strings = std::bit_cast<uint64_t>(stringsCopy.data());
    header.stringsSize = strings.size();

    bool ok = true;
    unsigned numberOfCodeBlocksFailed = 0;
    {
        DeferGC deferGC(vm);
        uint32_t count = *reinterpret_cast<const uint32_t*>(strings.data());
        auto* slots = static_cast<uintptr_t*>(Region::allocate(Region::Arena::Data, static_cast<size_t>(count) * sizeof(uintptr_t), pageSizeOfImage));
        header.stringSlots = std::bit_cast<uint64_t>(slots);
        DecoderStringTable table(stringsCopy, slots);

        // Every string is an atom in a table that holds nothing else, so at run time any equal string atomizes to the one in the
        // static heap.
        // (This table is only used while building. The table that is kept is created once all the atoms exist.)
        auto atoms = makeUnique<AtomStringTable>();
        atoms->table().reserveInitialCapacity(count);
        for (bool isPrivate : { false, true }) {
            s_symbolRegistriesBeingBuilt[isPrivate] = new (NotNull, Region::allocate(Region::Arena::Data, sizeof(SymbolRegistry), 16)) SymbolRegistry(isPrivate ? SymbolRegistry::Type::PrivateSymbol : SymbolRegistry::Type::PublicSymbol);
            header.symbolRegistries[isPrivate] = std::bit_cast<uint64_t>(s_symbolRegistriesBeingBuilt[isPrivate]);
        }
        AtomStringTable* usualAtoms = Thread::currentSingleton().setCurrentAtomStringTable(atoms.get());
        {
            Region::AllocationScope allocationScope;
            s_isBuilding = true;
            vm.heap.m_placeOfNextCell = placeOfEveryCellWhileBuilding;
            for (uint32_t ordinal = 0; ordinal < count; ++ordinal) {
                table.atomFor(vm, ordinal);
                // Create every JSString now, so that decoding at run time never has to create one, which would write to the slot.
                // If nothing is decoded at run time, the only JSStrings needed are the ones that decoding here asks for.
                if (!keptPayloadStart)
                    table.jsStringFor(vm, ordinal);
            }

            if (!payload.empty() && !entryOffsetsOfModules.empty()) {
                auto payloadCopy = copy(payload);
                header.payload = std::bit_cast<uint64_t>(payloadCopy.data());
                header.payloadSize = payload.size();
                Vector<uint32_t> sortedOffsets(entryOffsetsOfModules);
                std::ranges::sort(sortedOffsets);
                auto* modules = static_cast<StaticHeapModule*>(Region::allocate(Region::Arena::Data, sortedOffsets.size() * sizeof(StaticHeapModule), 16));
                header.modules = std::bit_cast<uint64_t>(modules);
                header.numberOfModules = sortedOffsets.size();
                auto imageView = AOT::ImageView::tryCreate(imageOfCode, reinterpret_cast<const void*>(Region::startOf(Region::Arena::Image)));
                std::span<AOT::FunctionInfo> infosOfFunctions;
                if (imageView) {
                    // (In a writable arena: entries that are left empty are filled in at run time if they are needed. See
                    // AOT::Data::create().)
                    size_t size = imageView->numberOfFunctions() * sizeof(AOT::FunctionInfo);
                    infosOfFunctions = { static_cast<AOT::FunctionInfo*>(Region::allocate(Region::Arena::MutableMalloc, size, pageSizeOfImage)), imageView->numberOfFunctions() };
                    memset(static_cast<void*>(infosOfFunctions.data()), 0, size);
                    header.infosOfFunctions = std::bit_cast<uint64_t>(infosOfFunctions.data());
                    s_arrayDeduplicator = &arrayDeduplicator;
                    if (uint32_t count = imageView->numberOfIdentifiersOfProgram()) {
                        RELEASE_ASSERT(reportableSites.size() == imageView->numberOfFunctions());
                        header.hasIdentifiersOfProgram = true;
                        if (uint32_t constants = imageView->numberOfConstantsOfProgram()) {
                            s_constantsOfProgram = { static_cast<EncodedJSValue*>(Region::allocate(Region::Arena::Data, constants * sizeof(EncodedJSValue), sizeof(EncodedJSValue))), constants };
                            zeroSpan(s_constantsOfProgram);
                            header.constantsOfProgram = std::bit_cast<uint64_t>(s_constantsOfProgram.data());
                        }
                        s_identifiersOfProgram = { static_cast<UniquedStringImpl**>(Region::allocate(Region::Arena::Data, count * sizeof(UniquedStringImpl*), sizeof(UniquedStringImpl*))), count };
                        zeroSpan(s_identifiersOfProgram);
                        header.identifiersOfProgram = std::bit_cast<uint64_t>(s_identifiersOfProgram.data());
                    }
                    s_functionMetadataOffsetsBeingBuilt = { static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, imageView->numberOfFunctions() * sizeof(uint32_t), pageSizeOfImage)), imageView->numberOfFunctions() };
                    zeroSpan(s_functionMetadataOffsetsBeingBuilt);
                    header.functionMetadataOffsets = std::bit_cast<uint64_t>(s_functionMetadataOffsetsBeingBuilt.data());
                    s_allocatesFunctionsInScratch = keptPayloadStart && positionsToKeep;
                    header.numberOfFunctions = infosOfFunctions.size();
                }
                Vector<StaticHeapTDZ> tdz;
                s_tdzBeingBuilt = &tdz;
                s_emptyStringBeingBuilt = nullptr;
                for (size_t i = 0; i < sortedOffsets.size(); ++i) {
                    s_moduleBeingBuilt = i;
                    s_entryOffsetOfModuleBeingBuilt = sortedOffsets[i];
                    RefPtr<CachedBytecode> cachedBytecode;
                    {
                        Region::AllocationScope notInRegion(false);
                        cachedBytecode = CachedBytecode::create(payloadCopy, [](const void*) { }, { });
                    }
                    cachedBytecode->setPayloadIsPersistent();
                    cachedBytecode->setEntryOffset(sortedOffsets[i]);
                    void* address = addressOfDecoder(i);
                    Decoder& decoder = Decoder::createForStaticHeap(address, vm, *cachedBytecode, nullptr);
                    decoder.setExternalStrings(table);
                    {
                        SourceCodeKey key;
                        Vector<std::pair<UnlinkedFunctionExecutable*, std::pair<int32_t, int32_t>>> functions;
                        auto forgetFunctions = makeScopeExit([&] {
                            Region::AllocationScope notInRegion(false);
                            functions = { };
                        });
                        bool isBuiltinFunction = entryIsOfBuiltinFunction(decoder);
                        UnlinkedFunctionExecutable* builtinFunction = nullptr;
                        unsigned lengthOfBuiltin = 0;
                        unsigned stampOfBuiltin = 0;
                        LineStartTable lineStarts;
                        auto setLineStarts = [&](const LineStarts& ofCode) {
                            Region::AllocationScope notInRegion(false);
                            // (A short builtin comes without. Its positions are all on its first line.)
                            lineStarts.setLineStarts(ofCode ? LineStarts { ofCode } : LineStartTable::encode(Vector<unsigned> { 0 }));
                        };
                        if (isBuiltinFunction) {
                            // Without compiled code for the builtin, there is no benefit to having it in the static heap.
                            LineStarts lineStartsOfBuiltin;
                            if (imageView)
                                builtinFunction = decodeBuiltinForStaticHeap(decoder, lengthOfBuiltin, stampOfBuiltin, lineStartsOfBuiltin, functions);
                            if (builtinFunction) {
                                setLineStarts(lineStartsOfBuiltin);
                                static_cast<SourceProvider*>(addressOfSourceProvider(i))->ref();
                                SourceCode source { RefPtr { static_cast<SourceProvider*>(addressOfSourceProvider(i)) }, 0, static_cast<int>(lengthOfBuiltin) };
                                makeExecutables(vm, nullptr, source, false, sortedOffsets[i] + 1, *imageView, infosOfFunctions, lineStarts, builtinFunction);
                            }
                        }
                        UnlinkedCodeBlock* codeBlock = isBuiltinFunction ? nullptr : decodeAllForStaticHeap(decoder, key, functions);
                        if (codeBlock && (!key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1))
                            codeBlock = nullptr;
                        if (isBuiltinFunction)
                            numberOfCodeBlocksFailed += !builtinFunction;
                        else if (!codeBlock)
                            numberOfCodeBlocksFailed++;
                        else if (imageView) {
                            // The SourceProvider has not been constructed yet, and nothing here reads it. Only its reference count
                            // changes, in memory that is not kept. The extra ref() keeps the count from reaching zero, which would
                            // destroy it, if the module has no functions.
                            static_cast<SourceProvider*>(addressOfSourceProvider(i))->ref();
                            SourceCode source { RefPtr { static_cast<SourceProvider*>(addressOfSourceProvider(i)) }, 0, static_cast<int>(key.length()) };
                            setLineStarts(uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock)->lineStarts());
                            makeExecutables(vm, codeBlock, source, false, sortedOffsets[i] + 1, *imageView, infosOfFunctions, lineStarts);
                            // The top-level code of the module. Its executable is created at run time.
                            if (auto function = imageView->find(AOT::imageKeyForTopLevelCode(sortedOffsets[i] + 1)))
                                fillInfo(infosOfFunctions[function->index], *function, codeBlock, nullptr, CodeSpecializationKind::CodeForCall, lineStarts);
                        }
                        if (codeBlock) {
                            // The code block stays, and gives its line starts to the module's provider at run time. They are borrowed
                            // from the payload, so it gets a copy. Without bytecode, every position comes from makePositions().
                            auto* global = uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock);
                            auto bytes = global->lineStarts().bytes;
                            if (bytes.empty() || Options::omitBytecodeFromStaticHeap())
                                global->setLineStarts({ });
                            else
                                global->setLineStarts({ std::span { deduplicatedCopy(bytes, 1), bytes.size() }, nullptr });
                        }
                        // (Code that can still be interpreted has to be linked first, which finds variables by name where running it
                        // does not: to watch them, and to check what the bytecode optimizer resolved.)
                        if (decoder.leavesFunctionCodeInPayload() && Options::omitBytecodeFromStaticHeap()) {
                            DynamicallyResolvedNames lookedUp;
                            if (codeBlock) {
                                const Vector<uint32_t>* exported = nullptr;
                                for (size_t index = 0; index < variablesExportedByModules.size() && index < entryOffsetsOfModules.size(); ++index) {
                                    if (entryOffsetsOfModules[index] == sortedOffsets[i] && variablesExportedByModules[index])
                                        exported = &*variablesExportedByModules[index];
                                }
                                dropUnreferencedVariableNames(codeBlock, lookedUp, exported);
                            }
                            else if (builtinFunction) {
                                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                                    if (auto* code = builtinFunction->codeBlockIfExists(kind))
                                        dropUnreferencedVariableNames(code, lookedUp);
                                }
                            }
                            Region::AllocationScope notInRegion(false);
                            lookedUp.names.clear();
                        }
                        if (decoder.leavesFunctionCodeInPayload()) {
                            for (auto& [function, offsets] : functions) {
                                for (auto kind : { CodeSpecializationKind::CodeForCall, CodeSpecializationKind::CodeForConstruct }) {
                                    if (auto* code = function->codeBlockIfExists(kind); code && s_allocatesFunctionsInScratch) {
                                        Region::AllocationScope notInRegion(false);
                                        listsOfFunctionsInFunctions().append(code->functionDecls());
                                        listsOfFunctionsInFunctions().append(code->functionExprs());
                                    }
                                    if (auto* code = function->codeBlockIfExists(kind))
                                        code->leaveToStaticHeap(code->numberOfUnlinkedStringSwitchJumpTables() || code->numberOfConstantIdentifierSets(), !!s_arrayDeduplicator);
                                }
                                function->leaveCodeInPayload(decoder, offsets);
                            }
                        }
                        if (isBuiltinFunction)
                            modules[i] = { sortedOffsets[i], stampOfBuiltin, lengthOfBuiltin, 0, std::bit_cast<uint64_t>(builtinFunction && builtinFunction->staticExecutable() ? builtinFunction : nullptr), true };
                        else
                            modules[i] = { sortedOffsets[i], key.hash(), static_cast<uint32_t>(key.length()), key.flagsBits(), std::bit_cast<uint64_t>(codeBlock), false };
                    }
                    // (The Decoder is not destroyed, because objects refer to it. It holds one reference to the CachedBytecode,
                    // which is released here.)
                    decoder.clearDecodedObjects();
                    cachedBytecode->deref();
                    memset(address, 0, sizeof(Decoder));
                }
                s_tdzBeingBuilt = nullptr;
                if (imageView && imageView->keysAreOmitted()) {
                    // An executable created here records the index of its function. The other executables are created at run time
                    // and find their functions by key, so only those keys are kept.
                    auto isLookedUp = [&](const AOT::ImageKey& key) {
                        return key.record && key.kind != std::numeric_limits<uint32_t>::max() && !infosOfFunctions[imageView->indexOfFunctionWith(key)].executable();
                    };
                    size_t count = 0;
                    for (auto& key : imageView->keys())
                        count += isLookedUp(key);
                    size_t capacity = 16;
                    while (capacity * 3 < count * 4)
                        capacity *= 2;
                    std::span<AOT::ImageKey> kept { static_cast<AOT::ImageKey*>(Region::allocate(Region::Arena::Data, capacity * sizeof(AOT::ImageKey), alignof(AOT::ImageKey))), capacity };
                    zeroSpan(kept);
                    for (auto& key : imageView->keys()) {
                        if (!isLookedUp(key))
                            continue;
                        size_t bucket = key.hash() & (capacity - 1);
                        while (kept[bucket].record)
                            bucket = (bucket + 1) & (capacity - 1);
                        kept[bucket] = key;
                    }
                    header.keysOfImage = std::bit_cast<uint64_t>(kept.data());
                    header.capacityOfKeysOfImage = capacity;
                }
                if (positionsToKeep && !s_functionMetadataOffsetsBeingBuilt.empty()) {
                    size_t sizeOfText = 0;
                    for (auto& name : namesOfSources)
                        sizeOfText += name.length();
                    auto* starts = static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, (namesOfSources.size() + 1) * sizeof(uint32_t) + sizeOfText, alignof(uint32_t)));
                    auto* text = reinterpret_cast<char*>(starts + namesOfSources.size() + 1);
                    uint32_t at = 0;
                    for (size_t i = 0; i < namesOfSources.size(); ++i) {
                        starts[i] = at;
                        memcpy(text + at, namesOfSources[i].data(), namesOfSources[i].length());
                        at += namesOfSources[i].length();
                    }
                    starts[namesOfSources.size()] = at;
                    header.namesOfSources = std::bit_cast<uint64_t>(starts);
                    header.numberOfSources = namesOfSources.size();
                    header.hasPositionsOfCallSites = true;
                    if (Options::verboseAOTCompilation()) [[unlikely]]
                        dataLogLn("StaticHeap: ", s_numberOfPositions, " positions of call sites: ", s_bytesOfPositions, " bytes, in ", namesOfSources.size(), " sources whose names take ", sizeOfText);
                }
                s_positionsToKeep = nullptr;
                {
                    Region::AllocationScope notInRegion(false);
                    sources.clear();
                    namesOfSources.clear();
                    processedSymbolTables.clear();
                    arrayDeduplicator.clear();
                    s_arrayDeduplicator = nullptr;
                }
                s_functionMetadataOffsetsBeingBuilt = { };
                // (See parentScopeTDZVariablesOf().)
                if (keptPayloadStart) {
                    Region::AllocationScope notInRegion(false);
                    tdz = { };
                }
                std::ranges::sort(tdz, { }, &StaticHeapTDZ::executable);
                header.tdz = std::bit_cast<uint64_t>(copy(asByteSpan(tdz.span())).data());
                header.numberOfTDZ = tdz.size();
                {
                    Region::AllocationScope notInRegion(false);
                    tdz = { };
                }
            }
            s_isBuilding = false;
            vm.heap.m_placeOfNextCell = nullptr;
        }
        Thread::currentSingleton().setCurrentAtomStringTable(usualAtoms);
        {
            using StaticAtoms = AtomStringTable::StaticAtoms;
            // (At most half full. The table it is built from reserved far more capacity than it needs.)
            size_t capacity = WTF::roundUpToPowerOfTwo(std::max<size_t>(2 * atoms->table().size(), 8));
            RELEASE_ASSERT(hasOneBitSet(capacity) && capacity > atoms->table().size());
            std::span<uint32_t> entries { static_cast<uint32_t*>(Region::allocate(Region::Arena::Data, capacity * sizeof(uint32_t), pageSizeOfImage)), capacity };
            zeroSpan(entries);
            unsigned mask = capacity - 1;
            for (auto& atom : atoms->table()) {
                atom->becomeStatic();
                uintptr_t distance = std::bit_cast<uintptr_t>(atom.get()) - Region::base;
                RELEASE_ASSERT(contains(atom.get()) && distance && !(distance & ((1u << StaticAtoms::shift) - 1)) && !(distance >> StaticAtoms::shift >> 32));
                unsigned probes = 0;
                unsigned place = atom->hash() & mask;
                while (entries[place])
                    place = StaticAtoms::next(place, probes, mask);
                entries[place] = static_cast<uint32_t>(distance >> StaticAtoms::shift);
            }
            header.staticAtoms = std::bit_cast<uint64_t>(entries.data());
            header.capacityOfStaticAtoms = capacity;
        }
        for (auto* registry : s_symbolRegistriesBeingBuilt)
            registry->becomeStatic();
    }

    if (s_allocatesFunctionsInScratch) {
        retainNeededFunctionData(vm, header);
        deduplicateSymbolTables();
    }

    // The collector never visits the static heap, so nothing in it may hold the only reference to an object outside it.
    {
        ClosureChecker checker(vm);
        for (auto arena : { Region::Arena::Cells, Region::Arena::MutableCells }) {
            forEachCell(arena, [&](void* pointer, size_t) {
                checker.current = static_cast<JSCell*>(pointer);
                checker.current->methodTable()->visitChildren(checker.current, checker);
            });
        }
        if (checker.numberOfEscapes) {
            dataLogLn("StaticHeap: ", checker.numberOfEscapes, " references out of it");
            ok = false;
        }
    }

    // Give the cells the state they would have in the VM that is going to use them, after a collection that marked them.
    auto structures = structuresOf(vm);
    uint32_t blockOfStructures = vm.structureStructure->id().bits() & ~static_cast<uint32_t>(MarkedBlock::blockSize - 1);
    UncheckedKeyHashMap<uint32_t, uint32_t> idInFirstVM;
    for (auto arena : { Region::Arena::Cells, Region::Arena::MutableCells }) {
        forEachCell(arena, [&](void* pointer, size_t size) {
            auto* cell = static_cast<JSCell*>(pointer);
            uint32_t id = cell->structureID().bits();
            uint32_t translated = idInFirstVM.ensure(id, [&]() -> uint32_t {
                if ((id & ~static_cast<uint32_t>(MarkedBlock::blockSize - 1)) != blockOfStructures || header.numberOfStructures == Header::maxStructures)
                    return 0;
                for (size_t i = 0; i < structures.size(); ++i) {
                    if (structures[i] && structures[i]->id().bits() == id) {
                        uint32_t result = offsetOfFirstStructureBlock + (id & (MarkedBlock::blockSize - 1));
                        header.structures[header.numberOfStructures++] = { static_cast<uint32_t>(i), result };
                        return result;
                    }
                }
                return 0;
            }).iterator->value;
            if (!translated) {
                if (ok)
                    dataLogLn("StaticHeap: a ", cell->classInfo()->className, " has a structure that is not one of the VM's own");
                ok = false;
                return;
            }
            *reinterpret_cast<uint32_t*>(cell) = translated;
            cell->setCellState(CellState::PossiblyBlack);
        });
    }

    if (keptPayloadStart && header.payload && header.functionMetadataOffsets) {
        uint64_t start = roundUpToMultipleOf<pageSizeOfImage>(header.payload);
        uint64_t end = (header.payload + keptPayloadStart) & ~static_cast<uint64_t>(pageSizeOfImage - 1);
        if (end > start) {
            header.holeInData = start - Region::startOf(Region::Arena::Data);
            header.sizeOfHoleInData = end - start;
        }
    }

    // Compute now what a cell would otherwise compute lazily and cache, because these cells are read-only at run time.
    forEachCell(Region::Arena::Cells, [&](void* pointer, size_t) {
        if (auto* bigInt = dynamicDowncast<JSBigInt>(static_cast<JSCell*>(pointer)))
            bigInt->hash();
    });

    Region::clearFreeLists();
    Vector<uint8_t> image;
    if (ok) {
        size_t size = pageSizeOfImage;
        for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i) {
            header.arenaOffset[i] = size;
            header.arenaSize[i] = roundUpToMultipleOf<pageSizeOfImage>(Region::used(static_cast<Region::Arena>(i)));
            size += header.arenaSize[i];
            if (static_cast<Region::Arena>(i) == Region::Arena::Data)
                size -= header.sizeOfHoleInData;
        }
        header.size = size;
        image.fill(0, size);
        memcpy(image.mutableSpan().data(), &header, sizeof(header));
        if (header.sizeOfHoleInData) {
            constexpr unsigned data = static_cast<unsigned>(Region::Arena::Data);
            auto* from = reinterpret_cast<const uint8_t*>(Region::startOf(Region::Arena::Data));
            uint8_t* to = image.mutableSpan().data() + header.arenaOffset[data];
            size_t afterHole = header.holeInData + header.sizeOfHoleInData;
            memcpy(to, from, header.holeInData);
            memcpy(to + header.holeInData, from + afterHole, Region::used(Region::Arena::Data) - afterHole);
        }
        for (unsigned i = header.sizeOfHoleInData ? 1 : 0; i < Region::numberOfArenasInFile; ++i)
            memcpy(image.mutableSpan().data() + header.arenaOffset[i], reinterpret_cast<void*>(Region::startOf(static_cast<Region::Arena>(i))), Region::used(static_cast<Region::Arena>(i)));
    }

    PreciseAllocation::setContainerOfStaticCells(containerBefore);
    s_vmOfContainer = vmOfContainerBefore;
    s_vm = vmBefore;
    Region::endBuilding();
    return image;
}

std::optional<StaticHeap::Copies> StaticHeap::copiesIn(std::span<const uint8_t> image)
{
    if (image.size() < sizeof(Header))
        return std::nullopt;
    auto& header = *reinterpret_cast<const Header*>(image.data());
    if (header.magic != Header::expectedMagic || header.size > image.size())
        return std::nullopt;
    constexpr unsigned data = static_cast<unsigned>(Region::Arena::Data);
    auto offsetOf = [&](uint64_t address, uint64_t size) -> std::optional<size_t> {
        uint64_t inArena = address - Region::startOf(Region::Arena::Data);
        if (!address || inArena > header.arenaSize[data] || size > header.arenaSize[data] - inArena)
            return std::nullopt;
        if (inArena + size <= header.holeInData || !header.sizeOfHoleInData)
            return header.arenaOffset[data] + inArena;
        if (inArena >= header.holeInData + header.sizeOfHoleInData)
            return header.arenaOffset[data] + inArena - header.sizeOfHoleInData;
        return std::nullopt;
    };
    auto strings = offsetOf(header.strings, header.stringsSize);
    auto payload = offsetOf(header.payload, header.payloadSize);
    if (!strings || (!payload && !header.sizeOfHoleInData))
        return std::nullopt;
    return Copies { *strings, static_cast<size_t>(header.stringsSize), payload.value_or(0), static_cast<size_t>(header.payloadSize), !payload, static_cast<uintptr_t>(header.payload), !!header.hasPositionsOfCallSites };
}

bool StaticHeap::map(std::span<const uint8_t> image, int fileDescriptor, int64_t offsetInFile)
{
    if (s_header || image.size() < sizeof(Header))
        return false;
    auto& header = *reinterpret_cast<const Header*>(image.data());
    if (header.magic != Header::expectedMagic || header.stamp != AOT::imageStamp() || header.size > image.size())
        return false;
    // If the thread already has atoms, one of them could be equal to an atom in the static heap, and equal atoms must be the same
    // object.
    AtomStringTable* atoms = Thread::currentSingleton().atomStringTable();
    if (!atoms->table().isEmpty())
        return false;
    if (!Region::mapRestOfBss())
        return false;
    for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i) {
        uint64_t sizeInFile = header.arenaSize[i] - (static_cast<Region::Arena>(i) == Region::Arena::Data ? header.sizeOfHoleInData : 0);
        if (header.arenaOffset[i] % pageSizeOfImage || header.arenaOffset[i] > header.size || sizeInFile > header.size - header.arenaOffset[i])
            return false;
    }
    if (header.holeInData % pageSizeOfImage || header.sizeOfHoleInData % pageSizeOfImage || header.holeInData + header.sizeOfHoleInData > header.arenaSize[static_cast<unsigned>(Region::Arena::Data)])
        return false;
    for (unsigned i = 0; i < Region::numberOfArenasInFile; ++i) {
        // (If a later mapping fails, the arenas that are already mapped stay mapped. Nothing uses them, because s_header is not
        // set.)
        if (static_cast<Region::Arena>(i) == Region::Arena::Data && header.sizeOfHoleInData) {
            uint64_t afterHole = header.holeInData + header.sizeOfHoleInData;
            if (!Region::map(Region::Arena::Data, fileDescriptor, offsetInFile + header.arenaOffset[i], header.holeInData)
                || !Region::map(Region::Arena::Data, fileDescriptor, offsetInFile + header.arenaOffset[i] + header.holeInData, header.arenaSize[i] - afterHole, afterHole))
                return false;
            continue;
        }
        if (!Region::map(static_cast<Region::Arena>(i), fileDescriptor, offsetInFile + header.arenaOffset[i], header.arenaSize[i]))
            return false;
    }
    for (uint64_t at = header.guardedFrom; at < header.guardedTo; at += 2 * pageSizeOfImage)
        RELEASE_ASSERT(!mprotect(reinterpret_cast<void*>(Region::startOf(Region::Arena::Cells) + at + pageSizeOfImage), pageSizeOfImage, PROT_NONE));
    s_header = &header;
    s_rowsOfFunctions = std::bit_cast<const RowOfFunction*>(header.rowsOfFunctions);
    atoms->setStaticAtoms({ std::bit_cast<const uint32_t*>(header.staticAtoms), static_cast<uint32_t>(header.capacityOfStaticAtoms - 1), Region::base });
    return true;
}

void StaticHeap::prepareThread()
{
    if (!s_header || !s_vm || t_threadIsPrepared)
        return;
    AtomStringTable* atoms = Thread::currentSingleton().atomStringTable();
    if (!atoms->table().isEmpty())
        return;
    atoms->setStaticAtoms({ std::bit_cast<const uint32_t*>(s_header->staticAtoms), static_cast<uint32_t>(s_header->capacityOfStaticAtoms - 1), Region::base });
    t_threadIsPrepared = true;
}

void StaticHeap::willDestroy(VM& vm)
{
    auto* ofVM = JSC::ofVM(vm);
    if (!ofVM)
        return;
    if (t_ofVMOfThread == ofVM)
        t_ofVMOfThread = nullptr;
    vm.m_staticHeapOfVM = nullptr;
    // (The container is not freed, because a cell may still be asked for its container.)
    delete ofVM;
}

void StaticHeap::install(VM& vm)
{
    if (s_header && s_vm && s_vm != &vm && !vm.m_staticHeapOfVM) {
        // Objects whose locks are skipped (needsNoLocking()) must not be read by compiler threads, of any VM.
        if (!t_threadIsPrepared || !s_hasNoCompilerThreads || t_ofVMOfThread)
            return;
        static std::atomic<uint64_t> lastNumber { 1 };
        auto* ofVM = new StaticHeapOfVM;
        ofVM->number = ++lastNumber;
        ofVM->container = PreciseAllocation::createForStaticCells(vm.heap, &vm.heap.cellSpace);
        ofVM->topLevelExecutables.fill(nullptr, s_header->numberOfModules);
        vm.symbolRegistry().setStaticRegistry(std::bit_cast<const SymbolRegistry*>(s_header->symbolRegistries[0]));
        vm.privateSymbolRegistry().setStaticRegistry(std::bit_cast<const SymbolRegistry*>(s_header->symbolRegistries[1]));
        vm.m_staticHeapOfVM = ofVM;
        t_ofVMOfThread = ofVM;
        WTF::storeStoreFence();
        s_isShared = true;
        return;
    }
    if (!s_header || s_vm || &vm != addressOfVM())
        return;
    // If a structure does not match, the strings can still be used as strings, but the cells are never used.
    auto structures = structuresOf(vm);
    for (unsigned i = 0; i < s_header->numberOfStructures; ++i) {
        auto& expected = s_header->structures[i];
        if (expected.indexInVM >= structures.size() || !structures[expected.indexInVM] || structures[expected.indexInVM]->id().bits() != expected.id)
            return;
    }
    s_hasNoCompilerThreads = !Options::useJIT();
    vm.symbolRegistry().setStaticRegistry(std::bit_cast<const SymbolRegistry*>(s_header->symbolRegistries[0]));
    vm.privateSymbolRegistry().setStaticRegistry(std::bit_cast<const SymbolRegistry*>(s_header->symbolRegistries[1]));
    makeContainer(vm);
    s_vm = &vm;
}

std::unique_ptr<DecoderStringTable> StaticHeap::tryCreateStringTable(VM& vm, std::span<const uint8_t> strings)
{
    if (!s_header || !isUsedBy(vm) || strings.size() != s_header->stringsSize)
        return nullptr;
    auto* copy = std::bit_cast<const uint8_t*>(s_header->strings);
    // Both come from the same file. Comparing a sample guards against mistakes, not against tampering.
    size_t sample = std::min<size_t>(strings.size(), 4 * KB);
    if (memcmp(copy, strings.data(), sample) || memcmp(copy + strings.size() - sample, strings.data() + strings.size() - sample, sample))
        return nullptr;
    return makeUnique<DecoderStringTable>(std::span<const uint8_t> { copy, strings.size() }, std::bit_cast<uintptr_t*>(s_header->stringSlots));
}

RefPtr<TDZEnvironmentLink> StaticHeap::parentScopeTDZVariablesOf(const UnlinkedFunctionExecutable& executable)
{
    std::span<const StaticHeapTDZ> all { std::bit_cast<const StaticHeapTDZ*>(s_header->tdz), static_cast<size_t>(s_header->numberOfTDZ) };
    auto found = std::ranges::lower_bound(all, std::bit_cast<uint64_t>(&executable), { }, &StaticHeapTDZ::executable);
    if (found == all.end() || found->executable != std::bit_cast<uint64_t>(&executable))
        return nullptr;
    // (They are only needed to compile the functions nested in this one, and without the payload there is nothing to compile them
    // from.)
    if (payloadIsOmitted())
        return nullptr;
    // (The Decoder exists, because the executable came from a code block that codeFor() returned.)
    // The decoded result belongs to a VM. The Decoder at that address belongs to whichever VM created it first, which may have been
    // destroyed. It is only used to identify the module.
    auto& placed = *static_cast<Decoder*>(addressOfDecoder(found->moduleIndex));
    return decodeParentScopeTDZVariablesForStaticHeap(decoderForKeptPayload(executable.vm(), placed).get(), std::bit_cast<const void*>(found->record));
}

bool StaticHeap::hasIdentifiersOfProgram()
{
    return s_header && s_header->hasIdentifiersOfProgram;
}

bool StaticHeap::hasPositionsOfCallSites()
{
    return s_header && s_header->hasPositionsOfCallSites;
}

String StaticHeap::nameOfSource(uint32_t source)
{
    RELEASE_ASSERT(s_header && source && source <= s_header->numberOfSources);
    auto* starts = std::bit_cast<const uint32_t*>(s_header->namesOfSources);
    auto* text = reinterpret_cast<const char8_t*>(starts + s_header->numberOfSources + 1);
    return String::fromUTF8(std::span { text + starts[source - 1], static_cast<size_t>(starts[source] - starts[source - 1]) });
}

// Reads the first two values that makePositions() wrote.
LineColumn StaticHeap::whereFunctionStarts(uint32_t indexOfFunction)
{
    RELEASE_ASSERT(s_header && s_header->hasPositionsOfCallSites && indexOfFunction < s_header->numberOfFunctions);
    uint32_t word = std::bit_cast<const uint32_t*>(s_header->functionMetadataOffsets)[indexOfFunction];
    RELEASE_ASSERT(word && !(word & 1));
    const uint32_t* where = inData<AOT::FunctionMetadata>(word)->find(AOT::FunctionMetadata::ExpressionInfo);
    RELEASE_ASSERT(where);
    const uint8_t* at = inData<uint8_t>(*where);
    auto readVarint = [&] {
        unsigned value = 0;
        for (unsigned shift = 0;; shift += 7) {
            uint8_t byte = *at++;
            value |= static_cast<unsigned>(byte & 0x7f) << shift;
            if (!(byte & 0x80))
                return value;
        }
    };
    unsigned line = readVarint();
    return { line, readVarint() };
}

bool StaticHeap::payloadIsOmitted()
{
    return s_header && s_header->sizeOfHoleInData;
}

std::span<const uint8_t> StaticHeap::omittedPayload()
{
    RELEASE_ASSERT(payloadIsOmitted());
    return { std::bit_cast<const uint8_t*>(s_header->payload), static_cast<size_t>(s_header->payloadSize) };
}

UnlinkedCodeBlock* StaticHeap::codeFor(VM& vm, const SourceCodeKey& key, const CachedBytecode& cachedBytecode)
{
    ASSERT(isUsedBy(vm));
    if (!s_header->numberOfModules || !cachedBytecode.payloadIsPersistent() || cachedBytecode.size() < s_header->payloadSize)
        return nullptr;
    if (payloadIsOmitted() && cachedBytecode.span().data() != omittedPayload().data())
        return nullptr;
    // A bytecode order recording records what is read from the payload, so the payload has to be decoded.
    if (BytecodeOrderRecorder::ofVM(vm)) [[unlikely]]
        return nullptr;
    std::span<uint8_t> payload { std::bit_cast<uint8_t*>(s_header->payload), static_cast<size_t>(s_header->payloadSize) };
    static const uint8_t* s_samePayload = nullptr;
    if (cachedBytecode.span().data() != s_samePayload && cachedBytecode.span().data() != payload.data()) {
        size_t sample = std::min<size_t>(payload.size(), 4 * KB);
        if (memcmp(payload.data(), cachedBytecode.span().data(), sample))
            return nullptr;
        s_samePayload = cachedBytecode.span().data();
    }
    std::span<const StaticHeapModule> modules { std::bit_cast<const StaticHeapModule*>(s_header->modules), static_cast<size_t>(s_header->numberOfModules) };
    uint32_t entryOffset = static_cast<uint32_t>(cachedBytecode.entryOffset());
    size_t index = std::ranges::lower_bound(modules, entryOffset, { }, &StaticHeapModule::entryOffset) - modules.begin();
    if (index == modules.size() || modules[index].entryOffset != entryOffset || !modules[index].codeBlock || modules[index].isBuiltinFunction)
        return nullptr;
    auto* module = &modules[index];
    if (key.hash() != module->keyHash || key.length() != module->keyLength || key.flagsBits() != module->keyFlags || !key.name().isEmpty() || key.functionConstructorParametersEndPosition() != -1)
        return nullptr;

    // Objects in the static heap refer to the module's SourceProvider by its fixed address. A module that is loaded with another
    // provider is treated as a different module, and gets its own code.
    if (&key.source().provider() != addressOfSourceProvider(index))
        return nullptr;

    ensureDecoder(vm, index, key.source().provider());
    return std::bit_cast<UnlinkedCodeBlock*>(module->codeBlock);
}

// Objects in the static heap refer to the module's Decoder, which identifies the module they belong to.
void StaticHeap::ensureDecoder(VM& vm, size_t index, SourceProvider& provider)
{
    static NeverDestroyed<BitVector> s_hasDecoder;
    static Lock s_decodersLock;
    Locker locker { s_decodersLock };
    if (s_hasDecoder->get(index))
        return;
    Ref ownBytecode = CachedBytecode::create(std::span<uint8_t> { std::bit_cast<uint8_t*>(s_header->payload), static_cast<size_t>(s_header->payloadSize) }, [](const void*) { }, { });
    ownBytecode->setPayloadIsPersistent();
    ownBytecode->setEntryOffset(std::bit_cast<const StaticHeapModule*>(s_header->modules)[index].entryOffset);
    Decoder::createForStaticHeap(addressOfDecoder(index), vm, WTF::move(ownBytecode), &provider);
    s_hasDecoder->set(index);
}

class PlacedStringSourceProvider final : public StringSourceProvider {
public:
    PlacedStringSourceProvider(const String& source, const SourceOrigin& sourceOrigin, String&& sourceURL)
        : StringSourceProvider(source, sourceOrigin, SourceTaintedOrigin::Untainted, WTF::move(sourceURL), TextPosition(), SourceProviderSourceType::Program)
    {
    }
};

FunctionExecutable* StaticHeap::engineBuiltinFor(JSGlobalObject* globalObject, unsigned index, std::span<const Latin1Character> text)
{
    VM& vm = globalObject->vm();
    // The program runs in the VM's first realm. (AOT::Instance::ensure() checks that.)
    if (globalObject != vm.m_firstRealm || !hasExecutablesOfFunctions(vm) || BytecodeOrderRecorder::ofVM(vm))
        return nullptr;
    // For each builtin, one more than its entry offset in the payload. Zero if it is not in the static heap.
    static NeverDestroyed<Vector<uint32_t>> entries;
    static std::once_flag once;
    std::call_once(once, [] {
        std::span<const StaticHeapModule> modules { std::bit_cast<const StaticHeapModule*>(s_header->modules), static_cast<size_t>(s_header->numberOfModules) };
        for (auto& module : modules) {
            if (!module.isBuiltinFunction || !module.codeBlock || !BuiltinExecutables::isStamp(module.keyHash))
                continue;
            unsigned which = module.keyHash & 0xffff;
            while (entries->size() <= which)
                entries->append(0);
            entries.get()[which] = module.entryOffset + 1;
        }
    });
    if (index >= entries->size() || !entries.get()[index])
        return nullptr;
    // (Some are requested more than once: a builtin that is exposed under two names becomes two function objects.)
    if (vm.m_builtinsOfStaticHeap.isEmpty())
        vm.m_builtinsOfStaticHeap.fill(nullptr, entries->size());
    if (FunctionExecutable* given = vm.m_builtinsOfStaticHeap[index])
        return given;
    s_realmIsProgramRealm = true;
    FunctionExecutable* result = builtinFunctionFor(globalObject, entries.get()[index] - 1, BuiltinExecutables::stampOf(index), StringImpl::createWithoutCopying(text), SourceOrigin(), String());
    s_realmIsProgramRealm = false;
    if (result)
        vm.m_firstRealmHasBuiltinsOfStaticHeap = true;
    vm.m_builtinsOfStaticHeap[index] = result;
    return result;
}

bool StaticHeap::ensureSourceProviderOf(VM& vm, ScriptExecutable* executable)
{
    if (!s_header || !contains(executable) || executable->isShortForm())
        return true;
    uintptr_t place = std::bit_cast<uintptr_t>(executable->source().provider());
    uintptr_t first = std::bit_cast<uintptr_t>(addressOfSourceProvider(0));
    size_t index = (place - first) / sizeOfPlaceForSourceProvider;
    if (place < first || index >= s_header->numberOfModules)
        return true;
    auto& isMade = sourceProviderSlotStates(s_header->numberOfModules)[index].isMade;
    if (isMade.load(std::memory_order_acquire))
        return true;
    auto& module = std::bit_cast<const StaticHeapModule*>(s_header->modules)[index];
    // (It allocates.)
    if (module.isBuiltinFunction && BuiltinExecutables::isStamp(module.keyHash) && vm.m_aotInstanceOfProgram && vm.heap.mutatorState() == MutatorState::Running) {
        unsigned which = module.keyHash & 0xffff;
        engineBuiltinFor(vm.m_aotInstanceOfProgram->globalObject, which, BuiltinExecutables::textOf(which));
    }
    return isMade.load(std::memory_order_acquire);
}

FunctionExecutable* StaticHeap::builtinFunctionFor(JSGlobalObject* globalObject, uint32_t entryOffset, unsigned embedderStamp, const String& text, const SourceOrigin& sourceOrigin, const String& sourceURL)
{
    VM& vm = globalObject->vm();
    if (!hasExecutablesOfFunctions(vm) || BytecodeOrderRecorder::ofVM(vm))
        return nullptr;
    std::span<const StaticHeapModule> modules { std::bit_cast<const StaticHeapModule*>(s_header->modules), static_cast<size_t>(s_header->numberOfModules) };
    size_t index = std::ranges::lower_bound(modules, entryOffset, { }, &StaticHeapModule::entryOffset) - modules.begin();
    if (index == modules.size() || modules[index].entryOffset != entryOffset || !modules[index].codeBlock || !modules[index].isBuiltinFunction)
        return nullptr;
    if (modules[index].keyHash != embedderStamp || modules[index].keyLength != text.length())
        return nullptr;
    if (!s_realmIsProgramRealm && &AOT::Instance::ensure(globalObject) != vm.m_aotInstanceOfProgram)
        return nullptr;

    SourceProvider* provider = nullptr;
    if (void* place = takePlaceForSourceProvider(vm, entryOffset, sizeof(PlacedStringSourceProvider), provider)) {
        provider = new (NotNull, place) PlacedStringSourceProvider(text, sourceOrigin, String { sourceURL });
        provider->setAOTModuleID(entryOffset + 1);
        provider->becomeShareableBetweenThreads();
        didMakeSourceProvider(place);
    }
    if (!provider)
        return nullptr;
    ensureDecoder(vm, index, *provider);

    FunctionExecutable* executable = std::bit_cast<UnlinkedFunctionExecutable*>(modules[index].codeBlock)->staticExecutable();
    // The functions nested in the builtin need a top-level executable, which is written to, so it is a stand-in in the ordinary
    // heap.
    if (auto*& slot = topLevelExecutableOfModuleWithProvider(vm, provider); !slot) {
        slot = standInFor(vm, executable);
        slot->setUsesStaticExecutables();
    }
    return executable;
}

} // namespace JSC

#else // ENABLE(AOT)

#include "CachedTypes.h"
#include "FunctionExecutable.h"
#include "VariableEnvironment.h"
#include <wtf/text/SymbolRegistry.h>

// There is no static heap: nothing is in it, and nothing can be built or mapped.

namespace JSC {

bool StaticHeap::s_isBuilding = false;
const StaticHeap::RowOfFunction* StaticHeap::s_rowsOfFunctions = nullptr;
VM* StaticHeap::s_vm = nullptr;
bool StaticHeap::s_isShared = false;
bool StaticHeap::s_hasNoCompilerThreads = false;
const StaticHeap::Header* StaticHeap::s_header = nullptr;

Vector<uint8_t> StaticHeap::build(VM&, std::span<const uint8_t>, std::span<const uint8_t>, std::span<const uint32_t>, std::span<const uint8_t>, size_t, const PositionsToKeep*, std::span<const ReportableSitesOfFunction>, std::span<const std::optional<Vector<uint32_t>>>) { return { }; }
JSString* StaticHeap::emptyStringWhileBuilding(VM&) { RELEASE_ASSERT_NOT_REACHED(); }
WTF::SymbolRegistry& StaticHeap::symbolRegistryWhileBuilding(bool) { RELEASE_ASSERT_NOT_REACHED(); }
void StaticHeap::noteParentScopeTDZVariables(const UnlinkedFunctionExecutable&, const void*) { }
bool StaticHeap::map(std::span<const uint8_t>, int, int64_t) { return false; }
std::optional<StaticHeap::Copies> StaticHeap::copiesIn(std::span<const uint8_t>) { return std::nullopt; }
void StaticHeap::prepareThread() { }
void StaticHeap::install(VM&) { }
void StaticHeap::willDestroy(VM&) { }
bool StaticHeap::isUsedBy(VM&) { return false; }
PreciseAllocation* StaticHeap::containerOfSlow(const void*) { RELEASE_ASSERT_NOT_REACHED(); }
std::unique_ptr<DecoderStringTable> StaticHeap::tryCreateStringTable(VM&, std::span<const uint8_t>) { return nullptr; }
UnlinkedCodeBlock* StaticHeap::codeFor(VM&, const SourceCodeKey&, const CachedBytecode&) { return nullptr; }
bool StaticHeap::payloadIsOmitted() { return false; }
bool StaticHeap::hasPositionsOfCallSites() { return false; }
bool StaticHeap::hasIdentifiersOfProgram() { return false; }
WTF::UniquedStringImpl* const* StaticHeap::identifiersOfProgram() { return nullptr; }
String StaticHeap::nameOfSource(uint32_t) { return { }; }
std::span<const uint8_t> StaticHeap::omittedPayload() { return { }; }
FunctionExecutable* StaticHeap::builtinFunctionFor(JSGlobalObject*, uint32_t, unsigned, const String&, const SourceOrigin&, const String&) { return nullptr; }
FunctionExecutable* StaticHeap::engineBuiltinFor(JSGlobalObject*, unsigned, std::span<const Latin1Character>) { return nullptr; }
bool StaticHeap::ensureSourceProviderOf(VM&, ScriptExecutable*) { return true; }
RefPtr<TDZEnvironmentLink> StaticHeap::parentScopeTDZVariablesOf(const UnlinkedFunctionExecutable&) { return nullptr; }
LineColumn StaticHeap::whereFunctionStarts(uint32_t) { return { }; }
bool StaticHeap::keepsNothingForGeneratingCode() { return false; }
void* StaticHeap::tryAllocateCellSlow(VM&, size_t) { return nullptr; }
void StaticHeap::placeNextCell(VM&, void*) { RELEASE_ASSERT_NOT_REACHED(); }
void StaticHeap::didPlaceCell(VM&, JSCell*) { }
void* StaticHeap::takePlaceForSourceProvider(VM&, size_t, size_t, SourceProvider*& made) { made = nullptr; return nullptr; }
void StaticHeap::didMakeSourceProvider(void*) { }
std::pair<FunctionExecutable*, CodeSpecializationKind> StaticHeap::executableOfFunction(uint32_t) { return { nullptr, CodeSpecializationKind::CodeForCall }; }
bool StaticHeap::hasExecutablesOfFunctions(VM&) { return false; }
AOT::FunctionInfo* StaticHeap::infosOfFunctions(VM&) { return nullptr; }
const void* StaticHeap::constantsOfProgram(VM&) { return nullptr; }
const AOT::ImageFunction* StaticHeap::imageFunctionOfFunction(uint32_t) { return nullptr; }
const uint32_t* StaticHeap::functionMetadataOffsets(VM&) { return nullptr; }
Ref<Decoder> StaticHeap::decoderForKeptPayload(VM&, Decoder& placed) { return placed; }
void StaticHeap::ensureDecoder(VM&, size_t, SourceProvider&) { }
FunctionExecutable* StaticHeap::standInFor(VM&, FunctionExecutable* executable) { return executable; }
UnlinkedFunctionCodeBlock* StaticHeap::codeOf(VM&, const UnlinkedFunctionExecutable&, CodeSpecializationKind) { return nullptr; }
void StaticHeap::setCodeOf(VM&, const UnlinkedFunctionExecutable&, CodeSpecializationKind, UnlinkedFunctionCodeBlock*) { }
ScriptExecutable*& StaticHeap::topLevelExecutableOfModuleInOtherVM(VM&, size_t) { RELEASE_ASSERT_NOT_REACHED(); }
bool StaticHeap::canPlaceCellsOf(VM&) { return false; }
void* StaticHeap::allocateBlock(VM&, size_t) { return nullptr; }
void StaticHeap::freeBlock(void*, size_t) { }
void StaticHeap::willAllocateUnlinkedFunctionSlow() { }

} // namespace JSC

#endif // ENABLE(AOT)
