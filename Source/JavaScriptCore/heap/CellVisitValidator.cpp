/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "CellVisitValidator.h"

#include "BlockDirectoryInlines.h"
#include "Collector.h"
#include "FastMarkAppender.h"
#include "InlineCellVisits.h"
#include "JSCellInlines.h"
#include "MarkedSpaceInlines.h"
#include "Options.h"
#include "SlotVisitorInlines.h"
#include "VM.h"
#if ENABLE(AOT)
#include "AOTHeapHooks.h"
#endif
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/RawHex.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

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
    // No field names it: a cell that a field names lies in it.
    bool isNamedOnlyAsOuterCell(const void* reference) const { return m_namedOnlyAsOuterCells.contains(reference); }
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
        m_namedOnlyAsOuterCells.remove(reference);
        // The marker marks the cell that an inner cell lies in with the inner cell.
        auto* cell = static_cast<const HeapCell*>(reference);
        if (!cell->isPreciseAllocation() && cell->markedBlock().isInnerCell(cell)) {
            const void* outer = cell->markedBlock().outerCellOf(cell);
            if (m_references.add(outer).isNewEntry)
                m_namedOnlyAsOuterCells.add(outer);
        }
    }

    const SlotVisitor& m_marker;
    ConcurrentPtrHashSet m_opaqueRootStorage;
    UncheckedKeyHashSet<const void*> m_references;
    UncheckedKeyHashSet<const void*> m_namedOnlyAsOuterCells;
    bool m_sawRace { false };
};

ASCIILiteral nameOf(CellVisitKind kind)
{
    switch (kind) {
    case CellVisitKind::MethodTable:
        return "MethodTable"_s;
#define JSC_NAME_OF_CELL_VISIT_KIND(kind, cellType, placement) \
    case CellVisitKind::kind: \
        return #kind ""_s;
    FOR_EACH_INLINE_VISITED_CELL_CLASS(JSC_NAME_OF_CELL_VISIT_KIND)
#undef JSC_NAME_OF_CELL_VISIT_KIND
    }
    return "unknown"_s;
}

void describe(const SlotVisitor& visitor, const JSCell* cell, ASCIILiteral problem, const void* reference)
{
    if (!reference)
        return;
    auto* heapCell = static_cast<const HeapCell*>(reference);
    dataLog("GC: ", problem, ": ", RawPointer(reference), ", marked now: ", visitor.isMarked(reference));
    if (reference == cell->structure())
        dataLog(", the structure of the cell with stamp ", static_cast<unsigned>(cell->structure()->markingStamp()), " where this cycle stamps ", static_cast<unsigned>(Structure::markingStampFor(visitor.markingVersion())));
    if (heapCell->isPreciseAllocation())
        dataLog(", an allocation of its own");
    else {
        MarkedBlock& block = heapCell->markedBlock();
        dataLog(", in block ", RawPointer(&block), " of cells of ", block.cellSize(), " bytes with marks of version ", block.markingVersion());
        if (block.hasInnerCells())
            dataLog(", inner cell: ", block.isInnerCell(reference), ", in the visited cell: ", block.outerCellIfInnerCell(reference) == cell);
        if (&block.vm() != &cell->vm())
            dataLog(", of another heap");
    }
    dataLogLn(heapCell->cellKind() == HeapCell::JSCell ? ", a cell" : ", storage");
}

