/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SlotVisitor.h"

#include "AOTImage.h"
#include "AOTTypedCompactTable.h"
#include "AOTTypedData.h"
#include "AOTTypedObject.h"
#include "CellContainerInlines.h"
#include "DateInstance.h"
#include "Collector.h"
#include "ConservativeRoots.h"
#include "GCSegmentedArrayInlines.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "HeapCellInlines.h"
#include "IntegrityInlines.h"
#include "JSArray.h"
#include "JSCellButterfly.h"
#include "JSCellInlines.h"
#include "JSFunctionWithFields.h"
#include "JSMap.h"
#include "JSPromise.h"
#include "JSPromiseReaction.h"
#include "JSSet.h"
#include "JSString.h"
#include "MarkedBlockCage.h"
#include "MarkingConstraintSolver.h"
#include "SlotVisitorInlines.h"
#include "StopIfNecessaryTimer.h"
#include "VM.h"
#include <wtf/ListDump.h>
#include <wtf/Lock.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SlotVisitor);

#if ENABLE(GC_VALIDATION)
static void validate(JSCell* cell)
{
    RELEASE_ASSERT(cell);

    if (!cell->structure()) {
        dataLogF("cell at %p has a null structure\n" , cell);
        CRASH();
    }

    // Both the cell's structure, and the cell's structure's structure should be the Structure Structure.
    // I hate this sentence.
    if (cell->structure()->structure()->JSCell::classInfo() != cell->structure()->JSCell::classInfo()) {
        const char* parentClassName = 0;
        const char* ourClassName = 0;
        if (cell->structure()->structure() && cell->structure()->structure()->JSCell::classInfo())
            parentClassName = cell->structure()->structure()->JSCell::classInfo()->className;
        if (cell->structure()->JSCell::classInfo())
            ourClassName = cell->structure()->JSCell::classInfo()->className;
        dataLogF("parent structure (%p <%s>) of cell at %p doesn't match cell's structure (%p <%s>)\n",
            cell->structure()->structure(), parentClassName, cell, cell->structure(), ourClassName);
        CRASH();
    }

    // Make sure we can walk the ClassInfo chain
    const ClassInfo* info = cell->classInfo();
    do { } while ((info = info->parentClass));
}
#endif

SlotVisitor::SlotVisitor(Collector& collector, ASCIICString codeName)
    : Base(collector, WTF::move(codeName), collector.m_opaqueRoots)
    , m_markingVersion(MarkedSpace::initialVersion)
#if ASSERT_ENABLED
    , m_isCheckingForDefaultMarkViolation(false)
#endif
{
}

SlotVisitor::~SlotVisitor()
{
    clearMarkStacks();
}

void SlotVisitor::didStartMarking(CollectionScope scope, HeapVersion markingVersion, bool isAnalyzingHeap)
{
    // The previous collection's End reset this visitor.
    ASSERT(!m_bytesMarkedHeap);
    ASSERT(!m_bytesMarkedInHeap);
    ASSERT(!m_flushedBytesMarked);
    ASSERT(!m_extraMemoryHeap);
    ASSERT(!m_extraMemoryInHeap.hasOverflowed() && !m_extraMemoryInHeap);
    ASSERT(!m_currentCell);
    Base::didStartMarking(scope, isAnalyzingHeap);
    m_markingVersion = markingVersion;
}

void SlotVisitor::reset()
{
    Base::reset();
    // These counts should have been flushed to each heap by now.
    RELEASE_ASSERT(!m_bytesMarkedInHeap);
    RELEASE_ASSERT(!m_extraMemoryInHeap.hasOverflowed() && !m_extraMemoryInHeap);
    m_bytesMarkedHeap = nullptr;
    m_flushedBytesMarked = 0;
    m_extraMemoryHeap = nullptr;
    RELEASE_ASSERT(!m_currentCell);
}

void SlotVisitor::clearMarkStacks()
{
    forEachMarkStack(
        [&] (MarkStackArray& stack) -> IterationStatus {
            stack.clear();
            return IterationStatus::Continue;
        });
}

void SlotVisitor::append(const ConservativeRoots& conservativeRoots)
{
    HeapCell** roots = conservativeRoots.roots();
    size_t size = conservativeRoots.size();
    for (size_t i = 0; i < size; ++i)
        appendJSCellOrAuxiliary(roots[i]);
}

void SlotVisitor::appendJSCellOrAuxiliary(HeapCell* heapCell)
{
    if (!heapCell)
        return;
    
    ASSERT(!m_isCheckingForDefaultMarkViolation);
    ASSERT(m_collector.hasHeap(*heapCell->heap()));
    
    auto validateCell = [&] (JSCell* jsCell) {
        StructureID structureID = jsCell->structureID();
        
        auto die = [&] (const char* text) {
            WTF::dataFile().atomically(
                [&] (PrintStream& out) {
                    out.print(text);
                    out.print("GC type: ", jsCell->heap()->collectionScope(), "\n");
                    out.print("Object at: ", RawPointer(jsCell), "\n");
                    out.print("Structure ID: ", structureID.bits(), " (", RawPointer(structureID.decode()), ")\n");
                    out.print("Object contents:");
                    for (unsigned i = 0; i < 2; ++i)
                        out.print(" ", format("0x%016llx", std::bit_cast<uint64_t*>(jsCell)[i]));
                    out.print("\n");
                    CellContainer container = jsCell->cellContainer();
                    out.print("Is marked: ", container.isMarked(jsCell), "\n");
                    out.print("Is newly allocated: ", container.isNewlyAllocated(jsCell), "\n");
                    if (container.isMarkedBlock()) {
                        MarkedBlock& block = container.markedBlock();
                        out.print("Block: ", RawPointer(&block), "\n");
                        block.handle().dumpState(out);
                        out.print("\n");
                        out.print("Is marked raw: ", block.isMarkedRaw(jsCell), "\n");
                        out.print("Marking version: ", block.markingVersion(), "\n");
                        out.print("Heap marking version: ", jsCell->heap()->objectSpace().markingVersion(), "\n");
                        out.print("Is newly allocated raw: ", block.isNewlyAllocated(jsCell), "\n");
                        out.print("Newly allocated version: ", block.newlyAllocatedVersion(), "\n");
                        out.print("Heap newly allocated version: ", jsCell->heap()->objectSpace().newlyAllocatedVersion(), "\n");
                    }
                    UNREACHABLE_FOR_PLATFORM();
                });
        };
        
        // It's not OK for the structure to be null at any GC scan point. We must not GC while
        // an object is not fully initialized.
        if (!structureID)
            die("GC scan found corrupt object: structureID is zero!\n");
        
        // It's not OK for the structure to be nuked at any GC scan point.
        if (structureID.isNuked())
            die("GC scan found object in bad state: structureID is nuked!\n");

        // This detects the worst of the badness.
        Integrity::auditStructureID(structureID);
    };

    // In debug mode, we validate before marking since this makes it clearer what the problem
    // was. It's also slower, so we don't do it normally.
    if (ASSERT_ENABLED && isJSCellKind(heapCell->cellKind()))
        validateCell(static_cast<JSCell*>(heapCell));
    
    if (Heap::testAndSetMarked(m_markingVersion, heapCell))
        return;
    
    switch (heapCell->cellKind()) {
    case HeapCell::JSCell:
    case HeapCell::JSCellWithIndexingHeader: {
        // We have ample budget to perform validation here.
    
        JSCell* jsCell = static_cast<JSCell*>(heapCell);
        validateCell(jsCell);
        Integrity::auditCell(jsCell->vm(), jsCell);
        
        jsCell->setCellState(CellState::PossiblyGrey);

        appendToMarkStack(jsCell);
        return;
    }
        
    case HeapCell::Auxiliary: {
        noteLiveAuxiliaryCell(heapCell);
        return;
    } }
}

