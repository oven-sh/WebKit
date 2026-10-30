/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCheckedFrameCells.h"

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTModuleTable.h"
#include "AOTRuntimeValidation.h"
#include "AOTSchemaRuntimeOperations.h"
#include "AOTVMState.h"
#include "ConservativeRoots.h"
#include "JSCInlines.h"
#include "MarkedBlock.h"
#include <atomic>
#include <mutex>
#include <wtf/DataLog.h>
#include <wtf/HashFunctions.h>
#include <wtf/Lock.h>
#include <wtf/PageBlock.h>
#include <wtf/StackPointer.h>
#include <wtf/TZoneMallocInlines.h>

#if !OS(WINDOWS)
#include <sys/mman.h>
#endif

namespace JSC { namespace AOT {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CheckedFrameCells);

namespace {

enum class GranuleState : uint8_t { Unused, Live, Ended };

struct Granule {
    std::atomic<GranuleState> state { GranuleState::Unused };
    uint32_t size { 0 };
    const void* slot { nullptr };
    const void* wordOfFrame { nullptr };
    const CheckedFrameCells* owner { nullptr };
    const Image* image { nullptr };
    CheckedFrameCells::Allocation allocation;
};

// One for the process. A granule begins where a MarkedBlock would, with a page that is never readable: that is where a look for the block of a cell looks. The cell begins with the next page.
struct Range {
    size_t granuleSize { 0 };
    size_t offsetOfCell { 0 };
    uint32_t numberOfGranules { 0 };
    Granule* granules { nullptr };
    std::atomic<uint32_t> next { 0 };
    std::atomic<uint32_t> numberOfLiveCells { 0 };

