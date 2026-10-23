/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTVector.h"
#include "CagedAlignedMemoryAllocator.h"
#include "Completion.h"
#include "FastMallocAlignedMemoryAllocator.h"
#include "HeapInlines.h"
#include "HeapIterationScope.h"
#include "HeapUtil.h"
#include "JSArrayInlines.h"
#include "JSBigInt.h"
#include "JSCInlines.h"
#include "JSLock.h"
#include "MarkStack.h"
#include "MarkedBlock.h"
#include "MarkedBlockCage.h"
#include "MarkedSpaceInlines.h"
#include "ObjectConstructor.h"
#include "PreciseAllocation.h"
#include "ReservedRangeHeap.h"
#include "SourceCode.h"
#include "VM.h"
#include "VMTrapsInlines.h"
#include <array>
#include <cstring>
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

std::span<const NativeTest> heapCageTests();

namespace {

class TestVM {
    WTF_MAKE_NONCOPYABLE(TestVM);
public:
    TestVM()
        : m_vm(VM::create())
    {
        m_lock.emplace(*m_vm);
    }

    ~TestVM()
    {
        m_vm = nullptr;
        m_lock.reset();
    }

    VM& vm() { return *m_vm; }
    MarkedBlockCage& cage() { return m_vm->heap.cage(); }
    JSGlobalObject* createRealm() { return JSGlobalObject::create(*m_vm, JSGlobalObject::createStructure(*m_vm, jsNull())); }

private:
    RefPtr<VM> m_vm;
    std::optional<JSLockHolder> m_lock;
};

constexpr size_t rangeSize = 64 * MB;
constexpr size_t blockSize = MarkedBlock::blockSize;

} // anonymous namespace

static ReservedRangeHeap& createRange(size_t unusedPrefixSize = 0, size_t size = rangeSize)
{
    ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({ .name = "Range of a test"_s, .size = size, .minSize = size, .unusedPrefixSize = unusedPrefixSize });
    AOT_TEST_CHECK(range && range->size() == size);
    return *range;
}

static bool isInside(const ReservedRangeHeap& range, const void* pointer, size_t size)
{
    return range.contains(pointer) && range.contains(static_cast<const uint8_t*>(pointer) + size - 1);
}

static void fill(void* pointer, size_t size, uint8_t seed)
{
    uint8_t* bytes = static_cast<uint8_t*>(pointer);
    for (size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<uint8_t>(seed + index * 31);
}

static bool isFilled(const void* pointer, size_t size, uint8_t seed)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(pointer);
    for (size_t index = 0; index < size; ++index) {
        if (bytes[index] != static_cast<uint8_t>(seed + index * 31))
            return false;
    }
    return true;
}

static void testRangeIsAlignedAndSized()
{
    ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({ .name = "Aligned range"_s, .size = rangeSize, .minSize = rangeSize, .alignment = GB });
    AOT_TEST_CHECK(range && range->size() == rangeSize && range->start() && !(range->start() % GB));
    AOT_TEST_CHECK(range->contains(std::bit_cast<void*>(range->start())) && range->contains(std::bit_cast<void*>(range->start() + rangeSize - 1)));
    AOT_TEST_CHECK(!range->contains(std::bit_cast<void*>(range->start() - 1)) && !range->contains(std::bit_cast<void*>(range->start() + rangeSize)) && !range->contains(nullptr));

    AOT_TEST_CHECK(!(createRange().start() % rangeSize));
}

static void testPrefixIsNeverAllocated()
{
    constexpr size_t prefixSize = 64 * blockSize;
    constexpr size_t numberOfBlocks = std::min<size_t>(1000, 2 * rangeSize / blockSize / 8);
    ReservedRangeHeap& range = createRange(prefixSize, 2 * rangeSize);
    AOT_TEST_CHECK(range.contains(std::bit_cast<void*>(range.start())));
    for (size_t count = 0; count < numberOfBlocks; ++count) {
        void* block = range.tryAllocateAligned(blockSize, blockSize);
        AOT_TEST_CHECK(block && isInside(range, block, blockSize));
        AOT_TEST_CHECK(std::bit_cast<uintptr_t>(block) >= range.start() + prefixSize);
    }
    for (size_t size : { size_t { 16 }, 100 * KB, 4 * MB }) {
        void* memory = range.tryAllocate(size);
        AOT_TEST_CHECK(memory && std::bit_cast<uintptr_t>(memory) >= range.start() + prefixSize);
    }
}

static void testBlocksAreAligned()
{
    ReservedRangeHeap& range = createRange(blockSize);
    HashSet<void*> blocks;
    for (unsigned count = 0; count < 200; ++count) {
        void* block = range.tryAllocateAligned(blockSize, blockSize);
        AOT_TEST_CHECK(block && !(std::bit_cast<uintptr_t>(block) % blockSize) && blocks.add(block).isNewEntry);
        fill(block, blockSize, static_cast<uint8_t>(count));
    }
    unsigned count = 0;
    for (void* block : blocks)
        count += isFilled(block, blockSize, *static_cast<uint8_t*>(block));
    AOT_TEST_CHECK(count == blocks.size());
}