void SlotVisitor::appendSlow(JSCell* cell, Dependency dependency)
{
    if (m_isAnalyzingHeap) [[unlikely]] {
        // FIXME: A cross-heap edge appears in neither VM's snapshot: it goes to the target cell's VM's snapshot,
        // which has no node for the source and drops it.
        if (HeapAnalyzer* analyzer = cell->vm().activeHeapAnalyzer())
            analyzer->analyzeEdge(m_currentCell, cell, rootMarkReason());
    }

    appendHiddenSlowImpl(cell, dependency);
}

void SlotVisitor::appendHiddenSlow(JSCell* cell, Dependency dependency)
{
    appendHiddenSlowImpl(cell, dependency);
}

ALWAYS_INLINE void SlotVisitor::appendHiddenSlowImpl(JSCell* cell, Dependency dependency)
{
    ASSERT(!m_isCheckingForDefaultMarkViolation);

#if ENABLE(GC_VALIDATION)
    validate(cell);
#endif
    
    if (cell->isPreciseAllocation())
        setMarkedAndAppendToMarkStack(cell->preciseAllocation(), cell, dependency);
    else
        setMarkedAndAppendToMarkStack(cell->markedBlock(), cell, dependency);
}

template<typename ContainerType>
ALWAYS_INLINE void SlotVisitor::setMarkedAndAppendToMarkStack(ContainerType& container, JSCell* cell, Dependency dependency)
{
    if (container.testAndSetMarked(cell, dependency))
        return;
    
    ASSERT(cell->structure());
    
    // Indicate that the object is grey and that:
    // In case of concurrent GC: it's the first time it is grey in this GC cycle.
    // In case of eden collection: it's a new object that became grey rather than an old remembered object.
    cell->setCellState(CellState::PossiblyGrey);
    
    appendToMarkStack(container, cell);
}

void SlotVisitor::appendToMarkStack(JSCell* cell)
{
    if (cell->isPreciseAllocation())
        appendToMarkStack(cell->preciseAllocation(), cell);
    else
        appendToMarkStack(cell->markedBlock(), cell);
}

template<typename ContainerType>
ALWAYS_INLINE void SlotVisitor::appendToMarkStack(ContainerType& container, JSCell* cell)
{
    ASSERT(isMarked(cell));
#if CPU(X86_64)
    if (Options::dumpZappedCellCrashData()) [[unlikely]] {
        if (cell->isZapped()) [[unlikely]]
            reportZappedCellAndCrash(cell);
    }
#endif
    ASSERT(!cell->isZapped());

    container.noteMarked();
    
    m_visitCount++;
    noteBytesMarked(container.vm().heap, container.cellSize());

    m_collectorStack.append(cell);
}

void SlotVisitor::markAuxiliary(const void* base)
{
    HeapCell* cell = std::bit_cast<HeapCell*>(base);
    
    ASSERT(m_collector.hasHeap(*cell->heap()));
    
    if (Heap::testAndSetMarked(m_markingVersion, cell))
        return;
    
    noteLiveAuxiliaryCell(cell);
}

void SlotVisitor::noteLiveAuxiliaryCell(HeapCell* cell)
{
    // We get here once per GC under these circumstances:
    //
    // Eden collection: if the cell was allocated since the last collection and is live somehow.
    //
    // Full collection: if the cell is live somehow.
    
    CellContainer container = cell->cellContainer();
    
    container.assertValidCell(cell->vm(), cell);
    container.noteMarked();
    
    m_visitCount++;

    size_t cellSize = container.cellSize();
    noteBytesMarked(container.vm().heap, cellSize);
    m_nonCellVisitCount += cellSize;
}

ALWAYS_INLINE void SlotVisitor::noteBytesMarked(JSC::Heap& heap, size_t bytes)
{
    if (&heap != m_bytesMarkedHeap) [[unlikely]] {
        flushBytesMarked();
        m_bytesMarkedHeap = &heap;
    }
    m_bytesMarkedInHeap += bytes;
}

NEVER_INLINE void SlotVisitor::flushBytesMarked()
{
    if (!m_bytesMarkedInHeap)
        return;
    m_bytesMarkedHeap->reportBytesMarked(m_bytesMarkedInHeap);
    m_flushedBytesMarked += m_bytesMarkedInHeap;
    m_bytesMarkedInHeap = 0;
}

#if ENABLE(AOT)
bool SlotVisitor::markLeafCell(JSCell* cell)
{
    ASSERT(m_collector.hasHeap(*cell->heap()));

    if (m_isAnalyzingHeap) [[unlikely]] {
        appendUnbarriered(cell);
        return false;
    }

    if (Heap::testAndSetMarked(m_markingVersion, cell))
        return false;

    cell->setCellState(CellState::PossiblyBlack);

    CellContainer container = cell->cellContainer();
    container.noteMarked();

    m_visitCount++;
    noteBytesMarked(container.vm().heap, container.cellSize());
    return true;
}

void SlotVisitor::reportLeafCellExtraMemoryVisited(JSC::Heap& heap, size_t size)
{
    if (&heap != m_extraMemoryHeap) [[unlikely]] {
        flushExtraMemoryVisited();
        m_extraMemoryHeap = &heap;
    }
    m_nonCellVisitCount += size;
    m_extraMemoryInHeap += size;
}
#endif

class SetCurrentCellScope {
public:
    SetCurrentCellScope(SlotVisitor& visitor, const JSCell* cell)
        : m_visitor(visitor)
    {
        ASSERT(!m_visitor.m_currentCell);
        m_visitor.m_currentCell = const_cast<JSCell*>(cell);
    }

    ~SetCurrentCellScope()
    {
        ASSERT(m_visitor.m_currentCell);
        m_visitor.m_currentCell = nullptr;
    }

private:
    SlotVisitor& m_visitor;
};

ALWAYS_INLINE void SlotVisitor::visitChildren(const JSCell* cell)
{
    // Funny story: it's possible for the object to be black already, if we barrier the object at
    // about the same time that it's marked. That's fine. It's a gnarly and super-rare race. It's
    // not clear to me that it would be correct or profitable to bail here if the object is already
    // black.

    cell->setCellState(CellState::PossiblyBlack);

    WTF::storeLoadFence();

    visitChildrenOfBlackCell(cell);
}

ALWAYS_INLINE void SlotVisitor::visitChildrenOfBlackCell(const JSCell* cell)
{
    ASSERT(isMarked(cell));
    
    SetCurrentCellScope currentCellScope(*this, cell);
    
    if (false) {
        dataLog("Visiting ", RawPointer(cell));
        if (!m_isFirstVisit)
            dataLog(" (subsequent)");
        dataLog("\n");
    }
    
    switch (cell->type()) {
    case StringType:
        JSString::visitChildren(const_cast<JSCell*>(cell), *this);
        break;
        
    case FinalObjectType:
        JSFinalObject::visitChildren(const_cast<JSCell*>(cell), *this);
        break;

    case ArrayType:
        JSArray::visitChildren(const_cast<JSCell*>(cell), *this);
        break;
        
    default:
        // FIXME: This could be so much better.
        // https://bugs.webkit.org/show_bug.cgi?id=162462
#if CPU(X86_64)
        if (Options::dumpZappedCellCrashData()) [[unlikely]] {
            Structure* structure = cell->structure();
            if (structure) [[likely]] {
                const MethodTable* methodTable = &structure->classInfoForCells()->methodTable;
                methodTable->visitChildren(const_cast<JSCell*>(cell), *this);
                break;
            }
            reportZappedCellAndCrash(const_cast<JSCell*>(cell));
        }
#endif
        cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), *this);
        break;
    }

    if (m_isAnalyzingHeap) [[unlikely]] {
        if (m_isFirstVisit) {
            if (HeapAnalyzer* analyzer = cell->vm().activeHeapAnalyzer())
                analyzer->analyzeNode(const_cast<JSCell*>(cell));
        }
    }
}

