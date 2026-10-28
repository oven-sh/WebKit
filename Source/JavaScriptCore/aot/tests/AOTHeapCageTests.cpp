/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNativeTest.h"

#if ENABLE(AOT)

#include "AOTChildProcess.h"
#include "AOTRuntimeTable.h"
#include "AOTVector.h"
#include "AddressSpaceBudget.h"
#include "CagedAlignedMemoryAllocator.h"
#include "Completion.h"
#include "DeferGCInlines.h"
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
#include "OwedOutOfMemoryError.h"
#include "PreciseAllocation.h"
#include "ReservedRangeHeap.h"
#include "SourceCode.h"
#include "StructureAlignedMemoryAllocator.h"
#include "TopExceptionScope.h"
#include "VM.h"
#include "VMTrapsInlines.h"
#include <array>
#include <cstring>
#include <optional>
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/OSAllocator.h>
#include <wtf/PageBlock.h>
#include <wtf/ParkingLot.h>
#include <wtf/Threading.h>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

#if USE(MIMALLOC)
#include <bmalloc/mimalloc.h>
#endif

#if OS(LINUX)
#include <cxxabi.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" char __executable_start;
#endif

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
        AOT_TEST_CHECK(memory && isInside(range, memory, size) && range.sizeOf(memory) >= size && isInside(range, memory, range.sizeOf(memory)));
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

#if OS(LINUX) && USE(MIMALLOC)
// A range gives its free memory back at malloc's pace, and touches nothing that another range has freed: the threads of a program that end together do not wait for one another.
static void testReleasingFreeMemoryPurgesNoOtherRange()
{
    if (!bmalloc::api::isEnabled())
        return;
    auto isResident = [](void* page) {
        unsigned char state = 0;
        AOT_TEST_CHECK(!mincore(page, pageSize(), &state));
        return !!(state & 1);
    };
    long delay = mi_option_get(mi_option_purge_delay);
    mi_option_set(mi_option_purge_delay, 600 * 1000);

    ReservedRangeHeap& first = createRange(blockSize);
    ReservedRangeHeap& second = createRange(blockSize);
    void* memory = first.tryAllocate(4 * MB);
    AOT_TEST_CHECK(memory);
    fill(memory, 4 * MB, 3);
    void* page = static_cast<char*>(memory) + 2 * MB;
    AOT_TEST_CHECK(isResident(page));
    first.free(memory);
    first.releaseFreeMemory();
    bool wasLeftByItsOwnRange = isResident(page);
    second.releaseFreeMemory();
    bool wasLeftByOtherRange = isResident(page);

    mi_option_set(mi_option_purge_delay, delay);
    AOT_TEST_CHECK(wasLeftByItsOwnRange && wasLeftByOtherRange);
}
#endif