    uint8_t* cellOf(uint32_t granule) const { return std::bit_cast<uint8_t*>(CheckedFrameCells::startOfRange + granule * granuleSize + offsetOfCell); }
    size_t roomForCell() const { return granuleSize - offsetOfCell; }
};

} // anonymous namespace

// Each is a mapping of its own, and a process may have so many mappings and no more.
static constexpr uint32_t maxNumberOfLiveCells = 8192;

static constexpr uint32_t numberOfBirthsThatAreAllChecked = 1024;
static constexpr uint32_t intervalOfCheckedBirths = 64;

static Range s_range;
static Lock s_listLock;
static CheckedFrameCells* s_first WTF_GUARDED_BY_LOCK(s_listLock);
std::atomic<bool> CheckedFrameCells::s_isInUse;

#if OS(WINDOWS)
// There is no range, so every cell stays in its frame.
static void* tryReserveAt(uintptr_t, size_t, bool)
{
    return nullptr;
}

static bool tryMakeAccessible(uint8_t*, size_t)
{
    return false;
}

static void makeInaccessible(uint8_t*, size_t)
{
}
#else
#if defined(MAP_NORESERVE)
static constexpr int flagsOfMapping = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#else
static constexpr int flagsOfMapping = MAP_PRIVATE | MAP_ANON;
#endif

static void* tryReserveAt(uintptr_t address, size_t size, bool isAccessible)
{
    void* wanted = std::bit_cast<void*>(address);
    void* result = mmap(wanted, size, isAccessible ? PROT_READ | PROT_WRITE : PROT_NONE, flagsOfMapping, -1, 0);
    if (result == MAP_FAILED)
        return nullptr;
    if (address && result != wanted) {
        munmap(result, size);
        return nullptr;
    }
    return result;
}

// Fails where the process may have no more mappings. The pages hold zeros.
static bool tryMakeAccessible(uint8_t* cell, size_t size)
{
    return !mprotect(cell, WTF::roundUpToMultipleOf(pageSize(), size), PROT_READ | PROT_WRITE);
}

// A new mapping in place of the old one, so the pages go back to the system.
static void makeInaccessible(uint8_t* cell, size_t size)
{
    size = WTF::roundUpToMultipleOf(pageSize(), size);
    if (mmap(cell, size, PROT_NONE, flagsOfMapping | MAP_FIXED, -1, 0) == MAP_FAILED)
        mprotect(cell, size, PROT_NONE);
}
#endif

// The words of a frame of compiled code.
SUPPRESS_ASAN static uint64_t readWordOfFrame(const void* word)
{
    return *static_cast<const uint64_t*>(word);
}

SUPPRESS_ASAN static void writeWordOfFrame(void* word, uint64_t value)
{
    *static_cast<uint64_t*>(word) = value;
}

static const Range& range()
{
    static std::once_flag once;
    std::call_once(once, [] {
        s_range.offsetOfCell = pageSize();
        s_range.granuleSize = std::max<size_t>(MarkedBlock::blockSize, 2 * pageSize());
        // Under a limit on address space a smaller range has to do.
        for (size_t size = size_t(1) << CheckedFrameCells::bitsOfOffsetInRange; size >= 16 * MB; size /= 4) {
            if (!tryReserveAt(CheckedFrameCells::startOfRange, size, false))
                continue;
            uint32_t count = static_cast<uint32_t>(size / s_range.granuleSize);
            s_range.granules = static_cast<Granule*>(tryReserveAt(0, count * sizeof(Granule), true));
            if (s_range.granules)
                s_range.numberOfGranules = count;
            break;
        }
    });
    return s_range;
}

CheckedFrameCells::CheckedFrameCells(VM& vm)
    : m_vm(vm)
    , m_image(*vm.aotState().image)
{
}

CheckedFrameCells::~CheckedFrameCells()
{
    while (!m_liveGranules.isEmpty())
        end(m_liveGranules.takeLast());
}

CheckedFrameCells* CheckedFrameCells::find(VM& vm)
{
    Locker locker { s_listLock };
    for (CheckedFrameCells* cells = s_first; cells; cells = cells->m_next) {
        if (&cells->m_vm == &vm)
            return cells;
    }
    return nullptr;
}

CheckedFrameCells& CheckedFrameCells::ensure(VM& vm)
{
    if (CheckedFrameCells* cells = find(vm)) [[likely]]
        return *cells;
    auto* cells = new CheckedFrameCells(vm);
    Locker locker { s_listLock };
    cells->m_next = std::exchange(s_first, cells);
    s_isInUse.store(true, std::memory_order_relaxed);
    return *cells;
}

void CheckedFrameCells::willDestroy(VM& vm)
{
    CheckedFrameCells* found = nullptr;
    {
        Locker locker { s_listLock };
        for (CheckedFrameCells** link = &s_first; *link; link = &(*link)->m_next) {
            if (&(*link)->m_vm != &vm)
                continue;
            found = std::exchange(*link, (*link)->m_next);
            break;
        }
    }
    delete found;
}

void CheckedFrameCells::end(uint32_t index)
{
    Granule& granule = s_range.granules[index];
    RELEASE_ASSERT(granule.state.load(std::memory_order_relaxed) == GranuleState::Live && granule.owner == this);
    makeInaccessible(s_range.cellOf(index), granule.size);
    s_range.numberOfLiveCells.fetch_sub(1, std::memory_order_relaxed);
    granule.state.store(GranuleState::Ended, std::memory_order_release);
}

void* CheckedFrameCells::make(void* slot, const void* wordOfFrame, size_t size, const Allocation& allocation)
{
    const Range& range = AOT::range();
    didLeaveFramesBelow(currentStackPointer());

    // The slot remembers the cell that was last made for it. A cell that is made again takes the place of the earlier one.
    static_assert(sizeof(uint64_t) <= MarkedBlock::atomSize);
    if (uint64_t last = readWordOfFrame(slot); last < range.numberOfGranules) {
        const Granule& granule = range.granules[last];
        if (granule.state.load(std::memory_order_relaxed) == GranuleState::Live && granule.owner == this && granule.slot == slot) {
            size_t position = m_liveGranules.reverseFind(static_cast<uint32_t>(last));
            RELEASE_ASSERT(position != notFound);
            m_liveGranules.removeAt(position);
            end(static_cast<uint32_t>(last));
        }
    }

    if (size > range.roomForCell() || !range.numberOfGranules || range.numberOfLiveCells.load(std::memory_order_relaxed) >= maxNumberOfLiveCells)
        return slot;
    uint32_t birth = m_numberOfBirths[pairIntHash(allocation.function, allocation.value) % m_numberOfBirths.size()]++;
    if (birth >= numberOfBirthsThatAreAllChecked && birth % intervalOfCheckedBirths)
        return slot;
    for (uint32_t attempt = 0; attempt < range.numberOfGranules; ++attempt) {
        uint32_t index = s_range.next.fetch_add(1, std::memory_order_relaxed) % range.numberOfGranules;
        Granule& granule = range.granules[index];
        GranuleState state = granule.state.load(std::memory_order_acquire);
        if (state == GranuleState::Live || !granule.state.compare_exchange_strong(state, GranuleState::Live, std::memory_order_acquire))
            continue;
        uint8_t* cell = range.cellOf(index);
        if (!tryMakeAccessible(cell, size)) {
            granule.state.store(state, std::memory_order_release);
            return slot;
        }
        granule.size = static_cast<uint32_t>(size);
        granule.slot = slot;
        granule.wordOfFrame = wordOfFrame;
        granule.owner = this;
        granule.image = &m_image;
        granule.allocation = allocation;
        s_range.numberOfLiveCells.fetch_add(1, std::memory_order_relaxed);
        m_liveGranules.append(index);
        writeWordOfFrame(slot, index);
        return cell;
    }
    return slot;
}

void CheckedFrameCells::didLeaveFrameOf(const void* wordOfFrame)
{
    while (!m_liveGranules.isEmpty() && s_range.granules[m_liveGranules.last()].wordOfFrame <= wordOfFrame)
        end(m_liveGranules.takeLast());
}

void CheckedFrameCells::didLeaveFramesBelow(const void* address)
{
    while (!m_liveGranules.isEmpty() && s_range.granules[m_liveGranules.last()].wordOfFrame < address)
        end(m_liveGranules.takeLast());
}

template<typename Visitor>
void CheckedFrameCells::visitLiveCellsWith(Visitor& visitor)
{
    if (m_liveGranules.isEmpty())
        return;
    ConservativeRoots roots(m_vm.heap);
    for (uint32_t index : m_liveGranules) {
        uint8_t* cell = s_range.cellOf(index);
        roots.add(cell, cell + WTF::roundUpToMultipleOf<sizeof(void*)>(s_range.granules[index].size));
    }
    visitor.append(roots);
}

void CheckedFrameCells::visitLiveCells(SlotVisitor& visitor)
{
    visitLiveCellsWith(visitor);
}

void CheckedFrameCells::visitLiveCells(AbstractSlotVisitor& visitor)
{
    visitLiveCellsWith(visitor);
}

void CheckedFrameCells::report(const void* address)
{
    uint64_t index = s_range.granuleSize ? (std::bit_cast<uintptr_t>(address) - startOfRange) / s_range.granuleSize : 0;
    if (index >= s_range.numberOfGranules || s_range.granules[index].state.load(std::memory_order_acquire) == GranuleState::Unused) {
        dataLogLn("AOT validation: ", nameOf(ValidationFailureKind::EscapedFrameCell), " f- v-");
        return;
    }
    const Granule& granule = s_range.granules[index];
    const Allocation& allocation = granule.allocation;
    // The ids come first: the path is looked up in the tables of the image.
    dataLog("AOT validation: ", nameOf(ValidationFailureKind::EscapedFrameCell), " f", allocation.function, " v", allocation.value, ", ");
    if (!granule.image->isObfuscated() && ModuleID(allocation.module))
        dataLog(ModuleTable(granule.image->programTables()).pathTextForLineColumns(ModuleID(allocation.module)), ":");
    dataLogLn(allocation.line, ":", allocation.column);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTMakeCheckedFrameCell, void*, (Instance* instance, void* slot, void* wordOfFrame, uint64_t sizeAndFunction, uint64_t valueAndModule, uint64_t lineAndColumn))
{
    CheckedFrameCells::Allocation allocation {
        .function = static_cast<uint32_t>(sizeAndFunction >> 32),
        .value = static_cast<uint32_t>(valueAndModule),
        .module = static_cast<uint32_t>(valueAndModule >> 32),
        .line = static_cast<uint32_t>(lineAndColumn),
        .column = static_cast<uint32_t>(lineAndColumn >> 32),
    };
    return CheckedFrameCells::ensure(*instance->vm).make(slot, wordOfFrame, static_cast<uint32_t>(sizeAndFunction), allocation);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTLeaveFrameOfCheckedCells, void, (Instance* instance, void* wordOfFrame))
{
    if (CheckedFrameCells* cells = CheckedFrameCells::find(*instance->vm))
        cells->didLeaveFrameOf(wordOfFrame);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTReportEscapedFrameCell, void, (Instance*, void* cell))
{
    static Lock lock;
    lock.lock();
    CheckedFrameCells::report(cell);
    CRASH_WITH_INFO(static_cast<uint64_t>(ValidationFailureKind::EscapedFrameCell), std::bit_cast<uintptr_t>(cell));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