#if ENABLE(AOT)
class CompiledLayoutVisitor {
    static NEVER_INLINE void appendOutOfLine(SlotVisitor& visitor, JSCell* cell) { visitor.appendUnbarriered(cell); }
    static NEVER_INLINE void appendHiddenOutOfLine(SlotVisitor& visitor, JSCell* cell) { visitor.appendHiddenUnbarriered(cell); }
    static NEVER_INLINE void visitThroughMethodTable(SlotVisitor& visitor, Structure* structure, const JSCell* cell) { structure->classInfoForCells()->methodTable.visitChildren(const_cast<JSCell*>(cell), visitor); }
    static NEVER_INLINE void visitOutOfLine(SlotVisitor& visitor, const JSCell* cell)
    {
        switch (cell->type()) {
        case SlimClosureType:
        case StructureType:
        case GetterSetterType:
        case JSCellButterflyType:
        case JSFunctionType:
        case JSPromiseType:
        case JSFullPromiseReactionType:
        case JSSlimPromiseReactionType:
        case JSDateType:
            return visitOutsideLoop(visitor, cell);
        default:
            return visitor.visitChildrenOfBlackCell(cell);
        }
    }
    static NEVER_INLINE void expand(MarkStackArray& stack) { stack.expand(); }

    class Appender {
    public:
        ALWAYS_INLINE Appender(SlotVisitor& visitor)
            : m_visitor(visitor)
            , m_stack(visitor.m_collectorStack)
            , m_markingVersion(visitor.m_markingVersion)
            , m_markingStamp(Structure::markingStampFor(visitor.m_markingVersion))
        {
            loadTop();
        }

        ALWAYS_INLINE ~Appender() { storeTop(); }

        uint8_t markingStamp() const { return m_markingStamp; }