static void testRandomWindowsLieFarApart()
{
    constexpr size_t size = 64 * MB;
    constexpr size_t alignment = 32 * GB;
    uintptr_t lowest = std::numeric_limits<uintptr_t>::max();
    uintptr_t highest = 0;
    for (unsigned index = 0; index < 6; ++index) {
        ReservedRangeHeap* range = ReservedRangeHeap::tryCreate({ .name = "Range in a random window"_s, .size = size, .minSize = size, .alignment = alignment, .placement = ReservedRangeHeap::Placement::RandomWindow });
        AOT_TEST_CHECK(range && !(range->start() % alignment));
        lowest = std::min(lowest, range->start());
        highest = std::max(highest, range->start());
    }
    AOT_TEST_CHECK(highest - lowest > 4 * 1024 * GB);
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

#if OS(LINUX) && !ASAN_ENABLED
static constexpr ASCIILiteral frameLinePrefix = "frame 0x"_s;

// Addresses alone: there may be no memory to name them with.
static void printFramesAndEnd(int signalNumber, siginfo_t* info, void* context)
{
    auto print = [](ASCIILiteral prefix, uintptr_t value) {
        char line[64];
        int length = snprintf(line, sizeof(line), "%s%lx\n", prefix.characters(), static_cast<unsigned long>(value));
        if (write(STDERR_FILENO, line, length) < 0) { }
    };
    auto& machine = static_cast<ucontext_t*>(context)->uc_mcontext;
#if CPU(X86_64)
    uintptr_t pc = machine.gregs[REG_RIP];
    uintptr_t frame = machine.gregs[REG_RBP];
#else
    uintptr_t pc = machine.pc;
    uintptr_t frame = machine.regs[29];
#endif
    print("signal at address 0x"_s, std::bit_cast<uintptr_t>(info->si_addr));
    for (unsigned depth = 0; depth < 32; ++depth) {
        print(frameLinePrefix, pc);
        if (!frame || frame % sizeof(uintptr_t))
            break;
        auto* words = std::bit_cast<uintptr_t*>(frame);
        if (words[0] <= frame)
            break;
        pc = words[1];
        frame = words[0];
    }
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

static void printFramesAtSignals()
{
    struct sigaction action { };
    action.sa_sigaction = printFramesAndEnd;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
    for (int signalNumber : { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT })
        AOT_TEST_CHECK(!sigaction(signalNumber, &action, nullptr));
}

// The process ran this executable, so its addresses are named by the symbols of this one.
static String withNamedFrames(const String& output)
{
    int file = open("/proc/self/exe", O_RDONLY);
    struct stat status;
    if (file < 0 || fstat(file, &status))
        return output;
    auto* bytes = static_cast<const uint8_t*>(mmap(nullptr, status.st_size, PROT_READ, MAP_PRIVATE, file, 0));
    close(file);
    if (bytes == MAP_FAILED)
        return output;
    auto* header = reinterpret_cast<const ElfW(Ehdr)*>(bytes);
    auto sections = unsafeMakeSpan(reinterpret_cast<const ElfW(Shdr)*>(bytes + header->e_shoff), header->e_shnum);
    std::span<const ElfW(Sym)> symbols;
    const char* names = nullptr;
    for (auto& section : sections) {
        if (section.sh_type != SHT_SYMTAB)
            continue;
        symbols = unsafeMakeSpan(reinterpret_cast<const ElfW(Sym)*>(bytes + section.sh_offset), section.sh_size / sizeof(ElfW(Sym)));
        names = reinterpret_cast<const char*>(bytes + sections[section.sh_link].sh_offset);
    }
    uintptr_t slide = header->e_type == ET_DYN ? std::bit_cast<uintptr_t>(&__executable_start) : 0;

    StringBuilder result;
    for (auto line : StringView(output).split('\n')) {
        result.append(line);
        if (line.startsWith(frameLinePrefix)) {
            uintptr_t address = parseInteger<uint64_t>(line.substring(frameLinePrefix.length()), 16).value_or(0) - slide;
            for (auto& symbol : symbols) {
                if (ELF64_ST_TYPE(symbol.st_info) != STT_FUNC || address - symbol.st_value >= symbol.st_size)
                    continue;
                int status = -1;
                char* demangled = abi::__cxa_demangle(names + symbol.st_name, nullptr, nullptr, &status);
                result.append(' ', String::fromUTF8(status ? names + symbol.st_name : demangled));
                free(demangled);
                break;
            }
        }
        result.append('\n');
    }
    munmap(const_cast<uint8_t*>(bytes), status.st_size);
    return result.toString();
}

// Within a limit on address space every cage of the process takes from one range.
template<void (*body)()>
static void runInProcessWithSharedRange()
{
    ASCIILiteral name;
    for (const NativeTest& test : heapCageTests()) {
        if (test.run == &runInProcessWithSharedRange<body>)
            name = test.name;
    }
    AOT_TEST_CHECK(!name.isNull());

    if (MarkedBlockCage::isEnabled()) {
        AOT_TEST_CHECK(MarkedBlockCage::require().has_value() && MarkedBlockCage::isRequired());
        printFramesAtSignals();
        body();
        return;
    }

    ChildProcessLauncher launcher("/bin/sh"_s, { });
    Vector<String> arguments { "-c"_s, "ulimit -c 0 && ulimit -v 1048576 && exec \"$0\" \"$@\""_s, String(programPath()), String(nativeSubcommandName), String(singleProcessFlag), String(name), "--useAOT=true"_s };
    ChildProcessResult result = launcher.run(arguments.span());
    if (!result.exitedWith(0)) {
        dataLogLn(withNamedFrames(result.output));
        dataLogLn("The process under a limit on address space ", result.description());
        failNativeTest(__FILE__, __LINE__, name.characters());
    }
}

static constexpr size_t sizeOfCellLargerThanBlock = 2 * blockSize;

static void takeAllRoom(MarkedBlockCage& cage)
{
    for (size_t size = MB; size >= blockSize; size /= 2) {
        while (cage.tryAllocate(size)) { }
    }
    while (cage.tryAllocate(sizeOfCellLargerThanBlock)) { }
    while (cage.tryAllocateAligned(blockSize, blockSize)) { }
}

static Vector<void*, 128> takeBlocksOfSharedReserve(MarkedBlockCage& cage)
{
    Vector<void*, 128> blocks;
    AOT_TEST_CHECK(!cage.tryAllocateAligned(blockSize, blockSize) && !cage.tryAllocateAlignedFromSharedReserve(blockSize, blockSize));
    AOT_TEST_CHECK(cage.releaseLastReserve() && !cage.releaseLastReserve());
    while (void* block = cage.tryAllocateAlignedFromSharedReserve(blockSize, blockSize)) {
        AOT_TEST_CHECK(cage.contains(block) && !(std::bit_cast<uintptr_t>(block) % blockSize));
        blocks.append(block);
    }
    return blocks;
}

// A cage takes every block of the reserve that the cages of a range share, an out-of-memory error is thrown, and the cage goes away. A cage that was there all along finds as many.
static void checkSharedReserveIsWholeAgainBehindCageThatThrew()
{
    MarkedBlockCage first;
    MarkedBlockCage waiting;
    AOT_TEST_CHECK(first.base() && waiting.base() == first.base());
    takeAllRoom(first);

    size_t whole;
    {
        MarkedBlockCage cage;
        auto blocks = takeBlocksOfSharedReserve(cage);
        whole = blocks.size();
        AOT_TEST_CHECK(whole == MB / blockSize);
        cage.didThrowOutOfMemoryError();
        for (void* block : blocks)
            cage.free(block);
    }
    AOT_TEST_CHECK(takeBlocksOfSharedReserve(waiting).size() == whole);
}

// The same where the cage stays: it gives a block back for each block that it takes from the range.
static void checkSharedReserveIsWholeAgainBehindCageThatStays()
{
    MarkedBlockCage first;
    MarkedBlockCage waiting;
    MarkedBlockCage cage;
    // Room for what the cage keeps for itself.
    std::array<void*, 8> spare;
    for (void*& block : spare)
        block = first.tryAllocateAligned(blockSize, blockSize);
    takeAllRoom(first);

    auto blocks = takeBlocksOfSharedReserve(cage);
    size_t whole = blocks.size();
    cage.didThrowOutOfMemoryError();
    for (void* block : blocks)
        cage.free(block);
    for (void* block : spare)
        first.free(block);
    for (size_t count = 0; count < whole; ++count)
        cage.keepReserveOnceHalfFull();
    takeAllRoom(first);
    AOT_TEST_CHECK(takeBlocksOfSharedReserve(waiting).size() == whole);
}

// Four cages at once get a cell that is larger than a block, and what they free is there for the next.
static void checkSharedReserveHoldsLargeCellsOfSeveralCages()
{
    MarkedBlockCage first;
    std::array<MarkedBlockCage, 4> cages;
    std::array<void*, 4> cells;
    for (MarkedBlockCage& cage : cages) {
        cage.keepReserveOnceHalfFull();
        AOT_TEST_CHECK(cage.reserveSize() == blockSize);
    }
    takeAllRoom(first);

    // What a cage kept for itself goes back to the range. Whether that is room for such a cell is up to the range, so another cage takes it.
    for (MarkedBlockCage& cage : cages) {
        AOT_TEST_CHECK(!cage.tryAllocate(sizeOfCellLargerThanBlock) && cage.releaseLastReserve());
        takeAllRoom(first);
    }

    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned index = 0; index < cages.size(); ++index) {
            MarkedBlockCage& cage = cages[index];
            cells[index] = cage.tryAllocate(sizeOfCellLargerThanBlock);
            AOT_TEST_CHECK(cells[index] && cage.contains(cells[index]));
            fill(cells[index], sizeOfCellLargerThanBlock, index);
        }
        cells[0] = cages[0].tryReallocate(cells[0], 2 * sizeOfCellLargerThanBlock);
        AOT_TEST_CHECK(cells[0]);
        for (unsigned index = 0; index < cages.size(); ++index) {
            AOT_TEST_CHECK(isFilled(cells[index], sizeOfCellLargerThanBlock, index));
            cages[index].free(cells[index]);
        }
    }
    void* largest = cages[0].tryAllocate(MB);
    AOT_TEST_CHECK(largest && !cages[1].tryAllocate(sizeOfCellLargerThanBlock));
    cages[0].free(largest);
}

static void checkCellInFullRangeGrowsIntoSharedReserve()
{
    MarkedBlockCage first;
    MarkedBlockCage cage;
    void* cell = cage.tryAllocate(sizeOfCellLargerThanBlock);
    AOT_TEST_CHECK(cell);
    fill(cell, sizeOfCellLargerThanBlock, 1);
    takeAllRoom(first);

    AOT_TEST_CHECK(!cage.tryReallocate(cell, 2 * sizeOfCellLargerThanBlock) && isFilled(cell, sizeOfCellLargerThanBlock, 1));
    AOT_TEST_CHECK(cage.releaseLastReserve());
    void* grown = cage.tryReallocate(cell, 2 * sizeOfCellLargerThanBlock);
    AOT_TEST_CHECK(grown && grown != cell && cage.contains(grown) && isFilled(grown, sizeOfCellLargerThanBlock, 1));
    cage.free(grown);
    void* largest = cage.tryAllocate(MB);
    AOT_TEST_CHECK(largest);
    cage.free(largest);
}