static void testAnySize()
{
    ReservedRangeHeap& range = createRange(blockSize, 256 * MB);
    Vector<std::pair<void*, size_t>> allocations;
    for (size_t size : { size_t { 16 }, 100 * KB, 40 * MB }) {
        void* memory = range.tryAllocate(size);
        AOT_TEST_CHECK(memory && isInside(range, memory, size));
        fill(memory, size, static_cast<uint8_t>(allocations.size()));
        allocations.append({ memory, size });
    }
    for (size_t index = 0; index < allocations.size(); ++index)
        AOT_TEST_CHECK(isFilled(allocations[index].first, allocations[index].second, static_cast<uint8_t>(index)));
    for (auto [memory, size] : allocations)
        range.free(memory);
}

static void testReallocateKeepsBytes()
{
    ReservedRangeHeap& range = createRange(blockSize);
    void* memory = range.tryAllocate(100 * KB);
    AOT_TEST_CHECK(memory);
    fill(memory, 100 * KB, 5);
    void* neighbor = range.tryAllocate(100 * KB);
    AOT_TEST_CHECK(neighbor);
    fill(neighbor, 100 * KB, 9);

    void* grown = range.tryReallocate(memory, 3 * MB);
    AOT_TEST_CHECK(grown && isInside(range, grown, 3 * MB) && isFilled(grown, 100 * KB, 5));
    fill(grown, 3 * MB, 5);

    void* shrunk = range.tryReallocate(grown, 10 * KB);
    AOT_TEST_CHECK(shrunk && isInside(range, shrunk, 10 * KB) && isFilled(shrunk, 10 * KB, 5));
    AOT_TEST_CHECK(isFilled(neighbor, 100 * KB, 9));

    AOT_TEST_CHECK(!range.tryReallocate(shrunk, 2 * rangeSize) && isFilled(shrunk, 10 * KB, 5));
}

static void testFreeThenAllocateAgain()
{
    ReservedRangeHeap& range = createRange(blockSize);
    for (unsigned round = 0; round < 10000; ++round) {
        uint8_t* memory = static_cast<uint8_t*>(range.tryAllocate(MB));
        AOT_TEST_CHECK(memory && isInside(range, memory, MB));
        memory[0] = static_cast<uint8_t>(round);
        memory[MB - 1] = static_cast<uint8_t>(round);
        range.free(memory);
    }
}

static void testRangeTakesWhatBudgetLeaves()
{
    auto sizeWithBudget = [](size_t budget) {
        ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({ .name = "Range under a budget"_s, .size = rangeSize, .minSize = 16 * MB, .addressSpaceBudget = budget });
        AOT_TEST_CHECK(range && !(range->start() % roundUpToPowerOfTwo(range->size())));
        void* block = range->tryAllocateAligned(blockSize, blockSize);
        AOT_TEST_CHECK(block && isInside(*range, block, blockSize));
        return range->size();
    };
    AOT_TEST_CHECK(sizeWithBudget(32 * MB) == 32 * MB);
    AOT_TEST_CHECK(sizeWithBudget(33 * MB) == 33 * MB);
    AOT_TEST_CHECK(sizeWithBudget(63 * MB) == 63 * MB);
    AOT_TEST_CHECK(sizeWithBudget(63 * MB + blockSize) == 63 * MB);
    AOT_TEST_CHECK(sizeWithBudget(rangeSize) == rangeSize);
    AOT_TEST_CHECK(sizeWithBudget(std::numeric_limits<size_t>::max()) == rangeSize);
    AOT_TEST_CHECK(sizeWithBudget(MB) == 16 * MB);
    AOT_TEST_CHECK(sizeWithBudget(0) == 16 * MB);
}

static void testExhaustionReturnsNull()
{
    ReservedRangeHeap& range = createRange(blockSize);
    AOT_TEST_CHECK(!range.tryAllocate(2 * rangeSize) && !range.tryAllocate(rangeSize));

    Vector<void*> allocations;
    while (void* memory = range.tryAllocate(MB)) {
        AOT_TEST_CHECK(isInside(range, memory, MB) && allocations.size() < rangeSize / MB);
        allocations.append(memory);
    }
    AOT_TEST_CHECK(allocations.size() >= 2);
    AOT_TEST_CHECK(!range.tryAllocate(MB) && !range.tryAllocateAligned(blockSize, MB));

    range.free(allocations.takeLast());
    range.free(allocations.takeLast());
    AOT_TEST_CHECK(range.tryAllocate(MB));
}