        ALWAYS_INLINE void appendCell(JSCell* cell)
        {
            using Word = uint64_t;
            MarkedBlock& block = cell->markedBlock();
            HeapVersion version = block.markingVersion();
            uintptr_t dependency = 0;
#if CPU(ARM64)
            __asm__ volatile("eor %w[dependency], %w[version], %w[version]" : [dependency] "=r"(dependency), [version] "+r"(version));
#else
            WTF::loadLoadFence();
#endif
            if (version != m_markingVersion) [[unlikely]] {
                storeTop();
                appendOutOfLine(m_visitor, cell);
                loadTop();
                return;
            }
            uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
            auto* word = std::bit_cast<Atomic<Word>*>(std::bit_cast<uintptr_t>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64)) + dependency);
            if (word->loadRelaxed() >> (atom % 64) & 1)
                return;
            Word mask = static_cast<Word>(1) << (atom % 64);
            if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
                return;
            cell->setCellState(CellState::PossiblyGrey);
            block.noteMarked();
            ++m_visitor.m_visitCount;
            m_visitor.m_bytesMarkedInHeap += block.cellSize();
            if (m_nextSlot == m_endOfSlots) [[unlikely]] {
                storeTop();
                expand(m_stack);
                loadTop();
            }
            *m_nextSlot++ = cell;
        }

        ALWAYS_INLINE void appendValue(JSValue value)
        {
            if ((std::rotr(static_cast<uint64_t>(JSValue::encode(value)), 3) - 1) >> 45)
                return;
            if (value.asCell()->isPreciseAllocation()) [[unlikely]] {
                storeTop();
                appendHiddenOutOfLine(m_visitor, value.asCell());
                loadTop();
            } else
                appendCell(value.asCell());
        }

        ALWAYS_INLINE void appendStructure(Structure* structure)
        {
            uint8_t& stampAndFlags = structure->markingStampAndFlags();
            if (stampAndFlags != m_markingStamp) [[unlikely]] {
                appendCell(structure);
                stampAndFlags = (stampAndFlags & ~Structure::markingStampMask) | m_markingStamp;
            }
        }

    private:
        ALWAYS_INLINE void storeTop()
        {
            m_stack.m_top = m_nextSlot - m_firstSlot;
#if ASSERT_ENABLED
            m_stack.m_segments.head()->m_top = m_nextSlot - m_firstSlot;
#endif
        }

        ALWAYS_INLINE void loadTop()
        {
            m_firstSlot = m_stack.m_segments.head()->data();
            m_nextSlot = m_firstSlot + m_stack.m_top;
            m_endOfSlots = m_firstSlot + MarkStackArray::s_segmentCapacity;
        }

        SlotVisitor& m_visitor;
        MarkStackArray& m_stack;
        const JSCell** m_firstSlot;
        const JSCell** m_nextSlot;
        const JSCell** m_endOfSlots;
        const HeapVersion m_markingVersion;
        const uint8_t m_markingStamp;
    };

    static NEVER_INLINE void visitOutsideLoop(SlotVisitor& visitor, const JSCell* cell)
    {
        if (!tryVisitOutsideLoop(visitor, cell)) [[unlikely]]
            visitor.visitChildrenOfBlackCell(cell);
    }

    static ALWAYS_INLINE bool tryAppendObjectWithoutElements(SlotVisitor& visitor, Appender& appender, const JSCell* cell, StructureID structureID, Structure* structure)
    {
        auto* object = static_cast<JSObjectWithButterfly*>(const_cast<JSCell*>(cell));
        PropertyOffset maxOffset = structure->maxOffset();
        IndexingType indexingMode;
        Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
        if (indexingMode != NonArray)
            return false;
        Butterfly* butterfly;
        Dependency butterflyDependency = indexingModeDependency.consume(object)->fencedButterfly(butterfly);
        if (butterflyDependency.consume(cell)->structureID() != structureID || butterflyDependency.consume(structure)->maxOffset() != maxOffset)
            return false;
        appender.appendStructure(structure);
        if (!butterfly)
            return true;
        visitor.markAuxiliary(butterfly->base(0, Structure::outOfLineCapacity(maxOffset)));
        const WriteBarrierBase<Unknown>* endOfProperties = butterfly->propertyStorage();
        for (const WriteBarrierBase<Unknown>* property = endOfProperties - Structure::outOfLineSize(maxOffset); property != endOfProperties; ++property)
            appender.appendValue(property->get());
        return true;
    }

    static ALWAYS_INLINE bool tryVisitOutsideLoop(SlotVisitor& visitor, const JSCell* cell)
    {
        using namespace AOT::Typed;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cell);
        switch (cell->type()) {
        case SlimClosureType: {
            static_assert(SlimClosure::offsetOfCaptures() == sizeof(SlimClosure));
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            WTF::loadLoadFence();
            Structure* structure = structureID.decode();
            if (structure->typeInfo().type() != SlimClosureType)
                return false;
            uint64_t word = *reinterpret_cast<const uint64_t*>(bytes + SlimClosure::offsetOfWord());
            WTF::loadLoadFence();
            if (cell->structureID() != structureID)
                return false;
            Appender appender(visitor);
            appender.appendStructure(structure);
            if (!word)
                return true;
            AOT::Image* image = AOT::Image::withCode();
            const Data* data = image->at<Data>(image->header().typedDataOffset);
            const EncodedJSValue* capture = reinterpret_cast<const EncodedJSValue*>(bytes + SlimClosure::offsetOfCaptures());
            for (const EncodedJSValue* end = capture + data->at<std::array<uint8_t, 2>>(data->closureCountsOffset)[AOT::ClosureWord::functionOf(word)][0]; capture != end; ++capture)
                appender.appendValue(JSValue::decode(*capture));
            return true;
        }
        case StructureType: {
            auto* structure = static_cast<Structure*>(const_cast<JSCell*>(cell));
            if (structure->variant() != Structure::StructureVariant::Normal || structure->existingAOTFields().aotInstance) [[unlikely]]
                return false;
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            ConcurrentJSLocker locker(structure->m_lock);
            appender.appendValue(std::bit_cast<JSCell*>(structure->m_realm.get()));
            structure->markingStampAndFlags() = (structure->markingStampAndFlags() & ~Structure::markingStampMask) | appender.markingStamp();
            if (!structure->isObject())
                structure->m_cachedPrototypeChain.clear();
            else {
                appender.appendValue(structure->m_prototype.get());
                appender.appendValue(std::bit_cast<JSCell*>(structure->m_cachedPrototypeChain.get()));
            }
            appender.appendValue(structure->m_previousOrRareData.get());
            if (structure->isPinnedPropertyTable() || structure->protectPropertyTableWhileTransitioning())
                appender.appendValue(std::bit_cast<JSCell*>(structure->m_propertyTableUnsafe.get()));
            else if (structure->m_propertyTableUnsafe)
                structure->m_propertyTableUnsafe.clear();
            if (visitor.collectionScope() != CollectionScope::Full)
                appender.appendValue(structure->m_transitionTable.trySingleTransition());
            return true;
        }
        case GetterSetterType: {
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            appender.appendValue(*reinterpret_cast<JSCell* const*>(bytes + GetterSetter::offsetOfGetter()));
            appender.appendValue(*reinterpret_cast<JSCell* const*>(bytes + GetterSetter::offsetOfSetter()));
            return true;
        }
        case JSFunctionType:
        case JSPromiseType:
        case JSDateType: {
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            Structure* structure = structureID.decode();
            const ClassInfo* classInfo = structure->classInfoForCells();
            std::array<EncodedJSValue, 4> references;
            unsigned count = 0;
            uint64_t closureWord = 0;
            auto addValueAt = [&](ptrdiff_t offset) ALWAYS_INLINE_LAMBDA { references[count++] = *reinterpret_cast<const EncodedJSValue*>(bytes + offset); };
            if (classInfo == JSPromise::info()) {
                references[count++] = JSValue::encode(static_cast<const JSPromise*>(cell)->payloadCell());
                addValueAt(JSPromise::offsetOfSlot());
            } else if (classInfo == JSFunction::info() || classInfo == JSFunctionWithFields::info() || classInfo == Closure::info()) {
                addValueAt(JSCallee::offsetOfScopeChain());
                references[count++] = *reinterpret_cast<const EncodedJSValue*>(bytes + JSFunction::offsetOfExecutableOrRareData()) & ~static_cast<EncodedJSValue>(JSFunction::rareDataTag);
                if (classInfo == JSFunctionWithFields::info()) {
                    static_assert(JSFunctionWithFields::numberOfInternalFields == 2);
                    addValueAt(JSFunctionWithFields::offsetOfInternalFields());
                    addValueAt(JSFunctionWithFields::offsetOfInternalFields() + sizeof(EncodedJSValue));
                } else if (classInfo == Closure::info())
                    closureWord = static_cast<const Closure*>(cell)->entry();
            } else if (classInfo != DateInstance::info())
                return false;
            Appender appender(visitor);
            if (!tryAppendObjectWithoutElements(visitor, appender, cell, structureID, structure))
                return false;
            for (unsigned i = 0; i < count; ++i)
                appender.appendValue(JSValue::decode(references[i]));
            if (!closureWord)
                return true;
            AOT::Image* image = AOT::Image::withCode();
            const Data* data = image->at<Data>(image->header().typedDataOffset);
            const EncodedJSValue* capture = reinterpret_cast<const EncodedJSValue*>(bytes + Closure::offsetOfCaptures());
            for (const EncodedJSValue* end = capture + data->at<std::array<uint8_t, 2>>(data->closureCountsOffset)[AOT::ClosureWord::functionOf(closureWord)][0]; capture != end; ++capture)
                appender.appendValue(JSValue::decode(*capture));
            return true;
        }
        case JSFullPromiseReactionType: {
            auto* reaction = static_cast<const JSFullPromiseReaction*>(cell);
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            appender.appendValue(reaction->promise());
            appender.appendValue(reaction->next());
            appender.appendValue(reaction->onFulfilled());
            appender.appendValue(reaction->onRejected());
            appender.appendValue(reaction->context());
            return true;
        }
        case JSSlimPromiseReactionType: {
            auto* reaction = static_cast<const JSSlimPromiseReaction*>(cell);
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            appender.appendValue(reaction->promise());
            appender.appendValue(reaction->next());
            appender.appendValue(reaction->handlerOrContext());
            return true;
        }
        case JSCellButterflyType: {
            Appender appender(visitor);
            appender.appendStructure(cell->structure());
            if (!hasContiguous(cell->indexingType()))
                return true;
            Butterfly* butterfly = static_cast<const JSCellButterfly*>(cell)->toButterfly();
            const WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
            for (unsigned i = 0, length = butterfly->publicLength(); i < length; ++i)
                appender.appendValue(elements[i].get());
            return true;
        }
        default:
            return false;
        }
    }