struct MallocThatRefusesOnRequest : FastMalloc {
    static void* tryMalloc(size_t size) { return s_refuses ? nullptr : FastMalloc::tryMalloc(size); }
    static void* tryZeroedMalloc(size_t size) { return s_refuses ? nullptr : FastMalloc::tryZeroedMalloc(size); }
    static inline bool s_refuses { false };
};

// A table that was asked to make room for one more key does not grow when the key is added. Where it gets no memory it says so and holds what it held.
static void checkTableThatFindsNoMemoryToGrowStaysAsItWas()
{
    using Map = HashMap<unsigned, unsigned, DefaultHash<unsigned>, HashTraits<unsigned>, HashTraits<unsigned>, WTF::HashTableTraits, WTF::ShouldValidateKey::Yes, MallocThatRefusesOnRequest>;
    Map map;
    auto holdsKeysUpTo = [&](unsigned last) {
        if (map.size() != last)
            return false;
        for (unsigned key = 1; key <= last; ++key) {
            if (map.get(key) != key + 1)
                return false;
        }
        return !map.contains(last + 1);
    };

    unsigned key = 0;
    while (key < 5000) {
        AOT_TEST_CHECK(map.tryReserveCapacityForAdd());
        unsigned capacity = map.capacity();
        ++key;
        map.add(key, key + 1);
        AOT_TEST_CHECK(map.capacity() == capacity);
    }
    AOT_TEST_CHECK(holdsKeysUpTo(key));

    MallocThatRefusesOnRequest::s_refuses = true;
    unsigned capacity = map.capacity();
    while (map.tryReserveCapacityForAdd()) {
        ++key;
        map.add(key, key + 1);
        AOT_TEST_CHECK(map.capacity() == capacity);
    }
    AOT_TEST_CHECK(key > 5000 && key < capacity);
    AOT_TEST_CHECK(map.capacity() == capacity);
    AOT_TEST_CHECK(holdsKeysUpTo(key));

    // Keys that were removed leave buckets that a table of the same size does without: that takes memory too.
    for (unsigned removed = key; removed > key - 100; --removed)
        map.remove(removed);
    key -= 100;
    AOT_TEST_CHECK(!map.tryReserveCapacityForAdd());
    AOT_TEST_CHECK(holdsKeysUpTo(key));

    MallocThatRefusesOnRequest::s_refuses = false;
    AOT_TEST_CHECK(map.tryReserveCapacityForAdd());
    AOT_TEST_CHECK(map.capacity() == 2 * capacity);
    AOT_TEST_CHECK(holdsKeysUpTo(key));

    Map empty;
    MallocThatRefusesOnRequest::s_refuses = true;
    AOT_TEST_CHECK(!empty.tryReserveCapacityForAdd());
    AOT_TEST_CHECK(!empty.capacity());
    MallocThatRefusesOnRequest::s_refuses = false;
    AOT_TEST_CHECK(empty.tryReserveCapacityForAdd());
    AOT_TEST_CHECK(empty.capacity());

    AOT_TEST_CHECK(empty.tryReserveCapacityForAdd(3000));
    capacity = empty.capacity();
    for (key = 1; key <= 3000; ++key)
        empty.add(key, key);
    AOT_TEST_CHECK(empty.capacity() == capacity);
}

#if USE(MIMALLOC)
// All the address space is taken. A request to malloc that may fail gets null, requests that may not fail are served, and the thread is told. When address space comes back the ballast is kept again.
static void checkRequestsThatMayNotFailUseBallast()
{
    static unsigned timesToldOfUse;
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
    bmalloc::api::setAddressSpaceBallastHandler([](bmalloc::api::UseOfAddressSpaceBallast) { ++timesToldOfUse; });

    struct Mapping {
        void* pages;
        size_t size;
    };
    Vector<Mapping, 64> mappings;
    for (size_t size = 64 * MB; size >= pageSize(); size /= 2) {
        while (void* pages = OSAllocator::tryReserveUncommitted(size))
            mappings.append({ pages, size });
    }
    Vector<void*> pieces;
    pieces.reserveInitialCapacity(256 * KB);
    while (void* piece = tryFastAlignedMalloc(16, 16 * KB))
        pieces.append(piece);
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast() && !timesToldOfUse);

    // An allocator that does not remember what it refused cannot be asked again.
    if (void* again = bmalloc::api::memalignAgainWithBallast()) {
        fastFree(again);
        std::array<void*, 32> served;
        for (void*& piece : served) {
            piece = fastMalloc(16 * KB);
            fill(piece, 16 * KB, 3);
        }
        served[0] = fastRealloc(served[0], 48 * KB);
        AOT_TEST_CHECK(!bmalloc::api::hasWholeAddressSpaceBallast() && timesToldOfUse);
        for (void* piece : served) {
            AOT_TEST_CHECK(isFilled(piece, 16 * KB, 3));
            fastFree(piece);
        }
    }

    for (void* piece : pieces)
        fastFree(piece);
    for (auto [pages, size] : mappings)
        OSAllocator::releaseDecommitted(pages, size, 0);
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
    bmalloc::api::setAddressSpaceBallastHandler(nullptr);
}

// All the address space is taken, and requests that may not fail have let go of every piece. What the last piece left serves one more.
static void checkRequestsThatMayNotFailLookInWhatLastPieceLeft()
{
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());

    struct Mapping {
        void* pages;
        size_t size;
    };
    Vector<Mapping, 64> mappings;
    for (size_t size = 64 * MB; size >= pageSize(); size /= 2) {
        while (void* pages = OSAllocator::tryReserveUncommitted(size))
            mappings.append({ pages, size });
    }
    Vector<void*> pieces;
    pieces.reserveInitialCapacity(256 * KB);
    while (void* piece = tryFastAlignedMalloc(16, 16 * KB))
        pieces.append(piece);
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());

    // An allocator that does not remember what it refused cannot be asked again.
    if (void* served = bmalloc::api::memalignAgainWithBallast()) {
        pieces.append(served);
        while (bmalloc::api::sizeOfAddressSpaceBallastKept() && pieces.size() < pieces.capacity())
            pieces.append(fastMalloc(16 * KB));
        AOT_TEST_CHECK(!bmalloc::api::sizeOfAddressSpaceBallastKept());
        // Another size needs a page of its own.
        pieces.append(fastMalloc(20 * KB));
    }

    for (void* piece : pieces)
        fastFree(piece);
    for (auto [pages, size] : mappings)
        OSAllocator::releaseDecommitted(pages, size, 0);
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
}