// Last, because a reference that is no cell any more can make it fault.
void describeClass(const void* reference)
{
    if (!reference || static_cast<const HeapCell*>(reference)->cellKind() != HeapCell::JSCell)
        return;
    auto* words = static_cast<const uint64_t*>(reference);
    dataLogLn("GC: ", RawPointer(reference), " begins with ", RawHex(words[0]), " ", RawHex(words[1]));
    dataLogLn("GC: ", RawPointer(reference), " is a ", static_cast<const JSCell*>(reference)->classInfo()->className);
}

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

        const void* namedWithoutMark = nullptr;
        const JSCell* markedWithoutName = nullptr;
        size_t numberMarked = 0;
        // Whoever is the first to mark an inner cell marks its outer cell next. Another marker that was the first may stand between the two marks right now. validateMarksOfInnerCells() asks for that mark once all markers are done.
        auto isLeftToAnotherMarker = [&](const void* reference) {
            if (!recorder.isNamedOnlyAsOuterCell(reference))
                return false;
            for (const JSCell* marked : fastVisitor.m_collectorStack) {
                if (!marked->isPreciseAllocation() && marked->markedBlock().outerCellIfInnerCell(marked) == reference)
                    return false;
            }
            return true;
        };
        if (!recorder.sawRace()) {
            for (const void* reference : recorder.references()) {
                if (reference == droppedCell || (!visitor.isMarked(reference) && !isLeftToAnotherMarker(reference)))
                    namedWithoutMark = reference;
            }
            for (const JSCell* marked : fastVisitor.m_collectorStack) {
                ++numberMarked;
                if (!recorder.references().contains(marked) && !recorderOfInnerCells.references().contains(marked))
                    markedWithoutName = marked;
            }
        }
        if (namedWithoutMark || markedWithoutName) [[unlikely]] {
            ReferenceRecorder secondRecorder(visitor);
            cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), secondRecorder);
            bool namesTheSame = secondRecorder.references().size() == recorder.references().size();
            for (const void* reference : secondRecorder.references())
                namesTheSame &= recorder.references().contains(reference);

            dataLogLn("GC: the visits of ", RawPointer(cell), " differ: kind ", nameOf(cell->structure()->cellVisitKind()), ", structure ", RawPointer(cell->structure()), ", cell state ", static_cast<unsigned>(cell->cellState()),
                visitor.m_isFirstVisit ? ", first visit (the stack of the collector)" : ", visited again (the stack of the mutator)",
                visitor.m_collectionScope == CollectionScope::Full ? ", full collection" : ", eden collection", ", marking version ", visitor.m_markingVersion,
                "; the method table names ", recorder.references().size(), " references, the inline visit was the first to mark ", numberMarked);
            describe(visitor, cell, "named by the method table and without a mark"_s, namedWithoutMark);
            describe(visitor, cell, "marked by the inline visit and not named by the method table"_s, markedWithoutName);
            dataLogLn("GC: a second visit by the method table names ", namesTheSame ? "the same references" : "other references: the cell changed while the mutators were stopped");
            size_t numberOfWords = std::min<size_t>(cell->isPreciseAllocation() ? 2 : cell->markedBlock().cellSize() / sizeof(uint64_t), 16);
            dataLog("GC: the cell begins with");
            for (size_t i = 0; i < numberOfWords; ++i)
                dataLog(" ", RawHex(reinterpret_cast<const uint64_t*>(cell)[i]));
            dataLogLn();
            describeClass(namedWithoutMark);
            describeClass(markedWithoutName);
            crashOnMismatch(cell);
        }
    }

    // The inline visit marked cells for real, so they still have to be visited. The shipping visit of this cell reports its extra memory.
    visitor.addToVisitCount(fastVisitor.visitCount());
    fastVisitor.flushBytesMarked();
    visitor.m_flushedBytesMarked += fastVisitor.m_flushedBytesMarked;
    fastVisitor.m_extraMemoryInHeap = 0;
    fastVisitor.donateAll();
    fastVisitor.reset();
}

void CellVisitValidator::validateMarksOfInnerCells(JSC::Heap& heap)
{
    heap.objectSpace().forEachBlock([&](MarkedBlock::Handle* handle) {
        MarkedBlock& block = handle->block();
        if (!block.hasInnerCells() || block.areMarksStale())
            return;
        block.marks().forEachSetBit([&](size_t atom) {
            auto* inner = static_cast<HeapCell*>(handle->atomAt(atom));
            HeapCell* outer = block.outerCellIfInnerCell(inner);
            if (!outer || block.isMarkedRaw(outer)) [[likely]]
                return;
            dataLogLn("GC: at the end of marking ", RawPointer(inner), " has a mark and the cell that it lies in has none: ", RawPointer(outer), ", in block ", RawPointer(&block), " of cells of ", block.cellSize(), " bytes with marks of version ", block.markingVersion());
            crashOnMismatch(static_cast<JSCell*>(inner));
        });
    });
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

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