public:
    template<bool drainsCollectorStack, bool mutatorIsStopped>
    static NEVER_INLINE void drain(SlotVisitor& visitor, MarkStackArray& stack, unsigned countdown)
    {
        using namespace AOT::Typed;
        using Word = uint64_t;
        const uintptr_t structureBase = structureIDBase();
        const uintptr_t cageBase = MarkedBlockCage::base();
        const HeapVersion markingVersion = visitor.m_markingVersion;
        visitor.noteBytesMarked(visitor.collector().heap(), 0);
        MarkStackArray& collectorStack = visitor.m_collectorStack;
        const JSCell** firstSlot = collectorStack.m_segments.head()->data();
        const JSCell** nextSlot = firstSlot + collectorStack.m_top;
        const JSCell** endOfSlots = firstSlot + MarkStackArray::s_segmentCapacity;
#if CPU(X86_64)
        size_t& visitCount = visitor.m_visitCount;
        size_t& bytesVisited = visitor.m_bytesMarkedInHeap;
        size_t& nonCellVisitCount = visitor.m_nonCellVisitCount;
#else
        size_t visitCount = 0;
        size_t bytesVisited = 0;
        size_t nonCellVisitCount = 0;
#endif
        const uint8_t markingStamp = Structure::markingStampFor(markingVersion);

        auto storeTop = [&] ALWAYS_INLINE_LAMBDA {
            collectorStack.m_top = nextSlot - firstSlot;
#if ASSERT_ENABLED
            collectorStack.m_segments.head()->m_top = nextSlot - firstSlot;
#endif
        };

        auto loadTop = [&] ALWAYS_INLINE_LAMBDA {
            firstSlot = collectorStack.m_segments.head()->data();
            nextSlot = firstSlot + collectorStack.m_top;
            endOfSlots = firstSlot + MarkStackArray::s_segmentCapacity;
        };

        auto appendCell = [&](JSCell* cell) ALWAYS_INLINE_LAMBDA {
            MarkedBlock& block = cell->markedBlock();
            HeapVersion version = block.markingVersion();
            uintptr_t dependency = 0;
#if CPU(ARM64)
            __asm__ volatile("eor %w[dependency], %w[version], %w[version]" : [dependency] "=r"(dependency), [version] "+r"(version));
#else
            WTF::loadLoadFence();
#endif
            if (version != markingVersion) [[unlikely]] {
                storeTop();
                appendOutOfLine(visitor, cell);
                loadTop();
                return;
            }
            static_assert(std::is_same_v<std::remove_cvref_t<decltype(block.marks().storage()[0])>, Word>);
            static_assert(MarkedBlock::atomSize == 16 && MarkedBlock::atomsPerBlock % 64 == 0);
            uintptr_t atom = std::bit_cast<uintptr_t>(cell) / MarkedBlock::atomSize;
            auto* word = std::bit_cast<Atomic<Word>*>(std::bit_cast<uintptr_t>(block.marks().storage().data() + atom / 64 % (MarkedBlock::atomsPerBlock / 64)) + dependency);
            if (word->loadRelaxed() >> (atom % 64) & 1)
                return;
            Word mask = static_cast<Word>(1) << (atom % 64);
            if (word->exchangeOr(mask, std::memory_order_relaxed) & mask) [[unlikely]]
                return;
            cell->setCellState(CellState::PossiblyGrey);
            block.noteMarked();
            ++visitCount;
            bytesVisited += block.cellSize();
            if (nextSlot == endOfSlots) [[unlikely]] {
                storeTop();
                expand(collectorStack);
                loadTop();
            }
            *nextSlot++ = cell;
        };

        auto appendValue = [&](JSValue value) ALWAYS_INLINE_LAMBDA {
            static_assert(!(JSValue::NotCellMask & 0x0001fffffffffff8ull) && (JSValue::NotCellMask & 7) && !JSValue::ValueEmpty);
            if ((std::rotr(static_cast<uint64_t>(JSValue::encode(value)), 3) - 1) >> 45)
                return;
            if (value.asCell()->isPreciseAllocation()) [[unlikely]] {
                storeTop();
                appendHiddenOutOfLine(visitor, value.asCell());
                loadTop();
            } else
                appendCell(value.asCell());
        };

        auto appendNarrowReference = [&](uint32_t word) ALWAYS_INLINE_LAMBDA {
            if (word >= minCell32)
                appendCell(std::bit_cast<JSCell*>(cageBase + (static_cast<uintptr_t>(word) << MarkedBlockCage::shift)));
        };

        auto appendStructure = [&](Structure* structure) ALWAYS_INLINE_LAMBDA -> bool {
            uint8_t& stampAndFlags = structure->markingStampAndFlags();
            if (stampAndFlags != markingStamp) [[unlikely]] {
                if (stampAndFlags & Structure::isStructureWithExtraProperties)
                    return false;
                if ((stampAndFlags & Structure::markingStampMask) != markingStamp) {
                    appendCell(structure);
                    stampAndFlags = (stampAndFlags & ~Structure::markingStampMask) | markingStamp;
                }
            }
            return true;
        };

        auto markAuxiliary = [&](HeapCell* cell) ALWAYS_INLINE_LAMBDA {
            if (cell->isPreciseAllocation()) [[unlikely]] {
                visitor.markAuxiliary(cell);
                return;
            }
            MarkedBlock& block = cell->markedBlock();
            Dependency dependency = block.aboutToMark(markingVersion, cell);
            if (block.testAndSetMarked(cell, dependency))
                return;
            block.noteMarked();
            ++visitCount;
            size_t size = block.cellSize();
            bytesVisited += size;
            nonCellVisitCount += size;
        };

        auto visitExtension = [&](uint32_t extensionWord, const uint32_t* offset) ALWAYS_INLINE_LAMBDA {
            const uint8_t* extension = std::bit_cast<const uint8_t*>(cageBase + (static_cast<uintptr_t>(extensionWord) << MarkedBlockCage::shift));
            markAuxiliary(std::bit_cast<HeapCell*>(extension));
            uint32_t size = *reinterpret_cast<const uint32_t*>(extension + offsetOfExtensionSize);
            uint32_t numberOfBoxed = offset[0];
            uint32_t numberOfNarrow = offset[1];
            offset += 2;
            const uint32_t* endOfBoxed = offset + numberOfBoxed;
            for (; offset != endOfBoxed && *offset + sizeof(EncodedJSValue) <= size; ++offset)
                appendValue(JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(extension + *offset)));
            offset = endOfBoxed;
            for (const uint32_t* end = offset + numberOfNarrow; offset != end && *offset + sizeof(uint32_t) <= size; ++offset)
                appendNarrowReference(*reinterpret_cast<const uint32_t*>(extension + *offset));
        };

        auto tryVisitArray = [&](const JSCell* cell) ALWAYS_INLINE_LAMBDA -> bool {
            auto* array = static_cast<JSObjectWithButterfly*>(const_cast<JSCell*>(cell));
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            Structure* structure = std::bit_cast<Structure*>(structureBase + structureID.bits());
            PropertyOffset maxOffset = structure->maxOffset();
            IndexingType indexingMode;
            Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
            if (indexingMode != ArrayWithContiguous && indexingMode != ArrayWithInt32 && indexingMode != ArrayWithDouble && indexingMode != ArrayWithUndecided)
                return false;
            Butterfly* butterfly;
            Dependency butterflyDependency = indexingModeDependency.consume(array)->fencedButterfly(butterfly);
            if (butterfly) {
                if (butterflyDependency.consume(cell)->structureID() != structureID || butterflyDependency.consume(structure)->maxOffset() != maxOffset)
                    return false;
            }
            appendStructure(structure);
            if (!butterfly)
                return true;
            markAuxiliary(static_cast<HeapCell*>(butterfly->base(0, Structure::outOfLineCapacity(maxOffset))));
            const WriteBarrierBase<Unknown>* endOfProperties = butterfly->propertyStorage();
            for (const WriteBarrierBase<Unknown>* property = endOfProperties - Structure::outOfLineSize(maxOffset); property != endOfProperties; ++property)
                appendValue(property->get());
            if (indexingMode != ArrayWithContiguous)
                return true;
            const WriteBarrier<Unknown>* elements = butterfly->contiguous().data();
            for (unsigned i = 0, length = butterfly->publicLength(); i < length; ++i)
                appendValue(elements[i].get());
            return true;
        };

        auto tryVisitString = [&](const JSCell* cell, Structure* structure) ALWAYS_INLINE_LAMBDA -> bool {
            appendStructure(structure);
            uintptr_t pointer = static_cast<const JSString*>(cell)->fiberConcurrently();
            if (pointer & JSString::isRopeInPointer) {
                auto& fibers = *static_cast<const JSRopeString*>(cell);
                if (pointer & JSRopeString::isSubstringInPointer) {
                    appendCell(fibers.fiber1());
                    return true;
                }
                JSString* fiber = std::bit_cast<JSString*>(pointer & JSRopeString::stringMask);
                if (!fiber)
                    return true;
                appendCell(fiber);
                fiber = fibers.fiber1();
                if (!fiber)
                    return true;
                appendCell(fiber);
                fiber = fibers.fiber2();
                if (fiber)
                    appendCell(fiber);
                return true;
            }
            if (StringImpl* impl = std::bit_cast<StringImpl*>(pointer)) {
                if constexpr (drainsCollectorStack) {
                    if (!AOT::Image::isBackedByStaticData(*impl)) [[likely]]
                        visitor.reportLeafCellExtraMemoryVisited(cell->vm().heap, impl->costDuringGC());
                }
            }
            return true;
        };

        auto tryVisitObjectWithoutElements = [&](const JSCell* cell, bool hasInlineProperties) ALWAYS_INLINE_LAMBDA -> bool {
            auto* object = static_cast<JSObjectWithButterfly*>(const_cast<JSCell*>(cell));
            StructureID structureID = cell->structureID();
            if (structureID.isNuked())
                return false;
            Structure* structure = std::bit_cast<Structure*>(structureBase + structureID.bits());
            PropertyOffset maxOffset = structure->maxOffset();
            IndexingType indexingMode;
            Dependency indexingModeDependency = structure->fencedIndexingMode(indexingMode);
            if (indexingMode != NonArray)
                return false;
            Butterfly* butterfly;
            Dependency butterflyDependency = indexingModeDependency.consume(object)->fencedButterfly(butterfly);
            if (butterflyDependency.consume(cell)->structureID() != structureID || butterflyDependency.consume(structure)->maxOffset() != maxOffset)
                return false;
            appendStructure(structure);
            if (butterfly) {
                markAuxiliary(static_cast<HeapCell*>(butterfly->base(0, Structure::outOfLineCapacity(maxOffset))));
                const WriteBarrierBase<Unknown>* endOfProperties = butterfly->propertyStorage();
                for (const WriteBarrierBase<Unknown>* property = endOfProperties - Structure::outOfLineSize(maxOffset); property != endOfProperties; ++property)
                    appendValue(property->get());
            }
            if (hasInlineProperties) {
                const WriteBarrierBase<Unknown>* property = object->inlineStorageUnsafe();
                for (const WriteBarrierBase<Unknown>* end = property + std::min<unsigned>(maxOffset + 1, structure->inlineCapacity()); property != end; ++property)
                    appendValue(property->get());
            }
            return true;
        };

        auto tryVisitMapOrSet = [&](const JSCell* cell) ALWAYS_INLINE_LAMBDA -> bool {
            if (!tryVisitObjectWithoutElements(cell, false))
                return false;
            static_assert(sizeof(JSMap) == sizeof(JSSet));
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cell);
            appendValue(*reinterpret_cast<JSCell* const*>(bytes + JSMap::offsetOfStorage()));
            appendValue(*reinterpret_cast<JSCell* const*>(bytes + JSMap::offsetOfCompactTable()));
            return true;
        };

        auto tryVisitCompactTable = [&](const JSCell* cell, Structure* structure) ALWAYS_INLINE_LAMBDA -> bool {
            using AOT::Typed::CompactTable;
            auto* table = static_cast<const CompactTable*>(cell);
            Locker locker { const_cast<JSCell*>(cell)->cellLock() };
            if (!table->isLive() || table->keyWidth() != CompactTable::Width::Narrow || table->valueWidth() == CompactTable::Width::Wide)
                return false;
            appendStructure(structure);
            std::span keys = table->narrowKeys();
            WTF::loadLoadFence();
            if (table->valueWidth() == CompactTable::Width::None) {
                for (uint32_t key : keys)
                    appendNarrowReference(key);
                return true;
            }
            const uint32_t* values = keys.data() + table->capacity();
            for (size_t entry = 0; entry < keys.size(); ++entry) {
                appendNarrowReference(keys[entry]);
                appendNarrowReference(values[entry]);
            }
            return true;
        };

        auto tryVisit = [&](const JSCell* cell) ALWAYS_INLINE_LAMBDA -> bool {
            Structure* structure = std::bit_cast<Structure*>(structureBase + cell->structureID().decontaminate().bits());
            auto& aotFields = structure->existingAOTFields();
            if (!aotFields.isCompiledCellStructure()) {
                JSType type = cell->type();
                if (type == ArrayType)
                    return tryVisitArray(cell);
                if (type == StringType)
                    return tryVisitString(cell, structure);
                if (type == FinalObjectType)
                    return tryVisitObjectWithoutElements(cell, true);
                if (type == JSMapType || type == JSSetType)
                    return tryVisitMapOrSet(cell);
                if (type == CellType && structure->classInfoForCells() == AOT::Typed::CompactTable::info())
                    return tryVisitCompactTable(cell, structure);
                return false;
            }
            const LayoutData* layout = aotFields.compiledLayout();
            if (!layout)
                layout = &environmentLayout(*aotFields.compiledEnvironmentData(), structure->firstCompiledEnvironmentShape(), cell);
            const uint8_t* fields = reinterpret_cast<const uint8_t*>(cell);
            if (!appendStructure(structure)) [[unlikely]] {
                uint32_t offsetOfExtraProperties = layout->offsetOfExtraProperties();
                if (!offsetOfExtraProperties) {
                    storeTop();
                    visitThroughMethodTable(visitor, structure, cell);
                    loadTop();
                    return true;
                }
                appendCell(structure);
                appendNarrowReference(*reinterpret_cast<const uint32_t*>(fields + offsetOfExtraProperties));
            }
            if (!layout->referencesOffset)
                return true;
            const uint32_t* references = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(layout) + layout->referencesOffset);
            const uint32_t* offset = references + 3;
            for (const uint32_t* end = offset + references[0] + references[1]; offset != end; ++offset)
                appendValue(JSValue::decode(*reinterpret_cast<const EncodedJSValue*>(fields + *offset)));
            for (const uint32_t* end = offset + references[2]; offset != end; ++offset)
                appendNarrowReference(*reinterpret_cast<const uint32_t*>(fields + *offset));
            if (layout->hasExtension) {
                if (uint32_t extensionWord = *reinterpret_cast<const uint32_t*>(fields + offsetOfExtension))
                    visitExtension(extensionWord, offset);
            }
            return true;
        };