// Each VM that runs compiled code adds to the ballast, and what it added is let go of when the VM is gone.
static void checkBallastGrowsWithVMs()
{
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::sizeOfAddressSpaceBallastKept() == sizeOfAddressSpaceBallast);
    size_t numberOfVMsToPiece = MB / sizeOfAddressSpaceBallastOfVM;
    g_numberOfVMsWithAddressSpaceBallast = 2 * numberOfVMsToPiece + 1;
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::sizeOfAddressSpaceBallastKept() == sizeOfAddressSpaceBallast + 2 * MB && bmalloc::api::hasWholeAddressSpaceBallast());
    g_numberOfVMsWithAddressSpaceBallast = numberOfVMsToPiece - 1;
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::sizeOfAddressSpaceBallastKept() == sizeOfAddressSpaceBallast && bmalloc::api::hasWholeAddressSpaceBallast());
    g_numberOfVMsWithAddressSpaceBallast = 0;
}

// Who asks for address space of its own takes the whole limit for free, and who decides for others takes it for taken.
static void checkAddressSpaceThatCannotBeCountedCountsAsTaken()
{
    struct rlimit addressSpace;
    AOT_TEST_CHECK(!getrlimit(RLIMIT_AS, &addressSpace) && addressSpace.rlim_cur != RLIM_INFINITY);
    size_t counted = addressSpaceLeft();
    AOT_TEST_CHECK(counted && counted < addressSpace.rlim_cur);
    AOT_TEST_CHECK(addressSpaceLeft(UncountedAddressSpace::IsTaken) && addressSpaceLeft(UncountedAddressSpace::IsTaken) < addressSpace.rlim_cur);

    struct rlimit limit;
    AOT_TEST_CHECK(!getrlimit(RLIMIT_NOFILE, &limit));
    struct rlimit noMoreFiles { 0, limit.rlim_max };
    AOT_TEST_CHECK(!setrlimit(RLIMIT_NOFILE, &noMoreFiles));
    size_t leftForOwnRequests = addressSpaceLeft();
    size_t leftForOthers = addressSpaceLeft(UncountedAddressSpace::IsTaken);
    AOT_TEST_CHECK(!setrlimit(RLIMIT_NOFILE, &limit));
    AOT_TEST_CHECK(leftForOwnRequests == addressSpace.rlim_cur);
    AOT_TEST_CHECK(!leftForOthers);
}

// All the address space is taken. What a request that may not fail has let go of is not handed to a large request that may fail, on this thread or another, while small requests are
// served. Once address space has come back, a large request is served and the ballast is whole.
static void checkWhatBallastLetsGoOfStaysWithRequestsThatMayNotFail()
{
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
    bmalloc::api::setAddressSpaceBallastKeeper([] {
        keepAddressSpaceBallast();
        return addressSpaceLeft(UncountedAddressSpace::IsTaken);
    });

    auto largeRequestFindsMemory = [] {
        void* memory = nullptr;
        if (!tryFastMalloc(64 * KB).getValue(memory))
            return false;
        fastFree(memory);
        return true;
    };
    // There will be no room for the stack of a thread, nor for what its first wait asks malloc for.
    BinarySemaphore isReady;
    BinarySemaphore mayAsk;
    bool foundOnOtherThread = true;
    Ref otherThread = Thread::create("large request"_s, [&] {
        Atomic<uint8_t> word { 0 };
        ParkingLot::compareAndPark(&word, static_cast<uint8_t>(1));
        isReady.signal();
        mayAsk.wait();
        foundOnOtherThread = largeRequestFindsMemory();
    });
    isReady.wait();

    struct Mapping {
        void* pages;
        size_t size;
    };
    Vector<Mapping, 64> mappings;
    for (size_t size = 64 * MB; size >= pageSize(); size /= 2) {
        while (void* pages = OSAllocator::tryReserveUncommitted(size))
            mappings.append({ pages, size });
    }
    size_t leftBehindMappings = addressSpaceLeft();
    Vector<void*> pieces;
    pieces.reserveInitialCapacity(256 * KB);
    while (void* piece = tryFastAlignedMalloc(16, 16 * KB))
        pieces.append(piece);
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
    size_t leftBehindPieces = addressSpaceLeft();

    // An allocator that does not remember what it refused cannot be asked again.
    void* served = bmalloc::api::memalignAgainWithBallast();
    AOT_TEST_CHECK(!served || !bmalloc::api::hasWholeAddressSpaceBallast());
    size_t leftBehindServedRequest = addressSpaceLeft();
    bool found = largeRequestFindsMemory();
    size_t leftBehindLargeRequest = addressSpaceLeft();
    mayAsk.signal();
    otherThread->waitForCompletion();
    if (!served)
        return;
    if (found) {
        dataLogLn("Address space left behind the mappings: ", leftBehindMappings, ", behind the pieces: ", leftBehindPieces, ", behind the request that was served: ", leftBehindServedRequest, ", behind the large request: ", leftBehindLargeRequest,
            ". Ballast kept: ", bmalloc::api::sizeOfAddressSpaceBallastKept(), ". Served: ", RawPointer(served), ". ", mappings.size(), " mappings, ", pieces.size(), " pieces.");
    }
    AOT_TEST_CHECK(!found);
    AOT_TEST_CHECK(!foundOnOtherThread);
    // Wherever the memory of further requests that may not fail lies, they open nothing to a large request that may fail.
    std::array<void*, 64> more;
    for (void*& memory : more)
        memory = fastMalloc(16 * KB);
    AOT_TEST_CHECK(!largeRequestFindsMemory());
    std::array<void*, 512> smallRequests;
    for (void*& memory : smallRequests)
        AOT_TEST_CHECK(tryFastMalloc(48).getValue(memory));
    AOT_TEST_CHECK(!bmalloc::api::hasWholeAddressSpaceBallast() && !largeRequestFindsMemory());

    for (void* memory : smallRequests)
        fastFree(memory);
    for (void* memory : more)
        fastFree(memory);
    fastFree(served);
    for (auto [pages, size] : mappings)
        OSAllocator::releaseDecommitted(pages, size, 0);
    AOT_TEST_CHECK(largeRequestFindsMemory() && bmalloc::api::hasWholeAddressSpaceBallast());
    for (void* piece : pieces)
        fastFree(piece);
    bmalloc::api::setAddressSpaceBallastKeeper(nullptr);
}

