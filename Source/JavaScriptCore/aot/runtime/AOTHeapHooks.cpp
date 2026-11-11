/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTHeapHooks.h"

#if ENABLE(AOT)

#include "AOTCompiledCallee.h"
#include "AOTCompiledFrame.h"
#include "AOTEmptyPages.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTState.h"
#include "AOTUnwinding.h"
#include "AOTVMState.h"
#include "AbstractSlotVisitorInlines.h"
#include "Collector.h"
#include "FastMarkAppender.h"
#include "HeapInlines.h"
#include "HeapUtil.h"
#include "JSCInlines.h"
#include "JSObject.h"
#include "MarkedBlockCage.h"
#include "SimpleMarkingConstraint.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#include <algorithm>
#include <span>
#include <type_traits>

extern "C" void Bun__didFinishFirstCollectionOfCompiledProgram();
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

template<RootVolatility volatility, typename Visitor>
static void visitRootsOfCompiledCode(VM& vm, Visitor& visitor)
{
    if (!vm.hasCompiledCode() || !visitor.collector().hasHeap(vm.heap))
        return;
    SetRootMarkReasonScope rootScope(visitor, RootMarkReason::CodeBlocks);
    VMState& state = vm.aotState();
    state.caches->visitRoots(visitor, volatility);

    SetRootMarkReasonScope scopeOfWhatFollowsRealms(visitor, RootMarkReason::Output);
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        if (!visitor.isMarked(livenessToken(instance)))
            continue;
        CacheRegistry& caches = *instance->state->caches;
        caches.visitRoots(visitor, volatility);
        if (volatility == RootVolatility::GreyedByMarking && (std::is_same_v<Visitor, AbstractSlotVisitor> || !caches.hasVisitedExecutionRoots()))
            caches.visitRoots(visitor, RootVolatility::GreyedByExecution);
    }
}

WTF::Vector<std::unique_ptr<MarkingConstraint>> createMarkingConstraints(VM& vm)
{
    WTF::Vector<std::unique_ptr<MarkingConstraint>> constraints;
    constraints.append(makeUnique<SimpleMarkingConstraint>(
        "Ao"_s, "Instances of Statically Compiled Code"_s,
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([&vm] (auto& visitor) {
            visitRootsOfCompiledCode<RootVolatility::GreyedByMarking>(vm, visitor);
        })),
        ConstraintVolatility::GreyedByMarking,
        ConstraintConcurrency::Concurrent,
        ConstraintParallelism::Sequential));
    constraints.append(makeUnique<SimpleMarkingConstraint>(
        "At"_s, "Static Variables of Compiled Code"_s,
        MAKE_MARKING_CONSTRAINT_EXECUTOR_PAIR(([&vm] (auto& visitor) {
            visitRootsOfCompiledCode<RootVolatility::GreyedByExecution>(vm, visitor);
        })),
        ConstraintVolatility::GreyedByExecution,
        ConstraintConcurrency::Concurrent,
        ConstraintParallelism::Sequential));
    return constraints;
}

void initializeCollectorWords(VM& vm, Instance& instance)
{
    instance.barrierThreshold = vm.heap.barrierThreshold();
    instance.mutatorShouldBeFenced = vm.heap.mutatorShouldBeFenced();
    instance.cellCageBase = vm.heap.cage().base();
    instance.heap.storedCellLog = &vm.heap.storedCellLog();
    instance.heap.mutatorMarkStack = &HeapUtil::mutatorMarkStack(vm.heap);
    instance.heap.barriersExecuted = &HeapUtil::barriersExecuted(vm.heap);
    auto* headSegmentWord = std::bit_cast<MutatorMarkStackLayout::Segment**>(std::bit_cast<uint8_t*>(instance.heap.mutatorMarkStack) + MutatorMarkStackLayout::offsetOfHeadSegment());
    RELEASE_ASSERT(*headSegmentWord == MutatorMarkStackLayout::headSegmentOf(*instance.heap.mutatorMarkStack));
}