#if CPU(X86_64)
        for (; countdown; --countdown) {
            const JSCell* cell;
            if constexpr (drainsCollectorStack) {
                if (nextSlot == firstSlot)
                    break;
                cell = *--nextSlot;
            } else {
                if (!stack.canRemoveLast())
                    break;
                cell = stack.removeLast();
            }
            cell->setCellState(CellState::PossiblyBlack);
            if constexpr (!mutatorIsStopped)
                WTF::storeLoadFence();
            if (tryVisit(cell))
                continue;
            storeTop();
            visitOutOfLine(visitor, cell);
            loadTop();
        }
#else
        auto popAndPrefetch = [&] ALWAYS_INLINE_LAMBDA -> const JSCell* {
            if (!countdown)
                return nullptr;
            if constexpr (drainsCollectorStack) {
                if (nextSlot == firstSlot)
                    return nullptr;
                --countdown;
                const JSCell* cell = *--nextSlot;
                __builtin_prefetch(cell);
                return cell;
            } else {
                if (!stack.canRemoveLast())
                    return nullptr;
                --countdown;
                return stack.popAndPrefetch();
            }
        };

        for (const JSCell* next = popAndPrefetch(); next;) {
            const JSCell* cell = next;
            next = popAndPrefetch();
            cell->setCellState(CellState::PossiblyBlack);
            if constexpr (!mutatorIsStopped)
                WTF::storeLoadFence();
            if (tryVisit(cell))
                continue;
            storeTop();
            visitOutOfLine(visitor, cell);
            loadTop();
        }