// All the address space is taken, and a request that may not fail has let go of a piece. Another thread that makes small blocks is served, and is told that it takes what the piece has left. A thread that is exempt is served and not told.
static void checkThreadThatTakesSmallPagesOfWhatBallastLetGoOfIsTold()
{
    using Use = bmalloc::api::UseOfAddressSpaceBallast;
    static std::atomic<unsigned> timesToldOfSmallPage;
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
    bmalloc::api::setAddressSpaceBallastKeeper([] {
        keepAddressSpaceBallast();
        return addressSpaceLeft(UncountedAddressSpace::IsTaken);
    });
    bmalloc::api::setAddressSpaceBallastHandler([](Use use) {
        if (use == Use::TakesWhatPiecesLeft)
            ++timesToldOfSmallPage;
    });

    struct MakerOfSmallBlocks {
        BinarySemaphore isReady;
        BinarySemaphore mayAsk;
        unsigned numberServed { 0 };
        RefPtr<Thread> thread;
    };
    // There will be no room for the stack of a thread, nor for what the allocator keeps for one.
    std::array<MakerOfSmallBlocks, 2> makers;
    for (auto& maker : makers) {
        bool isExempt = &maker == &makers[1];
        maker.thread = Thread::create("small blocks"_s, [&maker, isExempt] {
            fastFree(fastMalloc(48));
            // So does the first wait of a thread.
            Atomic<uint8_t> word { 0 };
            ParkingLot::compareAndPark(&word, static_cast<uint8_t>(1));
            if (isExempt)
                bmalloc::api::exemptThisThreadFromAddressSpaceBallast();
            maker.isReady.signal();
            maker.mayAsk.wait();
            std::array<void*, 16> blocks;
            for (void*& block : blocks) {
                if (!tryFastMalloc(8 * KB).getValue(block))
                    block = nullptr;
                maker.numberServed += !!block;
            }
            for (void* block : blocks)
                fastFree(block);
        });
        maker.isReady.wait();
    }

    struct Mapping {
        void* pages;
        size_t size;
    };
    Vector<Mapping, 64> mappings;
    for (size_t size = 64 * MB; size >= pageSize(); size /= 2) {
        while (void* pages = OSAllocator::tryReserveUncommitted(size))
            mappings.append({ pages, size });
    }
    Vector<void*> pieces;
    pieces.reserveInitialCapacity(256 * KB);
    while (void* piece = tryFastAlignedMalloc(16, 16 * KB))
        pieces.append(piece);
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());
    for (size_t size = 8 * KB; size >= 16; size /= 2) {
        void* piece;
        while (pieces.size() < pieces.capacity() && tryFastMalloc(size).getValue(piece))
            pieces.append(piece);
    }
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast() && !timesToldOfSmallPage);

    // An allocator that does not remember what it refused cannot be asked again.
    void* served = bmalloc::api::mallocAgainWithBallast();
    AOT_TEST_CHECK(!served || !bmalloc::api::hasWholeAddressSpaceBallast());
    std::array<unsigned, 2> timesTold;
    for (size_t i = 0; i < makers.size(); ++i) {
        makers[i].mayAsk.signal();
        makers[i].thread->waitForCompletion();
        timesTold[i] = timesToldOfSmallPage;
    }
    if (served) {
        AOT_TEST_CHECK(makers[0].numberServed && timesTold[0]);
        AOT_TEST_CHECK(makers[1].numberServed && timesTold[1] == timesTold[0]);
        fastFree(served);
    }

    for (void* piece : pieces)
        fastFree(piece);
    for (auto [pages, size] : mappings)
        OSAllocator::releaseDecommitted(pages, size, 0);
    bmalloc::api::setAddressSpaceBallastHandler(nullptr);
    bmalloc::api::setAddressSpaceBallastKeeper(nullptr);
}

// All the address space is taken. The first request of a thread also needs what the allocator keeps for the thread. Where the request may not fail, both are served.
static void checkFirstRequestsOfThreadsAreServed()
{
    static std::atomic<bool> mayAsk;
    static std::atomic<bool> mayEnd;
    static std::atomic<unsigned> numberServed;
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());

    // More threads than the allocator has room for in the pages it has.
    std::array<RefPtr<Thread>, 48> threads;
    for (auto& thread : threads) {
        thread = Thread::create("first request"_s, [] {
            while (!mayAsk.load())
                usleep(1000);
            void* block = fastMalloc(48);
            fill(block, 48, 5);
            ++numberServed;
            while (!mayEnd.load())
                usleep(1000);
            AOT_TEST_CHECK(isFilled(block, 48, 5));
            fastFree(block);
        });
    }

    struct Mapping {
        void* pages;
        size_t size;
    };
    Vector<Mapping, 64> mappings;
    for (size_t size = 64 * MB; size >= pageSize(); size /= 2) {
        while (void* pages = OSAllocator::tryReserveUncommitted(size))
            mappings.append({ pages, size });
    }
    Vector<void*> pieces;
    pieces.reserveInitialCapacity(256 * KB);
    while (void* piece = tryFastAlignedMalloc(16, 16 * KB))
        pieces.append(piece);
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());
    for (size_t size = 8 * KB; size >= 16; size /= 2) {
        void* piece;
        while (pieces.size() < pieces.capacity() && tryFastMalloc(size).getValue(piece))
            pieces.append(piece);
    }
    AOT_TEST_CHECK(pieces.size() < pieces.capacity());
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());

    // An allocator that does not remember what it refused cannot be asked again.
    if (void* served = bmalloc::api::mallocAgainWithBallast()) {
        mayAsk.store(true);
        while (numberServed.load() < threads.size())
            usleep(1000);
        fastFree(served);
    }

    for (void* piece : pieces)
        fastFree(piece);
    for (auto [pages, size] : mappings)
        OSAllocator::releaseDecommitted(pages, size, 0);
    mayAsk.store(true);
    mayEnd.store(true);
    for (auto& thread : threads)
        thread->waitForCompletion();
}
#endif
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
    AOT_TEST_CHECK(static_cast<uint64_t>(indices.first) + indices.count <= (cage.start() - cage.base() + cage.size()) >> MarkedBlockCage::shift);

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

#if OS(LINUX)
extern "C" char _end;

static void checkNoAddressInImageNamesCell()
{
    uintptr_t endOfImage = std::bit_cast<uintptr_t>(&_end);
    if (endOfImage > std::numeric_limits<uint32_t>::max())
        return;
    TestVM test;
    test.createRealm();
    MarkedBlockCage::Indices indices = test.cage().usedIndices();
    AOT_TEST_CHECK(indices.count && indices.first >= endOfImage);
    AOT_TEST_CHECK(test.cage().start() - test.cage().base() == MarkedBlockCage::distanceOfStartFromBase());
}
#endif

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
    uintptr_t middle = cage.start() + cage.size() / 2;
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

// In the scope, a fallible allocation in a full cage uses the reserve instead of returning null.
static void checkInfallibleScopeUsesReserve()
{
    TestVM test;
    VM& vm = test.vm();
    MarkedBlockCage& cage = test.cage();
    VMTraps& traps = vm.traps();
    constexpr unsigned shortLength = 1;
    constexpr unsigned longLength = 4 * KB / sizeof(JSBigInt::Digit);
    constexpr unsigned lengthOfLargeCell = 900 * KB / sizeof(JSBigInt::Digit);
    {
        DeferGCForAWhile deferGC(vm);
        while (JSBigInt::tryCreateWithLength(vm, shortLength)) { }
        size_t whole = cage.reserveSize();
        AOT_TEST_CHECK(whole && !JSBigInt::tryCreateWithLength(vm, longLength));
        AOT_TEST_CHECK(cage.reserveSize() == whole && !traps.needsOutOfMemoryError() && !cage.allocationsAreInfallible());
        {
            MarkedBlockCage::InfallibleAllocationScope scope(cage);
            AOT_TEST_CHECK(cage.allocationsAreInfallible());
            for (unsigned length : { shortLength, longLength }) {
                JSBigInt* bigInt = JSBigInt::tryCreateWithLength(vm, length);
                AOT_TEST_CHECK(bigInt && cage.contains(bigInt) && !bigInt->isPreciseAllocation());
            }
            JSBigInt* large = JSBigInt::tryCreateWithLength(vm, lengthOfLargeCell);
            AOT_TEST_CHECK(large && cage.contains(large) && large->isPreciseAllocation());
        }
        AOT_TEST_CHECK(!cage.allocationsAreInfallible() && cage.reserveSize() < whole && traps.needsOutOfMemoryError() && cage.owesCollection());
        AOT_TEST_CHECK(!cage.tryTakeBackOutOfMemoryError(vm) && traps.needsOutOfMemoryError());
    }
    AOT_TEST_CHECK(cage.tryTakeBackOutOfMemoryError(vm));
    AOT_TEST_CHECK(!traps.needsOutOfMemoryError() && !cage.owesCollection() && cage.reserveSize());
}

