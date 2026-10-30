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

class Image;
struct Instance;
struct VMState;

namespace HeapHooks {

// The phase is a number that changes with every change of the phase of the collector.
void finalize(VM&, CollectionScope, uint64_t phase);
// The function objects that name frames of compiled code. They come and go as frames do, so the collector visits them where it scans the stack.
void visitCalleesOfFrames(VM&, SlotVisitor&, uint64_t phase);
void visitCalleesOfFrames(VM&, AbstractSlotVisitor&, uint64_t phase);
// For a thread of the collector, before it does anything for the heap of the VM: what it asks of the image it asks of the image of that VM.
void willWorkOnThisThread(VM&);
// While it lives, this thread does the work of the VM's own thread, which is stopped. Of several images in one realm, the one that the VM runs is the one that runs here,
// and whoever runs another one for a while changes both. willWorkOnThisThread() looked while the program still ran, which goes from image to image.
class WorkForStoppedVMScope {
    WTF_MAKE_NONCOPYABLE(WorkForStoppedVMScope);
public:
    explicit WorkForStoppedVMScope(VM&);
    ~WorkForStoppedVMScope();

private:
    bool m_hasSeveralImages;
    VMState* m_stateOfThread { nullptr };
    const Image* m_imageOfThread { nullptr };
};
void didChangeBarrierThreshold(VM&, unsigned threshold, bool mutatorShouldBeFenced);
void willAnalyzeHeap(VM&);
void willCollectForExhaustedCage(VM&);
String nameOfCompiledCallee(JSCell*);
bool isModuleEnvironment(JSCell*);
[[noreturn]] void crashOnCellVisitMismatch(VM&, const JSCell*);
// Where the cells of a live Structure keep their 32-bit references, from the start of the cell. Nothing for a Structure of other cells than compiled objects.
std::span<const uint32_t> offsetsOfNarrowReferences(const Structure*);
// For a validating image, while the mutators are stopped. Allocates nothing and runs no code, so a thread that marks may call it.
void validateSlotsOf(VM&, const JSCell* compiledObject);

// While a heap snapshot names an object after the function that it finds under `constructor`. The name is a label for whoever reads the snapshot, so the compiler
// counts no read for it. A validating image lets this one look name a field that was removed as unread, and no other name and no other look.
class LookForConstructorBySnapshot {
    WTF_MAKE_NONCOPYABLE(LookForConstructorBySnapshot);
public:
    JS_EXPORT_PRIVATE LookForConstructorBySnapshot();
    JS_EXPORT_PRIVATE ~LookForConstructorBySnapshot();
};

} // namespace HeapHooks

// They belong behind the constraints of the heap itself. Static variables are visited for a global object that is marked, and in a full collection the roots of the heap are what marks it.
JS_EXPORT_PRIVATE WTF::Vector<std::unique_ptr<MarkingConstraint>> createMarkingConstraints(VM&);
JS_EXPORT_PRIVATE void initializeCollectorWords(VM&, Instance&);

#if ENABLE(DFG_DOES_GC_VALIDATION)
// How many places this thread has passed where a collection may start. A validating image counts them around a call that must not collect.
extern thread_local uint64_t placesThatMayCollect;
#endif

// For a validating image. A function cell in a frame is only ever called: its properties would lie on the heap, outlive the frame and point back into it.
void validateFunctionCellIsNotInFrame(VM&, const JSCell*);

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
