/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CellVisitValidator.h"

#include "Collector.h"
#include "FastMarkAppender.h"
#include "InlineCellVisits.h"
#include "JSCellInlines.h"
#include "Options.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#if ENABLE(AOT)
#include "AOTHeapHooks.h"
#endif
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>

namespace JSC {

namespace {

class ReferenceRecorder final : public AbstractSlotVisitor {
    WTF_MAKE_NONCOPYABLE(ReferenceRecorder);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit ReferenceRecorder(const SlotVisitor& marker)
        : AbstractSlotVisitor(marker.collector(), "Rr"_s, m_opaqueRootStorage)
        , m_marker(marker)
    {
        didStartMarking(marker.collectionScope(), false);
    }

    const UncheckedKeyHashSet<const void*>& references() const LIFETIME_BOUND { return m_references; }
    bool sawRace() const { return m_sawRace; }

    NO_RETURN_DUE_TO_CRASH void append(const ConservativeRoots&) final { RELEASE_ASSERT_NOT_REACHED(); }
    void appendUnbarriered(JSCell* cell) final { record(cell); }
    void appendHiddenUnbarriered(JSCell* cell) final { record(cell); }
    void markAuxiliary(const void* base) final
    {
        // Storage that lies in the cell of its owner has no mark of its own.
        auto* cell = static_cast<const HeapCell*>(base);
        if (cell->isPreciseAllocation() || !cell->markedBlock().hasInnerCells())
            record(base);
    }

    bool isFirstVisit() const final { return m_marker.isFirstVisit(); }
    bool isMarked(const void* cell) const final { return m_marker.isMarked(cell); }
    bool isMarked(MarkedBlock& block, HeapCell* cell) const final { return m_marker.isMarked(block, cell); }
    bool isMarked(PreciseAllocation& allocation, HeapCell* cell) const final { return m_marker.isMarked(allocation, cell); }
    bool mutatorsAreStopped() const final { return true; }

    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif

    void didRace(const VisitRaceKey&) final { m_sawRace = true; }

    NO_RETURN_DUE_TO_CRASH void visitAsConstraint(const JSCell*) final { RELEASE_ASSERT_NOT_REACHED(); }
    NO_RETURN_DUE_TO_CRASH void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }
    NO_RETURN_DUE_TO_CRASH void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { RELEASE_ASSERT_NOT_REACHED(); }

private:
    void record(const void* reference)
    {
        if (!reference)
            return;
        m_references.add(reference);
        // The marker marks the cell that an inner cell lies in with the inner cell.
        auto* cell = static_cast<const HeapCell*>(reference);
        if (!cell->isPreciseAllocation() && cell->markedBlock().isInnerCell(cell))
            m_references.add(cell->markedBlock().outerCellOf(cell));
    }

    const SlotVisitor& m_marker;
    ConcurrentPtrHashSet m_opaqueRootStorage;
    UncheckedKeyHashSet<const void*> m_references;
    bool m_sawRace { false };
};

} // anonymous namespace

void CellVisitValidator::validate(SlotVisitor& visitor, const JSCell* cell)
{
    if (!visitor.m_mutatorsAreStopped || visitor.m_isAnalyzingHeap)
        return;

    SlotVisitor fastVisitor(visitor.m_collector, "Fv"_s);
    fastVisitor.didStartMarking(visitor.m_collectionScope, visitor.m_markingVersion, false, false);
    fastVisitor.m_isFirstVisit = visitor.m_isFirstVisit;
    fastVisitor.m_mutatorsAreStopped = true;

    bool isHandled;
    {
        FastMarkAppender appender(fastVisitor);
        isHandled = tryVisitInline(appender, const_cast<JSCell*>(cell), cell->structure(), cell->structure()->cellVisitKind());
    }

    if (isHandled) {
        ReferenceRecorder recorder(visitor);
        cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), recorder);

        // The inline visit of a cell visits the inner cells that it is the first to mark. Whether it was the first is not known here, so it may have marked what they refer to and need not have.
        ReferenceRecorder recorderOfInnerCells(visitor);
        if (!cell->isPreciseAllocation() && cell->markedBlock().hasInnerCells()) [[unlikely]] {
            for (const void* reference : recorder.references()) {
                auto* inner = static_cast<JSCell*>(const_cast<void*>(reference));
                if (!inner->isPreciseAllocation() && &inner->markedBlock() == &cell->markedBlock() && cell->markedBlock().outerCellIfInnerCell(inner) == cell)
                    inner->methodTable()->visitChildren(inner, recorderOfInnerCells);
            }
        }

        // A mark cannot be taken back, so the comparison treats the last cell that the inline visit marked as unmarked.
        const JSCell* droppedCell = nullptr;
        if (Options::dropLastFastVisitReferenceForTesting() && fastVisitor.m_collectorStack.canRemoveLast()) [[unlikely]] {
            droppedCell = fastVisitor.m_collectorStack.removeLast();
            fastVisitor.m_collectorStack.append(droppedCell);
        }

        bool differs = false;
        if (!recorder.sawRace()) {
            for (const void* reference : recorder.references())
                differs |= reference == droppedCell || !visitor.isMarked(reference);
            for (const JSCell* marked : fastVisitor.m_collectorStack)
                differs |= !recorder.references().contains(marked) && !recorderOfInnerCells.references().contains(marked);
        }
        if (differs) [[unlikely]]
            crashOnMismatch(cell);
    }

    // The inline visit marked cells for real, so they still have to be visited. The shipping visit of this cell reports its extra memory.
    visitor.addToVisitCount(fastVisitor.visitCount());
    fastVisitor.flushBytesMarked();
    visitor.m_flushedBytesMarked += fastVisitor.m_flushedBytesMarked;
    fastVisitor.m_extraMemoryInHeap = 0;
    fastVisitor.donateAll();
    fastVisitor.reset();
}

void CellVisitValidator::crashOnMismatch(const JSCell* cell)
{
#if ENABLE(AOT)
    if (cell->vm().hasCompiledCode())
        AOT::HeapHooks::crashOnCellVisitMismatch(cell->vm(), cell);
#endif
    dataLogLn("GC: the inline visit and the method table visit of ", RawPointer(cell), " (", cell->classInfo()->className, ") differ");
    CRASH();
}

} // namespace JSC