static void testRangeGrowsBySegments()
{
    constexpr size_t size = GB;
    constexpr size_t initialSegmentSize = 64 * MB;
    ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({
        .name = "Growable range"_s,
        .size = size,
        .minSize = initialSegmentSize,
        .initialSegmentSize = initialSegmentSize,
        .alignment = GB,
        .unusedPrefixSize = blockSize,
        .placement = ReservedRangeHeap::Placement::RandomWindow,
        .initialAccess = ReservedRangeHeap::InitialAccess::None,
    });
    AOT_TEST_CHECK(range && !(range->start() % GB));
    if (range->size() == range->initialSegmentSize())
        return;
    AOT_TEST_CHECK(range->size() == size && range->initialSegmentSize() == initialSegmentSize && range->reservedSize() == initialSegmentSize);

    Vector<void*> blocks;
    while (blocks.size() * blockSize < 3 * initialSegmentSize) {
        void* block = range->tryAllocateAligned(blockSize, blockSize);
        AOT_TEST_CHECK(block && isInside(*range, block, blockSize) && !(std::bit_cast<uintptr_t>(block) % blockSize));
        fill(block, 64, static_cast<uint8_t>(blocks.size()));
        blocks.append(block);
    }
    size_t reservedSize = range->reservedSize();
    AOT_TEST_CHECK(reservedSize > 3 * initialSegmentSize && reservedSize <= size);

    void* large = range->tryAllocate(100 * MB);
    AOT_TEST_CHECK(large && isInside(*range, large, 100 * MB));
    fill(large, 64, 7);
    large = range->tryReallocate(large, 200 * MB);
    AOT_TEST_CHECK(large && isInside(*range, large, 200 * MB) && isFilled(large, 64, 7));
    range->free(large);

    for (size_t index = 0; index < blocks.size(); ++index)
        AOT_TEST_CHECK(isFilled(blocks[index], 64, static_cast<uint8_t>(index)));
    for (void* block : blocks)
        range->free(block);
    range->releaseFreeMemory();

    reservedSize = range->reservedSize();
    for (size_t index = 0; index < blocks.size(); ++index)
        blocks[index] = range->tryAllocateAligned(blockSize, blockSize);
    AOT_TEST_CHECK(!blocks.contains(nullptr) && range->reservedSize() == reservedSize);

    AOT_TEST_CHECK(!range->tryAllocate(size));
    size_t count = 0;
    while (range->tryAllocate(MB))
        ++count;
    AOT_TEST_CHECK(count < size / MB && range->reservedSize() <= size);
}

static void testEncodeDecodeRoundTrip()
{
    static_assert(MarkedBlockCage::shift == 3 && MarkedBlockCage::maxSize == 32 * GB && MarkedBlockCage::minIndex == 1u << 21);
    constexpr uint64_t firstOffset = static_cast<uint64_t>(MarkedBlockCage::minIndex) << MarkedBlockCage::shift;
    constexpr uint64_t bases[] = { 0, MarkedBlockCage::maxSize, 0x7f00 * GB };
    constexpr uint64_t offsets[] = { firstOffset, firstOffset + 8, firstOffset + 16, 4 * GB - 8, 4 * GB, MarkedBlockCage::maxSize - 16, MarkedBlockCage::maxSize - 8 };
    for (uintptr_t base : bases) {
        AOT_TEST_CHECK(!(base % MarkedBlockCage::maxSize));
        for (uint64_t offset : offsets) {
            void* pointer = std::bit_cast<void*>(base + static_cast<uintptr_t>(offset));
            uint32_t index = MarkedBlockCage::encode(base, pointer);
            AOT_TEST_CHECK(index == offset >> MarkedBlockCage::shift && index >= MarkedBlockCage::minIndex);
            AOT_TEST_CHECK(MarkedBlockCage::decode(base, index) == pointer);
        }
    }
}

static void testBaseOfAnyCell()
{
    constexpr uintptr_t base = static_cast<uintptr_t>(0x7f00 * GB);
    constexpr uint64_t offsets[] = { 0, 8, 16 * MB, 4 * GB + 24, MarkedBlockCage::maxSize - 8 };
    for (uint64_t offset : offsets) {
        void* pointer = std::bit_cast<void*>(base + static_cast<uintptr_t>(offset));
        AOT_TEST_CHECK(MarkedBlockCage::baseOf(pointer) == base && cageBaseOf(static_cast<HeapCell*>(pointer)) == base);
    }
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(std::bit_cast<void*>(base + static_cast<uintptr_t>(MarkedBlockCage::maxSize))) == base + MarkedBlockCage::maxSize);
    AOT_TEST_CHECK(MarkedBlockCage::baseOf(std::bit_cast<void*>(base - 8)) == base - MarkedBlockCage::maxSize);
}