static void fillSmallSizeClasses(VM& vm)
{
    for (unsigned length = 1; length <= 64; ++length) {
        while (JSBigInt::tryCreateWithLength(vm, length)) { }
    }
}

static void checkConstantsAreCompiledInFullCage()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    DeferGCForAWhile deferGC(test.vm());
    fillSmallSizeClasses(test.vm());
    NakedPtr<Exception> exception;
    JSValue result = evaluate(realm, makeSource("[1.5, 2.5].length + ['a', 'b', 'c'].length + Number(123456789012345678901234567890n % 7n)"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted), JSValue(), exception);
    AOT_TEST_CHECK(!exception && result.isInt32() && result.asInt32() == 5);
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

static constexpr size_t sizeOfFrameAboveDeadStack = 256 * KB;

static NEVER_INLINE uintptr_t createErrorInDeeperFrame(MarkedBlockCage& cage)
{
    volatile char frame[sizeOfFrameAboveDeadStack];
    frame[0] = 0;
    cage.didCreateOutOfMemoryError();
    return std::bit_cast<uintptr_t>(&frame[0]);
}

static NEVER_INLINE void checkDeadStackInDeeperFrame(TestVM& test, bool isBelowSoftLimit)
{
    volatile char frame[sizeOfFrameAboveDeadStack];
    frame[0] = 0;
    uintptr_t endOfFrame = std::bit_cast<uintptr_t>(&frame[0]);
    VM& vm = test.vm();
    AOT_TEST_CHECK((endOfFrame < std::bit_cast<uintptr_t>(vm.softStackLimit())) == isBelowSoftLimit);
    uintptr_t endOfFrameOfError = createErrorInDeeperFrame(test.cage());
    AOT_TEST_CHECK(test.cage().hasDeadStackToClear());
    uintptr_t lowest = std::bit_cast<uintptr_t>(test.cage().takeLowestAddressOfDeadStack(vm));
    AOT_TEST_CHECK(!test.cage().hasDeadStackToClear());
    AOT_TEST_CHECK(lowest < endOfFrameOfError && lowest >= std::bit_cast<uintptr_t>(vm.stackLimit()));
    lowest = std::bit_cast<uintptr_t>(test.cage().takeLowestAddressOfDeadStack(vm));
    AOT_TEST_CHECK(lowest > endOfFrameOfError && lowest < endOfFrame);
}

// The dead stack begins below the frame that made the error and ends below the frame that asks, wherever the soft limit lies.
static void checkDeadStackLiesBelowTheFrameThatAsks()
{
    TestVM test;
    checkDeadStackInDeeperFrame(test, false);
    size_t softReservedZoneSize = test.vm().updateSoftReservedZoneSize(Options::maxPerThreadStackUsage());
    checkDeadStackInDeeperFrame(test, true);
    test.vm().updateSoftReservedZoneSize(softReservedZoneSize);
}

// A heap that stays almost full of live objects finds no room again and again, with little allocated in between. The fourth time in a row the error is owed.
static void checkErrorIsOwedOnceCollectionsBuyNextToNothing()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();
    VMTraps& traps = vm.traps();
    auto much = [&] {
        return vm.heap.objectSpace().capacity() / 64;
    };
    auto isOwedWhenNoRoomIsFoundAfterAllocating = [&](size_t bytes) {
        if (bytes)
            vm.heap.didAllocate(bytes);
        test.cage().didFindNoRoom(vm, MarkedBlockCage::ExhaustedHeap::Cells);
        return traps.needsOutOfMemoryError();
    };
    auto isOwedAfterThreeTimesLittle = [&] {
        bool isOwed = false;
        for (unsigned times = 0; times < 3; ++times)
            isOwed |= isOwedWhenNoRoomIsFoundAfterAllocating(much() - 1);
        return isOwed;
    };
    AOT_TEST_CHECK(much() > 1);

    AOT_TEST_CHECK(!isOwedWhenNoRoomIsFoundAfterAllocating(much()) && !isOwedAfterThreeTimesLittle());
    // One request tries again after its collection.
    AOT_TEST_CHECK(!isOwedWhenNoRoomIsFoundAfterAllocating(0) && !isOwedWhenNoRoomIsFoundAfterAllocating(0));
    AOT_TEST_CHECK(isOwedWhenNoRoomIsFoundAfterAllocating(1));
    traps.setNeedsOutOfMemoryError(false);

    // A collection that bought much starts the count anew.
    AOT_TEST_CHECK(!isOwedWhenNoRoomIsFoundAfterAllocating(much()) && !isOwedAfterThreeTimesLittle());
    AOT_TEST_CHECK(!isOwedWhenNoRoomIsFoundAfterAllocating(much()) && !isOwedAfterThreeTimesLittle());
    AOT_TEST_CHECK(isOwedWhenNoRoomIsFoundAfterAllocating(much() - 1));
    traps.setNeedsOutOfMemoryError(false);

    // So does an error that the program is given in another way.
    AOT_TEST_CHECK(!isOwedWhenNoRoomIsFoundAfterAllocating(much()) && !isOwedAfterThreeTimesLittle());
    AOT_TEST_CHECK(createOutOfMemoryError(realm));
    AOT_TEST_CHECK(!isOwedAfterThreeTimesLittle());
    AOT_TEST_CHECK(isOwedWhenNoRoomIsFoundAfterAllocating(much() - 1));
    traps.setNeedsOutOfMemoryError(false);
}

// A request for storage that fails gives the program an error of its own. Cells in a range of a fixed size have nothing to do with it. Cells in a range that grows lack the same room.
template<bool isSettled>
static void checkErrorOfAnotherRequestAndErrorOwedForCells()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();
    auto findNoRoomAfterAllocating = [&](size_t bytes) {
        vm.heap.didAllocate(bytes);
        test.cage().didFindNoRoom(vm, MarkedBlockCage::ExhaustedHeap::Cells);
    };
    // Much has been allocated since the last time, which starts the count anew.
    findNoRoomAfterAllocating(vm.heap.objectSpace().capacity() / 64);
    for (unsigned times = 0; times < 4; ++times)
        findNoRoomAfterAllocating(1);
    AOT_TEST_CHECK(vm.traps().needsOutOfMemoryError());
    AOT_TEST_CHECK(createOutOfMemoryError(realm));
    AOT_TEST_CHECK(vm.traps().needsOutOfMemoryError() == !isSettled);
    vm.traps().setNeedsOutOfMemoryError(false);
}