namespace HeapHooks {

void finalize(VM& vm, CollectionScope scope)
{
    VMState& state = vm.aotState();
    {
        Locker locker { state.instancesLock };
        state.instances.removeAllMatching([&](Instance* instance) {
            if (vm.heap.isMarked(livenessToken(instance)))
                return false;
            state.instancesToDestroy.append(instance);
            return true;
        });
        state.didChangeInstances();
    }
    if (state.realmWhoseDatesAreWrittenInPlace && !vm.heap.isMarked(state.realmWhoseDatesAreWrittenInPlace))
        state.realmWhoseDatesAreWrittenInPlace = nullptr;

    state.caches->finalize(vm, scope);
    for (Instance* instance : state.instances)
        instance->state->caches->finalize(vm, scope);
    if (!std::exchange(state.didFinishCollection, true))
        Bun__didFinishFirstCollectionOfCompiledProgram();
}

void didChangeBarrierThreshold(VM& vm, unsigned threshold, bool mutatorShouldBeFenced)
{
    VMState& state = vm.aotState();
    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        instance->barrierThreshold = threshold;
        instance->mutatorShouldBeFenced = mutatorShouldBeFenced;
    }
}

void willAnalyzeHeap(VM& vm)
{
    VMState& state = vm.aotState();
    state.caches->willAnalyzeHeap(vm);
    for (Instance* instance : state.instances)
        instance->state->caches->willAnalyzeHeap(vm);
}

void willCollectForExhaustedCage(VM& vm)
{
    vm.aotState().returnAddressOfCallThatCollected = returnAddressOfInnermostCompiledCall(vm);
}

void didFindNoRoomWhereCollectionMayRun(VM& vm)
{
    vm.aotState().returnAddressOfCallThatCollected = returnAddressOfInnermostCompiledCall(vm);
}

String compiledCalleeName(JSCell* cell)
{
    return CompiledCallee::isCompiledCallee(cell) ? CompiledCallee::name(asObject(cell)) : String();
}

std::span<const uint32_t> narrowReferenceOffsets(const Structure* structure)
{
    if (!MarkedBlock::isAtomAligned(structure))
        return { };
    if (!structure->isCompiledStructure() || !uncheckedDowncast<CompiledStructure>(structure)->describesObject())
        return { };
    std::span<const uint32_t> table = inlineReferenceTableOf(uncheckedDowncast<CompiledStructure>(structure)->layout());
    if (table.empty())
        return { };
    return table.subspan(numberOfInlineReferenceCounts + table[0] + table[1], table[2]);
}

void willWorkOnThisThread(VM& vm)
{
    Image::setCurrentOfThread(vm.aotState().image);
}

void crashOnCellVisitMismatch(VM&, const JSCell* cell)
{
    CRASH_WITH_INFO(std::bit_cast<uintptr_t>(cell));
}

} // namespace HeapHooks

WTF_MAKE_TZONE_ALLOCATED_IMPL(CacheRegistry);

CacheRegistry::CacheRegistry() = default;
CacheRegistry::~CacheRegistry() = default;

static void zeroNonzeroPieces(std::span<uint8_t> bytes)
{
    constexpr size_t pieceSize = 4 * KB;
    while (!bytes.empty()) {
        auto piece = bytes.first(std::min(bytes.size(), pieceSize));
        if (!holdsOnlyZeros(piece))
            zeroSpan(piece);
        bytes = bytes.subspan(piece.size());
    }
}

#if OS(LINUX)
static constexpr bool pageGivenBackStaysAwayWhenRead = true;
#else
static constexpr bool pageGivenBackStaysAwayWhenRead = false;
#endif