static void testCageIsOffWithoutCompiledCode()
{
    if (Options::useAOT())
        return;
    AOT_TEST_CHECK(!MarkedBlockCage::isEnabled() && !MarkedBlockCage::isRequired());
    auto required = MarkedBlockCage::require();
    AOT_TEST_CHECK(!required && required.error() == MarkedBlockCage::Error::Disabled && !MarkedBlockCage::isRequired());

    MarkedBlockCage cage;
    AOT_TEST_CHECK(!cage.base() && !cage.size() && !cage.contains(&cage) && !cage.tryAllocate(16) && !cage.tryAllocateAligned(blockSize, blockSize) && !cage.usedIndices().count);
}

static constexpr unsigned cageSizeInMB = 256;

template<void (*body)()>
static void runInProcessWithCage()
{
    ASCIILiteral name;
    for (const NativeTest& test : heapCageTests()) {
        if (test.run == &runInProcessWithCage<body>)
            name = test.name;
    }
    AOT_TEST_CHECK(!name.isNull());

    if (MarkedBlockCage::isEnabled()) {
        AOT_TEST_CHECK(MarkedBlockCage::require().has_value() && MarkedBlockCage::isRequired());
        body();
        return;
    }

    ChildProcessLauncher launcher(String(programPath()), { });
    Vector<String> arguments { String(nativeSubcommandName), String(singleProcessFlag), String(name), "--useAOT=true"_s, makeString("--markedBlockCageSizeInMB="_s, cageSizeInMB), "--warmUpMarkedBlockStartAfterBlocks=64"_s };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(result.output);
        dataLogLn("The process with a cage ", result.description());
        failNativeTest(__FILE__, __LINE__, name.characters());
    }
}

#if OS(LINUX) && !ASAN_ENABLED
static constexpr ASCIILiteral markStackTest = "heap-cage/cage/mark-stack-that-cannot-grow-says-why"_s;

// A collection at the end of what a limit on address space leaves finds no memory for its mark stack. It cannot go on, and the process says what it ran out of.
static void testMarkStackThatCannotGrowSaysWhy()
{
    if (MarkedBlockCage::isEnabled()) {
        AOT_TEST_CHECK(MarkedBlockCage::require().has_value() && MarkedBlockCage::isRequired());
        void* taken = nullptr;
        for (size_t size : { 64 * MB, MB, 64 * KB, 4 * KB, KB, static_cast<size_t>(256), static_cast<size_t>(64), static_cast<size_t>(16) }) {
            void* block;
            while (tryFastMalloc(size).getValue(block)) {
                *static_cast<void**>(block) = taken;
                taken = block;
            }
        }
        MarkStackArray stack;
        return;
    }

    ChildProcessLauncher launcher("/bin/sh"_s, { });
    Vector<String> arguments { "-c"_s, "ulimit -c 0 && ulimit -v 2097152 && exec \"$0\" \"$@\""_s, String(programPath()), String(nativeSubcommandName), String(singleProcessFlag), String(markStackTest), "--useAOT=true"_s };
    ChildProcessResult result = launcher.run(arguments.span());
    if (result.outcome != ChildProcessOutcome::Crashed || !result.output.contains("FATAL: out of memory. The limit on address space (ulimit -v) leaves no room for more JavaScript objects. Raise the limit."_s)) {
        dataLogLn(result.output);
        dataLogLn("The process under a limit on address space ", result.description());
        failNativeTest(__FILE__, __LINE__, markStackTest.characters());
    }
}
#endif

static bool covers(MarkedBlockCage::Indices indices, uintptr_t base, const void* pointer, size_t size)
{
    uint32_t first = MarkedBlockCage::encode(base, pointer);
    uint32_t last = MarkedBlockCage::encode(base, static_cast<const uint8_t*>(pointer) + size - 8);
    return first - indices.first < indices.count && last - indices.first < indices.count;
}

static void checkUsedIndicesCoverAllocations()
{
    MarkedBlockCage cage;
    AOT_TEST_CHECK(cage.base() && !(cage.base() % MarkedBlockCage::maxSize) && cage.size() == cageSizeInMB * MB);
    AOT_TEST_CHECK(!cage.usedIndices().count);

    void* block = cage.tryAllocateAligned(blockSize, blockSize);
    AOT_TEST_CHECK(block && cage.contains(block));
    MarkedBlockCage::Indices afterOneBlock = cage.usedIndices();
    AOT_TEST_CHECK(afterOneBlock.base == cage.base() && afterOneBlock.first == MarkedBlockCage::encode(cage.base(), block) && afterOneBlock.count == blockSize >> MarkedBlockCage::shift);
    AOT_TEST_CHECK(afterOneBlock.first >= MarkedBlockCage::minIndex);

    void* memory = cage.tryAllocate(100 * KB);
    void* moved = cage.tryReallocate(cage.tryAllocate(KB), 3 * MB);
    AOT_TEST_CHECK(memory && moved);
    MarkedBlockCage::Indices indices = cage.usedIndices();
    AOT_TEST_CHECK(covers(indices, cage.base(), block, blockSize) && covers(indices, cage.base(), memory, 100 * KB) && covers(indices, cage.base(), moved, 3 * MB));
    AOT_TEST_CHECK(static_cast<uint64_t>(indices.first) + indices.count <= cage.size() >> MarkedBlockCage::shift);

    cage.free(block);
    cage.free(memory);
    cage.free(moved);
}

