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
#include "AOTFunctionTables.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImage.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
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
#include <limits>
#include <span>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

// What was called from there, or from below, has returned.
static const void* stackPointerAtTop(VM& vm)
{
    CallFrame* top = vm.topCallFrame;
    return top ? stackPointerOfCaller(top) : std::bit_cast<const void*>(std::numeric_limits<uintptr_t>::max());
}

template<RootVolatility volatility, typename Visitor>
static void visitRootsOfCompiledCode(VM& vm, Visitor& visitor)
{
    if (!vm.hasCompiledCode() || !visitor.collector().hasHeap(vm.heap))
        return;
    SetRootMarkReasonScope rootScope(visitor, RootMarkReason::CodeBlocks);
    VMState& state = vm.aotState();
    if (state.functionTables)
        state.functionTables->visitRecordedCallees(visitor, stackPointerAtTop(vm));
    state.caches->visitRoots(visitor, volatility);

    Locker locker { state.instancesLock };
    for (Instance* instance : state.instances) {
        if (!visitor.isMarked(livenessToken(instance)))
            continue;
        CacheRegistry& caches = *instance->state->caches;
        caches.visitRoots(visitor, volatility);
        // "At" runs once per stop of the mutator, so it misses a realm first reached during draining. The GC verifier always needs these roots.
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
    instance.heap.recordedCallees = FunctionTables::recordedCalleesOf(vm.aotState().functionTables.get());
    auto* headSegmentWord = std::bit_cast<MutatorMarkStackLayout::Segment**>(std::bit_cast<uint8_t*>(instance.heap.mutatorMarkStack) + MutatorMarkStackLayout::offsetOfHeadSegment());
    RELEASE_ASSERT(*headSegmentWord == MutatorMarkStackLayout::headSegmentOf(*instance.heap.mutatorMarkStack));
}

void validateFunctionCellIsNotInFrame(VM& vm, const JSCell* cell)
{
    if (!Thread::currentSingleton().stack().contains(const_cast<JSCell*>(cell))) [[likely]]
        return;
    crashOnValidationFailure(vm, { ValidationFailureKind::FunctionCellInFrameGotProperties, functionOf(cell).value_or(FunctionID()), { }, cell });
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

    if (state.functionTables)
        state.functionTables->finalizeRecordedCallees(vm, stackPointerAtTop(vm));

    CacheValidation validation = state.validatesRuntime ? CacheValidation::On : CacheValidation::Off;
    state.caches->finalize(vm, scope, validation);
    for (Instance* instance : state.instances)
        instance->state->caches->finalize(vm, scope, validation);
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

String nameOfCompiledCallee(JSCell* cell)
{
    return CompiledCallee::isCompiledCallee(cell) ? CompiledCallee::name(asObject(cell)) : String();
}

std::span<const uint32_t> offsetsOfNarrowReferences(const Structure* structure)
{
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

void crashOnCellVisitMismatch(VM& vm, const JSCell* cell)
{
    crashOnValidationFailure(vm, { ValidationFailureKind::VisitedCellsDiffer, { }, { }, cell });
}

void validateSlotsOf(VM& vm, const JSCell* cell)
{
    validateSlotTypes(vm, *uncheckedDowncast<Object>(cell));
}

} // namespace HeapHooks

WTF_MAKE_TZONE_ALLOCATED_IMPL(CacheRegistry);

CacheRegistry::CacheRegistry() = default;
CacheRegistry::~CacheRegistry() = default;

// Reading a zero-filled page that was never written does not commit it. Writing does.
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

[[noreturn]] static void crashOnUnrecordedWrite(VM& vm, ASCIILiteral name)
{
    dataLogLn("AOT: cache \"", name, "\" was written without a record");
    crashOnValidationFailure(vm, { ValidationFailureKind::CacheEntryDropped, { }, { }, { } });
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

void CacheRegistry::addClearedEachCollection(ASCIILiteral name, std::span<uint8_t> zeroedByClear)
{
    Entry entry {
        .kind = CacheKind::ClearedEachCollection,
        .name = name,
        .owner = zeroedByClear.data(),
        .size = zeroedByClear.size(),
    };
    entry.clear = [](Entry& entry) {
        zeroNonzeroPieces(unsafeMakeSpan(static_cast<uint8_t*>(entry.owner), entry.size));
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

    // The GC verifier marks on its own and leaves the state of the real collection alone.
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        if (volatility == RootVolatility::GreyedByExecution)
            m_hasVisitedExecutionRoots = true;
    }
}

template void CacheRegistry::visitRoots<SlotVisitor>(SlotVisitor&, RootVolatility);
template void CacheRegistry::visitRoots<AbstractSlotVisitor>(AbstractSlotVisitor&, RootVolatility);

void CacheRegistry::finalize(VM& vm, CollectionScope scope, CacheValidation validation)
{
    bool shouldValidate = validation == CacheValidation::On;
    for (Entry& entry : m_entries) {
        switch (entry.kind) {
        case CacheKind::StrongRoots:
        case CacheKind::Observer:
            break;
        case CacheKind::WeakByKey:
            entry.removeDeadEntries(entry, vm, scope);
            // An Eden collection kills young cells only, and a store of a young cell is recorded. So the regions it skipped have no dead entry.
            if (shouldValidate && scope == CollectionScope::Eden && entry.removeDeadEntries(entry, vm, CollectionScope::Full))
                crashOnUnrecordedWrite(vm, entry.name);
            break;
        case CacheKind::ClearedEachCollection:
            entry.clear(entry);
            if (shouldValidate && entry.size && !holdsOnlyZeros(unsafeMakeSpan(static_cast<const uint8_t*>(entry.owner), entry.size)))
                crashOnUnrecordedWrite(vm, entry.name);
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
    m_cells.append(cell);
}

template<typename Functor>
ALWAYS_INLINE void ImmortalCells::forEachSegment(size_t firstIndex, const Functor& functor)
{
    size_t size = m_cells.size();
    for (size_t begin = firstIndex; begin < size;) {
        size_t end = std::min(begin - begin % cellsPerSegment + cellsPerSegment, size);
        functor(unsafeMakeSpan(&m_cells[begin], end - begin));
        begin = end;
    }
}

// Needs no lock: a segment never moves, and these cells were added before the visit that deferred them.
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
    size_t first = 0;
    if constexpr (std::is_same_v<Visitor, SlotVisitor>) {
        JSC::Heap& heap = m_state.instance.vm->heap;
        HeapVersion version = heap.objectSpace().newlyAllocatedVersion();
        bool isFirstVisitInCollection = version != m_lastVisitVersion;
        m_lastVisitVersion = version;
        bool visitsEveryCell = isFirstVisitInCollection && heap.collectionScope() != CollectionScope::Eden;
        if (!visitsEveryCell)
            first = m_numberOfCellsVisited;
        m_numberOfCellsVisited = m_cells.size();

        if (visitsEveryCell && visitor.defersRootRegions()) {
            forEachSegment(0, [&](std::span<JSCell*> cells) {
                visitor.deferRootRegion({ visitDeferredSegment, cells.data(), 0, cells.size() });
            });
            return;
        }
    }
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        forEachSegment(first, [&](std::span<JSCell*> cells) ALWAYS_INLINE_LAMBDA {
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
