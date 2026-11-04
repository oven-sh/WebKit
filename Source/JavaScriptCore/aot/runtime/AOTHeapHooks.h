/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "CollectionScope.h"
#include "JSExportMacros.h"
#include "MarkStack.h"
#include <memory>
#include <span>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC {

class AbstractSlotVisitor;
class JSCell;
class MarkingConstraint;
class SlotVisitor;
class Structure;
class VM;

namespace AOT {

struct Instance;

namespace HeapHooks {

void finalize(VM&, CollectionScope);
// For a thread of the collector, before it does anything for the heap of the VM: what it asks of the image it asks of the image of that VM.
void willWorkOnThisThread(VM&);
void didChangeBarrierThreshold(VM&, unsigned threshold, bool mutatorShouldBeFenced);
void willAnalyzeHeap(VM&);
void willCollectForExhaustedCage(VM&);
// No collection runs because it would find no memory for itself. The call in compiled code that led here throws the error that is owed, as after a collection.
void didFindNoRoomWhereCollectionMayRun(VM&);
String nameOfCompiledCallee(JSCell*);
// A cell that lists what the variables of a compiled module hold: ProgramStatics::willAnalyzeHeap(). A snapshot shows it as it shows the environment of a module.
bool standsForModuleInHeapSnapshots(JSCell*);
[[noreturn]] void crashOnCellVisitMismatch(VM&, const JSCell*);
// Where the cells of a live Structure keep their 32-bit references, from the start of the cell. Nothing for a Structure of other cells than compiled objects.
std::span<const uint32_t> offsetsOfNarrowReferences(const Structure*);

} // namespace HeapHooks

// They belong behind the constraints of the heap itself. Static variables are visited for a global object that is marked, and in a full collection the roots of the heap are what marks it.
JS_EXPORT_PRIVATE WTF::Vector<std::unique_ptr<MarkingConstraint>> createMarkingConstraints(VM&);
JS_EXPORT_PRIVATE void initializeCollectorWords(VM&, Instance&);

// Where a stub finds what MarkStackArray::append() reads and writes.
class MutatorMarkStackLayout final : public MarkStackArray {
public:
    using Segment = GCArraySegment<const JSCell*>;

    static constexpr size_t segmentCapacity = CapacityFromSize<Segment::blockSize>::value;
    static constexpr ptrdiff_t offsetOfFirstCellInSegment = sizeof(Segment);
#if ASSERT_ENABLED
    // The segment counts its cells too, and append() compares the two counts.
    static constexpr ptrdiff_t offsetOfTopInSegment = OBJECT_OFFSETOF(Segment, m_top);
#endif
    static ptrdiff_t offsetOfTop() { return OBJECT_OFFSETOF(MutatorMarkStackLayout, m_top); }
    // The list of segments begins with its head. initializeCollectorWords() checks that.
    static ptrdiff_t offsetOfHeadSegment() { return OBJECT_OFFSETOF(MutatorMarkStackLayout, m_segments); }
    static Segment* headSegmentOf(MarkStackArray& stack) { return static_cast<MutatorMarkStackLayout&>(stack).m_segments.head(); }
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