#endif

        storeTop();
#if !CPU(X86_64)
        visitor.m_visitCount += visitCount;
        visitor.m_bytesMarkedInHeap += bytesVisited;
        visitor.m_nonCellVisitCount += nonCellVisitCount;
#endif
    }
};
#endif // ENABLE(AOT)

void SlotVisitor::visitAsConstraint(const JSCell* cell)
{
    m_isFirstVisit = false;
    visitChildren(cell);
}

inline void SlotVisitor::propagateExternalMemoryVisitedIfNecessary()
{
    if (m_isFirstVisit)
        flushExtraMemoryVisited();
}

void SlotVisitor::flushExtraMemoryVisited()
{
    if (m_extraMemoryInHeap.hasOverflowed())
        m_extraMemoryHeap->reportExtraMemoryVisited(std::numeric_limits<size_t>::max());
    else if (m_extraMemoryInHeap)
        m_extraMemoryHeap->reportExtraMemoryVisited(m_extraMemoryInHeap);
    m_extraMemoryInHeap = 0;
}

void SlotVisitor::donateKnownParallel(MarkStackArray& from, MarkStackArray& to)
{
    // NOTE: Because we re-try often, we can afford to be conservative, and
    // assume that donating is not profitable.

    // Avoid locking when a thread reaches a dead end in the object graph.
    if (from.size() < 2)
        return;

    constexpr size_t minCellsPerHelper = 512;
    if (from.size() < minCellsPerHelper && !collector().hasStartedHelpers())
        return;

    // If there's already some shared work queued up, be conservative and assume
    // that donating more is not profitable.
    if (to.size())
        return;

    // If we're contending on the lock, be conservative and assume that another
    // thread is already donating.
    if (!collector().m_markingMutex.tryLock())
        return;
    Locker locker { AdoptLock, collector().m_markingMutex };

    // Otherwise, assume that a thread will go idle soon, and donate.
    from.donateSomeCellsTo(to);

#if USE(BUN_JSC_ADDITIONS)
    if (Options::useAOT()) {
        collector().startHelpers();
        collector().m_markingConditionVariable.notifyOne();
        return;
    }
#endif
    collector().m_markingConditionVariable.notifyAll();
}

void SlotVisitor::donateKnownParallel()
{
    forEachMarkStack(
        [&] (MarkStackArray& stack) -> IterationStatus {
            donateKnownParallel(stack, correspondingGlobalStack(stack));
            return IterationStatus::Continue;
        });
}