static void checkEveryCellIsInside()
{
    TestVM test;
    test.createRealm();
    unsigned numberOfCells = 0;
    unsigned numberOfStructures = 0;
    HeapIterationScope iterationScope(test.vm().heap);
    test.vm().heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
        if (!isJSCellKind(kind))
            return IterationStatus::Continue;
        ++numberOfCells;
        bool isStructure = static_cast<JSCell*>(cell)->type() == StructureType;
        numberOfStructures += isStructure;
        AOT_TEST_CHECK(isStructure != test.cage().contains(cell));
        AOT_TEST_CHECK(isStructure || cageBaseOf(cell) == test.cage().base());
        return IterationStatus::Continue;
    });
    AOT_TEST_CHECK(numberOfStructures > 100 && numberOfCells > 2 * numberOfStructures);
}

static void checkPreciseCellIsInside()
{
    TestVM test;
    JSBigInt* bigInt = JSBigInt::tryCreateWithLength(test.vm(), 200 * KB / sizeof(JSBigInt::Digit));
    AOT_TEST_CHECK(bigInt && bigInt->isPreciseAllocation() && !bigInt->preciseAllocation().isLowerTierPrecise());
    AOT_TEST_CHECK(test.cage().contains(bigInt) && test.cage().contains(reinterpret_cast<uint8_t*>(bigInt) + 200 * KB));
    AOT_TEST_CHECK(MarkedBlockCage::decode(test.cage().base(), MarkedBlockCage::encode(test.cage().base(), bigInt)) == bigInt);
}

static void checkLowerTierCellIsInside()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    NakedPtr<Exception> exception;
    JSValue collator = evaluate(realm, makeSource("new Intl.Collator"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted), JSValue(), exception);
    AOT_TEST_CHECK(!exception && collator.isCell());
    JSCell* cell = collator.asCell();
    AOT_TEST_CHECK(cell->isPreciseAllocation() && cell->preciseAllocation().isLowerTierPrecise());
    AOT_TEST_CHECK(test.cage().contains(cell));
}

static void checkButterflyIsOutside()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    JSArray* array = JSArray::tryCreate(test.vm(), realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 1000000);
    AOT_TEST_CHECK(array && test.cage().contains(array) && !test.cage().contains(array->butterfly()));

    JSArray* smallArray = JSArray::tryCreate(test.vm(), realm->arrayStructureForIndexingTypeDuringAllocation(ArrayWithContiguous), 4);
    AOT_TEST_CHECK(smallArray && test.cage().contains(smallArray) && !test.cage().contains(smallArray->butterfly()));
}

static void checkSecondHeapGetsItsOwnBase()
{
    TestVM first;
    TestVM second;
    AOT_TEST_CHECK(first.cage().base() && second.cage().base() && first.cage().base() != second.cage().base());
    AOT_TEST_CHECK(first.cage().size() == cageSizeInMB * MB && second.cage().size() == cageSizeInMB * MB);

    JSCell* cellOfFirst = first.vm().smallStrings.emptyString();
    JSCell* cellOfSecond = second.vm().smallStrings.emptyString();
    AOT_TEST_CHECK(first.cage().contains(cellOfFirst) && !first.cage().contains(cellOfSecond));
    AOT_TEST_CHECK(second.cage().contains(cellOfSecond) && !second.cage().contains(cellOfFirst));
}

// What is held back so that a full cage can end in an error: nothing until the cage is half full, then a block for each block that the heap takes.
static void checkReserveIsKeptOnceHalfFull()
{
    TestVM test;
    MarkedBlockCage& cage = test.cage();
    cage.keepReserveOnceHalfFull();
    AOT_TEST_CHECK(!cage.reserveSize() && !cage.releaseReserve());

    Vector<void*> blocks;
    uintptr_t middle = cage.base() + cage.size() / 2;
    do {
        blocks.append(cage.tryAllocateAligned(blockSize, blockSize));
        AOT_TEST_CHECK(blocks.last() && blocks.size() <= cage.size() / blockSize);
    } while (std::bit_cast<uintptr_t>(blocks.last()) + blockSize <= middle);

    constexpr size_t whole = 4 * MB;
    static_assert(cageSizeInMB * MB / 64 == whole);
    cage.keepReserveOnceHalfFull();
    AOT_TEST_CHECK(cage.reserveSize() == blockSize);
    for (size_t count = 0; count < 2 * whole / blockSize; ++count)
        cage.keepReserveOnceHalfFull();
    AOT_TEST_CHECK(cage.reserveSize() == whole);

    // A quarter stays behind for a thread that finds no place to throw the error and has to be ended.
    AOT_TEST_CHECK(cage.releaseReserve() && cage.reserveSize() == whole / 4 && !cage.releaseReserve());
    cage.keepReserveOnceHalfFull();
    AOT_TEST_CHECK(cage.reserveSize() == whole / 4);
    AOT_TEST_CHECK(cage.releaseLastReserve() && !cage.reserveSize() && !cage.releaseLastReserve() && !cage.releaseReserve());
    cage.keepReserveOnceHalfFull();
    AOT_TEST_CHECK(!cage.reserveSize());
    cage.didThrowOutOfMemoryError();
    cage.keepReserveOnceHalfFull();
    AOT_TEST_CHECK(cage.reserveSize() == blockSize);
    for (void* block : blocks)
        cage.free(block);
}