// The error that barren collections owe is offered to a poll first: one more full collection runs there. If little can be allocated behind that one too, the error is owed again and is anybody's to throw.
static void checkErrorOwedByBarrenCollectionsIsOfferedToPollOnce()
{
    TestVM test;
    test.createRealm();
    VM& vm = test.vm();
    VMTraps& traps = vm.traps();
    MarkedBlockCage& cage = test.cage();
    auto findNoRoomAfterAllocating = [&](size_t bytes) {
        vm.heap.didAllocate(bytes);
        cage.didFindNoRoom(vm, MarkedBlockCage::ExhaustedHeap::Cells);
    };
    findNoRoomAfterAllocating(vm.heap.objectSpace().capacity() / 64);
    for (unsigned times = 0; times < 4; ++times) {
        AOT_TEST_CHECK(!traps.needsOutOfMemoryError() && !cage.errorWaitsForPoll());
        findNoRoomAfterAllocating(1);
    }
    AOT_TEST_CHECK(traps.needsOutOfMemoryError() && cage.errorWaitsForPoll());
    AOT_TEST_CHECK(cage.tryTakeBackOutOfMemoryErrorAtPoll(vm));
    AOT_TEST_CHECK(!traps.needsOutOfMemoryError() && !cage.errorWaitsForPoll());

    findNoRoomAfterAllocating(1);
    AOT_TEST_CHECK(traps.needsOutOfMemoryError() && !cage.errorWaitsForPoll());
    AOT_TEST_CHECK(!cage.tryTakeBackOutOfMemoryErrorAtPoll(vm) && traps.needsOutOfMemoryError());
    traps.setNeedsOutOfMemoryError(false);

    // No poll came by before the next barren collection.
    findNoRoomAfterAllocating(vm.heap.objectSpace().capacity() / 64);
    for (unsigned times = 0; times < 4; ++times)
        findNoRoomAfterAllocating(1);
    AOT_TEST_CHECK(traps.needsOutOfMemoryError() && cage.errorWaitsForPoll());
    findNoRoomAfterAllocating(1);
    AOT_TEST_CHECK(traps.needsOutOfMemoryError() && !cage.errorWaitsForPoll());
    traps.setNeedsOutOfMemoryError(false);
}

static void runOutOfStructures(VM& vm)
{
    // A whole reserve, as the allocator keeps once a quarter of its heap is in use.
    StructureAlignedMemoryAllocator::releaseReserve();
    AOT_TEST_CHECK(StructureAlignedMemoryAllocator::tryKeepReleasedReserveAgain());

    AOT_TEST_CHECK(StructureAlignedMemoryAllocator::releaseReserve());
    vm.heap.cage().addExhaustedHeap(MarkedBlockCage::ExhaustedHeap::Structures);
    vm.traps().setNeedsOutOfMemoryError(true);
}

static bool throwErrorIfOwed(VM& vm, JSGlobalObject* realm)
{
    auto topScope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool hasThrown = [&] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return throwOutOfMemoryErrorIfOwed(vm, realm, scope);
    }();
    AOT_TEST_CHECK(hasThrown == !!topScope.exception());
    topScope.clearException();
    return hasThrown;
}

enum class FirstToRunOut : uint8_t { Structures, Storage };
enum class ErrorGiven : uint8_t { TheOneOwed, OneOfAnotherRequest };

// Structures and storage both run out before an error can be thrown: one error is owed for the two. The allocator of Structures has given out its reserve, and keeps one again once it hears that the error was thrown.
template<FirstToRunOut first, ErrorGiven given>
static void checkErrorOwedForStructuresAndStorageIsHeardOfByBoth()
{
    TestVM test;
    JSGlobalObject* realm = test.createRealm();
    VM& vm = test.vm();

    if constexpr (first == FirstToRunOut::Storage)
        MarkedBlockCage::didUseAddressSpaceBallast(vm);
    runOutOfStructures(vm);
    if constexpr (first == FirstToRunOut::Structures)
        MarkedBlockCage::didUseAddressSpaceBallast(vm);
    AOT_TEST_CHECK(vm.traps().needsOutOfMemoryError() && test.cage().describeExhaustedLimit().contains("Structures"_s));

    if constexpr (given == ErrorGiven::TheOneOwed)
        AOT_TEST_CHECK(throwErrorIfOwed(vm, realm));
    else
        AOT_TEST_CHECK(createOutOfMemoryError(realm));
    AOT_TEST_CHECK(!vm.traps().needsOutOfMemoryError() && !test.cage().describeExhaustedLimit().contains("Structures"_s));
    // Only an allocator that still waits for the error takes back what it gave out.
    AOT_TEST_CHECK(!StructureAlignedMemoryAllocator::tryKeepReleasedReserveAgain());
}

#if OS(LINUX) && !ASAN_ENABLED && USE(MIMALLOC)
// The collection at a poll makes room. The error is taken back, and both have again what they gave out.
template<FirstToRunOut first>
static void checkErrorOwedForStructuresAndStorageIsTakenBackForBoth()
{
    keepAddressSpaceBallast();
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast());
    TestVM test;
    test.createRealm();
    VM& vm = test.vm();

    if constexpr (first == FirstToRunOut::Storage)
        MarkedBlockCage::didUseAddressSpaceBallast(vm);
    runOutOfStructures(vm);
    if constexpr (first == FirstToRunOut::Structures)
        MarkedBlockCage::didUseAddressSpaceBallast(vm);

    AOT_TEST_CHECK(test.cage().tryTakeBackOutOfMemoryErrorAtPoll(vm) && !vm.traps().needsOutOfMemoryError());
    AOT_TEST_CHECK(bmalloc::api::hasWholeAddressSpaceBallast() && StructureAlignedMemoryAllocator::releaseReserve());
}
#endif

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

#if !ENABLE(MALLOC_HEAP_BREAKDOWN)
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
#endif

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
#if OS(LINUX) && USE(MIMALLOC)
        { "heap-cage/reserved-range/releasing-free-memory-purges-no-other-range"_s, testReleasingFreeMemoryPurgesNoOtherRange },
#endif
        { "heap-cage/reserved-range/random-windows-lie-far-apart"_s, testRandomWindowsLieFarApart },
        { "heap-cage/cage/encode-decode-round-trip"_s, testEncodeDecodeRoundTrip },
        { "heap-cage/cage/base-of-any-cell"_s, testBaseOfAnyCell },
        { "heap-cage/cage/off-without-compiled-code"_s, testCageIsOffWithoutCompiledCode },