NEVER_INLINE void SlotVisitor::drain(MonotonicTime timeout)
{
    if (!m_isInParallelMode) {
        dataLog("FATAL: attempting to drain when not in parallel mode.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }

    while (!hasElapsed(timeout)) {
        IterationStatus status = forEachMarkStack(
            [&] (MarkStackArray& stack) -> IterationStatus {
                if (stack.isEmpty())
                    return IterationStatus::Continue;

                stack.refill();

                m_isFirstVisit = (&stack == &m_collectorStack);

                // The highest cost of GC marking is a cache miss when reading a cell, coming from the GC marker stack,
                // because each cell would be likely placed in a random place. We perform software prefetching onto
                // one next cell while accessing the current cell to make memory fetching in flight while handling
                // the current cell.
                unsigned countdown = Options::minimumNumberOfScansBetweenRebalance();
#if ENABLE(AOT)
                if (Options::useAOT() && !m_isAnalyzingHeap) {
                    if (!m_isFirstVisit)
                        CompiledLayoutVisitor::drain<false, false>(*this, stack, countdown);
                    else if (m_mutatorsAreStopped)
                        CompiledLayoutVisitor::drain<true, true>(*this, stack, countdown);
                    else
                        CompiledLayoutVisitor::drain<true, false>(*this, stack, countdown);
                    return IterationStatus::Done;
                }
#endif
                auto popAndPrefetch = [&] ALWAYS_INLINE_LAMBDA -> const JSCell* {
                    if (!countdown || !stack.canRemoveLast())
                        return nullptr;
                    --countdown;
                    return stack.popAndPrefetch();
                };
                for (const JSCell* next = popAndPrefetch(); next;) {
                    const JSCell* cell = next;
                    next = popAndPrefetch();
                    cell->setCellState(CellState::PossiblyBlack);
                    WTF::storeLoadFence();
                    visitChildrenOfBlackCell(cell);
                }
                return IterationStatus::Done;
            });
        propagateExternalMemoryVisitedIfNecessary();
        if (status == IterationStatus::Continue)
            break;

        donateKnownParallel();
    }
}

size_t SlotVisitor::performIncrementOfDraining(size_t bytesRequested)
{
    RELEASE_ASSERT(m_isInParallelMode);

    size_t cellsRequested = bytesRequested / MarkedBlock::atomSize;
    {
        Locker locker { collector().m_markingMutex };
        forEachMarkStack(
            [&] (MarkStackArray& stack) -> IterationStatus {
                cellsRequested -= correspondingGlobalStack(stack).transferTo(stack, cellsRequested);
                return cellsRequested ? IterationStatus::Continue : IterationStatus::Done;
            });
    }

    size_t cellBytesVisited = 0;
    m_nonCellVisitCount = 0;

    auto bytesVisited = [&] () -> size_t {
        return cellBytesVisited + m_nonCellVisitCount;
    };

    auto isDone = [&] () -> bool {
        return bytesVisited() >= bytesRequested;
    };
    
    while (!isDone()) {
        IterationStatus status = forEachMarkStack(
            [&](MarkStackArray& stack) -> IterationStatus {
                if (stack.isEmpty() || isDone())
                    return IterationStatus::Continue;

                stack.refill();

                m_isFirstVisit = (&stack == &m_collectorStack);

                unsigned countdown = Options::minimumNumberOfScansBetweenRebalance();
                auto popAndPrefetch = [&] ALWAYS_INLINE_LAMBDA -> const JSCell* {
                    if (!countdown || !stack.canRemoveLast() || isDone())
                        return nullptr;
                    --countdown;
                    return stack.popAndPrefetch();
                };
                for (const JSCell* next = popAndPrefetch(); next;) {
                    const JSCell* cell = next;
                    next = popAndPrefetch();
                    cellBytesVisited += cell->cellSize();
                    visitChildren(cell);
                }
                return IterationStatus::Done;
            });
        propagateExternalMemoryVisitedIfNecessary();
        if (status == IterationStatus::Continue)
            break;
        donateKnownParallel();
    }

    donateAll();

    return bytesVisited();
}

bool SlotVisitor::didReachTermination()
{
    Locker locker { collector().m_markingMutex };
    return didReachTermination(locker);
}

bool SlotVisitor::didReachTermination(const AbstractLocker& locker)
{
    return !collector().m_numberOfActiveParallelMarkers
        && !hasWork(locker);
}

bool SlotVisitor::hasWork(const AbstractLocker&)
{
    return !isEmpty()
        || !collector().m_sharedCollectorMarkStack->isEmpty()
        || !collector().m_sharedMutatorMarkStack->isEmpty();
}

NEVER_INLINE SlotVisitor::SharedDrainResult SlotVisitor::drainFromShared(SharedDrainMode sharedDrainMode, MonotonicTime timeout)
{
    ASSERT(m_isInParallelMode);
    
    ASSERT(Options::numberOfGCMarkers());

    bool isActive = false;
    while (true) {
        RefPtr<SharedTask<void(SlotVisitor&)>> bonusTask;
        
        {
            Locker locker { collector().m_markingMutex };
            if (isActive)
                collector().m_numberOfActiveParallelMarkers--;
            collector().m_numberOfWaitingParallelMarkers++;
            auto stopWaiting = makeScopeExit([&] {
                locker.assertIsHolding(collector().m_markingMutex);
                collector().m_numberOfWaitingParallelMarkers--;
            });

            if (sharedDrainMode == MainDrain) {
                while (true) {
                    if (hasElapsed(timeout))
                        return SharedDrainResult::TimedOut;

                    if (didReachTermination(locker)) {
                        // No marker needs waking here. There is no work which can be processed in the helpers.
                        return SharedDrainResult::Done;
                    }
                    
                    if (hasWork(locker))
                        break;

                    collector().m_markingConditionVariable.waitUntil(collector().m_markingMutex, timeout);
                }
            } else {
                ASSERT(sharedDrainMode == HelperDrain);

                if (hasElapsed(timeout))
                    return SharedDrainResult::TimedOut;
                
                if (didReachTermination(locker)) {
                    // This is necessary to wake up MainDrain side.
                    collector().m_markingConditionVariable.notifyAll();
                    
                    // If we're in concurrent mode, then we know that the mutator will eventually do
                    // the right thing because:
                    // - It's possible that the collector has the conn. In that case, the collector will
                    //   wake up from the notification above. This will happen if the app released heap
                    //   access. Native apps can spend a lot of time with heap access released.
                    // - It's possible that the mutator will allocate soon. Then it will check if we
                    //   reached termination. This is the most likely outcome in programs that allocate
                    //   a lot.
                    // - WebCore never releases access. But WebCore has a runloop. The runloop will check
                    //   if we reached termination.
                    // So, this tells the runloop that it's got things to do.
                    m_collector.heap().m_stopIfNecessaryTimer->scheduleSoon();
                }

                auto isReady = [&] () -> bool {
                    locker.assertIsHolding(collector().m_markingMutex);
                    return hasWork(locker)
                        || collector().m_bonusVisitorTask
                        || collector().m_parallelMarkersShouldExit;
                };

                collector().m_markingConditionVariable.waitUntil(collector().m_markingMutex, timeout, isReady);

                if (collector().m_parallelMarkersShouldExit)
                    return SharedDrainResult::Done;

                if (!hasWork(locker) && collector().m_bonusVisitorTask)
                    bonusTask = collector().m_bonusVisitorTask;
            }
            
            if (!bonusTask && isEmpty()) {
                forEachMarkStack(
                    [&] (MarkStackArray& stack) -> IterationStatus {
                        locker.assertIsHolding(collector().m_markingMutex);
                        stack.stealSomeCellsFrom(
                            correspondingGlobalStack(stack),
                            collector().m_numberOfWaitingParallelMarkers);
                        return IterationStatus::Continue;
                    });
#if USE(BUN_JSC_ADDITIONS)
                if (Options::useAOT() && hasWork(locker))
                    collector().m_markingConditionVariable.notifyOne();
#endif
            }

            collector().m_numberOfActiveParallelMarkers++;
        }

        if (bonusTask) {
            bonusTask->run(*this);

            // The main thread could still be running, and may run for a while. Unless we clear the task
            // ourselves, we will keep looping around trying to run the task.
            {
                Locker locker { collector().m_markingMutex };
                if (collector().m_bonusVisitorTask == bonusTask)
                    collector().m_bonusVisitorTask = nullptr;
                bonusTask = nullptr;
                collector().m_bonusVisitorTaskConditionVariable.notifyOne();
            }
        } else {
            RELEASE_ASSERT(!isEmpty());
            drain(timeout);
        }
        
        isActive = true;
    }
}

SlotVisitor::SharedDrainResult SlotVisitor::drainInParallel(MonotonicTime timeout)
{
    donateAndDrain(timeout);
    return drainFromShared(MainDrain, timeout);
}

SlotVisitor::SharedDrainResult SlotVisitor::drainInParallelPassively(MonotonicTime timeout)
{
    ASSERT(m_isInParallelMode);
    
    ASSERT(Options::numberOfGCMarkers());
    
    JSC::Heap& heap = m_collector.heap();
    if (Options::numberOfGCMarkers() == 1
        || (heap.m_worldState.load() & Heap::mutatorWaitingBit)
        || !heap.hasHeapAccess()
        || heap.worldIsStopped()) {
        // This is an optimization over drainInParallel() when we have a concurrent mutator but
        // otherwise it is not profitable.
        return drainInParallel(timeout);
    }

    Locker locker { collector().m_markingMutex };
    donateAll(locker);

    // Wait for termination.
    for (;;) {
        if (hasElapsed(timeout))
            return SharedDrainResult::TimedOut;

        if (didReachTermination(locker)) {
            // No marker needs waking here. There is no work which can be processed in the helpers.
            return SharedDrainResult::Done;
        }

        collector().m_markingConditionVariable.waitUntil(collector().m_markingMutex, timeout);
    }
}

void SlotVisitor::donateAll()
{
    if (isEmpty())
        return;
    
    donateAll(Locker { collector().m_markingMutex });
}

void SlotVisitor::donateAll(const AbstractLocker&)
{
    forEachMarkStack(
        [&] (MarkStackArray& stack) -> IterationStatus {
            stack.transferTo(correspondingGlobalStack(stack));
            return IterationStatus::Continue;
        });

    if (!collector().m_sharedCollectorMarkStack->isEmpty() || !collector().m_sharedMutatorMarkStack->isEmpty())
        collector().startHelpers();
    collector().m_markingConditionVariable.notifyAll();
}

void SlotVisitor::donate()
{
    if (!m_isInParallelMode) {
        dataLog("FATAL: Attempting to donate when not in parallel mode.\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (Options::numberOfGCMarkers() == 1)
        return;
    
    donateKnownParallel();
}

void SlotVisitor::donateAndDrain(MonotonicTime timeout)
{
    donate();
    drain(timeout);
}

void SlotVisitor::didRace(const VisitRaceKey& race)
{
    dataLogLnIf(Options::verboseVisitRace(), toUTF8CString("GC visit race: ", race));
    
    Locker locker { collector().m_raceMarkStackLock };
    JSCell* cell = race.cell();
    cell->setCellState(CellState::PossiblyGrey);
    collector().m_raceMarkStack->append(cell);
}

void SlotVisitor::dump(PrintStream& out) const
{
    out.print("Collector: [", pointerListDump(collectorMarkStack()), "], Mutator: [", pointerListDump(mutatorMarkStack()), "]");
}

MarkStackArray& SlotVisitor::correspondingGlobalStack(MarkStackArray& stack)
{
    if (&stack == &m_collectorStack)
        return *collector().m_sharedCollectorMarkStack;
    RELEASE_ASSERT(&stack == &m_mutatorStack);
    return *collector().m_sharedMutatorMarkStack;
}

NO_RETURN_DUE_TO_CRASH void SlotVisitor::addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>)
{
    RELEASE_ASSERT_NOT_REACHED();
}

void SlotVisitor::addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>> task)
{
    RELEASE_ASSERT(m_currentSolver);
    RELEASE_ASSERT(m_currentConstraint);
    RELEASE_ASSERT(task);

    m_currentSolver->addParallelTask(task, *m_currentConstraint);
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