// An error that waits for the next function entry holds the limit that entries test, not the polls of loops, and an ordinary stop that comes and goes leaves it waiting.
static void checkErrorWaitsAtFunctionEntriesAlone()
{
    TestVM test;
    VMTraps& traps = test.vm().traps();
    void* marker = std::bit_cast<void*>(StackManager::StopRequestMarkerValue);
    StackManager::Mirror mirror;
    traps.registerMirror(mirror);
    AOT_TEST_CHECK(!traps.needsOutOfMemoryError() && !traps.stopsAtEveryPoll() && mirror.trapAwareSoftStackLimit() == traps.softStackLimit());

    traps.setNeedsOutOfMemoryError(true);
    AOT_TEST_CHECK(traps.needsOutOfMemoryError() && !traps.stopsAtEveryPoll() && mirror.trapAwareSoftStackLimit() == marker && mirror.softStackLimit() == traps.softStackLimit());

    traps.requestStop();
    AOT_TEST_CHECK(traps.stopsAtEveryPoll() && mirror.trapAwareSoftStackLimit() == marker);
    traps.cancelStop();
    AOT_TEST_CHECK(traps.needsOutOfMemoryError() && !traps.stopsAtEveryPoll() && mirror.trapAwareSoftStackLimit() == marker);

    StackManager::Mirror later;
    traps.registerMirror(later);
    AOT_TEST_CHECK(later.trapAwareSoftStackLimit() == marker);

    traps.requestStop();
    traps.setNeedsOutOfMemoryError(false);
    AOT_TEST_CHECK(!traps.needsOutOfMemoryError() && mirror.trapAwareSoftStackLimit() == marker);
    traps.cancelStop();
    AOT_TEST_CHECK(mirror.trapAwareSoftStackLimit() == traps.softStackLimit() && later.trapAwareSoftStackLimit() == traps.softStackLimit());

    traps.setNeedsOutOfMemoryError(true);
    traps.setNeedsOutOfMemoryError(false);
    AOT_TEST_CHECK(mirror.trapAwareSoftStackLimit() == traps.softStackLimit());
    traps.unregisterMirror(later);
    traps.unregisterMirror(mirror);
}

static void checkRangeIsReused()
{
    uintptr_t baseOfFirst;
    {
        TestVM first;
        first.createRealm();
        baseOfFirst = first.cage().base();
    }
    TestVM second;
    second.createRealm();
    AOT_TEST_CHECK(baseOfFirst && second.cage().base() == baseOfFirst);
}

// Far more blocks than the helper thread keeps, with pauses in which it fills its supply: some come from the supply and some straight from the cage.
static void checkBlocksToFillAreFreshBlocksOfTheCage()
{
    uintptr_t base;
    {
        CagedAlignedMemoryAllocator allocator;
        base = allocator.cage().base();
        AOT_TEST_CHECK(base && Options::useWarmUpMarkedBlocks() && Options::warmUpMarkedBlockCount());
        unsigned numberOfBlocks = 4 * (Options::warmUpMarkedBlockStartAfterBlocks() + Options::warmUpMarkedBlockCount());
        HashSet<void*> blocks;
        for (unsigned count = 0; count < numberOfBlocks; ++count) {
            void* block = allocator.tryAllocateBlockToFill();
            AOT_TEST_CHECK(block && allocator.cage().contains(block) && !(std::bit_cast<uintptr_t>(block) % blockSize) && blocks.add(block).isNewEntry);
            const uint64_t* words = static_cast<const uint64_t*>(block);
            for (size_t index = 0; index < blockSize / sizeof(uint64_t); ++index)
                AOT_TEST_CHECK(!words[index]);
            fill(block, blockSize, static_cast<uint8_t>(count));
            if (!(count % 24))
                WTF::sleep(1_ms);
        }
        unsigned count = 0;
        for (void* block : blocks)
            count += isFilled(block, blockSize, *static_cast<uint8_t*>(block));
        AOT_TEST_CHECK(count == blocks.size());
        for (void* block : blocks)
            allocator.freeAlignedMemory(block);
    }
    // The helper has ended and has given back what it held, so the range serves the next heap.
    CagedAlignedMemoryAllocator next;
    AOT_TEST_CHECK(next.cage().base() == base);
    void* block = next.tryAllocateBlockToFill();
    AOT_TEST_CHECK(block && next.cage().contains(block));
    next.freeAlignedMemory(block);
}