#if OS(LINUX) && !ASAN_ENABLED
        { markStackTest, testMarkStackThatCannotGrowSaysWhy },
        { "heap-cage/cage/shared-reserve-is-whole-again-behind-a-cage-that-threw"_s, runInProcessWithSharedRange<checkSharedReserveIsWholeAgainBehindCageThatThrew> },
        { "heap-cage/cage/shared-reserve-is-whole-again-behind-a-cage-that-stays"_s, runInProcessWithSharedRange<checkSharedReserveIsWholeAgainBehindCageThatStays> },
        { "heap-cage/cage/shared-reserve-holds-large-cells-of-several-cages"_s, runInProcessWithSharedRange<checkSharedReserveHoldsLargeCellsOfSeveralCages> },
        { "heap-cage/cage/cell-in-a-full-range-grows-into-the-shared-reserve"_s, runInProcessWithSharedRange<checkCellInFullRangeGrowsIntoSharedReserve> },
        { "heap-cage/cage/no-address-in-the-image-names-a-cell-under-a-limit-on-address-space"_s, runInProcessWithSharedRange<checkNoAddressInImageNamesCell> },
        { "heap-cage/cage/error-of-another-request-settles-cells-under-a-limit-on-address-space"_s, runInProcessWithSharedRange<checkErrorOfAnotherRequestAndErrorOwedForCells<true>> },
#if USE(MIMALLOC)
        { "heap-cage/tables/a-table-that-finds-no-memory-to-grow-stays-as-it-was"_s, checkTableThatFindsNoMemoryToGrowStaysAsItWas },
        { "heap-cage/ballast/requests-that-may-not-fail-use-it"_s, runInProcessWithSharedRange<checkRequestsThatMayNotFailUseBallast> },
        { "heap-cage/ballast/requests-that-may-not-fail-look-in-what-the-last-piece-left"_s, runInProcessWithSharedRange<checkRequestsThatMayNotFailLookInWhatLastPieceLeft> },
        { "heap-cage/ballast/it-grows-with-the-vms"_s, runInProcessWithSharedRange<checkBallastGrowsWithVMs> },
        { "heap-cage/ballast/address-space-that-cannot-be-counted-counts-as-taken"_s, runInProcessWithSharedRange<checkAddressSpaceThatCannotBeCountedCountsAsTaken> },
        { "heap-cage/ballast/what-it-lets-go-of-stays-with-requests-that-may-not-fail"_s, runInProcessWithSharedRange<checkWhatBallastLetsGoOfStaysWithRequestsThatMayNotFail> },
        { "heap-cage/ballast/a-thread-that-takes-small-pages-of-what-it-let-go-of-is-told"_s, runInProcessWithSharedRange<checkThreadThatTakesSmallPagesOfWhatBallastLetGoOfIsTold> },
        { "heap-cage/ballast/first-requests-of-threads-are-served"_s, runInProcessWithSharedRange<checkFirstRequestsOfThreadsAreServed> },
        { "heap-cage/cage/error-owed-for-structures-then-storage-is-taken-back-for-both"_s, runInProcessWithSharedRange<checkErrorOwedForStructuresAndStorageIsTakenBackForBoth<FirstToRunOut::Structures>> },
        { "heap-cage/cage/error-owed-for-storage-then-structures-is-taken-back-for-both"_s, runInProcessWithSharedRange<checkErrorOwedForStructuresAndStorageIsTakenBackForBoth<FirstToRunOut::Storage>> },
#endif
#endif
        { "heap-cage/cage/used-indices-cover-allocations"_s, withCage<checkUsedIndicesCoverAllocations> },
        { "heap-cage/cage/every-cell-is-inside"_s, withCage<checkEveryCellIsInside> },
#if OS(LINUX)
        { "heap-cage/cage/no-address-in-the-image-names-a-cell"_s, withCage<checkNoAddressInImageNamesCell> },
#endif
        { "heap-cage/cage/precise-cell-is-inside"_s, withCage<checkPreciseCellIsInside> },
        { "heap-cage/cage/lower-tier-cell-is-inside"_s, withCage<checkLowerTierCellIsInside> },
        { "heap-cage/cage/butterfly-is-outside"_s, withCage<checkButterflyIsOutside> },
        { "heap-cage/cage/second-heap-gets-its-own-base"_s, withCage<checkSecondHeapGetsItsOwnBase> },
        { "heap-cage/cage/range-is-reused"_s, withCage<checkRangeIsReused> },
        { "heap-cage/cage/reserve-is-kept-once-half-full"_s, withCage<checkReserveIsKeptOnceHalfFull> },
        { "heap-cage/cage/infallible-scope-uses-reserve"_s, withCage<checkInfallibleScopeUsesReserve> },
        { "heap-cage/cage/constants-are-compiled-in-full-cage"_s, withCage<checkConstantsAreCompiledInFullCage> },
        { "heap-cage/cage/error-waits-at-function-entries-alone"_s, withCage<checkErrorWaitsAtFunctionEntriesAlone> },
        { "heap-cage/cage/dead-stack-lies-below-the-frame-that-asks"_s, withCage<checkDeadStackLiesBelowTheFrameThatAsks> },
        { "heap-cage/cage/error-is-owed-once-collections-buy-next-to-nothing"_s, withCage<checkErrorIsOwedOnceCollectionsBuyNextToNothing> },
        { "heap-cage/cage/error-owed-by-barren-collections-is-offered-to-a-poll-once"_s, withCage<checkErrorOwedByBarrenCollectionsIsOfferedToPollOnce> },
        { "heap-cage/cage/error-owed-for-structures-then-storage-is-heard-of-by-both"_s, withCage<checkErrorOwedForStructuresAndStorageIsHeardOfByBoth<FirstToRunOut::Structures, ErrorGiven::TheOneOwed>> },
        { "heap-cage/cage/error-owed-for-storage-then-structures-is-heard-of-by-both"_s, withCage<checkErrorOwedForStructuresAndStorageIsHeardOfByBoth<FirstToRunOut::Storage, ErrorGiven::TheOneOwed>> },
        { "heap-cage/cage/error-of-another-request-settles-structures-then-storage"_s, withCage<checkErrorOwedForStructuresAndStorageIsHeardOfByBoth<FirstToRunOut::Structures, ErrorGiven::OneOfAnotherRequest>> },
        { "heap-cage/cage/error-of-another-request-settles-storage-then-structures"_s, withCage<checkErrorOwedForStructuresAndStorageIsHeardOfByBoth<FirstToRunOut::Storage, ErrorGiven::OneOfAnotherRequest>> },
        { "heap-cage/cage/error-of-another-request-leaves-what-is-owed-for-cells"_s, withCage<checkErrorOfAnotherRequestAndErrorOwedForCells<false>> },
        { "heap-cage/cage/blocks-to-fill-are-fresh-blocks-of-the-cage"_s, withCage<checkBlocksToFillAreFreshBlocksOfTheCage> },
#if !ENABLE(MALLOC_HEAP_BREAKDOWN)
        { "heap-cage/cage/other-allocator-warms-up-with-the-cage"_s, withCage<checkOtherAllocatorWarmsUpWithTheCage> },
#endif
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