static void zeroNonzeroPiecesAndGiveBackWholePages(std::span<uint8_t> bytes)
{
    size_t pieceSize = pageSize();
    std::span<uint8_t> pagesToGiveBack;
    auto giveBack = [&] {
        if (!pagesToGiveBack.empty())
            decommitPagesOfZeros(pagesToGiveBack.data(), pagesToGiveBack.size());
        pagesToGiveBack = { };
    };
    while (!bytes.empty()) {
        size_t upToNextPage = pieceSize - reinterpret_cast<uintptr_t>(bytes.data()) % pieceSize;
        auto piece = bytes.first(std::min(bytes.size(), upToNextPage));
        bytes = bytes.subspan(piece.size());
        if (holdsOnlyZeros(piece))
            continue;
        zeroSpan(piece);
        if (piece.size() != pieceSize)
            continue;
        if (pagesToGiveBack.empty() || pagesToGiveBack.data() + pagesToGiveBack.size() != piece.data()) {
            giveBack();
            pagesToGiveBack = piece;
        } else
            pagesToGiveBack = { pagesToGiveBack.data(), pagesToGiveBack.size() + pieceSize };
    }
    giveBack();
}

void CacheRegistry::append(Entry&& entry)
{
    Locker locker { m_lock };
    m_entries.append(WTF::move(entry));
}

void CacheRegistry::addStrongRoots(ASCIILiteral name, std::span<JSCell* const> cellsOrNull, RootVolatility volatility)
{
    Entry entry {
        .kind = CacheKind::StrongRoots,
        .volatility = volatility,
        .name = name,
        .owner = const_cast<JSCell**>(cellsOrNull.data()),
        .size = cellsOrNull.size(),
    };
    auto visitCells = [](Entry& entry, auto& visitor) {
        for (JSCell* cell : unsafeMakeSpan(static_cast<JSCell* const*>(entry.owner), entry.size)) {
            if (cell)
                visitor.appendUnbarriered(cell);
        }
    };
    entry.visitWithSlotVisitor = visitCells;
    entry.visitWithAbstractSlotVisitor = visitCells;
    append(WTF::move(entry));
}

void CacheRegistry::addClearedEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear, const uint8_t* mayBeFilled)
{
    Entry entry {
        .kind = CacheKind::ClearedEachCollection,
        .name = name,
        .owner = zeroedByClear.data(),
        .size = zeroedByClear.size(),
        .mayBeFilled = mayBeFilled,
    };
    entry.clear = [](Entry& entry) {
        if (!entry.mayBeFilled || *entry.mayBeFilled)
            zeroNonzeroPieces(unsafeMakeSpan(static_cast<uint8_t*>(entry.owner), entry.size));
    };
    append(WTF::move(entry));
}

void CacheRegistry::addClearedAndGivenBackEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear)
{
    Entry entry {
        .kind = CacheKind::ClearedEachCollection,
        .name = name,
        .owner = zeroedByClear.data(),
        .size = zeroedByClear.size(),
    };
    entry.clear = [](Entry& entry) {
        auto bytes = unsafeMakeSpan(static_cast<uint8_t*>(entry.owner), entry.size);
        if (pageGivenBackStaysAwayWhenRead)
            zeroNonzeroPiecesAndGiveBackWholePages(bytes);
        else
            zeroNonzeroPieces(bytes);
    };
    append(WTF::move(entry));
}

template<typename Visitor>
void CacheRegistry::visitRoots(Visitor& visitor, RootVolatility volatility)
{
    Locker locker { m_lock };
    for (Entry& entry : m_entries) {
        if (entry.kind != CacheKind::StrongRoots || entry.volatility != volatility)
            continue;

        if constexpr (std::is_same_v<Visitor, SlotVisitor>)
            entry.visitWithSlotVisitor(entry, visitor);
        else
            entry.visitWithAbstractSlotVisitor(entry, visitor);
    }

    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (volatility == RootVolatility::GreyedByExecution)
            m_hasVisitedExecutionRoots = true;
    }
}

template void CacheRegistry::visitRoots<SlotVisitor>(SlotVisitor&, RootVolatility);
template void CacheRegistry::visitRoots<AbstractSlotVisitor>(AbstractSlotVisitor&, RootVolatility);