// The blocks of array storage come from the other allocator, which waits for as many requests of its own. The cage's count starts its supply too.
static void checkOtherAllocatorWarmsUpWithTheCage()
{
    auto takeBlockOutsideTheCage = [] {
        FastMallocAlignedMemoryAllocator outside;
        void* block = outside.tryAllocateAlignedMemory(blockSize, blockSize);
        AOT_TEST_CHECK(block);
        outside.freeAlignedMemory(block);
    };
    auto waitForSupply = [] {
        for (unsigned attempt = 0; attempt < 2000 && !warmUpMarkedBlockCountForTesting(); ++attempt)
            WTF::sleep(1_ms);
        return warmUpMarkedBlockCountForTesting();
    };
    AOT_TEST_CHECK(warmUpMarkedBlocksAreEnabledForTesting() && Options::warmUpMarkedBlockStartAfterBlocks() > 2);
    takeBlockOutsideTheCage();
    WTF::sleep(20_ms);
    AOT_TEST_CHECK(!warmUpMarkedBlockCountForTesting());

    CagedAlignedMemoryAllocator allocator;
    Vector<void*> blocks;
    for (unsigned count = 0; count < Options::warmUpMarkedBlockStartAfterBlocks(); ++count)
        blocks.append(allocator.tryAllocateBlockToFill());
    takeBlockOutsideTheCage();
    AOT_TEST_CHECK(waitForSupply());
    for (void* block : blocks) {
        AOT_TEST_CHECK(block);
        allocator.freeAlignedMemory(block);
    }
}

namespace {

enum class CellKind : bool { InBlock, Precise };
enum class WordForm : uint8_t { IndexInLowHalf, IndexInHighHalf, Offset, OffsetIntoCell, OtherIndex };

constexpr unsigned numberOfCandidates = 32;
using Words = std::array<uint64_t, numberOfCandidates>;

} // anonymous namespace