void CacheRegistry::finalize(VM& vm, CollectionScope scope)
{
    for (Entry& entry : m_entries) {
        switch (entry.kind) {
        case CacheKind::StrongRoots:
        case CacheKind::Observer:
            break;
        case CacheKind::WeakByKey:
            entry.removeDeadEntries(entry, vm, scope);
            break;
        case CacheKind::ClearedEachCollection:
            entry.clear(entry);
            break;
        }
    }

    for (Entry& entry : m_entries) {
        if (entry.kind == CacheKind::Observer)
            entry.didFinishCollection(entry, vm, scope);
    }

    m_hasVisitedExecutionRoots = false;
}

void CacheRegistry::willAnalyzeHeap(VM& vm)
{
    for (Entry& entry : m_entries) {
        if (entry.willAnalyzeHeap)
            entry.willAnalyzeHeap(entry, vm);
    }
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImmortalCells);

void ImmortalCells::add(JSCell* cell)
{
    ASSERT(cell);
    Locker locker { m_state.caches->lock() };
    if (!m_last || m_last->size == Segment::capacity) {
        Segment* segment = static_cast<Segment*>(m_state.instance.vm->aotState().pages->allocate(sizeof(Segment)));
        segment->next = nullptr;
        segment->size = 0;
        (m_last ? m_last->next : m_first) = segment;
        m_last = segment;
    }
    m_last->cells[m_last->size++] = cell;
}

template<typename Functor>
ALWAYS_INLINE void ImmortalCells::forEachSegment(Segment* first, size_t firstInSegment, const Functor& functor)
{
    for (Segment* segment = first; segment; segment = segment->next) {
        if (segment->size > firstInSegment)
            functor(unsafeMakeSpan(segment->cells, segment->size).subspan(firstInSegment));
        firstInSegment = 0;
    }
}

static void visitDeferredSegment(SlotVisitor& visitor, void* segment, size_t begin, size_t end)
{
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        for (JSCell* cell : unsafeMakeSpan(static_cast<JSCell**>(segment), end).subspan(begin))
            appender.appendUnbarriered(cell);
    });
}

template<typename Visitor>
void ImmortalCells::visitRoots(Visitor& visitor)
{
    Segment* first = m_first;
    size_t firstInSegment = 0;
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        JSC::Heap& heap = m_state.instance.vm->heap;
        HeapVersion version = heap.objectSpace().newlyAllocatedVersion();
        bool isFirstVisitInCollection = version != m_lastVisitVersion;
        m_lastVisitVersion = version;
        bool visitsEveryCell = isFirstVisitInCollection && heap.collectionScope() != CollectionScope::Eden;
        if (!visitsEveryCell && m_lastVisited) {
            first = m_lastVisited;
            firstInSegment = m_numberVisitedInLastVisited;
        }
        m_lastVisited = m_last;
        m_numberVisitedInLastVisited = m_last ? m_last->size : 0;

        if (visitsEveryCell && visitor.defersRootRegions()) {
            forEachSegment(m_first, 0, [&](std::span<JSCell*> cells) {
                visitor.deferRootRegion({ visitDeferredSegment, cells.data(), 0, cells.size() });
            });
            return;
        }
    }
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        forEachSegment(first, firstInSegment, [&](std::span<JSCell*> cells) ALWAYS_INLINE_LAMBDA {
            for (JSCell* cell : cells)
                appender.appendUnbarriered(cell);
        });
    });
}

template void ImmortalCells::visitRoots(AbstractSlotVisitor&);
template void ImmortalCells::visitRoots(SlotVisitor&);

ImmortalCells::ImmortalCells(State& state)
    : m_state(state)
{
    state.caches->add(CacheKind::StrongRoots, "immortal cells"_s, *this, RootVolatility::GreyedByExecution);
}

ImmortalCells::~ImmortalCells()
{
    PagesOfVM& pages = *m_state.instance.vm->aotState().pages;
    for (Segment* segment = m_first; segment;) {
        Segment* next = segment->next;
        pages.release(segment, sizeof(Segment));
        segment = next;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