static uint64_t wordFor(uint32_t index, WordForm form)
{
    switch (form) {
    case WordForm::IndexInLowHalf:
        return index;
    case WordForm::IndexInHighHalf:
        return static_cast<uint64_t>(index) << 32;
    case WordForm::Offset:
        return static_cast<uint64_t>(index) << MarkedBlockCage::shift;
    case WordForm::OffsetIntoCell:
        return (static_cast<uint64_t>(index) << MarkedBlockCage::shift) + 8;
    case WordForm::OtherIndex:
        return index ^ 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t indexIn(uint64_t word, WordForm form)
{
    switch (form) {
    case WordForm::IndexInLowHalf:
        return static_cast<uint32_t>(word);
    case WordForm::IndexInHighHalf:
        return static_cast<uint32_t>(word >> 32);
    case WordForm::Offset:
        return static_cast<uint32_t>(word >> MarkedBlockCage::shift);
    case WordForm::OffsetIntoCell:
        return static_cast<uint32_t>((word - 8) >> MarkedBlockCage::shift);
    case WordForm::OtherIndex:
        return static_cast<uint32_t>(word) ^ 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static NEVER_INLINE void createUnreferencedCells(TestVM& test, JSGlobalObject* realm, CellKind kind, WordForm form, Words& words)
{
    for (uint64_t& word : words) {
        JSCell* cell = kind == CellKind::Precise ? static_cast<JSCell*>(JSBigInt::tryCreateWithLength(test.vm(), 200 * KB / sizeof(JSBigInt::Digit))) : constructEmptyObject(realm);
        AOT_TEST_CHECK(cell && cell->isPreciseAllocation() == (kind == CellKind::Precise));
        word = wordFor(MarkedBlockCage::encode(test.cage().base(), cell), form);
    }
}

static NEVER_INLINE unsigned countCellsMarkedThroughWords(CellKind kind, WordForm form)
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    Words created;
    createUnreferencedCells(test, realm, kind, form, created);

    volatile uint64_t wordsOnStack[numberOfCandidates];
    for (unsigned index = 0; index < numberOfCandidates; ++index) {
        wordsOnStack[index] = created[index];
        created[index] = 0;
    }
    test.vm().heap.collectNow(Sync, CollectionScope::Full);

    unsigned numberOfMarkedCells = 0;
    for (unsigned index = 0; index < numberOfCandidates; ++index)
        numberOfMarkedCells += HeapUtil::classifyWord(test.vm().heap, MarkedBlockCage::decode(test.cage().base(), indexIn(wordsOnStack[index], form))) == HeapUtil::WordKind::MarkedCell;
    return numberOfMarkedCells;
}

template<CellKind kind, WordForm form>
static void checkWordKeepsCellsAlive()
{
    AOT_TEST_CHECK(countCellsMarkedThroughWords(kind, form) == numberOfCandidates);
}

template<CellKind kind>
static void checkOtherIndexKeepsNothingAlive()
{
    AOT_TEST_CHECK(countCellsMarkedThroughWords(kind, WordForm::OtherIndex) < numberOfCandidates / 2);
}

template<void (*body)()>
static constexpr void (*withCage)() = runInProcessWithCage<body>;

std::span<const NativeTest> heapCageTests()
{
    static constexpr NativeTest tests[] = {
        { "heap-cage/reserved-range/aligned-and-sized"_s, testRangeIsAlignedAndSized },
        { "heap-cage/reserved-range/prefix-is-never-allocated"_s, testPrefixIsNeverAllocated },
        { "heap-cage/reserved-range/blocks-are-aligned"_s, testBlocksAreAligned },
        { "heap-cage/reserved-range/any-size"_s, testAnySize },
        { "heap-cage/reserved-range/reallocate-keeps-bytes"_s, testReallocateKeepsBytes },
        { "heap-cage/reserved-range/free-then-allocate-again"_s, testFreeThenAllocateAgain },
        { "heap-cage/reserved-range/takes-what-the-budget-leaves"_s, testRangeTakesWhatBudgetLeaves },
        { "heap-cage/reserved-range/exhaustion-returns-null"_s, testExhaustionReturnsNull },
        { "heap-cage/reserved-range/grows-by-segments"_s, testRangeGrowsBySegments },
        { "heap-cage/cage/encode-decode-round-trip"_s, testEncodeDecodeRoundTrip },
        { "heap-cage/cage/base-of-any-cell"_s, testBaseOfAnyCell },
        { "heap-cage/cage/off-without-compiled-code"_s, testCageIsOffWithoutCompiledCode },
#if OS(LINUX) && !ASAN_ENABLED
        { markStackTest, testMarkStackThatCannotGrowSaysWhy },
#endif
        { "heap-cage/cage/used-indices-cover-allocations"_s, withCage<checkUsedIndicesCoverAllocations> },
        { "heap-cage/cage/every-cell-is-inside"_s, withCage<checkEveryCellIsInside> },
        { "heap-cage/cage/precise-cell-is-inside"_s, withCage<checkPreciseCellIsInside> },
        { "heap-cage/cage/lower-tier-cell-is-inside"_s, withCage<checkLowerTierCellIsInside> },
        { "heap-cage/cage/butterfly-is-outside"_s, withCage<checkButterflyIsOutside> },
        { "heap-cage/cage/second-heap-gets-its-own-base"_s, withCage<checkSecondHeapGetsItsOwnBase> },
        { "heap-cage/cage/range-is-reused"_s, withCage<checkRangeIsReused> },
        { "heap-cage/cage/reserve-is-kept-once-half-full"_s, withCage<checkReserveIsKeptOnceHalfFull> },
        { "heap-cage/cage/error-waits-at-function-entries-alone"_s, withCage<checkErrorWaitsAtFunctionEntriesAlone> },
        { "heap-cage/cage/blocks-to-fill-are-fresh-blocks-of-the-cage"_s, withCage<checkBlocksToFillAreFreshBlocksOfTheCage> },
        { "heap-cage/cage/other-allocator-warms-up-with-the-cage"_s, withCage<checkOtherAllocatorWarmsUpWithTheCage> },
        { "heap-cage/scan/index-in-low-half"_s, withCage<checkWordKeepsCellsAlive<CellKind::InBlock, WordForm::IndexInLowHalf>> },
        { "heap-cage/scan/index-in-high-half"_s, withCage<checkWordKeepsCellsAlive<CellKind::InBlock, WordForm::IndexInHighHalf>> },
        { "heap-cage/scan/offset-from-base"_s, withCage<checkWordKeepsCellsAlive<CellKind::InBlock, WordForm::Offset>> },
        { "heap-cage/scan/offset-into-cell"_s, withCage<checkWordKeepsCellsAlive<CellKind::InBlock, WordForm::OffsetIntoCell>> },
        { "heap-cage/scan/other-index-keeps-nothing-alive"_s, withCage<checkOtherIndexKeepsNothingAlive<CellKind::InBlock>> },
        { "heap-cage/scan/precise-cell-index-in-low-half"_s, withCage<checkWordKeepsCellsAlive<CellKind::Precise, WordForm::IndexInLowHalf>> },
        { "heap-cage/scan/precise-cell-index-in-high-half"_s, withCage<checkWordKeepsCellsAlive<CellKind::Precise, WordForm::IndexInHighHalf>> },
        { "heap-cage/scan/precise-cell-offset-from-base"_s, withCage<checkWordKeepsCellsAlive<CellKind::Precise, WordForm::Offset>> },
        { "heap-cage/scan/precise-cell-other-index-keeps-nothing-alive"_s, withCage<checkOtherIndexKeepsNothingAlive<CellKind::Precise>> },
    };
    return tests;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
